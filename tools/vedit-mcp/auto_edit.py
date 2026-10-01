#!/usr/bin/env python3
"""End-to-end auto-edit smoke test, driven over vedit JSON-RPC.

Drives a running vedit instance directly over the JSON-RPC socket, using
the same methods the MCP server forwards to. It does not go through the
MCP server itself -- that would need an MCP client in the loop -- but
every call here has an MCP tool of the same name.

Exercises the full edit pipeline (import, multi-track timeline, split,
trim, save, export) and probes the exported file.

Verified 2026-10-01 against a live instance: RESULT: PASS, exit 0, an
MP4 carrying h264 640x360 and aac at 2.453 s, plus a project XML. The
exported size depends on the sources, so nothing here pins a byte count --
the script probes the container and reports the size it got. The script
needs sources of at least 60 frames; the short clips in tests/data have only
50, so generate longer ones before running it.

Scenario:
  1. Reset instance state (remove tracks and library items).
  2. Import two sources (SRC_A and SRC_B env vars).
  3. Build a 2-track timeline using short clips from each source.
  4. Split a clip at its midpoint.
  5. Trim a clip.
  6. Save project XML.
  7. Export to MP4 and probe the result.

Exits 0 on PASS, 2 on FAIL. Requires exactly one running vedit
instance (discover via /tmp/vedit-*.sock).

Usage:
    SRC_A=/path/a.mp4 SRC_B=/path/b.mp4 python3 auto_edit.py
    SRC_A=/path/a.mp4 SRC_B=/path/b.mp4 \
        OUT_MP4=/tmp/out.mp4 OUT_XML=/tmp/out.xml \
        python3 auto_edit.py
"""
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vedit_client import VeditClient, discover_instances

SRC_A = os.environ.get('SRC_A')
SRC_B = os.environ.get('SRC_B')
OUT_MP4 = os.environ.get('OUT_MP4', '/tmp/vedit-auto-edit.mp4')
OUT_XML = os.environ.get('OUT_XML', '/tmp/vedit-auto-edit.xml')

missing = []
for label, val in (('SRC_A', SRC_A), ('SRC_B', SRC_B)):
    if not val:
        missing.append(label)
if missing:
    print(f'ERROR: required env vars not set: {", ".join(missing)}',
          file=sys.stderr)
    print('Usage: SRC_A=/path/to/a.mp4 SRC_B=/path/to/b.mp4 '
          'python3 auto_edit.py', file=sys.stderr)
    sys.exit(1)

for label, path in (('SRC_A', SRC_A), ('SRC_B', SRC_B)):
    if not os.path.isfile(path):
        print(f'ERROR: {label} file not found: {path}', file=sys.stderr)
        sys.exit(1)

insts = discover_instances()
if len(insts) != 1:
    print(f'ERROR: expected exactly 1 vedit instance, got {len(insts)}: '
          f'{insts}', file=sys.stderr)
    sys.exit(1)

c = VeditClient(insts[0].socket_path, timeout=300.0)
print(f'connected to {insts[0]}')


def call(method, params=None):
    r = c.call(method, params or {})
    print(f'  {method}({params or {}}) -> {r}')
    return r


# 0. Reset instance state: remove all tracks first, then library
#    items in reverse index order (removing shifts remaining indices).
state = call('state.get')
for t in state.get('tracks', []):
    call('track.remove', {'track_id': t['id']})
lib_items = state.get('library', [])
for item in sorted(lib_items, key=lambda x: -x['index']):
    call('library.remove', {'index': item['index']})

# 1. Import sources.
src_a_mi = call('library.import', {'path': SRC_A})['index']
src_b_mi = call('library.import', {'path': SRC_B})['index']

# Read frame counts from library state.
state = call('state.get')
a_frames = state['library'][0]['frame_count']
b_frames = state['library'][1]['frame_count']

for item in state['library']:
    print(f'  library[{item["index"]}] = {item["name"]} '
          f'{item["width"]}x{item["height"]} '
          f'{item["duration_sec"]:.1f}s '
          f'{item["frame_count"]} frames fps={item["fps"]:.2f}')

# Each source needs at least 60 frames.
MIN_FRAMES = 60
for label, fc in (('SRC_A', a_frames), ('SRC_B', b_frames)):
    if fc < MIN_FRAMES:
        print(f'ERROR: {label} has only {fc} frames, need >= {MIN_FRAMES}',
              file=sys.stderr)
        sys.exit(1)

# 2. Add tracks.
v1 = call('track.add', {'kind': 'video', 'name': 'V1'})['track_id']
v2 = call('track.add', {'kind': 'video', 'name': 'V2'})['track_id']

# 3. V1: first 30 frames of A, then first 30 frames of B (concat).
CLIP_LEN = 30
call('clip.append', {'track_id': v1, 'media_index': src_a_mi,
                     'in': 0, 'out': CLIP_LEN})
call('clip.append', {'track_id': v1, 'media_index': src_b_mi,
                     'in': 0, 'out': CLIP_LEN})

# 4. V2: frames 30..60 of B as overlay.
call('clip.append', {'track_id': v2, 'media_index': src_b_mi,
                     'in': CLIP_LEN, 'out': CLIP_LEN * 2})

# 5. Split V1 clip 2 at its midpoint.
state = call('state.get')
v1_clips = [t for t in state['tracks'] if t['id'] == v1][0]['clips']
clip2 = v1_clips[1]
split_pos = clip2['start'] + clip2['length'] // 2
call('clip.split', {'clip_id': clip2['id'], 'at_position': split_pos})

# 6. Trim V2 clip to ~2/3 of its current range.
state = call('state.get')
v2_clips = [t for t in state['tracks'] if t['id'] == v2][0]['clips']
v2_clip = v2_clips[0]
trim_length = v2_clip['length'] * 2 // 3
call('clip.trim', {'clip_id': v2_clip['id'],
                   'in': v2_clip['in'],
                   'out': v2_clip['in'] + trim_length})

# 7. Final state.
state = call('state.get')
print('\nFinal timeline:')
for t in state['tracks']:
    print(f'  Track {t["id"]} {t["name"]} ({t["kind"]}): '
          f'{len(t["clips"])} clip(s)')
    for cl in t['clips']:
        print(f'    clip id={cl["id"]} media={cl["media_index"]} '
              f'start={cl["start"]} len={cl["length"]} '
              f'in={cl["in"]} out={cl["out"]}')

# 8. Save project XML.
if os.path.exists(OUT_XML):
    os.remove(OUT_XML)
call('project.save', {'path': OUT_XML})
print(f'saved XML: {OUT_XML} ({os.path.getsize(OUT_XML)} bytes)')

# 9. Export to MP4.
if os.path.exists(OUT_MP4):
    os.remove(OUT_MP4)
job_id = call('export.start', {'path': OUT_MP4})['job_id']
while True:
    r = call('export.poll', {'job_id': job_id})
    if r['state'] != 'running':
        break
    time.sleep(0.2)
if r['state'] != 'done':
    print(f'FAIL: export {r["state"]}: {r.get("error", "")}', file=sys.stderr)
    sys.exit(2)
print(f'exported: {OUT_MP4} ({os.path.getsize(OUT_MP4)} bytes)')

# 10. Probe the exported file.
p = subprocess.run(
    ['ffprobe', '-v', 'error',
     '-show_entries',
     'format=duration:stream=codec_type,codec_name,width,height',
     '-of', 'json', OUT_MP4],
    capture_output=True, text=True)
probe = json.loads(p.stdout)
print('\nProbe result:')
print(json.dumps(probe, indent=2))

fmt = probe.get('format', {})
duration = float(fmt.get('duration', 0))
streams = probe.get('streams', [])
video = next((s for s in streams if s.get('codec_type') == 'video'), None)
audio = next((s for s in streams if s.get('codec_type') == 'audio'), None)

ok = True
if not video:
    print('FAIL: no video stream', file=sys.stderr); ok = False
if not audio:
    print('FAIL: no audio stream', file=sys.stderr); ok = False
if duration <= 0:
    print(f'FAIL: duration is zero or negative ({duration})',
          file=sys.stderr); ok = False
if video and (video.get('width', 0) < 320 or video.get('height', 0) < 240):
    print(f'FAIL: resolution {video.get("width")}x{video.get("height")} '
          f'too small', file=sys.stderr); ok = False

c.close()
print('\n=== RESULT:', 'PASS' if ok else 'FAIL', '===')
sys.exit(0 if ok else 2)

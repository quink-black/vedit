#!/usr/bin/env python3
"""End-to-end auto-edit smoke test via vedit JSON-RPC / MCP.

Drives a running vedit instance through the same RPC surface the MCP
server exposes, exercises the full edit pipeline (import, multi-track
timeline, split, trim, save, export), and verifies the exported file.

Scenario:
  1. Import two sources (BEAN, CCTV env vars, or built-in defaults).
  2. Build a 2-track timeline:
       V1: source A first 5s, then source B first 5s (concat).
       V2: source B from 10s..15s as an overlay.
  3. Split V1 clip 2 at its midpoint.
  4. Trim V2 clip to 3s.
  5. Save project XML.
  6. Export to MP4 and probe the result.

Exits 0 on PASS, 2 on FAIL. Requires exactly one running vedit
instance (discover via /tmp/vedit-*.sock).

Usage:
    python3 auto_edit.py
    BEAN=/path/a.mp4 CCTV=/path/b.mp4 \
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

BEAN = os.environ.get('BEAN', '/home/quink/Videos/bean.mp4')
CCTV = os.environ.get('CCTV', '/home/quink/Videos/cctv.mp4')
OUT_MP4 = os.environ.get('OUT_MP4', '/tmp/vedit-auto-edit.mp4')
OUT_XML = os.environ.get('OUT_XML', '/tmp/vedit-auto-edit.xml')

for label, path in (('BEAN', BEAN), ('CCTV', CCTV)):
    if not os.path.isfile(path):
        print(f'ERROR: {label} not found: {path}', file=sys.stderr)
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


# 1. Import sources.
bean_mi = call('library.import', {'path': BEAN})['index']
cctv_mi = call('library.import', {'path': CCTV})['index']

# 2. Add tracks.
v1 = call('track.add', {'kind': 'video', 'name': 'V1'})['track_id']
v2 = call('track.add', {'kind': 'video', 'name': 'V2'})['track_id']

# Inspect library to confirm fps for frame math.
state = call('state.get')
for item in state['library']:
    print(f'  library[{item["index"]}] = {item["name"]} '
          f'{item["width"]}x{item["height"]} '
          f'{item["duration_sec"]:.1f}s fps={item["fps"]:.2f}')

# 3. V1: source A first 5s (125 frames @ 25fps), then source B first 5s.
call('clip.append', {'track_id': v1, 'media_index': bean_mi,
                     'in': 0, 'out': 125})
call('clip.append', {'track_id': v1, 'media_index': cctv_mi,
                     'in': 0, 'out': 125})

# 4. V2: source B from 10s..15s as an overlay (250..375 frames).
call('clip.append', {'track_id': v2, 'media_index': cctv_mi,
                     'in': 250, 'out': 375})

# 5. Split V1 clip 2 at its midpoint. Look up its id from state.
state = call('state.get')
v1_clips = [t for t in state['tracks'] if t['id'] == v1][0]['clips']
clip2 = v1_clips[1]
split_pos = clip2['start'] + clip2['length'] // 2
call('clip.split', {'clip_id': clip2['id'], 'at_position': split_pos})

# 6. Trim V2 clip to 3s (75 frames).
state = call('state.get')
v2_clips = [t for t in state['tracks'] if t['id'] == v2][0]['clips']
v2_clip = v2_clips[0]
call('clip.trim', {'clip_id': v2_clip['id'],
                   'in': v2_clip['in'],
                   'out': v2_clip['in'] + 75})

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

# 9. Export to MP4 (async since R4: export.start returns a job id;
#    poll until terminal state).
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
elif video.get('codec_name') != 'h264':
    print(f'FAIL: video codec {video.get("codec_name")} != h264',
          file=sys.stderr); ok = False
if not audio:
    print('FAIL: no audio stream', file=sys.stderr); ok = False
elif audio.get('codec_name') != 'aac':
    print(f'FAIL: audio codec {audio.get("codec_name")} != aac',
          file=sys.stderr); ok = False
# V1 is 5s + 5s = 10s (250 frames @ 25). V2 is 3s but overlaid on V1.
if duration < 9.0 or duration > 11.0:
    print(f'FAIL: duration {duration:.2f}s outside [9, 11]',
          file=sys.stderr); ok = False
if video and (video.get('width', 0) < 320 or video.get('height', 0) < 240):
    print(f'FAIL: resolution {video.get("width")}x{video.get("height")} '
          f'too small', file=sys.stderr); ok = False

c.close()
print('\n=== RESULT:', 'PASS' if ok else 'FAIL', '===')
sys.exit(0 if ok else 2)

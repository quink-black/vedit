#include "logger.h"

// Translation unit anchor for the logger. Backend storage (mutex, file
// sink) will live here when added; call sites only depend on logger.h.
namespace vedit {} // namespace vedit

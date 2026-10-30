#pragma once
#include <windows.h>

#include <cstdint>

// Where a frame's time goes, logged every few seconds (translator::present,
// and OpenRemixHost.exe's own): times in performance-counter ticks.
namespace openremix::d3d9::perf {

inline int64_t now() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}

inline double milliseconds(int64_t ticks) {
  static const int64_t frequency = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
  }();
  return double(ticks) * 1000.0 / double(frequency);
}

// Adds the scope's time to a counter.
class Timer {
public:
  explicit Timer(int64_t &into) : m_into(into), m_start(now()) {}
  ~Timer() { m_into += now() - m_start; }
  Timer(const Timer &) = delete;
  Timer &operator=(const Timer &) = delete;

private:
  int64_t &m_into;
  int64_t m_start;
};

// The plugin's counters, since the last report.
struct Counters {
  int64_t capture = 0;       // draw hooks: state, classification, translation
  int64_t translate = 0;     //   of which the translator
  int64_t geometry = 0;      //     of which decoding and hashing (memo misses)
  int64_t vertexCapture = 0; //       of which vertex capture
  int64_t textureRead = 0;   // texture hashing and read-back
  int64_t send = 0;          // the bridge's sends (a full ring waits)
  int64_t present = 0;       // Present's renderer calls and frame slot wait
  uint32_t draws = 0, meshes = 0, textures = 0;
  uint32_t kinds[4] = {}; // draws by DrawKind: forwarded, skipped, UI, world
  uint64_t meshBytes = 0, textureBytes = 0, sentBytes = 0;
};
inline Counters g_counters;

} // namespace openremix::d3d9::perf

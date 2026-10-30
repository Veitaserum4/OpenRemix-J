#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace openremix::d3d9 {
namespace {
std::mutex g_lock;
FILE *g_file = nullptr;
} // namespace

void openLog(const std::filesystem::path &path) {
  std::lock_guard<std::mutex> lock(g_lock);
  if (!g_file)
    g_file = _wfopen(path.c_str(), L"w");
}

void log(const char *format, ...) {
  std::lock_guard<std::mutex> lock(g_lock);
  if (!g_file)
    return;
  va_list args;
  va_start(args, format);
  std::vfprintf(g_file, format, args);
  va_end(args);
  std::fputc('\n', g_file);
  std::fflush(g_file);
}

} // namespace openremix::d3d9

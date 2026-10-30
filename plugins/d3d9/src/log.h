#pragma once
#include <filesystem>

// openremix-d3d9.log next to the game (openremix.log = False turns it off).
namespace openremix::d3d9 {

void openLog(const std::filesystem::path &path);
void log(const char *format, ...);

} // namespace openremix::d3d9

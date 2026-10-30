#pragma once
#include <vector>

// Per-game defaults (dxvk-remix's application profiles), as rtx.conf text,
// applied below user.conf and rtx.conf. profiles.cpp is generated
// (scripts/gen_profiles.py).
namespace openremix::d3d9 {

struct Profile {
  const char *executable; // ECMAScript regular expression over the executable's path
  const char *settings;   // rtx.conf lines
};
const std::vector<Profile> &profiles();

} // namespace openremix::d3d9

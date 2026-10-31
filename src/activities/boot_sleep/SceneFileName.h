#pragma once

#include <cstddef>
#include <cstring>
#include <strings.h>

// File extensions of a charging-animation scene on the SD card. The format itself (magic "CPSC") is
// unchanged; only the extension was renamed. New files use .rhythmscene, and .cpscene stays readable so
// scenes users already copied to their card keep working.
namespace SceneFileName {

constexpr const char kExtension[] = ".rhythmscene";
constexpr const char kLegacyExtension[] = ".cpscene";

// Length of the scene extension `name` ends with (either spelling, case-insensitive), or 0.
inline size_t extensionLength(const char* name, const size_t length) {
  const size_t current = sizeof(kExtension) - 1;
  const size_t legacy = sizeof(kLegacyExtension) - 1;
  if (length > current && strcasecmp(name + length - current, kExtension) == 0) return current;
  if (length > legacy && strcasecmp(name + length - legacy, kLegacyExtension) == 0) return legacy;
  return 0;
}

// True when `name` ends with the legacy extension rather than the current one.
inline bool isLegacy(const char* name, const size_t length) {
  return extensionLength(name, length) == sizeof(kLegacyExtension) - 1;
}

}  // namespace SceneFileName

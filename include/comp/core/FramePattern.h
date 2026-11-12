#pragma once

#include <string>

namespace comp {

// Expands a frame-sequence path for a given frame number.
//
// Supported tokens (first one found is used):
//   ####     padded to the number of '#' characters   plate.####.exr -> plate.0042.exr
//   %04d     printf-style padding                     plate.%04d.exr -> plate.0042.exr
//   @@@      padded to the number of '@' characters   plate.@@@.exr  -> plate.042.exr
//
// A path with no token is returned unchanged (a single still image).
// Negative frames keep their sign: frame -5 with #### -> -005 (width includes sign),
// matching how most compositors treat negative frames.
std::string expandFramePattern(const std::string& pattern, int frame);

// True if the path contains a frame token.
bool hasFrameToken(const std::string& pattern);

}  // namespace comp

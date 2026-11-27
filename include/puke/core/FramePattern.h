#pragma once

#include <optional>
#include <string>

namespace puke {

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

// Files on disk that make up a sequence.
struct FileSequence {
    std::string pattern;  // with a frame token
    int first = 0, last = 0;
    int count = 0;  // files found: fewer than last - first + 1 if frames are missing
};

// The frames on disk for a path with a frame token (in its file name, not
// a folder's): every file that matches, with any frame number the token
// would expand to. #### matches 0042 and 12345 but not 42 or 00042; %d
// matches any digits. Returns nullopt if no file matches, or the path has
// no token. `pattern` is returned as given.
std::optional<FileSequence> findFrames(const std::string& pattern);

// The sequence a file is part of: the last number in its name, before the
// extension, is taken as the frame (shot.1001.exr, shot_v2_1001.exr), and
// the folder is searched for names that differ only in that number. Needs
// at least two files, so a lone shot_v2.exr stays a still. The pattern
// uses #### (as many # as the number has digits).
std::optional<FileSequence> sequenceOf(const std::string& file);

}  // namespace puke

#pragma once

#include <string>

#include "puke/core/Box.h"

namespace puke {

// An image size: the display window of images made from nothing (by
// generators such as Constant), and the project's working resolution.
// Square pixels only, for now.
struct Format {
    int width = 1920;
    int height = 1080;

    Box box() const { return {0, 0, width, height}; }
    std::string str() const { return std::to_string(width) + "x" + std::to_string(height); }
    bool operator==(const Format&) const = default;
};

}  // namespace puke

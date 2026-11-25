#pragma once

#include <array>
#include <map>
#include <stdexcept>
#include <string>
#include <variant>

namespace comp {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

using Vec2 = std::array<double, 2>;   // x, y
using Color = std::array<double, 4>;  // r, g, b, a

using ParamValue = std::variant<bool, int, double, std::string, Vec2, Color>;
using ParamMap = std::map<std::string, ParamValue>;

}  // namespace comp

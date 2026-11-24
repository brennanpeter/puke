#pragma once

// Helpers for nodes' paramSpecs() lists. Internal to src/nodes.

#include <string>
#include <utility>
#include <vector>

#include "comp/color/ColorConfig.h"
#include "comp/graph/Node.h"

namespace comp::specs {

using Kind = ParamSpec::Kind;

inline ParamSpec make(std::string name, Kind kind, ParamValue def, std::string hint, double min = 0, double max = 1) {
    ParamSpec s;
    s.name = std::move(name);
    s.kind = kind;
    s.defaultValue = std::move(def);
    s.hint = std::move(hint);
    s.min = min;
    s.max = max;
    return s;
}

inline ParamSpec choice(std::string name, std::vector<std::string> options, std::string def, std::string hint) {
    ParamSpec s = make(std::move(name), Kind::Choice, std::move(def), std::move(hint));
    s.options = std::move(options);
    return s;
}

// A color space choice: the current config's spaces (roles work too).
inline ParamSpec colorSpace(std::string name, std::string def, std::string hint, bool withAuto) {
    std::vector<std::string> options;
    if (withAuto) options.push_back("auto");
    try {
        for (auto& cs : ColorConfig::current()->colorSpaces()) options.push_back(std::move(cs));
    } catch (const Error&) {  // a broken $OCIO: still describe the parameter
    }
    ParamSpec s = choice(std::move(name), std::move(options), std::move(def), std::move(hint));
    s.openChoice = true;
    return s;
}

inline ParamSpec optional(ParamSpec s) {
    s.optional = true;
    return s;
}

}  // namespace comp::specs

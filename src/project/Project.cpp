#include "comp/project/Project.h"

#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "comp/nodes/Registry.h"

namespace comp {
namespace {

using Json = nlohmann::ordered_json;  // keeps keys in the order written
namespace fs = std::filesystem;

// ---------------------------------------------------------------- saving

Json paramToJson(const Node& node, const std::string& key, const ParamValue& value) {
    auto finite = [&](double d) {
        if (!std::isfinite(d))
            throw Error(node.name() + ": parameter '" + key + "' is " + (std::isnan(d) ? "NaN" : "infinite") +
                        ", which a project file can't store");
        return d;
    };
    return std::visit(
        [&](const auto& v) -> Json {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, double>) return finite(v);
            else if constexpr (std::is_same_v<T, Vec2>) return Json::array({finite(v[0]), finite(v[1])});
            else if constexpr (std::is_same_v<T, Color>)
                return Json::array({finite(v[0]), finite(v[1]), finite(v[2]), finite(v[3])});
            else return v;  // bool, int, string
        },
        value);
}

// ---------------------------------------------------------------- loading

// Error messages say where things went wrong: "shot.comp: node 2 (Grade1): ...".
struct Where {
    const std::string& source;
    std::string node;

    [[noreturn]] void fail(const std::string& msg) const {
        throw Error(source + ": " + (node.empty() ? "" : node + ": ") + msg);
    }
};

const char* kindOf(const Json& j) {
    switch (j.type()) {
        case Json::value_t::null: return "null";
        case Json::value_t::boolean: return "a bool";
        case Json::value_t::number_integer:
        case Json::value_t::number_unsigned:
        case Json::value_t::number_float: return "a number";
        case Json::value_t::string: return "a string";
        case Json::value_t::array: return "a list";
        case Json::value_t::object: return "an object";
        default: return "an unsupported value";
    }
}

void checkKeys(const Where& where, const Json& obj, std::initializer_list<const char*> allowed) {
    for (const auto& [key, value] : obj.items()) {
        bool ok = false;
        for (const char* a : allowed) ok = ok || key == a;
        if (!ok) where.fail("unknown key '" + key + "'");
    }
}

const Json& require(const Where& where, const Json& obj, const char* key, Json::value_t type, const char* expected) {
    auto it = obj.find(key);
    if (it == obj.end()) where.fail(std::string("missing '") + key + "'");
    const bool ok = it->type() == type ||
                    (type == Json::value_t::number_integer && it->type() == Json::value_t::number_unsigned);
    if (!ok) where.fail(std::string("'") + key + "' must be " + expected + ", not " + kindOf(*it));
    return *it;
}

ParamValue paramFromJson(const Where& where, const std::string& key, const Json& j) {
    auto bad = [&](const std::string& why) -> ParamValue { where.fail("parameter '" + key + "': " + why); };
    switch (j.type()) {
        case Json::value_t::boolean: return j.get<bool>();
        case Json::value_t::string: return j.get<std::string>();
        case Json::value_t::number_float: return j.get<double>();
        case Json::value_t::number_integer:
        case Json::value_t::number_unsigned: {
            if (j.is_number_unsigned() && j.get<std::uint64_t>() > std::uint64_t(std::numeric_limits<int>::max()))
                return bad(j.dump() + " doesn't fit in a 32-bit int");
            const auto n = j.get<std::int64_t>();
            if (n < std::numeric_limits<int>::min() || n > std::numeric_limits<int>::max())
                return bad(j.dump() + " doesn't fit in a 32-bit int");
            return static_cast<int>(n);
        }
        case Json::value_t::array: {
            for (const auto& e : j)
                if (!e.is_number()) return bad("lists may only contain numbers");
            if (j.size() == 2) return Vec2{j[0].get<double>(), j[1].get<double>()};
            if (j.size() == 4) return Color{j[0].get<double>(), j[1].get<double>(), j[2].get<double>(), j[3].get<double>()};
            return bad("a list must have 2 numbers (Vec2) or 4 (Color), not " + std::to_string(j.size()));
        }
        default: return bad(std::string("unsupported value: ") + kindOf(j));
    }
}

}  // namespace

std::string projectToJson(const Graph& graph) {
    Json nodes = Json::array();
    for (const auto& node : graph.nodes()) {
        Json inputs = Json::array();
        int last = -1;  // index of the last connected input, to drop trailing nulls
        for (int i = 0; i < node->maxInputs(); ++i)
            if (node->input(i)) last = i;
        for (int i = 0; i <= last; ++i)
            inputs.push_back(node->input(i) ? Json(node->input(i)->name()) : Json(nullptr));

        Json params = Json::object();
        for (const auto& [key, value] : node->params())  // std::map: already sorted
            params[key] = paramToJson(*node, key, value);

        Json entry{{"name", node->name()},
                   {"type", node->typeName()},
                   {"inputs", std::move(inputs)},
                   {"params", std::move(params)}};
        if (const auto& pos = node->position())
            entry["position"] = Json::array({std::round((*pos)[0]), std::round((*pos)[1])});
        nodes.push_back(std::move(entry));
    }
    const Json doc{{"format", "comp"}, {"version", kProjectFormatVersion}, {"nodes", std::move(nodes)}};
    return doc.dump(2) + "\n";
}

Graph projectFromJson(const std::string& text, const std::string& source) {
    Json doc;
    try {
        doc = Json::parse(text);
    } catch (const Json::parse_error& e) {
        throw Error(source + ": not valid JSON: " + e.what());
    }

    const Where top{source, {}};
    if (!doc.is_object()) top.fail("a project must be a JSON object");
    checkKeys(top, doc, {"format", "version", "nodes"});
    if (require(top, doc, "format", Json::value_t::string, "a string") != "comp")
        top.fail("not a comp project (format is '" + doc["format"].get<std::string>() + "')");
    const auto version = require(top, doc, "version", Json::value_t::number_integer, "a whole number").get<std::int64_t>();
    if (version > kProjectFormatVersion)
        top.fail("saved by a newer version of comp (format version " + std::to_string(version) +
                 "); this build reads up to version " + std::to_string(kProjectFormatVersion));
    if (version < 1) top.fail("invalid format version " + std::to_string(version));
    const Json& nodes = require(top, doc, "nodes", Json::value_t::array, "a list");

    // Create every node first, then connect, so nodes can appear in any order.
    Graph graph;
    std::vector<std::pair<Node*, const Json*>> pending;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const Json& n = nodes[i];
        Where where{source, "node " + std::to_string(i + 1)};
        if (!n.is_object()) where.fail(std::string("must be an object, not ") + kindOf(n));
        const auto name = require(where, n, "name", Json::value_t::string, "a string").get<std::string>();
        where.node += " (" + name + ")";
        checkKeys(where, n, {"name", "type", "inputs", "params", "position"});
        const auto type = require(where, n, "type", Json::value_t::string, "a string").get<std::string>();

        std::unique_ptr<Node> node;
        try {
            node = createNode(type, name);
        } catch (const Error& e) {
            where.fail(e.what());
        }
        if (auto it = n.find("position"); it != n.end()) {
            if (!it->is_array() || it->size() != 2 || !(*it)[0].is_number() || !(*it)[1].is_number())
                where.fail("'position' must be a list of 2 numbers");
            node->setPosition(Vec2{(*it)[0].get<double>(), (*it)[1].get<double>()});
        }
        if (auto it = n.find("params"); it != n.end()) {
            if (!it->is_object()) where.fail(std::string("'params' must be an object, not ") + kindOf(*it));
            for (const auto& [key, value] : it->items()) node->setParam(key, paramFromJson(where, key, value));
        }
        try {
            pending.emplace_back(&graph.add(std::move(node)), &n);
        } catch (const Error& e) {
            where.fail(e.what());
        }
    }

    for (std::size_t i = 0; i < pending.size(); ++i) {
        auto [node, n] = pending[i];
        const Where where{source, "node " + std::to_string(i + 1) + " (" + node->name() + ")"};
        auto it = n->find("inputs");
        if (it == n->end()) continue;
        if (!it->is_array()) where.fail(std::string("'inputs' must be a list, not ") + kindOf(*it));
        if (it->size() > static_cast<std::size_t>(node->maxInputs()))
            where.fail("lists " + std::to_string(it->size()) + " inputs, but " + node->typeName() + " nodes have " +
                       std::to_string(node->maxInputs()));
        for (std::size_t j = 0; j < it->size(); ++j) {
            const Json& in = (*it)[j];
            if (in.is_null()) continue;
            if (!in.is_string()) where.fail("input " + std::to_string(j) + " must be a node name or null");
            Node* src = graph.find(in.get<std::string>());
            if (!src) where.fail("input " + std::to_string(j) + ": no node named '" + in.get<std::string>() + "'");
            try {
                graph.connect(*src, *node, static_cast<int>(j));
            } catch (const Error& e) {
                where.fail(e.what());
            }
        }
    }
    return graph;
}

void saveProject(const Graph& graph, const fs::path& path) {
    const std::string text = projectToJson(graph);  // before touching the disk: may throw
    const fs::path tmp = path.string() + ".partial";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text;
        out.close();
        if (!out) {
            std::error_code ec;
            fs::remove(tmp, ec);
            throw Error("cannot write project '" + path.string() + "'");
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        const std::string msg = ec.message();
        fs::remove(tmp, ec);
        throw Error("cannot move finished project into place at '" + path.string() + "': " + msg);
    }
}

Graph loadProject(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw Error("cannot open project '" + path.string() + "'");
    std::stringstream ss;
    ss << in.rdbuf();
    return projectFromJson(ss.str(), path.string());
}

}  // namespace comp

// Tests for project files: saving, loading, the exact text format, and errors.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

#include "TestUtil.h"
#include "comp/engine/Engine.h"
#include "comp/graph/Graph.h"
#include "comp/nodes/Registry.h"
#include "comp/project/Project.h"

using namespace comp;
namespace fs = std::filesystem;

static fs::path g_dir;

static Node& add(Graph& g, const std::string& type, const std::string& name) {
    return g.add(createNode(type, name));
}

static std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The format is part of the project's public surface: changing it should be
// a deliberate decision, so this pins the exact text.
static void testExactFormat() {
    Graph g;
    auto& read = add(g, "Read", "Read1");
    read.setParam("file", std::string("plate.####.exr"));
    read.setParam("first", 1001);
    read.setParam("last", 1100);
    auto& grade = add(g, "Grade", "Grade1");
    grade.setParam("gain", Color{1.2, 1.1, 1.0, 1.0});
    grade.setParam("white_clamp", true);
    grade.setParam("multiply", 2.0);
    g.connect(read, grade, 0);

    const std::string expected = R"({
  "format": "comp",
  "version": 1,
  "nodes": [
    {
      "name": "Read1",
      "type": "Read",
      "inputs": [],
      "params": {
        "file": "plate.####.exr",
        "first": 1001,
        "last": 1100
      }
    },
    {
      "name": "Grade1",
      "type": "Grade",
      "inputs": [
        "Read1"
      ],
      "params": {
        "gain": [
          1.2,
          1.1,
          1.0,
          1.0
        ],
        "multiply": 2.0,
        "white_clamp": true
      }
    }
  ]
}
)";
    const std::string actual = projectToJson(g);
    CHECK(actual == expected);
    if (actual != expected) std::fprintf(stderr, "--- got:\n%s", actual.c_str());
}

static void testRoundTrip() {
    Graph g;
    auto& read = add(g, "Read", "Plate");
    read.setParam("file", std::string("a \"quoted\" path/ünïcode.exr"));
    auto& xf = add(g, "Transform", "Move");
    xf.setParam("translate", Vec2{-3.5, 1e-9});
    xf.setParam("rotate", 0.1 + 0.2);  // a double that needs all 17 digits
    xf.setParam("invert", false);
    xf.setParam("count", -7);
    xf.setParam("whole", 3.0);  // must come back as a double, not an int
    auto& merge = add(g, "Merge", "Merge1");
    auto& lone = add(g, "Grade", "Unconnected");
    (void)lone;
    g.connect(read, xf, 0);
    g.connect(xf, merge, 1);  // input 0 left empty: saved as [null, "Move"]

    const std::string text = projectToJson(g);
    CHECK(text.find("\"inputs\": [\n        null,\n        \"Move\"\n      ]") != std::string::npos);

    Graph back = projectFromJson(text, "test");
    CHECK(projectToJson(back) == text);  // stable: save(load(x)) == x
    CHECK(back.nodes().size() == 4);
    Node& m = *back.find("Move");
    CHECK(m.param<double>("rotate") == 0.1 + 0.2);
    CHECK(m.param<double>("whole") == 3.0);
    CHECK(m.param<int>("count") == -7);
    CHECK((m.param<Vec2>("translate") == Vec2{-3.5, 1e-9}));
    CHECK(m.param<bool>("invert") == false);
    CHECK(back.find("Plate")->param<std::string>("file") == "a \"quoted\" path/ünïcode.exr");
    Node& bm = *back.find("Merge1");
    CHECK(bm.input(0) == nullptr && bm.input(1) == &m);
    CHECK(m.input(0) == back.find("Plate"));
    CHECK(back.find("Unconnected")->input(0) == nullptr);
}

static std::string wrap(const std::string& nodes);

static void testPositions() {
    Graph g;
    auto& grade = add(g, "Grade", "Grade1");
    grade.setPosition(Vec2{120.4, -36.6});
    add(g, "Grade", "Unplaced");
    const std::string text = projectToJson(g);
    CHECK(text.find("\"position\": [\n        120.0,\n        -37.0\n      ]") != std::string::npos);
    const Graph back = projectFromJson(text);
    CHECK((back.find("Grade1")->position() == Vec2{120, -37}));
    CHECK(!back.find("Unplaced")->position());
    CHECK(projectToJson(back) == text);
    CHECK_THROWS(projectFromJson(wrap(R"({"name": "G", "type": "Grade", "position": [1]})"), "p"),
                 "p: node 1 (G): 'position' must be a list of 2 numbers");
}

static void testSettings() {
    Graph g;
    CHECK(projectToJson(g).find("settings") == std::string::npos);  // defaults: left out
    g.setSettings({1001, 1100, 23.976});
    const std::string text = projectToJson(g);
    CHECK(text.find(R"("settings": {
    "first_frame": 1001,
    "last_frame": 1100,
    "fps": 23.976,
    "format": {
      "width": 1920,
      "height": 1080
    }
  },)") != std::string::npos);
    CHECK((projectFromJson(text).settings() == ProjectSettings{1001, 1100, 23.976}));
    g.setSettings({1, 1, 24, {2048, 858}});  // only the format differs from the defaults: still written
    CHECK((projectFromJson(projectToJson(g)).settings().format == Format{2048, 858}));
    // Any key may be left out.
    const std::string head = R"({"format": "comp", "version": 1, "nodes": [], "settings": )";
    CHECK((projectFromJson(head + R"({"last_frame": 50}})").settings() == ProjectSettings{1, 50, 24}));
    CHECK_THROWS(projectFromJson(head + R"({"first_frame": 10, "last_frame": 5}})", "p"),
                 "p: settings: the last frame (5) is before the first (10)");
    CHECK_THROWS(projectFromJson(head + R"({"fps": 0}})", "p"), "p: settings: fps must be");
    CHECK_THROWS(projectFromJson(head + R"({"first_frame": 1.5}})", "p"), "'first_frame' must be a whole number");
    CHECK_THROWS(projectFromJson(head + R"({"start": 1}})", "p"), "p: settings: unknown key 'start'");
    CHECK((projectFromJson(head + R"({"format": {"height": 720}}})").settings().format == Format{1920, 720}));
    CHECK_THROWS(projectFromJson(head + R"({"format": {"width": 0}}})", "p"), "p: settings: format: 'width' must be 1 to 65536");
    CHECK_THROWS(projectFromJson(head + R"({"format": {"width": 12.5}}})", "p"), "'width' must be a whole number");
    CHECK_THROWS(projectFromJson(head + R"({"format": {"depth": 3}}})", "p"), "p: settings: format: unknown key 'depth'");
    CHECK_THROWS(projectFromJson(head + R"({"format": "HD"}})", "p"), "p: settings: format: must be an object");
    CHECK_THROWS(g.setSettings({1, 1, 24, {1920, 70000}}), "the format (1920x70000) must be 1 to 65536 pixels each way");
    CHECK_THROWS(projectFromJson(head + R"([1, 2]})", "p"), "p: settings: must be an object");
    CHECK_THROWS(g.setSettings({5, 4, 24}), "before the first");
    CHECK((g.settings() == ProjectSettings{1, 1, 24, {2048, 858}}));  // unchanged
}

static void testSomeNodes() {
    Graph g;
    auto& read = add(g, "Read", "Read1");
    auto& grade = add(g, "Grade", "Grade1");
    auto& xf = add(g, "Transform", "Transform1");
    g.connect(read, grade, 0);
    g.connect(grade, xf, 0);
    grade.setParam("mix", 0.5);
    // Grade1 and Transform1, but not Read1: Grade1's input comes out unconnected.
    const Graph part = projectFromJson(nodesToJson(g, {&xf, &grade}));
    CHECK(part.nodes().size() == 2 && part.nodes()[0]->name() == "Grade1");  // graph order
    CHECK(part.find("Grade1")->input(0) == nullptr);
    CHECK(part.find("Transform1")->input(0) == part.find("Grade1"));
    CHECK(part.find("Grade1")->param<double>("mix") == 0.5);
    CHECK(projectFromJson(nodesToJson(g, {})).nodes().empty());
}

static void testLoadOrderAndOptionalFields() {
    // Consumers may come before their inputs; inputs and params may be omitted.
    const Graph g = projectFromJson(R"({"format": "comp", "version": 1, "nodes": [
        {"name": "Grade1", "type": "Grade", "inputs": ["Read1"]},
        {"name": "Read1", "type": "Read", "params": {"file": "x.exr", "first": 1, "scale": [1, 2]}}
    ]})");
    CHECK(g.find("Grade1")->input(0) == g.find("Read1"));
    CHECK(g.find("Read1")->param<int>("first") == 1);
    CHECK((g.find("Read1")->param<Vec2>("scale") == Vec2{1, 2}));  // whole numbers in a list are fine
    CHECK(projectFromJson(R"({"format": "comp", "version": 1, "nodes": []})").nodes().empty());
}

static std::string wrap(const std::string& nodes) {
    return R"({"format": "comp", "version": 1, "nodes": [)" + nodes + "]}";
}

static void testErrors() {
    CHECK_THROWS(projectFromJson("{\"format\": \"comp\",\n \"version\": 1,\n \"nodes\": [}", "shot.comp"),
                 "shot.comp: not valid JSON");
    CHECK_THROWS(projectFromJson("[]", "p"), "p: a project must be a JSON object");
    CHECK_THROWS(projectFromJson(R"({"format": "nuke", "version": 1, "nodes": []})", "p"),
                 "not a comp project (format is 'nuke')");
    CHECK_THROWS(projectFromJson(R"({"version": 1, "nodes": []})", "p"), "p: missing 'format'");
    CHECK_THROWS(projectFromJson(R"({"format": "comp", "version": 2, "nodes": []})", "p"),
                 "saved by a newer version of comp (format version 2); this build reads up to version 1");
    CHECK_THROWS(projectFromJson(R"({"format": "comp", "version": "1", "nodes": []})", "p"),
                 "'version' must be a whole number, not a string");
    CHECK_THROWS(projectFromJson(R"({"format": "comp", "version": 1, "nodes": [], "extra": 1})", "p"),
                 "p: unknown key 'extra'");

    CHECK_THROWS(projectFromJson(wrap(R"({"type": "Grade"})"), "p"), "p: node 1: missing 'name'");
    CHECK_THROWS(projectFromJson(wrap(R"({"name": "G", "type": "Grade", "colour": 1})"), "p"),
                 "p: node 1 (G): unknown key 'colour'");
    CHECK_THROWS(projectFromJson(wrap(R"({"name": "B", "type": "Defocus"})"), "p"),
                 "p: node 1 (B): unknown node type 'Defocus'");
    CHECK_THROWS(projectFromJson(wrap(R"({"name": "G", "type": "Grade"}, {"name": "G", "type": "Grade"})"), "p"),
                 "p: node 2 (G): a node named 'G' already exists");
    CHECK_THROWS(projectFromJson(wrap(R"({"name": "G", "type": "Grade", "inputs": ["Nope"]})"), "p"),
                 "p: node 1 (G): input 0: no node named 'Nope'");
    CHECK_THROWS(projectFromJson(wrap(R"({"name": "R", "type": "Read"}, {"name": "G", "type": "Grade", "inputs": ["R", "R"]})"), "p"),
                 "p: node 2 (G): lists 2 inputs, but Grade nodes have 1");
    CHECK_THROWS(projectFromJson(wrap(R"({"name": "A", "type": "Grade", "inputs": ["B"]},
                                         {"name": "B", "type": "Grade", "inputs": ["A"]})"), "p"),
                 "would create a cycle");
    CHECK_THROWS(projectFromJson(wrap(R"({"name": "G", "type": "Grade", "inputs": [3]})"), "p"),
                 "input 0 must be a node name or null");
    CHECK_THROWS(projectFromJson(wrap(R"({"name": "G", "type": "Grade", "params": {"gain": [1, 2, 3]}})"), "p"),
                 "p: node 1 (G): parameter 'gain': a list must have 2 numbers (Vec2) or 4 (Color), not 3");
    CHECK_THROWS(projectFromJson(wrap(R"({"name": "G", "type": "Grade", "params": {"gain": ["a", "b"]}})"), "p"),
                 "parameter 'gain': lists may only contain numbers");
    CHECK_THROWS(projectFromJson(wrap(R"({"name": "G", "type": "Grade", "params": {"gain": {"r": 1}}})"), "p"),
                 "parameter 'gain': unsupported value: an object");
    CHECK_THROWS(projectFromJson(wrap(R"({"name": "G", "type": "Grade", "params": {"n": 3000000000}})"), "p"),
                 "parameter 'n': 3000000000 doesn't fit in a 32-bit int");
    CHECK_THROWS(projectFromJson(wrap(R"({"name": "G", "type": "Grade", "params": [1]})"), "p"),
                 "'params' must be an object, not a list");

    Graph g;
    add(g, "Grade", "G").setParam("gain", std::numeric_limits<double>::quiet_NaN());
    CHECK_THROWS(projectToJson(g), "G: parameter 'gain' is NaN, which a project file can't store");
}

static void testFiles() {
    Graph g;
    auto& read = add(g, "Read", "Read1");
    read.setParam("file", std::string("in.exr"));
    auto& write = add(g, "Write", "Write1");
    write.setParam("file", std::string("out.exr"));
    g.connect(read, write, 0);

    const fs::path path = g_dir / "shot.comp";
    saveProject(g, path);
    CHECK(fs::exists(path) && !fs::exists(g_dir / "shot.comp.partial"));
    CHECK(readFile(path) == projectToJson(g));
    const Graph back = loadProject(path);
    CHECK(projectToJson(back) == projectToJson(g));

    // A save that fails leaves the existing file alone.
    add(g, "Grade", "Bad").setParam("x", std::numeric_limits<double>::infinity());
    CHECK_THROWS(saveProject(g, path), "is infinite");
    CHECK(readFile(path) == projectToJson(back));

    CHECK_THROWS(loadProject(g_dir / "missing.comp"), "cannot open project");
    CHECK_THROWS(saveProject(back, g_dir / "no_such_dir" / "x.comp"), "cannot write project");

    std::ofstream(g_dir / "broken.comp") << "{\"format\": \"comp\", \"version\": 1, \"nodes\": [{\"name\": 5}]}";
    CHECK_THROWS(loadProject(g_dir / "broken.comp"), "broken.comp: node 1: 'name' must be a string, not a number");
}

static void testLoadedGraphRenders() {
    // A loaded project renders the same pixels as the graph it was saved from.
    test::ExrOptions o;
    const std::string file = (g_dir / "plate.exr").string();
    test::writeImage(file, o, [](int x, int y, int c) { return 0.01f * x + 0.02f * y + 0.1f * c; });

    Graph g;
    auto& read = add(g, "Read", "Read1");
    read.setParam("file", file);
    auto& grade = add(g, "Grade", "Grade1");
    grade.setParam("gain", Color{2.0, 1.5, 1.0, 1.0});
    grade.setParam("gamma", 1.3);
    auto& xf = add(g, "Transform", "Transform1");
    xf.setParam("rotate", 12.5);
    xf.setParam("center", Vec2{16, 16});
    g.connect(read, grade, 0);
    g.connect(grade, xf, 0);

    const Graph back = projectFromJson(projectToJson(g));
    Engine e;
    const Box region{-8, -8, 40, 40};
    const Image a = e.render(xf, {}, region);
    const Image b = e.render(*back.find("Transform1"), {}, region);
    bool same = a.region() == b.region();
    for (int y = region.y0; same && y < region.y1; ++y)
        for (int x = region.x0; same && x < region.x1; ++x)
            for (int c = 0; c < a.numChannels(); ++c) same = same && a.pixel(x, y)[c] == b.pixel(x, y)[c];
    CHECK(same);
}

int main() {
    g_dir = fs::current_path() / "test_project_tmp";
    fs::remove_all(g_dir);
    fs::create_directories(g_dir);
    try {
        testExactFormat();
        testRoundTrip();
        testLoadOrderAndOptionalFields();
        testPositions();
        testSomeNodes();
        testSettings();
        testErrors();
        testFiles();
        testLoadedGraphRenders();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "unexpected exception: %s\n", e.what());
        ++test::failures;
    }
    if (test::failures == 0) fs::remove_all(g_dir);
    return test::finish("project");
}

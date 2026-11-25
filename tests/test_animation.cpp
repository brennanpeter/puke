// Keyframe animation: curves, nodes, the engine, project files.

#include <cmath>
#include <cstdio>
#include <string>

#include "TestUtil.h"
#include "comp/core/Path.h"
#include "comp/engine/Engine.h"
#include "comp/engine/Snapshot.h"
#include "comp/graph/Graph.h"
#include "comp/nodes/Registry.h"
#include "comp/project/Project.h"

using namespace comp;
using test::SourceNode;
using Interp = Curve::Interp;

namespace {
double num(const ParamValue& v) { return std::holds_alternative<int>(v) ? std::get<int>(v) : std::get<double>(v); }
}  // namespace

static void testCurves() {
    Curve c;
    CHECK_THROWS(c.valueAt(1), "an animation curve with no keys");
    c.setKey(10, 1.0, Interp::Linear);
    c.setKey(20, 3.0, Interp::Linear);
    c.setKey(0, 0.0, Interp::Linear);  // kept in frame order
    CHECK(c.keys().size() == 3 && c.keys()[0].frame == 0 && c.keys()[2].frame == 20);
    // Holds outside the keys; straight lines between.
    CHECK(num(c.valueAt(-5)) == 0 && num(c.valueAt(25)) == 3);
    CHECK_NEAR(num(c.valueAt(5)), 0.5);
    CHECK_NEAR(num(c.valueAt(15)), 2.0);
    CHECK(num(c.valueAt(10)) == 1);  // exactly the key's value

    // Constant holds until the next key.
    c.setInterp(10, Interp::Constant);
    CHECK(num(c.valueAt(19.9)) == 1);
    CHECK_THROWS(c.setInterp(11, Interp::Linear), "no key at frame");

    // Smooth: flat at the first and last key, through the middle one with
    // the slope of its neighbours, and passing through every key.
    Curve s;
    s.setKey(0, 0.0);
    s.setKey(10, 10.0);
    s.setKey(20, 10.0);
    CHECK_NEAR(num(s.valueAt(10)), 10);
    CHECK(num(s.valueAt(1)) < 1.0);  // eases out of the first key (linear would be 1)
    const double d = num(s.valueAt(10.0001)) - num(s.valueAt(9.9999));
    CHECK_NEAR(d / 0.0002, 0.5);  // slope at 10: (10 - 0) / (20 - 0)
    CHECK(num(s.valueAt(15)) > 10.0);  // overshoots a little after it, as Catmull-Rom does
    // A line of evenly spaced keys stays a line inside.
    Curve line;
    for (int f = 0; f <= 40; f += 10) line.setKey(f, double(f));
    CHECK_NEAR(num(line.valueAt(15)), 15);
    CHECK_NEAR(num(line.valueAt(23)), 23);

    // Replacing a key keeps its interp unless given; removing.
    c.setKey(10, 5.0);
    CHECK(c.keyAt(10)->interp == Interp::Constant && num(c.keyAt(10)->value) == 5);
    CHECK(c.removeKey(10) && !c.removeKey(10) && c.keys().size() == 2);

    // Types: ints round; Vec2 and Color per component; strings and bools
    // hold; mixed types hold.
    Curve i;
    i.setKey(0, 0, Interp::Linear);
    i.setKey(10, 3, Interp::Linear);
    CHECK(std::get<int>(i.valueAt(5)) == 2);  // 1.5 rounds away
    Curve v;
    v.setKey(0, Vec2{0, 10}, Interp::Linear);
    v.setKey(10, Vec2{10, 20}, Interp::Linear);
    CHECK((std::get<Vec2>(v.valueAt(5)) == Vec2{5, 15}));
    Curve col;
    col.setKey(0, Color{0, 0, 0, 0}, Interp::Linear);
    col.setKey(4, Color{1, 2, 3, 4}, Interp::Linear);
    CHECK((std::get<Color>(col.valueAt(1)) == Color{0.25, 0.5, 0.75, 1}));
    Curve str;
    str.setKey(0, std::string("a.exr"));
    str.setKey(10, std::string("b.exr"));
    CHECK(std::get<std::string>(str.valueAt(9)) == "a.exr" && std::get<std::string>(str.valueAt(10)) == "b.exr");
    Curve b;
    b.setKey(0, false);
    b.setKey(10, true);
    CHECK(!std::get<bool>(b.valueAt(9.5)));
    Curve mixed;
    mixed.setKey(0, 1.0, Interp::Linear);
    mixed.setKey(10, Vec2{5, 5});
    CHECK(num(mixed.valueAt(9)) == 1.0);

    CHECK(Curve::parseInterp("linear") == Interp::Linear && std::string(Curve::interpName(Interp::Constant)) == "constant");
    CHECK_THROWS(Curve::parseInterp("bezier"), "unknown interpolation 'bezier' (use smooth, linear or constant)");
}

static void testShapes() {
    // Shapes with the same points move point by point, handles included.
    Curve c;
    c.setKey(0, std::string("M 0 0 L 10 0 C 10 5 5 10 0 10 Z"), Interp::Linear);
    c.setKey(10, std::string("M 10 0 L 20 0 C 20 15 15 20 10 20 Z"), Interp::Linear);
    const Path mid = Path::parse(std::get<std::string>(c.valueAt(5)));
    CHECK((mid.curves()[0][0].point == Path::Point{5, 0}));
    CHECK((mid.curves()[0][1].out == Path::Point{15, 10}) && (mid.curves()[0][2].in == Path::Point{10, 15}));
    // Different numbers of points: hold.
    c.setKey(20, std::string("M 0 0 L 1 0 L 1 1 L 0 1 Z"));
    CHECK(std::get<std::string>(c.valueAt(15)) == std::get<std::string>(c.keys()[1].value));
}

static void testNodes() {
    Graph g;
    Node& grade = g.add(createNode("Grade", "Grade1"));
    grade.setParam("multiply", 2.0);
    Curve c;
    c.setKey(1, 1.0, Interp::Linear);
    c.setKey(11, 3.0, Interp::Linear);
    grade.setCurve("multiply", c);  // replaces the plain value
    CHECK(grade.animated("multiply") && !grade.params().count("multiply"));
    CHECK(num(*grade.valueAt("multiply", 6)) == 2.0);
    CHECK(num(grade.paramsAt(11).at("multiply")) == 3.0);
    CHECK(!grade.valueAt("gamma", 1));
    grade.setParam("multiply", 5.0);  // replaces the animation
    CHECK(!grade.animated("multiply") && grade.params().count("multiply") && num(grade.params().at("multiply")) == 5.0);
    grade.setCurve("multiply", c);
    grade.clearParam("multiply");  // unsets it entirely
    CHECK(!grade.animated("multiply") && !grade.valueAt("multiply", 6));
    grade.setCurve("multiply", c);
    grade.setCurve("multiply", {});  // an empty curve: unset
    CHECK(!grade.animated("multiply") && !grade.valueAt("multiply", 6));
}

static void testRendering() {
    Graph g;
    auto& src = g.create<SourceNode>("Src", Box{0, 0, 4, 4}, ChannelList{"R", "G", "B", "A"},
                                     [](int, int, int) { return 1.0f; });
    Node& grade = g.add(createNode("Grade", "Grade1"));
    g.connect(src, grade, 0);
    Curve c;
    c.setKey(1, 1.0, Interp::Linear);
    c.setKey(11, 3.0, Interp::Linear);
    grade.setCurve("multiply", c);

    // Each frame renders its own value, through the cache too.
    Engine e({.threads = 2, .tileSize = 2, .cacheBytes = 1 << 20});
    for (int pass = 0; pass < 2; ++pass)
        for (double f : {1.0, 6.0, 11.0, 20.0})
            CHECK_NEAR(e.render(grade, {f}, {0, 0, 1, 1}).pixel(0, 0)[0], std::min(3.0, 1 + (f - 1) / 5));

    // A snapshot keeps the animation it was taken with.
    const Snapshot snap = Snapshot::of(grade);
    Curve c2 = c;
    c2.setKey(6, 10.0);
    grade.setCurve("multiply", c2);
    CHECK_NEAR(e.render(snap, {6}, {0, 0, 1, 1}).pixel(0, 0)[0], 2.0);
    CHECK_NEAR(e.render(grade, {6}, {0, 0, 1, 1}).pixel(0, 0)[0], 10.0);
    // An evaluated snapshot has plain values only.
    const Snapshot at6 = snap.at(6);
    CHECK(!at6.output().curves && num(at6.output().params->at("multiply")) == 2.0);
}

static void testProjects() {
    Graph g;
    Node& grade = g.add(createNode("Grade", "Grade1"));
    Curve c;
    c.setKey(1001, 1.0);
    c.setKey(1010.5, Color{1, 2, 3, 4}, Interp::Linear);
    c.setKey(1020, 2, Interp::Constant);
    grade.setCurve("gain", c);
    grade.setParam("mix", 0.5);
    const std::string text = projectToJson(g);
    CHECK(text.find(R"("gain": {
          "curve": [
            [
              1001,
              1.0
            ],)") != std::string::npos);
    CHECK(text.find(R"("linear")") != std::string::npos && text.find("1010.5") != std::string::npos);
    const Graph back = projectFromJson(text);
    CHECK(*back.find("Grade1")->curve("gain") == c);
    CHECK(num(back.find("Grade1")->params().at("mix")) == 0.5);
    CHECK(projectToJson(back) == text);

    auto load = [](const std::string& param) {
        return projectFromJson(R"({"format": "comp", "version": 1, "nodes": [{"name": "G", "type": "Grade", "params": {"gain": )" +
                                   param + "}}]}",
                               "p");
    };
    CHECK(load(R"({"curve": [[1, 0.5], [3, 1.5, "linear"]]})").find("G")->curve("gain")->keys().size() == 2);
    CHECK_THROWS(load(R"({"keys": []})"), "p: node 1 (G): parameter 'gain': unknown key 'keys'");
    CHECK_THROWS(load(R"({"curve": []})"), "parameter 'gain': an animated parameter needs a \"curve\"");
    CHECK_THROWS(load(R"({"curve": [[1]]})"), "parameter 'gain': key 1 must be [frame, value]");
    CHECK_THROWS(load(R"({"curve": [["a", 1]]})"), "parameter 'gain': key 1: the frame must be a number");
    CHECK_THROWS(load(R"({"curve": [[1, 1], [1, 2]]})"), "parameter 'gain': key 2: a second key at frame 1");
    CHECK_THROWS(load(R"({"curve": [[1, 1, "bezier"]]})"), "key 1: unknown interpolation 'bezier'");
    CHECK_THROWS(load(R"({"curve": [[1, 1, 2]]})"), "key 1: the interpolation must be a string");
    CHECK_THROWS(load(R"({"curve": [[1, {"curve": []}]]})"), "key 1: a key's value can't be animated itself");
    CHECK_THROWS(load(R"({"curve": [[1, [1, 2, 3]]]})"), "a list must have 2 numbers (Vec2) or 4 (Color)");
}

int main() {
    try {
        testCurves();
        testShapes();
        testNodes();
        testRendering();
        testProjects();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "unexpected exception: %s\n", e.what());
        return 1;
    }
    return test::finish("animation");
}

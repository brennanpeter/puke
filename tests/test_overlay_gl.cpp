// OFX overlays drawn with OpenGL (the test Point plugin's red cross), through
// the GUI's OverlayRenderer. Needs an OpenGL context: ctest runs it under
// xvfb-run where that's installed; without one it reports itself skipped.

#include <QGuiApplication>
#include <QImage>

#include <cstdio>

#include "OverlayRenderer.h"
#include "TestUtil.h"
#include "comp/graph/Graph.h"
#include "comp/nodes/Registry.h"
#include "comp/ofx/Overlay.h"

using namespace comp;

namespace {

bool red(const QImage& img, int x, int y) {
    const QColor c = img.pixelColor(x, y);
    return c.red() > 200 && c.green() < 60 && c.alpha() > 200;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    compgui::OverlayRenderer renderer;
    if (!renderer.available()) {
        std::printf("skipped: no OpenGL context here\n");
        return 77;
    }
    try {
        Graph g;
        Node& point = g.add(createNode("ofx:org.comp.test.Point", "Point1"));
        auto overlay = ofx::Overlay::create(point);
        CHECK(overlay != nullptr);
        if (!overlay) return test::finish("overlay GL");

        // A 200x100 image at 1:1, the whole of it in view.
        ofx::Overlay::View view;
        view.flipY = 100;
        view.viewportWidth = 200;
        view.viewportHeight = 100;
        const auto rect = ofx::Overlay::canonicalRect(view, 0, 0, 200, 100);
        CHECK(rect[0] == 0 && rect[1] == 0 && rect[2] == 200 && rect[3] == 100);

        // The cross at canonical (10, 10): comp row 100 - 10 = 90, arms 10 long.
        QImage img = renderer.render(*overlay, view, QSize(200, 100), rect);
        CHECK(img.size() == QSize(200, 100));
        CHECK(red(img, 5, 90) || red(img, 5, 89));
        CHECK(red(img, 10, 85) || red(img, 9, 85));
        CHECK(!red(img, 100, 50) && img.pixelColor(100, 50).alpha() == 0);  // transparent elsewhere

        // Moved, and zoomed out 2x: the cross follows, half the length on screen.
        point.setParam("point", Vec2{120, 60});  // comp (120, 40)
        view.pixelScale = 2;
        const auto wide = ofx::Overlay::canonicalRect(view, 0, -100, 400, 100);  // 400x200 image units
        img = renderer.render(*overlay, view, QSize(200, 100), wide);
        // (120, 40) in comp is screen (60, 70); the arms are 20 units = 10 pixels.
        CHECK(red(img, 55, 70) || red(img, 55, 69));
        CHECK(!red(img, 20, 45) && !red(img, 5, 90));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "unexpected exception: %s\n", e.what());
        ++test::failures;
    }
    return test::finish("overlay GL");
}

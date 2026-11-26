#pragma once

#include <QImage>
#include <QSize>

#include <array>
#include <memory>

#include "comp/ofx/Overlay.h"

class QOffscreenSurface;
class QOpenGLContext;
class QOpenGLFramebufferObject;

namespace compgui {

// Draws OFX overlays (which use OpenGL) offscreen, for the viewer to paint
// over its picture. Needs an OpenGL 2.1 compatibility context; where none
// can be made (a headless session) overlays aren't drawn, though their
// mouse and key handling still works.
class OverlayRenderer {
public:
    OverlayRenderer();
    ~OverlayRenderer();

    // Whether OpenGL is there (tried once).
    bool available();
    // A transparent image of `size` with the overlay drawn, mapping the
    // canonical rectangle `rect` (left, bottom, right, top) to it. Null if
    // OpenGL isn't available.
    QImage render(comp::ofx::Overlay& overlay, const comp::ofx::Overlay::View& view, QSize size,
                  const std::array<double, 4>& rect);

private:
    bool tried_ = false;
    std::unique_ptr<QOpenGLContext> context_;
    std::unique_ptr<QOffscreenSurface> surface_;
    std::unique_ptr<QOpenGLFramebufferObject> fbo_;
};

}  // namespace compgui

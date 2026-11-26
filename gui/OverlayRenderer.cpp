#include "OverlayRenderer.h"

#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QSurfaceFormat>

#include <GL/gl.h>

namespace pukegui {

OverlayRenderer::OverlayRenderer() = default;

OverlayRenderer::~OverlayRenderer() {
    if (context_ && surface_ && context_->makeCurrent(surface_.get())) {
        fbo_.reset();  // with its context current
        context_->doneCurrent();
    }
}

bool OverlayRenderer::available() {
    if (tried_) return context_ != nullptr;
    tried_ = true;
    QSurfaceFormat format;
    format.setProfile(QSurfaceFormat::CompatibilityProfile);  // plugins draw with OpenGL 1.x calls
    format.setVersion(2, 1);
    auto context = std::make_unique<QOpenGLContext>();
    context->setFormat(format);
    if (!context->create()) return false;
    auto surface = std::make_unique<QOffscreenSurface>();
    surface->setFormat(context->format());
    surface->create();
    if (!surface->isValid() || !context->makeCurrent(surface.get())) return false;
    context->doneCurrent();
    context_ = std::move(context);
    surface_ = std::move(surface);
    return true;
}

QImage OverlayRenderer::render(puke::ofx::Overlay& overlay, const puke::ofx::Overlay::View& view, QSize size,
                               const std::array<double, 4>& rect) {
    if (size.isEmpty() || !available() || !context_->makeCurrent(surface_.get())) return {};
    if (!fbo_ || fbo_->size() != size) fbo_ = std::make_unique<QOpenGLFramebufferObject>(size);
    fbo_->bind();
    glViewport(0, 0, size.width(), size.height());
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(rect[0], rect[2], rect[1], rect[3], -1, 1);  // canonical coordinates, y up
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    overlay.draw(view);
    glFinish();
    fbo_->release();
    QImage image = fbo_->toImage();  // the right way up
    context_->doneCurrent();
    return image;
}

}  // namespace pukegui

#pragma once

#include <array>
#include <functional>
#include <memory>
#include <string>

#include "puke/graph/Node.h"

namespace puke::ofx {

// An OFX plugin's overlay: what it draws in the viewer over its node, and
// the mouse and key handling that goes with it (OFX 1.4 overlay interacts,
// drawn with OpenGL). One per node being shown.
//
// Coordinates given to it are puke's: full-size image pixels, y down. It
// converts to OFX canonical coordinates (y up) using View::flipY.
//
// Call from one thread (the GUI's). Calls are serialised with the node's
// other non-render actions.
class Overlay {
public:
    // nullptr if `node` isn't an OFX node or its plugin has no overlay.
    static std::unique_ptr<Overlay> create(Node& node);
    ~Overlay();
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    struct View {
        double time = 1;
        double renderScale = 1;     // the viewer's proxy scale
        double pixelScale = 1;      // full-size image pixels per screen pixel
        int flipY = 0;              // the viewed image's full-size display window y0 + y1
        int viewportWidth = 0, viewportHeight = 0;
        std::array<double, 3> background{0, 0, 0};
        Format format;              // the project's
    };

    // Draws. Needs a current OpenGL (compatibility profile) context, with
    // the projection mapping OFX canonical coordinates to the viewport:
    // see canonicalRect().
    void draw(const View& view);
    // The canonical rectangle (left, bottom, right, top) the viewport shows,
    // given where its corners are in puke's image coordinates. For glOrtho.
    static std::array<double, 4> canonicalRect(const View& view, double imageLeft, double imageTop,
                                               double imageRight, double imageBottom);

    // Values the plugin sets during an event go here (a GUI makes them
    // undoable edits); without one, they're set on the node directly.
    using Edit = std::function<void(const std::string& param, ParamValue value)>;

    // Pen events at (x, y) in puke's image coordinates. Each returns whether
    // the plugin used it (then the viewer shouldn't).
    bool penDown(const View& view, double x, double y, const Edit& edit = {});
    bool penMotion(const View& view, double x, double y, const Edit& edit = {});
    bool penUp(const View& view, double x, double y, const Edit& edit = {});

    enum class Key { Character, Return, Escape, Delete, Backspace, Tab, Left, Right, Up, Down };
    // `text` is the character typed, for Key::Character.
    bool keyDown(const View& view, Key key, const std::string& text, const Edit& edit = {});
    bool keyUp(const View& view, Key key, const std::string& text, const Edit& edit = {});

    // Whether the plugin asked to be redrawn since the last call.
    bool takeRedrawRequest();

    Node& node() const { return node_; }

    struct Impl;

private:
    Overlay(Node& node, std::unique_ptr<Impl> impl);
    Node& node_;
    std::unique_ptr<Impl> impl_;
};

}  // namespace puke::ofx

#pragma once

#include <QObject>
#include <QPointF>
#include <QString>

#include <functional>
#include <optional>

#include "puke/core/Path.h"

class QPainter;

namespace puke {
class Node;
}

namespace pukegui {

class Document;

// Drawing and editing the shape of the selected Roto node in the viewer.
// The canvas forwards its mouse and key events (in image pixel coordinates)
// and asks it to paint over the picture.
//
//   Draw mode (the viewer's Draw button): click to add points to a new
//   shape, click-drag to add a smooth point (dragging pulls its handles);
//   click the first point, or press Enter or Escape, to finish.
//   Otherwise: drag a point to move it, drag the selected point's handles to
//   bend its curve, double-click a point to switch it between smooth and
//   corner, Delete or Backspace to remove the selected point.
//
// Every gesture is one undo step. An animated shape is edited as it is at
// the current frame, and an edit sets a key there.
class RotoTool : public QObject {
    Q_OBJECT
public:
    explicit RotoTool(Document& doc, QObject* parent = nullptr);

    // The selected node, if it's a Roto: what this edits.
    puke::Node* node() const;
    bool drawing() const { return drawing_; }
    void setDrawing(bool on);

    // Selected point: (curve, knot).
    std::optional<std::pair<int, int>> selection() const { return selected_; }
    // Whether the canvas should take `key` from the window's shortcuts.
    bool wantsKey(int key) const;

    // Events, with `tolerance` the hit distance in image pixels. Each
    // returns whether it used the event.
    bool press(QPointF p, double tolerance);
    bool move(QPointF p);
    bool release(QPointF p);
    bool doubleClick(QPointF p, double tolerance);
    bool key(int key);

    // Draws the shape and handles; `toWidget` maps image to widget points,
    // `zoom` is widget pixels per image pixel.
    void paint(QPainter& p, const std::function<QPointF(QPointF)>& toWidget, double zoom) const;

signals:
    void changed();  // repaint
    void drawingChanged(bool drawing);

private:
    enum class Drag { None, Knot, In, Out, NewPoint };

    puke::Path shape() const;  // the node's, or empty if it doesn't parse
    void setShape(const puke::Path& path);
    std::optional<std::pair<int, int>> knotAt(const puke::Path& path, QPointF p, double tolerance) const;
    void finishCurve();

    Document& doc_;
    bool drawing_ = false;
    int drawingCurve_ = -1;  // the curve being drawn, in draw mode
    std::optional<std::pair<int, int>> selected_;
    Drag drag_ = Drag::None;
    QPointF dragFrom_;
    // A drag's undo step opens when it first changes something, so a click
    // that only selects records nothing.
    QString dragName_;
    bool editOpen_ = false;
    void openEdit();
};

}  // namespace pukegui

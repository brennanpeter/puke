#pragma once

#include <QPointF>
#include <QWidget>

#include <optional>
#include <string>
#include <vector>

#include "puke/graph/Curve.h"

class QListWidget;

namespace puke {
class Node;
}

namespace pukegui {

class Document;

// One drawn line: a component of an animated parameter of the selected node
// ("multiply", "gain.r", "translate.y").
struct CurveRef {
    std::string param;
    int component = 0;
    bool operator==(const CurveRef&) const = default;
};

// The graph of the curves: frames across, values up.
//
//   Click a key to select it (Shift adds), drag to move the selected keys
//   (frames snap to whole numbers). Drag a selected key's tangent handle to
//   set its slope. Double-click a curve to add a key there. Delete removes
//   the selected keys. Right-click: interpolation, flat or automatic
//   tangents, delete, frame all. F frames everything. Wheel zooms (Ctrl:
//   frames only, Shift: values only); middle-drag or Alt-drag pans.
//
// Each gesture is one undo step.
class CurveView : public QWidget {
    Q_OBJECT
public:
    CurveView(Document& doc, QWidget* parent = nullptr);

    void setCurves(std::vector<CurveRef> curves);  // which to draw
    const std::vector<CurveRef>& curves() const { return curves_; }
    void frameAll();

    struct Selected {
        CurveRef curve;
        double frame;
        bool operator==(const Selected&) const = default;
    };
    const std::vector<Selected>& selection() const { return selected_; }

    // Data <-> widget coordinates.
    QPointF toWidget(double frame, double value) const;
    QPointF toData(QPointF widget) const;  // x: frame, y: value

    // The selected keys' common edits (also on the right-click menu).
    void setInterp(puke::Curve::Interp interp);
    void setTangents(bool flat);  // flat, or back to automatic
    void deleteSelected();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    bool event(QEvent*) override;

private:
    enum class Drag { None, Keys, Tangent, Pan };

    puke::Node* node() const;
    const puke::Curve* curveOf(const CurveRef& c) const;
    std::optional<double> componentAt(const CurveRef& c, double frame) const;
    std::optional<Selected> keyAt(QPointF w) const;
    // The tangent handle of a selected key under `w`: which key, and which side.
    std::optional<std::pair<Selected, int>> handleAt(QPointF w) const;
    QPointF handlePos(const Selected& s, int side) const;
    void editCurves(const std::vector<std::pair<std::string, puke::Curve>>& curves, const QString& text);
    void dragKeysTo(QPointF w);

    Document& doc_;
    std::vector<CurveRef> curves_;
    std::vector<Selected> selected_;
    double frame0_ = 0, frameScale_ = 10;  // widget x = (frame - frame0_) * frameScale_
    double value0_ = 0, valueScale_ = 50;  // widget y = height - (value - value0_) * valueScale_

    Drag drag_ = Drag::None;
    QPoint pressAt_;
    QPointF lastMouse_;
    // Keys being dragged: the curves as they were at the press, so each move
    // applies the whole offset to them (no drift).
    std::vector<std::pair<std::string, puke::Curve>> dragStart_;
    std::optional<std::pair<Selected, int>> dragHandle_;
    double dragOffset_ = 0;  // frames the dragged keys have moved so far
    bool editOpen_ = false;
};

// The Curve Editor panel: a list of the selected node's animated parameters
// (one row per component; uncheck to hide) beside a CurveView.
class CurveEditor : public QWidget {
    Q_OBJECT
public:
    CurveEditor(Document& doc, QWidget* parent = nullptr);
    CurveView* view() const { return view_; }
    QListWidget* list() const { return list_; }

private:
    void rebuild();  // the selected node or its animated parameters changed

    Document& doc_;
    QListWidget* list_;
    CurveView* view_;
    puke::Node* node_ = nullptr;
    std::vector<std::string> params_;  // what the list shows, to notice changes
};

}  // namespace pukegui

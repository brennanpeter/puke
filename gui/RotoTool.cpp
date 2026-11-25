#include "RotoTool.h"

#include <QPainter>
#include <QPainterPath>

#include <cmath>

#include "Document.h"

namespace compgui {

namespace {

using Point = comp::Path::Point;

Point pt(QPointF p) { return {p.x(), p.y()}; }
QPointF qpt(const Point& p) { return {p[0], p[1]}; }
double dist(QPointF a, QPointF b) { return std::hypot(a.x() - b.x(), a.y() - b.y()); }

}  // namespace

RotoTool::RotoTool(Document& doc, QObject* parent) : QObject(parent), doc_(doc) {
    connect(&doc_, &Document::selectionChanged, this, [this] {
        selected_.reset();
        drag_ = Drag::None;
        if (!node()) setDrawing(false);
        emit changed();
    });
    auto recheck = [this](comp::Node* n) {
        if (n != node()) return;
        // An undo may have taken away the selected point.
        const comp::Path path = shape();
        if (selected_ && (selected_->first >= int(path.curves().size()) ||
                          selected_->second >= int(path.curves()[selected_->first].size())))
            selected_.reset();
        if (drawingCurve_ >= int(path.curves().size())) drawingCurve_ = -1;
        emit changed();
    };
    connect(&doc_, &Document::paramChanged, this, recheck);
    // An animated shape changes with the frame.
    connect(&doc_, &Document::frameChanged, this, [this, recheck] { recheck(node()); });
}

comp::Node* RotoTool::node() const {
    comp::Node* n = doc_.selected();
    return n && std::string(n->typeName()) == "Roto" ? n : nullptr;
}

void RotoTool::setDrawing(bool on) {
    if (on && !node()) on = false;
    if (on == drawing_) return;
    drawing_ = on;
    drawingCurve_ = -1;
    emit drawingChanged(on);
    emit changed();
}

void RotoTool::finishCurve() {
    drawingCurve_ = -1;
    emit changed();
}

comp::Path RotoTool::shape() const {
    comp::Node* n = node();
    if (!n) return {};
    try {
        const auto v = n->valueAt("shape", doc_.frame());  // animated shapes: as at this frame
        const auto* s = v ? std::get_if<std::string>(&*v) : nullptr;
        return comp::Path::parse(s ? *s : "");
    } catch (const comp::Error&) {
        return {};  // typed in wrong: the render says so; nothing to edit here
    }
}

void RotoTool::setShape(const comp::Path& path) {
    if (comp::Node* n = node()) doc_.setParam(*n, "shape", path.str());
}

bool RotoTool::wantsKey(int key) const {
    if (!node()) return false;
    if (key == Qt::Key_Delete || key == Qt::Key_Backspace) return selected_.has_value();
    if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Escape) return drawingCurve_ >= 0;
    return false;
}

std::optional<std::pair<int, int>> RotoTool::knotAt(const comp::Path& path, QPointF p, double tolerance) const {
    std::optional<std::pair<int, int>> best;
    double bestD = tolerance;
    for (int c = 0; c < int(path.curves().size()); ++c)
        for (int k = 0; k < int(path.curves()[c].size()); ++k) {
            const double d = dist(qpt(path.curves()[c][k].point), p);
            if (d <= bestD) best = {c, k}, bestD = d;
        }
    return best;
}

bool RotoTool::press(QPointF p, double tolerance) {
    if (!node()) return false;
    comp::Path path = shape();

    // The selected point's handles come first (they can sit on other points).
    if (selected_ && !drawing_) {
        const auto& k = path.curves()[selected_->first][selected_->second];
        const bool inUsed = k.in != k.point, outUsed = k.out != k.point;
        if (outUsed && dist(qpt(k.out), p) <= tolerance) drag_ = Drag::Out;
        else if (inUsed && dist(qpt(k.in), p) <= tolerance) drag_ = Drag::In;
        if (drag_ != Drag::None) {
            dragName_ = "Move handle";
            dragFrom_ = p;
            return true;
        }
    }

    if (const auto hit = knotAt(path, p, tolerance)) {
        // Clicking the first point of the shape being drawn finishes it.
        if (drawing_ && hit->first == drawingCurve_ && hit->second == 0 &&
            path.curves()[drawingCurve_].size() >= 3) {
            finishCurve();
            return true;
        }
        selected_ = hit;
        drag_ = Drag::Knot;
        dragFrom_ = p;
        dragName_ = "Move point";
        emit changed();
        return true;
    }

    if (drawing_) {
        if (drawingCurve_ < 0) {
            path.curves().emplace_back();
            drawingCurve_ = int(path.curves().size()) - 1;
        }
        auto& curve = path.curves()[drawingCurve_];
        curve.push_back({pt(p), pt(p), pt(p)});
        selected_ = std::pair{drawingCurve_, int(curve.size()) - 1};
        drag_ = Drag::NewPoint;
        dragFrom_ = p;
        dragName_ = "Add point";
        openEdit();
        setShape(path);
        emit changed();
        return true;
    }

    if (selected_) {  // clicked on nothing: deselect, and let the canvas carry on
        selected_.reset();
        emit changed();
    }
    return false;
}

bool RotoTool::move(QPointF p) {
    if (drag_ == Drag::None || !selected_) return false;
    comp::Path path = shape();
    auto& curve = path.curves()[selected_->first];
    auto& k = curve[selected_->second];
    switch (drag_) {
        case Drag::Knot: {  // the point and its handles together
            const QPointF d = p - dragFrom_;
            for (Point* q : {&k.point, &k.in, &k.out}) (*q)[0] += d.x(), (*q)[1] += d.y();
            dragFrom_ = p;
            break;
        }
        case Drag::In:
        case Drag::Out: {
            // Handles of a smooth point stay in line: the other one turns too,
            // keeping its length.
            Point& moved = drag_ == Drag::In ? k.in : k.out;
            Point& other = drag_ == Drag::In ? k.out : k.in;
            moved = pt(p);
            if (other != k.point) {
                const double len = std::hypot(other[0] - k.point[0], other[1] - k.point[1]);
                const double dx = k.point[0] - p.x(), dy = k.point[1] - p.y();
                const double l = std::hypot(dx, dy);
                if (l > 0) other = {k.point[0] + dx / l * len, k.point[1] + dy / l * len};
            }
            break;
        }
        case Drag::NewPoint:  // pulling the new point's handles out, mirrored
            k.out = pt(p);
            k.in = {2 * k.point[0] - p.x(), 2 * k.point[1] - p.y()};
            break;
        case Drag::None:
            break;
    }
    openEdit();
    setShape(path);
    return true;
}

void RotoTool::openEdit() {
    if (editOpen_) return;
    doc_.beginEdit(dragName_);
    editOpen_ = true;
}

bool RotoTool::release(QPointF) {
    if (drag_ == Drag::None) return false;
    drag_ = Drag::None;
    if (editOpen_) {
        doc_.endEdit();
        editOpen_ = false;
    }
    return true;
}

bool RotoTool::doubleClick(QPointF p, double tolerance) {
    if (!node() || drawing_) return false;
    comp::Path path = shape();
    const auto hit = knotAt(path, p, tolerance);
    if (!hit) return false;
    auto& curve = path.curves()[hit->first];
    auto& k = curve[hit->second];
    const int n = int(curve.size());
    if (k.in != k.point || k.out != k.point) {
        k.in = k.out = k.point;  // smooth -> corner
    } else if (n >= 2) {
        // Corner -> smooth: handles along the line from the previous point to
        // the next, a third of the way to each.
        const Point& prev = curve[(hit->second + n - 1) % n].point;
        const Point& next = curve[(hit->second + 1) % n].point;
        const double dx = next[0] - prev[0], dy = next[1] - prev[1], l = std::hypot(dx, dy);
        if (l > 0) {
            const double inLen = std::hypot(k.point[0] - prev[0], k.point[1] - prev[1]) / 3;
            const double outLen = std::hypot(next[0] - k.point[0], next[1] - k.point[1]) / 3;
            k.in = {k.point[0] - dx / l * inLen, k.point[1] - dy / l * inLen};
            k.out = {k.point[0] + dx / l * outLen, k.point[1] + dy / l * outLen};
        }
    }
    selected_ = hit;
    doc_.beginEdit("Change point");
    setShape(path);
    doc_.endEdit();
    return true;
}

bool RotoTool::key(int key) {
    if (!wantsKey(key)) return false;
    if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Escape) {
        finishCurve();
        return true;
    }
    comp::Path path = shape();
    auto& curves = path.curves();
    curves[selected_->first].erase(curves[selected_->first].begin() + selected_->second);
    if (curves[selected_->first].empty()) {
        curves.erase(curves.begin() + selected_->first);
        if (drawingCurve_ == selected_->first) drawingCurve_ = -1;
        else if (drawingCurve_ > selected_->first) --drawingCurve_;
    }
    selected_.reset();
    doc_.beginEdit("Delete point");
    setShape(path);
    doc_.endEdit();
    return true;
}

void RotoTool::paint(QPainter& p, const std::function<QPointF(QPointF)>& toWidget, double zoom) const {
    if (!node()) return;
    const comp::Path path = shape();
    p.setRenderHint(QPainter::Antialiasing);
    const QColor line(255, 190, 60), knot(255, 230, 150), handle(120, 200, 255);

    // The outline, about 2 screen pixels per piece.
    for (const auto& poly : path.polygons(std::max(0.05, 2 / zoom))) {
        QPainterPath outline;
        outline.moveTo(toWidget(qpt(poly[0])));
        for (std::size_t i = 1; i < poly.size(); ++i) outline.lineTo(toWidget(qpt(poly[i])));
        outline.closeSubpath();
        p.setPen(QPen(line, 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawPath(outline);
    }

    for (int c = 0; c < int(path.curves().size()); ++c)
        for (int k = 0; k < int(path.curves()[c].size()); ++k) {
            const auto& kn = path.curves()[c][k];
            const QPointF at = toWidget(qpt(kn.point));
            const bool sel = selected_ && selected_->first == c && selected_->second == k;
            if (sel) {
                for (const Point& h : {kn.in, kn.out}) {
                    if (h == kn.point) continue;
                    const QPointF hw = toWidget(qpt(h));
                    p.setPen(QPen(handle, 1));
                    p.drawLine(at, hw);
                    p.setBrush(handle);
                    p.drawEllipse(hw, 3.5, 3.5);
                }
            }
            p.setPen(QPen(Qt::black, 1));
            p.setBrush(sel ? QColor(Qt::white) : knot);
            const double r = sel ? 4.5 : 3.5;
            p.drawRect(QRectF(at.x() - r, at.y() - r, 2 * r, 2 * r));
        }
}

}  // namespace compgui

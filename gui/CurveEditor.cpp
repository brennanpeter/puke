#include "CurveEditor.h"

#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QSplitter>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "Document.h"

namespace compgui {

namespace {

constexpr double kHit = 6;       // pixels
constexpr double kHandle = 40;   // tangent handle length, pixels
constexpr double kMargin = 30;   // around framed curves, pixels

// The names of a value's components ("" for a plain number).
std::vector<std::string> componentNames(const comp::ParamValue& v) {
    if (std::holds_alternative<comp::Vec2>(v)) return {"x", "y"};
    if (std::holds_alternative<comp::Color>(v)) return {"r", "g", "b", "a"};
    if (std::holds_alternative<double>(v) || std::holds_alternative<int>(v)) return {""};
    return {};  // strings, shapes, bools: nothing to draw
}

void setComponent(comp::ParamValue& v, int c, double x) {
    if (auto* d = std::get_if<double>(&v)) *d = x;
    else if (auto* i = std::get_if<int>(&v)) *i = static_cast<int>(std::lround(x));
    else if (auto* a = std::get_if<comp::Vec2>(&v)) (*a)[c] = x;
    else if (auto* col = std::get_if<comp::Color>(&v)) (*col)[c] = x;
}

// A curve's colour: by component for Vec2 and Color, by name otherwise, so
// it's the same in the list and the graph, whatever else is shown.
QColor colorFor(const comp::ParamValue& v, const CurveRef& c) {
    const int component = c.component;
    const std::size_t index = std::hash<std::string>{}(c.param);
    if (std::holds_alternative<comp::Color>(v)) {
        static const QColor rgba[4] = {QColor(235, 90, 90), QColor(110, 200, 100), QColor(100, 150, 240), QColor(190, 190, 190)};
        return rgba[component];
    }
    if (std::holds_alternative<comp::Vec2>(v)) return component == 0 ? QColor(235, 110, 90) : QColor(120, 200, 110);
    static const QColor palette[] = {QColor(240, 170, 60), QColor(90, 200, 220), QColor(220, 120, 220), QColor(200, 210, 90)};
    return palette[index % 4];
}

// A round step for grid lines about `pixels` apart at `scale` pixels per unit.
double niceStep(double scale, double pixels) {
    const double raw = pixels / std::max(scale, 1e-12);
    const double p = std::pow(10.0, std::floor(std::log10(raw)));
    for (double m : {1.0, 2.0, 5.0, 10.0})
        if (m * p >= raw) return m * p;
    return 10 * p;
}

}  // namespace

// ================================================================ view

CurveView::CurveView(Document& doc, QWidget* parent) : QWidget(parent), doc_(doc) {
    setObjectName("curveView");
    setFocusPolicy(Qt::ClickFocus);
    setMouseTracking(false);
    setMinimumSize(200, 120);
    connect(&doc_, &Document::paramChanged, this, [this] {
        // Undo can take keys away: forget them.
        std::erase_if(selected_, [this](const Selected& s) {
            const comp::Curve* c = curveOf(s.curve);
            return !c || !c->keyAt(s.frame);
        });
        update();
    });
    connect(&doc_, &Document::frameChanged, this, qOverload<>(&QWidget::update));
}

comp::Node* CurveView::node() const { return doc_.selected(); }

const comp::Curve* CurveView::curveOf(const CurveRef& c) const {
    comp::Node* n = node();
    return n ? n->curve(c.param) : nullptr;
}

std::optional<double> CurveView::componentAt(const CurveRef& c, double frame) const {
    const comp::Curve* curve = curveOf(c);
    if (!curve || curve->empty()) return std::nullopt;
    const auto n = comp::Curve::components(curve->valueAt(frame));
    if (!n || c.component >= int(n->size())) return std::nullopt;
    return (*n)[c.component];
}

void CurveView::setCurves(std::vector<CurveRef> curves) {
    curves_ = std::move(curves);
    std::erase_if(selected_, [this](const Selected& s) {
        return std::find(curves_.begin(), curves_.end(), s.curve) == curves_.end();
    });
    update();
}

QPointF CurveView::toWidget(double frame, double value) const {
    return {(frame - frame0_) * frameScale_, height() - (value - value0_) * valueScale_};
}

QPointF CurveView::toData(QPointF w) const {
    return {w.x() / frameScale_ + frame0_, (height() - w.y()) / valueScale_ + value0_};
}

void CurveView::frameAll() {
    double f0 = doc_.settings().firstFrame, f1 = doc_.settings().lastFrame;
    double v0 = std::numeric_limits<double>::infinity(), v1 = -v0;
    for (const CurveRef& c : curves_) {
        const comp::Curve* curve = curveOf(c);
        if (!curve || curve->empty()) continue;
        const double a = curve->keys().front().frame, b = curve->keys().back().frame;
        f0 = std::min(f0, a), f1 = std::max(f1, b);
        // Sample between the keys too: smooth curves overshoot them.
        for (int k = 0; k <= 200; ++k)
            if (const auto v = componentAt(c, a + (b - a) * k / 200.0)) v0 = std::min(v0, *v), v1 = std::max(v1, *v);
    }
    if (!std::isfinite(v0)) v0 = 0, v1 = 1;
    if (v1 - v0 < 1e-9) v0 -= 0.5, v1 += 0.5;
    if (f1 - f0 < 1) f1 = f0 + 1;
    const double w = std::max(width(), 100) - 2 * kMargin, h = std::max(height(), 80) - 2 * kMargin;
    frameScale_ = w / (f1 - f0);
    valueScale_ = h / (v1 - v0);
    frame0_ = f0 - kMargin / frameScale_;
    value0_ = v0 - kMargin / valueScale_ - (std::max(height(), 80) - height()) / valueScale_;
    update();
}

// --- picking

std::optional<CurveView::Selected> CurveView::keyAt(QPointF w) const {
    std::optional<Selected> best;
    double bestD = kHit;
    for (const CurveRef& c : curves_) {
        const comp::Curve* curve = curveOf(c);
        if (!curve) continue;
        for (const auto& k : curve->keys()) {
            const auto n = comp::Curve::components(k.value);
            if (!n || c.component >= int(n->size())) continue;
            const QPointF p = toWidget(k.frame, (*n)[c.component]);
            const double d = std::hypot(p.x() - w.x(), p.y() - w.y());
            if (d <= bestD) best = Selected{c, k.frame}, bestD = d;
        }
    }
    return best;
}

QPointF CurveView::handlePos(const Selected& s, int side) const {
    const comp::Curve* curve = curveOf(s.curve);
    const auto& keys = curve->keys();
    const std::size_t i = std::find_if(keys.begin(), keys.end(), [&](const auto& k) { return k.frame == s.frame; }) - keys.begin();
    const auto slope = curve->slopeAt(i);
    const double m = s.curve.component < int(slope.size()) ? slope[s.curve.component] : 0;
    const auto n = comp::Curve::components(keys[i].value);
    const QPointF at = toWidget(s.frame, (*n)[s.curve.component]);
    const double dx = frameScale_, dy = -m * valueScale_, len = std::hypot(dx, dy);
    return at + QPointF(dx, dy) * (side * kHandle / len);
}

std::optional<std::pair<CurveView::Selected, int>> CurveView::handleAt(QPointF w) const {
    for (const Selected& s : selected_) {
        const comp::Curve* curve = curveOf(s.curve);
        const comp::Curve::Key* k = curve ? curve->keyAt(s.frame) : nullptr;
        if (!k || !comp::Curve::components(k->value)) continue;
        for (int side : {-1, 1}) {
            const QPointF h = handlePos(s, side);
            if (std::hypot(h.x() - w.x(), h.y() - w.y()) <= kHit) return std::pair{s, side};
        }
    }
    return std::nullopt;
}

// --- edits

void CurveView::editCurves(const std::vector<std::pair<std::string, comp::Curve>>& curves, const QString& text) {
    comp::Node* n = node();
    if (!n) return;
    if (!editOpen_) {
        doc_.beginEdit(text);
        editOpen_ = true;
    }
    for (const auto& [param, curve] : curves) doc_.editCurve(*n, param, curve, text);
}

void CurveView::dragKeysTo(QPointF w) {
    const double dframe = std::round((w.x() - pressAt_.x()) / frameScale_);
    const double dvalue = -(w.y() - pressAt_.y()) / valueScale_;
    // The selection as it was at the press: dragStart_ order matches.
    std::vector<std::pair<std::string, comp::Curve>> out;
    std::vector<Selected> moved;
    for (const auto& [param, start] : dragStart_) {
        comp::Curve curve = start;
        // Which keys of this parameter move, and which of their components.
        std::map<double, std::set<int>> moving;
        for (const Selected& s : selected_)
            if (s.curve.param == param) moving[s.frame - dragOffset_].insert(s.curve.component);
        // Take them all out first, so keys moving past each other don't collide.
        std::vector<comp::Curve::Key> keys;
        for (const auto& [f, comps] : moving)
            if (const comp::Curve::Key* k = start.keyAt(f)) {
                comp::Curve::Key copy = *k;
                const auto n = comp::Curve::components(copy.value);
                for (int c : comps)
                    if (n && c < int(n->size())) setComponent(copy.value, c, (*n)[c] + dvalue);
                copy.frame = f + dframe;
                keys.push_back(std::move(copy));
                curve.removeKey(f);
            }
        for (const auto& k : keys) {
            curve.setKey(k.frame, k.value, k.interp);
            if (!k.slope.empty()) curve.setSlope(k.frame, k.slope);
        }
        out.emplace_back(param, std::move(curve));
    }
    for (Selected& s : selected_) s.frame = s.frame - dragOffset_ + dframe;
    dragOffset_ = dframe;
    editCurves(out, "Move keys");
}

void CurveView::setInterp(comp::Curve::Interp interp) {
    std::map<std::string, comp::Curve> curves;
    for (const Selected& s : selected_) {
        if (!curves.count(s.curve.param)) curves[s.curve.param] = *curveOf(s.curve);
        curves[s.curve.param].setInterp(s.frame, interp);
    }
    editCurves({curves.begin(), curves.end()}, "Interpolation");
    if (editOpen_) doc_.endEdit(), editOpen_ = false;
}

void CurveView::setTangents(bool flat) {
    std::map<std::string, comp::Curve> curves;
    for (const Selected& s : selected_) {
        if (!curves.count(s.curve.param)) curves[s.curve.param] = *curveOf(s.curve);
        comp::Curve& c = curves[s.curve.param];
        const comp::Curve::Key* k = c.keyAt(s.frame);
        const auto n = k ? comp::Curve::components(k->value) : std::nullopt;
        if (!n) continue;
        c.setSlope(s.frame, flat ? std::vector<double>(n->size(), 0.0) : std::vector<double>{});
    }
    editCurves({curves.begin(), curves.end()}, flat ? "Flat tangents" : "Automatic tangents");
    if (editOpen_) doc_.endEdit(), editOpen_ = false;
}

void CurveView::deleteSelected() {
    if (selected_.empty()) return;
    std::map<std::string, comp::Curve> curves;
    for (const Selected& s : selected_) {
        if (!curves.count(s.curve.param)) curves[s.curve.param] = *curveOf(s.curve);
        curves[s.curve.param].removeKey(s.frame);
    }
    selected_.clear();
    editCurves({curves.begin(), curves.end()}, "Delete keys");
    if (editOpen_) doc_.endEdit(), editOpen_ = false;
}

// --- events

bool CurveView::event(QEvent* e) {
    // Delete removes keys here, not the selected node (the window's shortcut).
    if (e->type() == QEvent::ShortcutOverride && !selected_.empty()) {
        const int key = static_cast<QKeyEvent*>(e)->key();
        if (key == Qt::Key_Delete || key == Qt::Key_Backspace) {
            e->accept();
            return true;
        }
    }
    return QWidget::event(e);
}

void CurveView::mousePressEvent(QMouseEvent* e) {
    setFocus();
    lastMouse_ = e->position();
    pressAt_ = e->pos();
    if (e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && (e->modifiers() & Qt::AltModifier))) {
        drag_ = Drag::Pan;
        return;
    }
    if (e->button() != Qt::LeftButton) return;
    if (const auto h = handleAt(e->position())) {
        drag_ = Drag::Tangent;
        dragHandle_ = h;
        return;
    }
    const bool add = e->modifiers() & Qt::ShiftModifier;
    if (const auto k = keyAt(e->position())) {
        auto it = std::find(selected_.begin(), selected_.end(), *k);
        if (add && it != selected_.end()) {
            selected_.erase(it);  // shift-click a selected key: deselect
            update();
            return;
        }
        if (it == selected_.end()) {
            if (!add) selected_.clear();
            selected_.push_back(*k);
        }
        drag_ = Drag::Keys;
        dragOffset_ = 0;
        dragStart_.clear();
        for (const Selected& s : selected_)
            if (std::none_of(dragStart_.begin(), dragStart_.end(), [&](const auto& p) { return p.first == s.curve.param; }))
                dragStart_.emplace_back(s.curve.param, *curveOf(s.curve));
    } else if (!add) {
        selected_.clear();
    }
    update();
}

void CurveView::mouseMoveEvent(QMouseEvent* e) {
    const QPointF d = e->position() - lastMouse_;
    lastMouse_ = e->position();
    switch (drag_) {
        case Drag::Pan:
            frame0_ -= d.x() / frameScale_;
            value0_ += d.y() / valueScale_;
            update();
            break;
        case Drag::Keys:
            // A few pixels before anything moves, so a click doesn't nudge.
            if (editOpen_ || (e->pos() - pressAt_).manhattanLength() > 3) dragKeysTo(e->position());
            break;
        case Drag::Tangent: {
            const auto& [s, side] = *dragHandle_;
            comp::Curve c = *curveOf(s.curve);
            const auto& keys = c.keys();
            const std::size_t i = std::find_if(keys.begin(), keys.end(), [&](const auto& k) { return k.frame == s.frame; }) - keys.begin();
            const auto n = comp::Curve::components(keys[i].value);
            const QPointF at = toWidget(s.frame, (*n)[s.curve.component]);
            QPointF v = e->position() - at;
            if (std::abs(v.x()) < 1) v.setX(side);  // never vertical
            std::vector<double> slope = c.slopeAt(i);  // keep the other components' tangents
            slope[s.curve.component] = (-v.y() / valueScale_) / (v.x() / frameScale_);
            c.setSlope(s.frame, slope);
            editCurves({{s.curve.param, c}}, "Set tangent");
            break;
        }
        case Drag::None:
            break;
    }
}

void CurveView::mouseReleaseEvent(QMouseEvent*) {
    drag_ = Drag::None;
    dragHandle_.reset();
    dragStart_.clear();
    if (editOpen_) {
        doc_.endEdit();
        editOpen_ = false;
    }
}

void CurveView::mouseDoubleClickEvent(QMouseEvent* e) {
    // A key on the nearest curve under the mouse, at a whole frame.
    const double frame = std::round(toData(e->position()).x());
    std::optional<CurveRef> best;
    double bestD = 8;
    for (const CurveRef& c : curves_)
        if (const auto v = componentAt(c, frame)) {
            const double d = std::abs(toWidget(frame, *v).y() - e->position().y());
            if (d <= bestD) best = c, bestD = d;
        }
    if (!best) return;
    comp::Curve c = *curveOf(*best);
    c.setKey(frame, c.valueAt(frame));
    editCurves({{best->param, c}}, "Add key");
    if (editOpen_) doc_.endEdit(), editOpen_ = false;
    selected_ = {Selected{*best, frame}};
    update();
}

void CurveView::wheelEvent(QWheelEvent* e) {
    const double f = e->angleDelta().y() > 0 ? 1.2 : 1 / 1.2;
    const QPointF at = e->position(), before = toData(at);
    if (!(e->modifiers() & Qt::ShiftModifier)) frameScale_ *= f;
    if (!(e->modifiers() & Qt::ControlModifier)) valueScale_ *= f;
    // Keep the point under the mouse where it is.
    frame0_ = before.x() - at.x() / frameScale_;
    value0_ = before.y() - (height() - at.y()) / valueScale_;
    update();
}

void CurveView::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) deleteSelected();
    else if (e->key() == Qt::Key_F) frameAll();
    else QWidget::keyPressEvent(e);
}

void CurveView::contextMenuEvent(QContextMenuEvent* e) {
    QMenu menu;
    const bool any = !selected_.empty();
    using I = comp::Curve::Interp;
    for (auto [interp, name] : {std::pair{I::Smooth, "Smooth"}, {I::Linear, "Linear"}, {I::Constant, "Constant"}})
        menu.addAction(name, [this, interp] { setInterp(interp); })->setEnabled(any);
    menu.addSeparator();
    menu.addAction("Flat tangents", [this] { setTangents(true); })->setEnabled(any);
    menu.addAction("Automatic tangents", [this] { setTangents(false); })->setEnabled(any);
    menu.addAction("Delete keys", [this] { deleteSelected(); })->setEnabled(any);
    menu.addSeparator();
    menu.addAction("Frame all (F)", [this] { frameAll(); });
    menu.exec(e->globalPos());
}

// --- painting

void CurveView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(34, 34, 36));
    QFont small = font();
    small.setPointSizeF(7.5);
    p.setFont(small);

    // Grid.
    const QPointF lo = toData(QPointF(0, height())), hi = toData(QPointF(width(), 0));
    const double fs = std::max(1.0, niceStep(frameScale_, 80)), vs = niceStep(valueScale_, 40);
    for (double f = std::ceil(lo.x() / fs) * fs; f <= hi.x(); f += fs) {
        const double x = toWidget(f, 0).x();
        p.setPen(QColor(52, 52, 56));
        p.drawLine(QPointF(x, 0), QPointF(x, height()));
        p.setPen(QColor(120, 120, 120));
        p.drawText(QPointF(x + 3, height() - 4), QString::number(f));
    }
    for (double v = std::ceil(lo.y() / vs) * vs; v <= hi.y(); v += vs) {
        const double y = toWidget(0, v).y();
        p.setPen(std::abs(v) < vs / 2 ? QColor(80, 80, 86) : QColor(52, 52, 56));
        p.drawLine(QPointF(0, y), QPointF(width(), y));
        p.setPen(QColor(120, 120, 120));
        p.drawText(QPointF(3, y - 3), QString::number(std::abs(v) < vs / 1e6 ? 0.0 : v, 'g', 6));
    }
    // The project's range and the current frame.
    p.setPen(QPen(QColor(90, 90, 96), 1, Qt::DashLine));
    for (double f : {double(doc_.settings().firstFrame), double(doc_.settings().lastFrame)})
        p.drawLine(QPointF(toWidget(f, 0).x(), 0), QPointF(toWidget(f, 0).x(), height()));
    p.setPen(QPen(QColor(240, 170, 50), 1.5));
    const double cx = toWidget(doc_.frame(), 0).x();
    p.drawLine(QPointF(cx, 0), QPointF(cx, height()));

    p.setRenderHint(QPainter::Antialiasing);
    for (const CurveRef& c : curves_) {
        const comp::Curve* curve = curveOf(c);
        if (!curve || curve->empty()) continue;
        const QColor color = colorFor(curve->keys()[0].value, c);
        QPainterPath line;
        bool started = false;
        for (int x = 0; x <= width(); x += 2) {
            const auto v = componentAt(c, toData(QPointF(x, 0)).x());
            if (!v) continue;
            const QPointF q(x, toWidget(0, *v).y());
            if (started) line.lineTo(q);
            else line.moveTo(q), started = true;
        }
        p.setPen(QPen(color, 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawPath(line);

        for (const auto& k : curve->keys()) {
            const auto n = comp::Curve::components(k.value);
            if (!n || c.component >= int(n->size())) continue;
            const QPointF at = toWidget(k.frame, (*n)[c.component]);
            const Selected self{c, k.frame};
            const bool sel = std::find(selected_.begin(), selected_.end(), self) != selected_.end();
            if (sel) {  // tangent handles
                p.setPen(QPen(QColor(150, 200, 255), 1));
                for (int side : {-1, 1}) {
                    const QPointF h = handlePos(self, side);
                    p.drawLine(at, h);
                    p.setBrush(QColor(150, 200, 255));
                    p.drawEllipse(h, 3, 3);
                }
            }
            p.setPen(QPen(Qt::black, 1));
            p.setBrush(sel ? QColor(Qt::white) : color);
            const double r = sel ? 5 : 4;
            const QPointF d[4] = {at + QPointF(0, -r), at + QPointF(r, 0), at + QPointF(0, r), at + QPointF(-r, 0)};
            p.drawPolygon(d, 4);
        }
    }
    if (curves_.empty()) {
        p.setPen(QColor(150, 150, 150));
        p.drawText(rect(), Qt::AlignCenter,
                   node() ? "No animated parameters on this node.\nSet keys with the ◇ buttons in Properties."
                          : "Select a node to see its animation.");
    }
}

// ================================================================ panel

CurveEditor::CurveEditor(Document& doc, QWidget* parent) : QWidget(parent), doc_(doc) {
    setObjectName("curveEditor");
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* split = new QSplitter;
    list_ = new QListWidget;
    list_->setObjectName("curveList");
    list_->setMaximumWidth(220);
    view_ = new CurveView(doc_);
    split->addWidget(list_);
    split->addWidget(view_);
    split->setStretchFactor(1, 1);
    layout->addWidget(split);

    connect(list_, &QListWidget::itemChanged, this, [this] {
        std::vector<CurveRef> shown;
        for (int i = 0; i < list_->count(); ++i) {
            QListWidgetItem* it = list_->item(i);
            if (it->checkState() == Qt::Checked)
                shown.push_back({it->data(Qt::UserRole).toString().toStdString(), it->data(Qt::UserRole + 1).toInt()});
        }
        view_->setCurves(shown);
    });
    connect(&doc_, &Document::selectionChanged, this, &CurveEditor::rebuild);
    connect(&doc_, &Document::paramChanged, this, [this](comp::Node* n) {
        if (n == node_) rebuild();
    });
    connect(&doc_, &Document::structureChanged, this, &CurveEditor::rebuild);
    rebuild();
}

void CurveEditor::rebuild() {
    comp::Node* n = doc_.selected();
    std::vector<std::string> params;
    if (n)
        for (const auto& [key, curve] : n->curves())
            if (!curve.empty() && !componentNames(curve.keys()[0].value).empty()) params.push_back(key);
    if (n == node_ && params == params_) return;  // same curves: keep what's shown
    const bool newNode = n != node_;
    node_ = n;
    params_ = params;

    // Keep hidden curves hidden across a rebuild of the same node.
    std::set<std::pair<std::string, int>> hidden;
    if (!newNode)
        for (int i = 0; i < list_->count(); ++i)
            if (list_->item(i)->checkState() != Qt::Checked)
                hidden.insert({list_->item(i)->data(Qt::UserRole).toString().toStdString(),
                               list_->item(i)->data(Qt::UserRole + 1).toInt()});
    QSignalBlocker block(list_);
    list_->clear();
    std::vector<CurveRef> shown;
    for (const std::string& key : params) {
        const auto names = componentNames(n->curve(key)->keys()[0].value);
        for (int c = 0; c < int(names.size()); ++c) {
            auto* item = new QListWidgetItem(QString::fromStdString(names[c].empty() ? key : key + "." + names[c]), list_);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setData(Qt::UserRole, QString::fromStdString(key));
            item->setData(Qt::UserRole + 1, c);
            item->setForeground(colorFor(n->curve(key)->keys()[0].value, CurveRef{key, c}));
            const bool show = !hidden.count({key, c});
            item->setCheckState(show ? Qt::Checked : Qt::Unchecked);
            if (show) shown.push_back({key, c});
        }
    }
    view_->setCurves(shown);
    if (newNode) view_->frameAll();
}

}  // namespace compgui

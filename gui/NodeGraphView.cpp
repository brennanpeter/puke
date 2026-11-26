#include "NodeGraphView.h"

#include <QApplication>
#include <QClipboard>
#include <QInputDialog>
#include <QGraphicsItem>
#include <QGraphicsSceneMouseEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QScrollBar>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <map>

#include "Document.h"
#include "puke/nodes/Registry.h"
#include "puke/project/Project.h"

namespace pukegui {

namespace {

constexpr double kNodeW = 120, kNodeH = 34, kPortR = 5.5;

QColor colorFor(const std::string& type) {
    if (type == "Read") return QColor(70, 110, 70);
    if (type == "Write") return QColor(130, 60, 60);
    if (type == "Merge") return QColor(60, 85, 135);
    if (type == "Grade" || type == "ColorSpace") return QColor(55, 115, 120);
    if (type == "Transform") return QColor(100, 75, 130);
    return QColor(85, 85, 90);  // OFX and anything else
}

// "ofx:net.sf.openfx.GammaPlugin" -> "GammaPlugin (OFX)"; built-ins as they are.
QString displayType(const std::string& type) {
    if (type.rfind("ofx:", 0) == 0) return QString::fromStdString(type.substr(type.find_last_of(".:") + 1)) + " (OFX)";
    return QString::fromStdString(type);
}

}  // namespace

// ================================================================ items

// A node, or a backdrop (a box drawn behind nodes; its position is its
// top-left corner, and moving it moves the nodes on it).
class NodeItem : public QGraphicsItem {
public:
    NodeItem(puke::Node& node, Document& doc)
        : node_(node), doc_(doc), backdrop_(std::string(node.typeName()) == "Backdrop") {
        setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
        setZValue(backdrop_ ? -1 : 1);
        const auto& p = node.position();
        setPos(p ? QPointF((*p)[0], (*p)[1]) : QPointF());
        setToolTip(QString::fromStdString(node.typeName()));
    }

    puke::Node& node() const { return node_; }
    bool isBackdrop() const { return backdrop_; }

    QSizeF backdropSize() const {
        if (resizing_) return *resizing_;
        const puke::Vec2 s = node_.vec2Param("size", puke::Vec2{400, 250});
        return {std::max(40.0, s[0]), std::max(40.0, s[1])};
    }
    // The backdrop's size or label changed.
    void paramsChanged() {
        prepareGeometryChange();
        update();
    }

    QRectF boundingRect() const override {
        if (backdrop_) return QRectF(QPointF(), backdropSize()).adjusted(-2, -2, 2, 2);
        return QRectF(-kNodeW / 2 - 6, -20, kNodeW + 12, kNodeH + 32);
    }

    // Port positions, in item coordinates.
    QPointF inputPos(int i) const {
        const int n = node_.maxInputs();
        return {-kNodeW / 2 + kNodeW * (i + 1) / (n + 1), 0.0};
    }
    QPointF outputPos() const { return {0.0, kNodeH}; }

    // Which port, if any, is at a scene position: {true, 0} for the output,
    // {false, i} for input i.
    std::optional<std::pair<bool, int>> portAt(QPointF scenePos) const {
        if (backdrop_) return std::nullopt;
        const QPointF p = mapFromScene(scenePos);
        auto near = [&](QPointF q) { return QLineF(p, q).length() <= kPortR + 5; };
        if (near(outputPos())) return std::make_pair(true, 0);
        for (int i = 0; i < node_.maxInputs(); ++i)
            if (near(inputPos(i))) return std::make_pair(false, i);
        return std::nullopt;
    }

    void paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*) override {
        p->setRenderHint(QPainter::Antialiasing);
        if (backdrop_) {
            paintBackdrop(p);
            return;
        }
        const QRectF body(-kNodeW / 2, 0, kNodeW, kNodeH);
        const bool viewed = doc_.viewed() == &node_;
        p->setPen(isSelected() ? QPen(QColor(255, 200, 80), 2) : QPen(QColor(20, 20, 20), 1));
        p->setBrush(colorFor(node_.typeName()));
        p->drawRoundedRect(body, 5, 5);

        p->setPen(Qt::white);
        QFont f = p->font();
        f.setPointSizeF(9);
        f.setBold(true);
        p->setFont(f);
        p->drawText(body.adjusted(4, 2, -4, -kNodeH / 2), Qt::AlignCenter, QString::fromStdString(node_.name()));
        f.setBold(false);
        f.setPointSizeF(7);
        p->setFont(f);
        p->setPen(QColor(220, 220, 220));
        p->drawText(body.adjusted(4, kNodeH / 2 - 2, -4, -2), Qt::AlignCenter, displayType(node_.typeName()));

        if (viewed) {  // a small "eye" marker on the right edge
            p->setPen(Qt::NoPen);
            p->setBrush(QColor(120, 200, 255));
            p->drawEllipse(QPointF(kNodeW / 2 - 8, 8), 4, 4);
        }

        // Ports: inputs on top (labelled), the output at the bottom.
        for (int i = 0; i < node_.maxInputs(); ++i) {
            const QPointF c = inputPos(i);
            p->setPen(QPen(QColor(20, 20, 20), 1));
            p->setBrush(node_.input(i) ? QColor(230, 230, 230) : (node_.inputOptional(i) ? QColor(120, 120, 120) : QColor(240, 180, 60)));
            p->drawEllipse(c, kPortR, kPortR);
            const std::string label = node_.inputLabel(i);
            if (!label.empty()) {
                p->setPen(QColor(200, 200, 200));
                p->drawText(QRectF(c.x() - 30, -19, 60, 12), Qt::AlignCenter, QString::fromStdString(label));
            }
        }
        p->setPen(QPen(QColor(20, 20, 20), 1));
        p->setBrush(QColor(230, 230, 230));
        p->drawEllipse(outputPos(), kPortR, kPortR);
    }

    std::function<void()> onMoved;

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant& value) override {
        if (change == ItemPositionHasChanged) {
            // A backdrop carries the nodes on it (those not already moving
            // because they're selected too).
            const QPointF delta = pos() - lastPos_;
            lastPos_ = pos();
            for (NodeItem* n : carried_) n->moveBy(delta.x(), delta.y());
            if (onMoved) onMoved();
        }
        return QGraphicsItem::itemChange(change, value);
    }

    void mousePressEvent(QGraphicsSceneMouseEvent* e) override {
        carried_.clear();
        lastPos_ = pos();
        if (backdrop_ && e->button() == Qt::LeftButton) {
            const QSizeF s = backdropSize();
            if (QRectF(s.width() - kHandle, s.height() - kHandle, kHandle, kHandle).contains(e->pos())) {
                resizing_ = s;  // the corner handle: resize
                resizeFrom_ = e->pos();
                e->accept();
                return;
            }
            const QRectF area = sceneBoundingRect();
            for (QGraphicsItem* it : scene()->items(area, Qt::ContainsItemBoundingRect))
                if (auto* n = dynamic_cast<NodeItem*>(it); n && n != this && !n->isBackdrop() && !n->isSelected())
                    carried_.push_back(n);
        }
        QGraphicsItem::mousePressEvent(e);
    }
    void mouseMoveEvent(QGraphicsSceneMouseEvent* e) override {
        if (resizing_) {
            const QPointF d = e->pos() - resizeFrom_;
            resizeFrom_ = e->pos();
            prepareGeometryChange();
            resizing_ = QSizeF(std::max(40.0, resizing_->width() + d.x()), std::max(40.0, resizing_->height() + d.y()));
            return;
        }
        QGraphicsItem::mouseMoveEvent(e);
    }
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* e) override {
        if (resizing_) {
            const QSizeF s = *resizing_;
            resizing_.reset();
            prepareGeometryChange();
            doc_.setParam(node_, "size", puke::Vec2{std::round(s.width()), std::round(s.height())});
            return;
        }
        QGraphicsItem::mouseReleaseEvent(e);
        carried_.clear();
    }

private:
    static constexpr double kHandle = 14;

    void paintBackdrop(QPainter* p) {
        const QRectF r(QPointF(), backdropSize());
        const puke::Color c = node_.colorParam("color", 0.3);
        auto ch = [](double v) { return std::clamp(static_cast<int>(v * 255 + 0.5), 0, 255); };
        const QColor fill(ch(c[0]), ch(c[1]), ch(c[2]));
        p->setPen(isSelected() ? QPen(QColor(255, 200, 80), 2) : QPen(fill.darker(160), 1));
        p->setBrush(fill);
        p->drawRect(r);
        p->setPen(Qt::NoPen);
        p->setBrush(fill.lighter(125));
        p->drawRect(QRectF(r.left(), r.top(), r.width(), 24));
        std::string label = node_.param<std::string>("label", "");
        if (label.empty()) label = node_.name();
        QFont f = p->font();
        f.setPointSizeF(11);
        f.setBold(true);
        p->setFont(f);
        p->setPen(Qt::white);
        p->drawText(QRectF(r.left() + 8, r.top(), r.width() - 16, 24), Qt::AlignVCenter | Qt::AlignLeft,
                    QString::fromStdString(label));
        p->setPen(QPen(fill.lighter(170), 1));  // the resize handle
        for (int i = 1; i <= 3; ++i)
            p->drawLine(QPointF(r.right() - 4 * i, r.bottom() - 2), QPointF(r.right() - 2, r.bottom() - 4 * i));
    }

    puke::Node& node_;
    Document& doc_;
    bool backdrop_;
    std::vector<NodeItem*> carried_;
    QPointF lastPos_;
    std::optional<QSizeF> resizing_;
    QPointF resizeFrom_;
};

class EdgeItem : public QGraphicsPathItem {
public:
    EdgeItem(NodeItem* from, NodeItem* to, int input) : from_(from), to_(to), input_(input) {
        setPen(QPen(QColor(190, 190, 190), 1.6));
        setZValue(0);
        update();
    }
    void update() {
        const QPointF a = from_->mapToScene(from_->outputPos());
        const QPointF b = to_->mapToScene(to_->inputPos(input_));
        QPainterPath path(a);
        const double dy = std::max(30.0, std::abs(b.y() - a.y()) / 2);
        path.cubicTo(a + QPointF(0, dy), b - QPointF(0, dy), b);
        setPath(path);
    }

private:
    NodeItem* from_;
    NodeItem* to_;
    int input_;
};

// ================================================================ create popup

namespace {

// A search box listing node types: type to filter, Enter or click to create.
class CreatePopup : public QWidget {
public:
    CreatePopup(QWidget* parent, std::function<void(const std::string&)> pick)
        : QWidget(parent, Qt::Popup), pick_(std::move(pick)) {
        setObjectName("createNodePopup");
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(4, 4, 4, 4);
        edit_ = new QLineEdit(this);
        edit_->setPlaceholderText("Create node...");
        list_ = new QListWidget(this);
        layout->addWidget(edit_);
        layout->addWidget(list_);
        resize(260, 300);
        for (const auto& t : puke::nodeTypes()) {
            auto* item = new QListWidgetItem(displayType(t), list_);
            item->setData(Qt::UserRole, QString::fromStdString(t));
            item->setToolTip(QString::fromStdString(t));
        }
        list_->setCurrentRow(0);
        connect(edit_, &QLineEdit::textChanged, this, [this](const QString& text) {
            int first = -1;
            for (int i = 0; i < list_->count(); ++i) {
                auto* it = list_->item(i);
                const bool show = it->text().contains(text, Qt::CaseInsensitive) ||
                                  it->data(Qt::UserRole).toString().contains(text, Qt::CaseInsensitive);
                it->setHidden(!show);
                if (show && first < 0) first = i;
            }
            if (first >= 0) list_->setCurrentRow(first);
        });
        connect(edit_, &QLineEdit::returnPressed, this, [this] { choose(list_->currentItem()); });
        connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem* it) { choose(it); });
        edit_->installEventFilter(this);
        edit_->setFocus();
    }

protected:
    bool eventFilter(QObject* o, QEvent* e) override {
        if (o == edit_ && e->type() == QEvent::KeyPress) {  // arrows move through the list while typing
            auto* k = static_cast<QKeyEvent*>(e);
            if (k->key() == Qt::Key_Down || k->key() == Qt::Key_Up) {
                int row = list_->currentRow();
                const int step = k->key() == Qt::Key_Down ? 1 : -1;
                for (int r = row + step; r >= 0 && r < list_->count(); r += step)
                    if (!list_->item(r)->isHidden()) {
                        list_->setCurrentRow(r);
                        break;
                    }
                return true;
            }
        }
        return QWidget::eventFilter(o, e);
    }

private:
    void choose(QListWidgetItem* it) {
        if (!it || it->isHidden()) return;
        const std::string type = it->data(Qt::UserRole).toString().toStdString();
        close();
        pick_(type);
    }

    std::function<void(const std::string&)> pick_;
    QLineEdit* edit_;
    QListWidget* list_;
};

}  // namespace

// ================================================================ view

NodeGraphView::NodeGraphView(Document& doc, QWidget* parent) : QGraphicsView(parent), doc_(doc) {
    setObjectName("nodeGraph");
    setScene(&scene_);
    scene_.setSceneRect(-100000, -100000, 200000, 200000);
    setRenderHint(QPainter::Antialiasing);
    setBackgroundBrush(QColor(42, 42, 44));
    setDragMode(RubberBandDrag);
    setTransformationAnchor(AnchorUnderMouse);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

    connect(&doc_, &Document::structureChanged, this, &NodeGraphView::rebuild);
    connect(&doc_, &Document::selectionChanged, this, &NodeGraphView::syncSelectionFromDoc);
    connect(&doc_, &Document::viewedChanged, this, [this] { scene_.update(); });
    connect(&doc_, &Document::nodeMoved, this, [this](puke::Node* n) {  // e.g. undo
        auto it = items_.find(n);
        if (it == items_.end() || !n->position()) return;
        const QPointF p((*n->position())[0], (*n->position())[1]);
        if (it->second->pos() != p) it->second->setPos(p);
    });
    connect(&doc_, &Document::paramChanged, this, [this](puke::Node* n) {
        auto it = items_.find(n);
        if (it != items_.end() && it->second->isBackdrop()) it->second->paramsChanged();
    });
    connect(&scene_, &QGraphicsScene::selectionChanged, this, [this] {
        if (syncing_) return;
        const auto sel = scene_.selectedItems();
        NodeItem* first = nullptr;
        for (auto* it : sel)
            if (auto* n = dynamic_cast<NodeItem*>(it)) first = n;
        doc_.setSelected(first ? &first->node() : nullptr);
    });
    rebuild();
}

NodeGraphView::~NodeGraphView() {
    // The scene, destroyed after this, deselects its items as it goes: don't
    // pass that on to the document, or let it reach the items' map.
    QObject::disconnect(&scene_, nullptr, this, nullptr);
    syncing_ = true;
    items_.clear();
}

void NodeGraphView::layoutUnplaced() {
    // Nodes without a position (e.g. from a script) get one by depth: inputs
    // above the nodes that use them.
    std::map<const puke::Node*, int> depth;
    std::function<int(const puke::Node*)> depthOf = [&](const puke::Node* n) -> int {
        if (auto it = depth.find(n); it != depth.end()) return it->second;
        depth[n] = 0;  // guards against cycles while we look
        int d = 0;
        for (int i = 0; i < n->maxInputs(); ++i)
            if (const puke::Node* in = n->input(i)) d = std::max(d, depthOf(in) + 1);
        return depth[n] = d;
    };
    std::map<int, int> perRow;
    for (const auto& n : doc_.graph().nodes()) {
        if (n->position()) continue;
        const int d = depthOf(n.get());
        n->setPosition(puke::Vec2{perRow[d]++ * 160.0, d * 90.0});  // placement only: not an edit
    }
}

void NodeGraphView::rebuild() {
    // Clearing the scene deletes the selected items, which fires selection
    // signals: forget the items first, and ignore those signals meanwhile.
    syncing_ = true;
    drag_.reset();
    items_.clear();
    edges_.clear();
    scene_.clear();
    syncing_ = false;
    layoutUnplaced();
    for (const auto& n : doc_.graph().nodes()) {
        auto* item = new NodeItem(*n, doc_);
        item->onMoved = [this] { updateEdges(); };
        scene_.addItem(item);
        items_[n.get()] = item;
    }
    for (const auto& n : doc_.graph().nodes())
        for (int i = 0; i < n->maxInputs(); ++i)
            if (const puke::Node* in = n->input(i)) {
                auto* e = new EdgeItem(items_.at(in), items_.at(n.get()), i);
                scene_.addItem(e);
                edges_.push_back(e);
            }
    syncSelectionFromDoc();
}

void NodeGraphView::updateEdges() {
    for (auto* e : edges_) e->update();
}

void NodeGraphView::syncSelectionFromDoc() {
    // The document only knows the main selected node: keep a selection of
    // several that already includes it.
    auto main = items_.find(doc_.selected());
    if (main != items_.end() && main->second->isSelected()) {
        scene_.update();
        return;
    }
    syncing_ = true;
    for (auto& [node, item] : items_) item->setSelected(node == doc_.selected());
    syncing_ = false;
    scene_.update();
}

void NodeGraphView::fitAll() {
    QRectF r = scene_.itemsBoundingRect();
    if (r.isEmpty()) r = QRectF(-200, -150, 400, 300);
    fitInView(r.adjusted(-60, -60, 60, 60), Qt::KeepAspectRatio);
    if (transform().m11() > 1.5) setTransform(QTransform::fromScale(1.5, 1.5));  // don't blow tiny graphs up
    centerOn(r.center());
}

void NodeGraphView::createAt(const std::string& type, QPointF scenePos) {
    if (type == "Backdrop") {
        createBackdrop(scenePos);
        return;
    }
    try {
        puke::Node* upstream = doc_.selected();
        QPointF pos = scenePos;
        if (upstream && upstream->position()) pos = QPointF((*upstream->position())[0], (*upstream->position())[1] + 90);
        doc_.createNode(type, pos, upstream);
    } catch (const puke::Error& e) {
        QMessageBox::warning(this, "Create node", QString::fromStdString(e.what()));
    }
}

void NodeGraphView::createBackdrop(QPointF scenePos) {
    // Around the selected nodes, or an empty one at the cursor.
    QRectF around;
    for (puke::Node* n : selectedNodes()) around |= items_.at(n)->sceneBoundingRect();
    doc_.beginEdit("Create Backdrop");
    try {
        if (around.isEmpty()) {
            doc_.createNode("Backdrop", scenePos);
        } else {
            around.adjust(-30, -50, 30, 30);  // room for the title
            puke::Node& b = doc_.createNode("Backdrop", around.topLeft());
            doc_.setParam(b, "size", puke::Vec2{std::round(around.width()), std::round(around.height())});
        }
    } catch (const puke::Error& e) {
        QMessageBox::warning(this, "Create node", QString::fromStdString(e.what()));
    }
    doc_.endEdit();
}

// --- clipboard

void NodeGraphView::copySelection() {
    const auto nodes = selectedNodes();
    if (nodes.empty()) return;
    QGuiApplication::clipboard()->setText(QString::fromStdString(doc_.copyNodes(nodes)));
}

void NodeGraphView::cutSelection() {
    const auto nodes = selectedNodes();
    if (nodes.empty()) return;
    copySelection();
    doc_.removeNodes(nodes);
}

void NodeGraphView::paste() {
    const std::string text = QGuiApplication::clipboard()->text().toStdString();
    if (text.empty()) return;
    // Under the cursor if it's over the graph, otherwise just off the
    // originals so the copies don't hide them.
    QPointF offset(40, 40);
    const QPoint cursor = mapFromGlobal(QCursor::pos());
    std::optional<QPointF> corner;  // the pasted nodes' top-left
    try {
        const puke::Graph nodes = puke::projectFromJson(text, "clipboard");
        for (const auto& n : nodes.nodes())
            if (const auto& p = n->position()) {
                const QPointF q((*p)[0], (*p)[1]);
                corner = corner ? QPointF(std::min(corner->x(), q.x()), std::min(corner->y(), q.y())) : q;
            }
    } catch (const puke::Error&) {
        return;  // not nodes: nothing to paste
    }
    if (corner && rect().contains(cursor)) offset = mapToScene(cursor) - *corner;
    pasteText(text, offset);
}

void NodeGraphView::duplicateSelection() {
    const auto nodes = selectedNodes();
    if (!nodes.empty()) pasteText(doc_.copyNodes(nodes), QPointF(40, 40));
}

void NodeGraphView::pasteText(const std::string& text, QPointF offset) {
    std::vector<puke::Node*> pasted;
    try {
        pasted = doc_.pasteNodes(text, offset);
    } catch (const puke::Error& e) {
        QMessageBox::warning(this, "Paste", QString::fromStdString(e.what()));
        return;
    }
    selectNodes(pasted);  // so it can be moved straight away
}

void NodeGraphView::selectNodes(const std::vector<puke::Node*>& nodes) {
    syncing_ = true;
    scene_.clearSelection();
    for (puke::Node* n : nodes) items_.at(n)->setSelected(true);
    syncing_ = false;
    doc_.setSelected(nodes.empty() ? nullptr : nodes.back());  // keeps the others (see syncSelectionFromDoc)
    scene_.update();
}

void NodeGraphView::renameSelected() {
    puke::Node* node = doc_.selected();
    if (!node) return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, "Rename", "Name:", QLineEdit::Normal,
                                               QString::fromStdString(node->name()), &ok);
    if (!ok) return;
    try {
        doc_.renameNode(*node, name.trimmed().toStdString());
    } catch (const puke::Error& e) {
        QMessageBox::warning(this, "Rename", QString::fromStdString(e.what()));
    }
}

void NodeGraphView::showCreatePopup() {
    const QPointF at = mapToScene(mapFromGlobal(QCursor::pos()));
    auto* popup = new CreatePopup(this, [this, at](const std::string& type) { createAt(type, at); });
    popup->setAttribute(Qt::WA_DeleteOnClose);
    popup->move(QCursor::pos());
    popup->show();
}

void NodeGraphView::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
        case Qt::Key_Tab:
            showCreatePopup();
            return;
        case Qt::Key_Delete:
        case Qt::Key_Backspace:
            doc_.removeNodes(selectedNodes());  // one undo step for all of them
            return;
        case Qt::Key_1:
            if (doc_.selected()) doc_.setViewed(doc_.selected());
            return;
        case Qt::Key_F:
            fitAll();
            return;
        case Qt::Key_F2:
            renameSelected();
            return;
        default:
            QGraphicsView::keyPressEvent(event);
    }
}

void NodeGraphView::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton) {
        panning_ = true;
        lastPan_ = event->pos();
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (event->button() == Qt::LeftButton) {
        const QPointF sp = mapToScene(event->pos());
        for (auto& [node, item] : items_) {
            auto port = item->portAt(sp);
            if (!port) continue;
            Drag d;
            puke::Node& n = item->node();
            if (port->first) {
                d.source = &n;  // from an output: a new connection
            } else if (puke::Node* src = n.input(port->second)) {
                d.source = src;  // from a connected input: pick the wire up
                d.detachedFrom = &n;
                d.detachedInput = port->second;
            } else {
                break;  // an empty input: nothing to drag
            }
            d.wire = scene_.addPath(QPainterPath(), QPen(QColor(255, 200, 80), 1.6, Qt::DashLine));
            drag_ = d;
            lastScenePos_ = sp;
            return;
        }
    }
    QGraphicsView::mousePressEvent(event);
}

void NodeGraphView::mouseMoveEvent(QMouseEvent* event) {
    if (panning_) {
        const QPoint d = event->pos() - lastPan_;
        lastPan_ = event->pos();
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - d.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - d.y());
        return;
    }
    if (drag_) {
        const QPointF a = items_.at(drag_->source)->mapToScene(items_.at(drag_->source)->outputPos());
        const QPointF b = mapToScene(event->pos());
        QPainterPath path(a);
        path.cubicTo(a + QPointF(0, 40), b - QPointF(0, 40), b);
        drag_->wire->setPath(path);
        return;
    }
    QGraphicsView::mouseMoveEvent(event);
}

void NodeGraphView::mouseReleaseEvent(QMouseEvent* event) {
    if (panning_ && event->button() == Qt::MiddleButton) {
        panning_ = false;
        unsetCursor();
        return;
    }
    if (drag_ && event->button() == Qt::LeftButton) {
        Drag d = *drag_;
        drag_.reset();
        delete d.wire;
        const QPointF sp = mapToScene(event->pos());
        std::optional<std::pair<puke::Node*, int>> target;
        for (auto& [node, item] : items_) {
            auto port = item->portAt(sp);
            if (port && !port->first) target = std::make_pair(&item->node(), port->second);
        }
        try {
            if (target && (target->first != d.detachedFrom || target->second != d.detachedInput)) {
                if (d.detachedFrom) {  // moving a wire: one undo step
                    doc_.beginEdit("Reconnect");
                    try {
                        doc_.disconnect(*d.detachedFrom, d.detachedInput);
                        doc_.connectNodes(*d.source, *target->first, target->second);
                    } catch (...) {
                        doc_.endEdit();
                        doc_.undoStack().undo();  // put the wire back
                        throw;
                    }
                    doc_.endEdit();
                } else {
                    doc_.connectNodes(*d.source, *target->first, target->second);
                }
            } else if (!target && d.detachedFrom) {
                doc_.disconnect(*d.detachedFrom, d.detachedInput);  // dropped on nothing: unplug
            }
        } catch (const puke::Error& e) {
            QMessageBox::warning(this, "Connect", QString::fromStdString(e.what()));
            rebuild();
        }
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
    // Nodes dragged: record where they ended up, as one undo step.
    std::vector<std::pair<puke::Node*, QPointF>> moves;
    for (auto& [node, item] : items_) {
        const auto& p = item->node().position();
        if (!p || QPointF((*p)[0], (*p)[1]) != item->pos()) moves.push_back({&item->node(), item->pos()});
    }
    if (!moves.empty()) doc_.moveNodes(moves);
}

std::vector<puke::Node*> NodeGraphView::selectedNodes() const {
    std::vector<puke::Node*> out;
    for (auto* it : scene_.selectedItems())
        if (auto* n = dynamic_cast<NodeItem*>(it)) out.push_back(&n->node());
    return out;
}

void NodeGraphView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (auto* item = dynamic_cast<NodeItem*>(itemAt(event->pos()))) {
        doc_.setViewed(&item->node());
        return;
    }
    QGraphicsView::mouseDoubleClickEvent(event);
}

void NodeGraphView::wheelEvent(QWheelEvent* event) {
    const double factor = event->angleDelta().y() > 0 ? 1.15 : 1 / 1.15;
    const double s = transform().m11() * factor;
    if (s < 0.1 || s > 4) return;
    scale(factor, factor);
}

void NodeGraphView::contextMenuEvent(QContextMenuEvent*) { showCreatePopup(); }

}  // namespace pukegui

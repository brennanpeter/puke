#include "Document.h"

#include <QSignalBlocker>
#include <QUndoCommand>

#include <algorithm>
#include <cctype>

#include "puke/engine/Engine.h"
#include "puke/nodes/Registry.h"
#include "puke/project/Project.h"

namespace pukegui {

// ================================================================ commands

// Base for the document's undo commands: gives them the raw operations.
// Commands hold nodes by shared_ptr, so a node removed by one command is
// still there to put back when it's undone.
class DocCommand : public QUndoCommand {
public:
    DocCommand(Document& doc, const QString& text) : doc_(doc) { setText(text); }

protected:
    Document& doc_;
    void insert(const std::shared_ptr<puke::Node>& n, std::size_t i) { doc_.rawInsert(n, i); }
    void take(puke::Node& n) { doc_.rawTake(n); }
    void connect(puke::Node* s, puke::Node& t, int i) { doc_.rawConnect(s, t, i); }
    void setParam(puke::Node& n, const std::string& k, const Document::ParamState& s) { doc_.rawSetParam(n, k, s); }
    void move(puke::Node& n, const std::optional<puke::Vec2>& p) { doc_.rawMove(n, p); }
    void rename(puke::Node& n, const std::string& name) { doc_.rawRename(n, name); }
    void setSettings(const puke::ProjectSettings& s) { doc_.rawSetSettings(s); }
    void select(puke::Node* n) { doc_.setSelected(n); }
    void view(puke::Node* n) { doc_.setViewed(n); }
    puke::Graph& graph() { return *doc_.graph_; }

    static std::shared_ptr<puke::Node> shared(puke::Node* n) {
        return n ? n->shared_from_this() : nullptr;
    }
};

namespace {

using NodePtr = std::shared_ptr<puke::Node>;

struct Connection {
    NodePtr source, target;
    int input;
};

// Adds nodes (already made, not yet in the graph) with connections among
// them or to existing nodes. Used for creating and pasting.
class AddNodes : public DocCommand {
public:
    AddNodes(Document& doc, const QString& text, std::vector<NodePtr> nodes, std::vector<Connection> conns)
        : DocCommand(doc, text), nodes_(std::move(nodes)), conns_(std::move(conns)) {}
    void redo() override {
        for (const auto& n : nodes_) insert(n, graph().nodes().size());
        for (const auto& c : conns_) connect(c.source.get(), *c.target, c.input);
        select(nodes_.empty() ? nullptr : nodes_.back().get());
    }
    void undo() override {
        for (auto it = nodes_.rbegin(); it != nodes_.rend(); ++it) take(**it);
    }

private:
    std::vector<NodePtr> nodes_;
    std::vector<Connection> conns_;
};

// Removes nodes, remembering everything needed to put them back exactly:
// their place in the node order, their inputs, and who used them.
class RemoveNodes : public DocCommand {
public:
    RemoveNodes(Document& doc, const std::vector<puke::Node*>& nodes)
        : DocCommand(doc, nodes.size() == 1 ? "Delete " + QString::fromStdString(nodes[0]->name())
                                            : QString("Delete %1 nodes").arg(nodes.size())) {
        for (puke::Node* n : nodes) {
            Record r;
            r.node = shared(n);
            r.index = graph().indexOf(*n);
            for (int i = 0; i < n->maxInputs(); ++i) r.inputs.push_back(shared(n->input(i)));
            for (puke::Node* c : graph().consumers(*n))
                for (int i = 0; i < c->maxInputs(); ++i)
                    if (c->input(i) == n) r.consumers.push_back({shared(c), i});
            r.wasViewed = doc.viewed() == n;
            r.wasSelected = doc.selected() == n;
            records_.push_back(std::move(r));
        }
        std::sort(records_.begin(), records_.end(), [](const Record& a, const Record& b) { return a.index < b.index; });
    }
    void redo() override {
        for (auto it = records_.rbegin(); it != records_.rend(); ++it) take(*it->node);
    }
    void undo() override {
        for (const auto& r : records_) insert(r.node, r.index);
        for (const auto& r : records_) {
            for (int i = 0; i < static_cast<int>(r.inputs.size()); ++i)
                if (r.inputs[i]) connect(r.inputs[i].get(), *r.node, i);
            for (const auto& [consumer, input] : r.consumers) connect(r.node.get(), *consumer, input);
        }
        for (const auto& r : records_) {
            if (r.wasViewed) view(r.node.get());
            if (r.wasSelected) select(r.node.get());
        }
    }

private:
    struct Record {
        NodePtr node;
        std::size_t index = 0;
        std::vector<NodePtr> inputs;
        std::vector<std::pair<NodePtr, int>> consumers;
        bool wasViewed = false, wasSelected = false;
    };
    std::vector<Record> records_;
};

// Sets what feeds one input (or nothing).
class Connect : public DocCommand {
public:
    Connect(Document& doc, NodePtr source, puke::Node& target, int input)
        : DocCommand(doc, source ? "Connect " + QString::fromStdString(source->name()) + " to " +
                                       QString::fromStdString(target.name())
                                 : "Disconnect " + QString::fromStdString(target.name())),
          before_(shared(target.input(input))), after_(std::move(source)), target_(shared(&target)), input_(input) {}
    void redo() override { connect(after_.get(), *target_, input_); }
    void undo() override { connect(before_.get(), *target_, input_); }

private:
    NodePtr before_, after_, target_;
    int input_;
};

class SetParam : public DocCommand {
public:
    SetParam(Document& doc, puke::Node& node, std::string key, Document::ParamState after, const QString& text)
        : DocCommand(doc, text), node_(shared(&node)), key_(std::move(key)),
          before_(Document::paramState(node, key_)), after_(std::move(after)) {}
    int id() const override { return 1; }
    // A run of the same change to the same parameter (a slider drag) is one step.
    bool mergeWith(const QUndoCommand* other) override {
        auto* o = dynamic_cast<const SetParam*>(other);
        if (!o || o->node_ != node_ || o->key_ != key_ || o->text() != text()) return false;
        after_ = o->after_;
        return true;
    }
    void redo() override { setParam(*node_, key_, after_); }
    void undo() override { setParam(*node_, key_, before_); }

private:
    NodePtr node_;
    std::string key_;
    Document::ParamState before_, after_;
};

class MoveNodes : public DocCommand {
public:
    MoveNodes(Document& doc, const std::vector<std::pair<puke::Node*, QPointF>>& moves)
        : DocCommand(doc, moves.size() == 1 ? "Move " + QString::fromStdString(moves[0].first->name())
                                            : QString("Move %1 nodes").arg(moves.size())) {
        for (const auto& [n, p] : moves) moves_.push_back({shared(n), n->position(), puke::Vec2{p.x(), p.y()}});
    }
    void redo() override {
        for (const auto& m : moves_) move(*m.node, m.after);
    }
    void undo() override {
        for (const auto& m : moves_) move(*m.node, m.before);
    }

private:
    struct Move {
        NodePtr node;
        std::optional<puke::Vec2> before;
        puke::Vec2 after;
    };
    std::vector<Move> moves_;
};

class Rename : public DocCommand {
public:
    Rename(Document& doc, puke::Node& node, std::string name)
        : DocCommand(doc, "Rename " + QString::fromStdString(node.name())), node_(shared(&node)), before_(node.name()),
          after_(std::move(name)) {}
    void redo() override { rename(*node_, after_); }
    void undo() override { rename(*node_, before_); }

private:
    NodePtr node_;
    std::string before_, after_;
};

class SetSettings : public DocCommand {
public:
    SetSettings(Document& doc, const puke::ProjectSettings& after)
        : DocCommand(doc, "Project settings"), before_(doc.settings()), after_(after) {}
    void redo() override { setSettings(after_); }
    void undo() override { setSettings(before_); }

private:
    puke::ProjectSettings before_, after_;
};

}  // namespace

// ================================================================ document

Document::Document(QObject* parent) : QObject(parent), graph_(std::make_unique<puke::Graph>()) {
    connect(&undo_, &QUndoStack::cleanChanged, this, [this](bool clean) { emit dirtyChanged(!clean); });
}

Document::~Document() {
    // Commands hold nodes; let them go before the graph. Clearing can change
    // the clean state, and whoever listens may already be gone: stay quiet.
    const QSignalBlocker quiet(this);
    undo_.clear();
}

void Document::setSelected(puke::Node* node) {
    if (node == selected_) return;
    selected_ = node;
    emit selectionChanged();
}

void Document::setFrame(int frame) {
    if (frame == frame_) return;
    frame_ = frame;
    emit frameChanged(frame);
}

void Document::setViewed(puke::Node* node) {
    if (node == viewed_) return;
    viewed_ = node;
    emit viewedChanged();
    emit renderNeeded();
}

namespace {

// "Grade12" -> "Grade1", "Grade13"...: the first name with `base`'s letters
// that `taken` rejects.
template <typename Taken>
std::string nextFreeName(const std::string& name, Taken taken) {
    std::string base = name;
    while (!base.empty() && std::isdigit(static_cast<unsigned char>(base.back()))) base.pop_back();
    if (base.empty()) base = "Node";
    for (int i = 1;; ++i) {
        std::string n = base + std::to_string(i);
        if (!taken(n)) return n;
    }
}

}  // namespace

std::string Document::freeName(const std::string& type) const {
    std::string base = type;
    if (base.rfind("ofx:", 0) == 0) base = base.substr(base.find_last_of(".:") + 1);
    for (int i = 1;; ++i) {
        std::string name = base + std::to_string(i);
        if (!graph_->find(name)) return name;
    }
}

// --- raw operations

void Document::rawInsert(const std::shared_ptr<puke::Node>& node, std::size_t index) {
    graph_->insert(node, index);
    emit structureChanged();
    emit renderNeeded();
}

void Document::rawTake(puke::Node& node) {
    if (selected_ == &node) setSelected(nullptr);
    if (viewed_ == &node) {
        viewed_ = nullptr;
        emit viewedChanged();
    }
    graph_->take(node);  // the command (and any render's snapshot) keeps it alive
    emit structureChanged();
    emit renderNeeded();
}

void Document::rawConnect(puke::Node* source, puke::Node& target, int input) {
    if (source) graph_->connect(*source, target, input);
    else graph_->disconnect(target, input);
    emit structureChanged();
    emit renderNeeded();
}

Document::ParamState Document::paramState(const puke::Node& node, const std::string& key) {
    ParamState s;
    if (const puke::Curve* c = node.curve(key)) s.curve = *c;
    else if (auto it = node.params().find(key); it != node.params().end()) s.value = it->second;
    return s;
}

void Document::rawSetParam(puke::Node& node, const std::string& key, const ParamState& state) {
    if (state.curve && !state.curve->empty()) node.setCurve(key, *state.curve);
    else if (state.value) node.setParam(key, *state.value);
    else node.clearParam(key);
    emit paramChanged(&node);
    emit renderNeeded();
}

void Document::rawMove(puke::Node& node, const std::optional<puke::Vec2>& position) {
    node.setPosition(position);
    emit nodeMoved(&node);
}

void Document::rawRename(puke::Node& node, const std::string& name) {
    graph_->rename(node, name);
    emit structureChanged();
    emit paramChanged(&node);  // panels showing its name
}

void Document::rawSetSettings(const puke::ProjectSettings& settings) {
    graph_->setSettings(settings);
    emit settingsChanged();
}

// --- edits

puke::Node& Document::createNode(const std::string& type, QPointF position, puke::Node* upstream) {
    // Made and checked before anything is recorded: a failure leaves no step.
    std::shared_ptr<puke::Node> node(puke::createNode(type, freeName(type)));
    node->setPosition(puke::Vec2{position.x(), position.y()});
    std::vector<Connection> conns;
    if (upstream && node->maxInputs() > 0) {
        conns.push_back({upstream->shared_from_this(), node, 0});
        // Some nodes start from their input's frame, as a user would expect:
        // Transform pivots around its middle (the node itself defaults to
        // the origin), Crop's box is all of it.
        if (type == "Transform" || type == "Crop") {
            try {
                puke::Engine engine(puke::EngineOptions{.threads = 1});
                const puke::Box f = engine.info(*upstream, {double(frame_), settings().format}).displayWindow;
                if (type == "Transform") {
                    node->setParam("center", puke::Vec2{(f.x0 + f.x1) / 2.0, (f.y0 + f.y1) / 2.0});
                } else {
                    node->setParam("box_min", puke::Vec2{double(f.x0), double(f.y0)});
                    node->setParam("box_max", puke::Vec2{double(f.x1), double(f.y1)});
                }
            } catch (const puke::Error&) {  // input not ready yet: keep the default
            }
        }
    }
    puke::Node& ref = *node;
    undo_.push(new AddNodes(*this, "Create " + QString::fromStdString(ref.name()), {node}, std::move(conns)));
    return ref;
}

void Document::connectNodes(puke::Node& source, puke::Node& target, int input) {
    if (target.input(input) == &source) return;
    // Try it first, so a cycle throws before anything is recorded.
    puke::Node* before = target.input(input);
    graph_->connect(source, target, input);
    if (before) graph_->connect(*before, target, input);
    else graph_->disconnect(target, input);
    undo_.push(new Connect(*this, source.shared_from_this(), target, input));
}

void Document::disconnect(puke::Node& target, int input) {
    if (!target.input(input)) return;
    undo_.push(new Connect(*this, nullptr, target, input));
}

void Document::removeNodes(const std::vector<puke::Node*>& nodes) {
    if (nodes.empty()) return;
    undo_.push(new RemoveNodes(*this, nodes));
}

void Document::moveNodes(const std::vector<std::pair<puke::Node*, QPointF>>& moves) {
    if (moves.empty()) return;
    undo_.push(new MoveNodes(*this, moves));
}

void Document::pushParam(puke::Node& node, const std::string& key, ParamState after, const QString& text) {
    if (after == paramState(node, key)) return;
    undo_.push(new SetParam(*this, node, key, std::move(after), text));
}

void Document::setParam(puke::Node& node, const std::string& key, puke::ParamValue value) {
    if (node.animated(key)) {
        setKey(node, key, std::move(value));
        return;
    }
    pushParam(node, key, {std::move(value), std::nullopt}, "Set " + QString::fromStdString(key));
}

void Document::clearParam(puke::Node& node, const std::string& key) {
    pushParam(node, key, {}, "Reset " + QString::fromStdString(key));
}

void Document::setKey(puke::Node& node, const std::string& key, puke::ParamValue value) {
    puke::Curve c = node.curve(key) ? *node.curve(key) : puke::Curve{};
    c.setKey(frame_, std::move(value));
    // The frame is in the name, so keys at different frames are different
    // steps (a drag at one frame still merges into one).
    pushParam(node, key, {std::nullopt, std::move(c)},
              QString("Key %1 at frame %2").arg(QString::fromStdString(key)).arg(frame_));
}

void Document::removeKey(puke::Node& node, const std::string& key) {
    const puke::Curve* old = node.curve(key);
    if (!old || !old->keyAt(frame_)) return;
    puke::Curve c = *old;
    const puke::ParamValue last = c.keyAt(frame_)->value;
    c.removeKey(frame_);
    ParamState after = c.empty() ? ParamState{last, std::nullopt} : ParamState{std::nullopt, std::move(c)};
    pushParam(node, key, std::move(after), "Delete key on " + QString::fromStdString(key));
}

void Document::removeAnimation(puke::Node& node, const std::string& key) {
    if (!node.animated(key)) return;
    pushParam(node, key, {node.valueAt(key, frame_), std::nullopt}, "Remove animation of " + QString::fromStdString(key));
}

void Document::editCurve(puke::Node& node, const std::string& key, puke::Curve curve, const QString& text) {
    if (curve.empty()) {
        pushParam(node, key, {node.valueAt(key, frame_), std::nullopt}, text);
        return;
    }
    pushParam(node, key, {std::nullopt, std::move(curve)}, text);
}

void Document::setInterp(puke::Node& node, const std::string& key, puke::Curve::Interp interp) {
    const puke::Curve* old = node.curve(key);
    if (!old || !old->keyAt(frame_)) return;
    puke::Curve c = *old;
    c.setInterp(frame_, interp);
    pushParam(node, key, {std::nullopt, std::move(c)}, "Interpolation of " + QString::fromStdString(key));
}

void Document::renameNode(puke::Node& node, const std::string& name) {
    if (name == node.name()) return;
    if (name.empty()) throw puke::Error("node name cannot be empty");
    if (graph_->find(name)) throw puke::Error("a node named '" + name + "' already exists");
    undo_.push(new Rename(*this, node, name));
}

std::string Document::copyNodes(const std::vector<puke::Node*>& nodes) const {
    return puke::nodesToJson(*graph_, std::vector<const puke::Node*>(nodes.begin(), nodes.end()));
}

std::vector<puke::Node*> Document::pasteNodes(const std::string& text, QPointF offset) {
    puke::Graph pasted = puke::projectFromJson(text, "pasted nodes");
    if (pasted.nodes().empty()) return {};

    // Note the connections, then take the nodes out (which disconnects them).
    std::vector<NodePtr> nodes = pasted.nodes();
    std::vector<Connection> conns;
    for (const auto& n : nodes)
        for (int i = 0; i < n->maxInputs(); ++i)
            if (puke::Node* in = n->input(i)) conns.push_back({in->shared_from_this(), n, i});
    for (const auto& n : nodes) pasted.take(*n);

    std::vector<std::string> names;  // the pasted nodes' new names so far
    auto taken = [&](const std::string& n) {
        return graph_->find(n) || std::find(names.begin(), names.end(), n) != names.end();
    };
    for (const auto& n : nodes) {
        if (taken(n->name())) n->setName(nextFreeName(n->name(), taken));
        names.push_back(n->name());
        if (const auto& p = n->position()) n->setPosition(puke::Vec2{(*p)[0] + offset.x(), (*p)[1] + offset.y()});
    }

    std::vector<puke::Node*> out;
    for (const auto& n : nodes) out.push_back(n.get());
    const QString text_ = nodes.size() == 1 ? "Paste " + QString::fromStdString(nodes[0]->name())
                                            : QString("Paste %1 nodes").arg(nodes.size());
    undo_.push(new AddNodes(*this, text_, std::move(nodes), std::move(conns)));
    return out;
}

void Document::setSettings(const puke::ProjectSettings& settings) {
    if (settings == graph_->settings()) return;
    puke::Graph check;
    check.setSettings(settings);  // throws if invalid, before anything is recorded
    undo_.push(new SetSettings(*this, settings));
}

// --- files

void Document::resetTo(std::unique_ptr<puke::Graph> graph, const QString& path, puke::Node* viewed) {
    undo_.clear();
    selected_ = nullptr;
    viewed_ = viewed;
    graph_ = std::move(graph);
    path_ = path;
    frame_ = graph_->settings().firstFrame;
    undo_.setClean();
    emit pathChanged();
    emit dirtyChanged(false);
    emit structureChanged();
    emit selectionChanged();
    emit viewedChanged();
    emit settingsChanged();
    emit frameChanged(frame_);
    emit renderNeeded();
}

void Document::newDocument() { resetTo(std::make_unique<puke::Graph>(), {}, nullptr); }

bool Document::load(const QString& path, QString* error) {
    std::unique_ptr<puke::Graph> graph;
    try {
        graph = std::make_unique<puke::Graph>(puke::loadProject(path.toStdString()));
    } catch (const puke::Error& e) {
        if (error) *error = QString::fromStdString(e.what());
        return false;
    }
    // View the last node that isn't a Write (usually the comp's output), or
    // the last node of all.
    puke::Node* viewed = nullptr;
    for (auto it = graph->nodes().rbegin(); it != graph->nodes().rend() && !viewed; ++it)
        if (std::string((*it)->typeName()) != "Write") viewed = it->get();
    if (!viewed && !graph->nodes().empty()) viewed = graph->nodes().back().get();
    resetTo(std::move(graph), path, viewed);
    return true;
}

bool Document::save(const QString& path, QString* error) {
    try {
        puke::saveProject(*graph_, path.toStdString());
    } catch (const puke::Error& e) {
        if (error) *error = QString::fromStdString(e.what());
        return false;
    }
    if (path != path_) {
        path_ = path;
        emit pathChanged();
    }
    undo_.setClean();
    return true;
}

}  // namespace pukegui

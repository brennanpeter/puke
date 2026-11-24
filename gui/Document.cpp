#include "Document.h"

#include "comp/engine/Engine.h"
#include "comp/nodes/Registry.h"
#include "comp/project/Project.h"

namespace compgui {

Document::Document(QObject* parent) : QObject(parent), graph_(std::make_unique<comp::Graph>()) {}

void Document::touch() {
    if (!dirty_) {
        dirty_ = true;
        emit dirtyChanged(true);
    }
}

void Document::setSelected(comp::Node* node) {
    if (node == selected_) return;
    selected_ = node;
    emit selectionChanged();
}

void Document::setViewed(comp::Node* node) {
    if (node == viewed_) return;
    viewed_ = node;
    emit viewedChanged();
    emit renderNeeded();
}

std::string Document::freeName(const std::string& type) const {
    std::string base = type;
    if (base.rfind("ofx:", 0) == 0) base = base.substr(base.find_last_of(".:") + 1);
    for (int i = 1;; ++i) {
        std::string name = base + std::to_string(i);
        if (!graph_->find(name)) return name;
    }
}

comp::Node& Document::createNode(const std::string& type, QPointF position, comp::Node* upstream) {
    comp::Node& node = graph_->add(comp::createNode(type, freeName(type)));
    node.setPosition(comp::Vec2{position.x(), position.y()});
    if (upstream && node.maxInputs() > 0) {
        graph_->connect(*upstream, node, 0);
        // Transform pivots around the middle of its input's frame, as a user
        // would expect (the node itself defaults to the origin).
        if (type == "Transform") {
            try {
                comp::Engine engine(comp::EngineOptions{.threads = 1});
                const comp::Box f = engine.info(*upstream, {}).displayWindow;
                node.setParam("center", comp::Vec2{(f.x0 + f.x1) / 2.0, (f.y0 + f.y1) / 2.0});
            } catch (const comp::Error&) {  // input not ready yet: keep the default
            }
        }
    }
    touch();
    emit structureChanged();
    setSelected(&node);
    emit renderNeeded();
    return node;
}

void Document::connectNodes(comp::Node& source, comp::Node& target, int input) {
    graph_->connect(source, target, input);
    touch();
    emit structureChanged();
    emit renderNeeded();
}

void Document::disconnect(comp::Node& target, int input) {
    graph_->disconnect(target, input);
    touch();
    emit structureChanged();
    emit renderNeeded();
}

void Document::removeNode(comp::Node& node) {
    if (selected_ == &node) setSelected(nullptr);
    if (viewed_ == &node) {
        viewed_ = nullptr;
        emit viewedChanged();
    }
    graph_->remove(node);  // the viewer's snapshots keep it alive as long as they need it
    touch();
    emit structureChanged();
    emit renderNeeded();
}

void Document::moveNode(comp::Node& node, QPointF position) {
    node.setPosition(comp::Vec2{position.x(), position.y()});
    touch();
    emit nodeMoved(&node);
}

void Document::setParam(comp::Node& node, const std::string& key, comp::ParamValue value) {
    auto it = node.params().find(key);
    if (it != node.params().end() && it->second == value) return;
    node.setParam(key, std::move(value));
    touch();
    emit paramChanged(&node);
    emit renderNeeded();
}

void Document::clearParam(comp::Node& node, const std::string& key) {
    if (!node.hasParam(key)) return;
    node.clearParam(key);
    touch();
    emit paramChanged(&node);
    emit renderNeeded();
}

void Document::newDocument() {
    selected_ = viewed_ = nullptr;
    graph_ = std::make_unique<comp::Graph>();
    path_.clear();
    dirty_ = false;
    emit pathChanged();
    emit dirtyChanged(false);
    emit structureChanged();
    emit selectionChanged();
    emit viewedChanged();
    emit renderNeeded();
}

bool Document::load(const QString& path, QString* error) {
    try {
        auto graph = std::make_unique<comp::Graph>(comp::loadProject(path.toStdString()));
        selected_ = viewed_ = nullptr;
        graph_ = std::move(graph);
        // View the last node that isn't a Write (usually the comp's output),
        // or the last node of all.
        for (auto it = graph_->nodes().rbegin(); it != graph_->nodes().rend() && !viewed_; ++it)
            if (std::string((*it)->typeName()) != "Write") viewed_ = it->get();
        if (!viewed_ && !graph_->nodes().empty()) viewed_ = graph_->nodes().back().get();
    } catch (const comp::Error& e) {
        if (error) *error = QString::fromStdString(e.what());
        return false;
    }
    path_ = path;
    dirty_ = false;
    emit pathChanged();
    emit dirtyChanged(false);
    emit structureChanged();
    emit selectionChanged();
    emit viewedChanged();
    emit renderNeeded();
    return true;
}

bool Document::save(const QString& path, QString* error) {
    try {
        comp::saveProject(*graph_, path.toStdString());
    } catch (const comp::Error& e) {
        if (error) *error = QString::fromStdString(e.what());
        return false;
    }
    if (path != path_) {
        path_ = path;
        emit pathChanged();
    }
    dirty_ = false;
    emit dirtyChanged(false);
    return true;
}

}  // namespace compgui

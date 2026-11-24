#pragma once

#include <QObject>
#include <QPointF>
#include <QString>

#include <memory>
#include <optional>
#include <string>

#include "comp/graph/Graph.h"

namespace compgui {

// The open project: its graph, file, and which nodes are selected and
// viewed. Every edit goes through here, so the views (node graph,
// properties, viewer) stay in step through the signals.
//
// All of this lives on the GUI thread. The viewer renders on its own thread
// from Snapshots taken here, so edits never wait for renders.
class Document : public QObject {
    Q_OBJECT
public:
    explicit Document(QObject* parent = nullptr);

    comp::Graph& graph() { return *graph_; }
    const QString& path() const { return path_; }
    bool dirty() const { return dirty_; }

    comp::Node* selected() const { return selected_; }
    comp::Node* viewed() const { return viewed_; }
    void setSelected(comp::Node* node);
    void setViewed(comp::Node* node);

    // Creates a node of `type` (a registry name, e.g. "Grade" or
    // "ofx:<id>"), named Type1, Type2... If `upstream` is given, its output
    // feeds the new node's first input. Throws comp::Error.
    comp::Node& createNode(const std::string& type, QPointF position, comp::Node* upstream = nullptr);
    // Throws comp::Error (e.g. for a cycle).
    void connectNodes(comp::Node& source, comp::Node& target, int input);
    void disconnect(comp::Node& target, int input);
    void removeNode(comp::Node& node);
    void moveNode(comp::Node& node, QPointF position);  // no re-render needed
    void setParam(comp::Node& node, const std::string& key, comp::ParamValue value);
    void clearParam(comp::Node& node, const std::string& key);

    void newDocument();
    bool load(const QString& path, QString* error);
    bool save(const QString& path, QString* error);

signals:
    void structureChanged();               // nodes or connections changed
    void paramChanged(comp::Node* node);   // a parameter of `node` changed
    void nodeMoved(comp::Node* node);
    void selectionChanged();
    void viewedChanged();
    void dirtyChanged(bool dirty);
    void pathChanged();
    void renderNeeded();                   // what the viewer shows may have changed

private:
    void touch();  // mark dirty
    std::string freeName(const std::string& type) const;

    std::unique_ptr<comp::Graph> graph_;
    QString path_;
    bool dirty_ = false;
    comp::Node* selected_ = nullptr;
    comp::Node* viewed_ = nullptr;
};

}  // namespace compgui

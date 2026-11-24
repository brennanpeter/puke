#pragma once

#include <QObject>
#include <QPointF>
#include <QString>
#include <QUndoStack>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "comp/graph/Graph.h"

namespace compgui {

class DocCommand;

// The open project: its graph, file, and which nodes are selected and
// viewed. Every edit goes through here, so the views (node graph,
// properties, viewer) stay in step through the signals.
//
// Every edit is an undoable command on undoStack(). Edits are checked
// before they're recorded (a connection that would make a cycle, a name
// that's taken...), so a failed edit throws comp::Error and leaves nothing
// on the stack. Unsaved changes are tracked by the stack too: undoing back
// to the saved state makes the document clean again.
//
// All of this lives on the GUI thread. The viewer renders on its own thread
// from Snapshots taken here, so edits never wait for renders.
class Document : public QObject {
    Q_OBJECT
public:
    explicit Document(QObject* parent = nullptr);
    ~Document() override;

    comp::Graph& graph() { return *graph_; }
    const QString& path() const { return path_; }
    bool dirty() const { return !undo_.isClean(); }
    QUndoStack& undoStack() { return undo_; }

    comp::Node* selected() const { return selected_; }
    comp::Node* viewed() const { return viewed_; }
    void setSelected(comp::Node* node);
    void setViewed(comp::Node* node);

    // --- edits (each one undoable)

    // Creates a node of `type` (a registry name, e.g. "Grade" or
    // "ofx:<id>"), named Type1, Type2... If `upstream` is given, its output
    // feeds the new node's first input. Throws comp::Error.
    comp::Node& createNode(const std::string& type, QPointF position, comp::Node* upstream = nullptr);
    // Feeds `source` into `target`'s input (replacing what was there).
    // Throws comp::Error (e.g. for a cycle).
    void connectNodes(comp::Node& source, comp::Node& target, int input);
    void disconnect(comp::Node& target, int input);
    void removeNodes(const std::vector<comp::Node*>& nodes);
    void removeNode(comp::Node& node) { removeNodes({&node}); }
    // Several nodes moved at once are one undo step.
    void moveNodes(const std::vector<std::pair<comp::Node*, QPointF>>& moves);
    void moveNode(comp::Node& node, QPointF position) { moveNodes({{&node, position}}); }
    // Changes to the same parameter in a row merge into one undo step (a
    // slider drag is one step, not hundreds).
    void setParam(comp::Node& node, const std::string& key, comp::ParamValue value);
    void clearParam(comp::Node& node, const std::string& key);
    // Throws comp::Error if the name is empty or taken.
    void renameNode(comp::Node& node, const std::string& name);

    // Copy and paste: nodes as project text (the .comp format), so they can
    // go through the clipboard, between windows or into a text editor.
    std::string copyNodes(const std::vector<comp::Node*>& nodes) const;
    // Adds the nodes in `text` as one undo step and returns them. Names
    // already in use get the next free number (Grade1 -> Grade2), and
    // connections between pasted nodes are kept. Positions are shifted by
    // `offset`. Throws comp::Error if the text isn't a project.
    std::vector<comp::Node*> pasteNodes(const std::string& text, QPointF offset);

    // Groups the edits made until endEdit() into one undo step.
    void beginEdit(const QString& text) { undo_.beginMacro(text); }
    void endEdit() { undo_.endMacro(); }

    // --- files (these clear the undo history)

    void newDocument();
    bool load(const QString& path, QString* error);
    bool save(const QString& path, QString* error);

signals:
    void structureChanged();               // nodes, names or connections changed
    void paramChanged(comp::Node* node);   // a parameter of `node` changed
    void nodeMoved(comp::Node* node);
    void selectionChanged();
    void viewedChanged();
    void dirtyChanged(bool dirty);
    void pathChanged();
    void renderNeeded();                   // what the viewer shows may have changed

private:
    friend class DocCommand;

    // The raw operations commands are built from. They change the graph and
    // emit signals; they never touch the undo stack.
    void rawInsert(const std::shared_ptr<comp::Node>& node, std::size_t index);
    void rawTake(comp::Node& node);
    void rawConnect(comp::Node* source, comp::Node& target, int input);  // null source: disconnect
    void rawSetParam(comp::Node& node, const std::string& key, const std::optional<comp::ParamValue>& value);
    void rawMove(comp::Node& node, const std::optional<comp::Vec2>& position);
    void rawRename(comp::Node& node, const std::string& name);

    std::string freeName(const std::string& type) const;
    void resetTo(std::unique_ptr<comp::Graph> graph, const QString& path, comp::Node* viewed);

    std::unique_ptr<comp::Graph> graph_;
    QString path_;
    QUndoStack undo_;
    comp::Node* selected_ = nullptr;
    comp::Node* viewed_ = nullptr;
};

}  // namespace compgui

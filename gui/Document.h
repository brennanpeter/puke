#pragma once

#include <QObject>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QUndoStack>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "puke/graph/Graph.h"

namespace pukegui {

class DocCommand;

// The open project: its graph, file, and which nodes are selected and
// viewed. Every edit goes through here, so the views (node graph,
// properties, viewer) stay in step through the signals.
//
// Every edit is an undoable command on undoStack(). Edits are checked
// before they're recorded (a connection that would make a cycle, a name
// that's taken...), so a failed edit throws puke::Error and leaves nothing
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

    puke::Graph& graph() { return *graph_; }
    const QString& path() const { return path_; }
    bool dirty() const { return !undo_.isClean(); }
    QUndoStack& undoStack() { return undo_; }

    // The frame being viewed. Changing it isn't an edit (not undoable, doesn't
    // make the document dirty).
    int frame() const { return frame_; }
    void setFrame(int frame);
    const puke::ProjectSettings& settings() const { return graph_->settings(); }

    puke::Node* selected() const { return selected_; }
    puke::Node* viewed() const { return viewed_; }
    void setSelected(puke::Node* node);
    void setViewed(puke::Node* node);

    // --- edits (each one undoable)

    // Creates a node of `type` (a registry name, e.g. "Grade" or
    // "ofx:<id>"), named Type1, Type2... If `upstream` is given, its output
    // feeds the new node's first input. Throws puke::Error.
    puke::Node& createNode(const std::string& type, QPointF position, puke::Node* upstream = nullptr);
    // Feeds `source` into `target`'s input (replacing what was there).
    // Throws puke::Error (e.g. for a cycle).
    void connectNodes(puke::Node& source, puke::Node& target, int input);
    void disconnect(puke::Node& target, int input);
    void removeNodes(const std::vector<puke::Node*>& nodes);
    void removeNode(puke::Node& node) { removeNodes({&node}); }
    // Several nodes moved at once are one undo step.
    void moveNodes(const std::vector<std::pair<puke::Node*, QPointF>>& moves);
    void moveNode(puke::Node& node, QPointF position) { moveNodes({{&node, position}}); }
    // Changes to the same parameter in a row merge into one undo step (a
    // slider drag is one step, not hundreds).
    // Sets a parameter. If it's animated, sets a key at the current frame
    // instead (as in Nuke: editing an animated value keys it).
    void setParam(puke::Node& node, const std::string& key, puke::ParamValue value);
    // A Read's file, and its frame range from the files on disk, as one
    // step. `asSequence`: a file that's one of a sequence (shot.1001.exr)
    // becomes the sequence (shot.####.exr). A path with a frame token gets
    // first/last from the frames found; without, they're left alone.
    void setReadFile(puke::Node& read, const std::string& path, bool asSequence);
    // Read nodes for these files, in a row from `position`, as one step:
    // one per sequence (any number of its files), one per other image.
    // Files OpenImageIO can't read, and folders, are skipped. Returns the
    // new nodes.
    std::vector<puke::Node*> importFiles(const QStringList& paths, QPointF position);
    // Back to the default: no value, no animation.
    void clearParam(puke::Node& node, const std::string& key);

    // Animation, at the current frame. setKey() animates a parameter that
    // isn't yet. Removing the last key leaves its value, unanimated, as
    // does removeAnimation() with the value at the current frame.
    void setKey(puke::Node& node, const std::string& key, puke::ParamValue value);
    void removeKey(puke::Node& node, const std::string& key);
    void removeAnimation(puke::Node& node, const std::string& key);
    // The interpolation leaving the key at the current frame.
    void setInterp(puke::Node& node, const std::string& key, puke::Curve::Interp interp);
    // Replaces a parameter's whole curve (the curve editor's edits). An
    // empty curve leaves the value at the current frame, unanimated.
    void editCurve(puke::Node& node, const std::string& key, puke::Curve curve, const QString& text);

    // A parameter's whole state: a plain value, a curve, or neither.
    struct ParamState {
        std::optional<puke::ParamValue> value;
        std::optional<puke::Curve> curve;
        bool operator==(const ParamState&) const = default;
    };
    static ParamState paramState(const puke::Node& node, const std::string& key);
    // Throws puke::Error if the name is empty or taken.
    void renameNode(puke::Node& node, const std::string& name);
    // Frame range and fps. Throws puke::Error if they're invalid.
    void setSettings(const puke::ProjectSettings& settings);

    // Copy and paste: nodes as project text (the .puke format), so they can
    // go through the clipboard, between windows or into a text editor.
    std::string copyNodes(const std::vector<puke::Node*>& nodes) const;
    // Adds the nodes in `text` as one undo step and returns them. Names
    // already in use get the next free number (Grade1 -> Grade2), and
    // connections between pasted nodes are kept. Positions are shifted by
    // `offset`. Throws puke::Error if the text isn't a project.
    std::vector<puke::Node*> pasteNodes(const std::string& text, QPointF offset);

    // Groups the edits made until endEdit() into one undo step.
    void beginEdit(const QString& text) { undo_.beginMacro(text); }
    void endEdit() { undo_.endMacro(); }

    // --- files (these clear the undo history)

    void newDocument();
    bool load(const QString& path, QString* error);
    bool save(const QString& path, QString* error);

signals:
    void structureChanged();               // nodes, names or connections changed
    void paramChanged(puke::Node* node);   // a parameter of `node` changed
    void nodeMoved(puke::Node* node);
    void selectionChanged();
    void viewedChanged();
    void dirtyChanged(bool dirty);
    void pathChanged();
    void renderNeeded();                   // what the viewer shows may have changed
    void frameChanged(int frame);
    void settingsChanged();

private:
    friend class DocCommand;

    // The raw operations commands are built from. They change the graph and
    // emit signals; they never touch the undo stack.
    void rawInsert(const std::shared_ptr<puke::Node>& node, std::size_t index);
    void rawTake(puke::Node& node);
    void rawConnect(puke::Node* source, puke::Node& target, int input);  // null source: disconnect
    void rawSetParam(puke::Node& node, const std::string& key, const ParamState& state);
    void pushParam(puke::Node& node, const std::string& key, ParamState after, const QString& text);
    void rawMove(puke::Node& node, const std::optional<puke::Vec2>& position);
    void rawRename(puke::Node& node, const std::string& name);
    void rawSetSettings(const puke::ProjectSettings& settings);

    std::string freeName(const std::string& type) const;
    void resetTo(std::unique_ptr<puke::Graph> graph, const QString& path, puke::Node* viewed);

    std::unique_ptr<puke::Graph> graph_;
    QString path_;
    QUndoStack undo_;
    puke::Node* selected_ = nullptr;
    puke::Node* viewed_ = nullptr;
    int frame_ = 1;
};

}  // namespace pukegui

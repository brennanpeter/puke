#pragma once

#include <QGraphicsView>

#include <map>
#include <optional>
#include <vector>

namespace puke {
class Node;
}

namespace pukegui {

class Document;
class NodeItem;
class EdgeItem;

// The node graph editor.
//
//   Tab / right-click   create a node (with a node selected, the new one is
//                       placed below it and connected to it)
//   drag output -> input   connect; drag a connected input away to disconnect
//   1 or double-click   view the node
//   Delete / Backspace  remove the selected nodes
//   F2                  rename the selected node
//   F                   fit the graph in the window
//   Ctrl+C/X/V, Ctrl+D  copy, cut, paste, duplicate (as actions in the main
//                       window's Edit menu)
//   Backdrops: created around the selected nodes; dragging one moves the
//   nodes on it, and its bottom-right corner resizes it.
//   wheel, middle-drag  zoom, pan
class NodeGraphView : public QGraphicsView {
    Q_OBJECT
public:
    NodeGraphView(Document& doc, QWidget* parent = nullptr);
    ~NodeGraphView() override;

    // Opens the create-node search at the cursor (also bound to Tab).
    void showCreatePopup();
    void fitAll();
    // The nodes selected in the graph (the document tracks only the main one).
    std::vector<puke::Node*> selectedNodes() const;

    void selectNodes(const std::vector<puke::Node*>& nodes);
    // Creates a node as the create popup does: below and connected to the
    // selected node, or for a Backdrop, around the selected nodes.
    void createAt(const std::string& type, QPointF scenePos);

    // The clipboard holds nodes as project text.
    void copySelection();
    void cutSelection();
    void paste();
    void duplicateSelection();
    void renameSelected();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    // Image files dropped from a file manager become Read nodes (one per
    // sequence), where they're dropped.
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    bool focusNextPrevChild(bool) override { return false; }  // keep Tab for us

private:
    void rebuild();
    void syncSelectionFromDoc();
    void updateEdges();
    void layoutUnplaced();
    void createBackdrop(QPointF scenePos);
    void pasteText(const std::string& text, QPointF offset);

    Document& doc_;
    QGraphicsScene scene_;
    std::map<const puke::Node*, NodeItem*> items_;
    std::vector<EdgeItem*> edges_;
    bool syncing_ = false;

    // Dragging a connection.
    struct Drag {
        puke::Node* source = nullptr;       // whose output the wire starts at
        puke::Node* detachedFrom = nullptr;  // input we pulled it off, if any
        int detachedInput = -1;
        QGraphicsPathItem* wire = nullptr;
    };
    std::optional<Drag> drag_;
    bool panning_ = false;
    QPoint lastPan_;
    QPointF lastScenePos_;
};

}  // namespace pukegui

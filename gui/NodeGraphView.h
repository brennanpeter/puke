#pragma once

#include <QGraphicsView>

#include <map>
#include <optional>
#include <vector>

namespace comp {
class Node;
}

namespace compgui {

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
//   F                   fit the graph in the window
//   wheel, middle-drag  zoom, pan
class NodeGraphView : public QGraphicsView {
    Q_OBJECT
public:
    NodeGraphView(Document& doc, QWidget* parent = nullptr);

    // Opens the create-node search at the cursor (also bound to Tab).
    void showCreatePopup();
    void fitAll();
    // The nodes selected in the graph (the document tracks only the main one).
    std::vector<comp::Node*> selectedNodes() const;

protected:
    void keyPressEvent(QKeyEvent* event) override;
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
    void createAt(const std::string& type, QPointF scenePos);

    Document& doc_;
    QGraphicsScene scene_;
    std::map<const comp::Node*, NodeItem*> items_;
    std::vector<EdgeItem*> edges_;
    bool syncing_ = false;

    // Dragging a connection.
    struct Drag {
        comp::Node* source = nullptr;       // whose output the wire starts at
        comp::Node* detachedFrom = nullptr;  // input we pulled it off, if any
        int detachedInput = -1;
        QGraphicsPathItem* wire = nullptr;
    };
    std::optional<Drag> drag_;
    bool panning_ = false;
    QPoint lastPan_;
    QPointF lastScenePos_;
};

}  // namespace compgui

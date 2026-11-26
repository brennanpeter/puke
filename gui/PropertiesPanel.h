#pragma once

#include <QScrollArea>

#include <functional>
#include <vector>

namespace puke {
class Node;
}

namespace pukegui {

class Document;

// Edits the selected node's parameters. The controls come from the node
// type's ParamSpecs: a bold label means the value is set (rather than the
// default), and its reset button clears it.
class PropertiesPanel : public QScrollArea {
    Q_OBJECT
public:
    PropertiesPanel(Document& doc, QWidget* parent = nullptr);

private:
    void rebuild();
    void refresh();  // re-read values into the existing controls

    Document& doc_;
    puke::Node* node_ = nullptr;
    std::vector<std::function<void()>> refreshers_;
};

}  // namespace pukegui

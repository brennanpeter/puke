#pragma once

#include <QMainWindow>

class QTabWidget;

namespace compgui {

class Document;
class NodeGraphView;
class CurveEditor;
class PropertiesPanel;
class RenderDialog;
class Timeline;
class Viewer;

// comp's look: Fusion style with a dark palette. Call once, after creating
// the QApplication.
void applyTheme();

// The main window: viewer and timeline above the node graph, properties on
// the right.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;

    Document& document() { return *doc_; }
    Viewer& viewer() { return *viewer_; }
    NodeGraphView& graphView() { return *graph_; }
    PropertiesPanel& properties() { return *properties_; }
    Timeline& timeline() { return *timeline_; }
    CurveEditor& curveEditor() { return *curves_; }
    QTabWidget& bottomTabs() { return *tabs_; }
    RenderDialog& renderDialog() { return *render_; }
    void showRenderDialog();

    bool openProject(const QString& path);  // reports errors in a dialog

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    bool maybeSave();  // false: the user cancelled
    bool save();
    bool saveAs();
    void updateTitle();

    Document* doc_;
    Viewer* viewer_;
    NodeGraphView* graph_;
    PropertiesPanel* properties_;
    Timeline* timeline_;
    CurveEditor* curves_;
    QTabWidget* tabs_;
    RenderDialog* render_;
};

}  // namespace compgui

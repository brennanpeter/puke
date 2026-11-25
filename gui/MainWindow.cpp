#include "MainWindow.h"

#include <QAction>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenuBar>
#include <QMessageBox>
#include <QSplitter>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QStatusBar>
#include <QStyleFactory>
#include <QApplication>
#include <QPalette>

#include "CurveEditor.h"
#include "Document.h"
#include "NodeGraphView.h"
#include "ProjectSettingsDialog.h"
#include "PropertiesPanel.h"
#include "RenderDialog.h"
#include "Timeline.h"
#include "Viewer.h"
#include "comp/nodes/Registry.h"

namespace compgui {

void applyTheme() {
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    // A dark palette: easier on the eyes next to images.
    QPalette p;
    p.setColor(QPalette::Window, QColor(50, 50, 52));
    p.setColor(QPalette::WindowText, QColor(220, 220, 220));
    p.setColor(QPalette::Base, QColor(36, 36, 38));
    p.setColor(QPalette::AlternateBase, QColor(46, 46, 48));
    p.setColor(QPalette::Text, QColor(220, 220, 220));
    p.setColor(QPalette::Button, QColor(58, 58, 60));
    p.setColor(QPalette::ButtonText, QColor(220, 220, 220));
    p.setColor(QPalette::Highlight, QColor(200, 140, 40));
    p.setColor(QPalette::HighlightedText, Qt::black);
    p.setColor(QPalette::ToolTipBase, QColor(60, 60, 62));
    p.setColor(QPalette::ToolTipText, QColor(230, 230, 230));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(120, 120, 120));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(120, 120, 120));
    QApplication::setPalette(p);

}

MainWindow::MainWindow() {
    setObjectName("mainWindow");
    doc_ = new Document(this);
    viewer_ = new Viewer(*doc_);
    graph_ = new NodeGraphView(*doc_);
    properties_ = new PropertiesPanel(*doc_);

    timeline_ = new Timeline(*doc_, *viewer_);
    auto* top = new QWidget;
    auto* topLayout = new QVBoxLayout(top);
    topLayout->setContentsMargins(0, 0, 0, 0);
    topLayout->setSpacing(0);
    topLayout->addWidget(viewer_, 1);
    topLayout->addWidget(timeline_);

    auto* split = new QSplitter(Qt::Vertical);
    split->addWidget(top);
    // The node graph and the curve editor share the bottom, as tabs.
    curves_ = new CurveEditor(*doc_);
    tabs_ = new QTabWidget;
    tabs_->setObjectName("bottomTabs");
    tabs_->setDocumentMode(true);
    tabs_->addTab(graph_, "Node Graph");
    tabs_->addTab(curves_, "Curve Editor");
    split->addWidget(tabs_);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    split->setSizes({520, 340});
    setCentralWidget(split);

    auto* dock = new QDockWidget("Properties", this);
    dock->setObjectName("propertiesDock");
    dock->setWidget(properties_);
    dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    addDockWidget(Qt::RightDockWidgetArea, dock);

    // --- File
    QMenu* file = menuBar()->addMenu("&File");
    file->addAction("&New", QKeySequence::New, this, [this] {
        if (maybeSave()) doc_->newDocument();
    });
    file->addAction("&Open...", QKeySequence::Open, this, [this] {
        if (!maybeSave()) return;
        const QString path = QFileDialog::getOpenFileName(this, "Open project", {}, "comp projects (*.comp);;All files (*)");
        if (!path.isEmpty()) openProject(path);
    });
    file->addAction("&Save", QKeySequence::Save, this, [this] { save(); });
    file->addAction("Save &As...", QKeySequence::SaveAs, this, [this] { saveAs(); });
    file->addSeparator();
    file->addAction("&Quit", QKeySequence::Quit, this, &QWidget::close);

    // --- Edit
    QMenu* edit = menuBar()->addMenu("&Edit");
    QAction* undo = doc_->undoStack().createUndoAction(this, "&Undo");
    undo->setShortcut(QKeySequence::Undo);
    QAction* redo = doc_->undoStack().createRedoAction(this, "&Redo");
    redo->setShortcuts({QKeySequence::Redo, QKeySequence("Ctrl+Shift+Z")});
    edit->addAction(undo);
    edit->addAction(redo);
    edit->addSeparator();
    // Their shortcuts only act in the node graph, so the usual keys still
    // copy and paste text in the properties' fields.
    auto graphAction = [&](const QString& text, const QKeySequence& key, auto fn) {
        QAction* a = edit->addAction(text, this, fn);
        a->setShortcut(key);
        a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        graph_->addAction(a);
        return a;
    };
    graphAction("Cu&t", QKeySequence::Cut, [this] { graph_->cutSelection(); });
    graphAction("&Copy", QKeySequence::Copy, [this] { graph_->copySelection(); });
    graphAction("&Paste", QKeySequence::Paste, [this] { graph_->paste(); });
    graphAction("D&uplicate", QKeySequence(Qt::CTRL | Qt::Key_D), [this] { graph_->duplicateSelection(); });
    graphAction("Project &Settings...", QKeySequence(Qt::Key_S), [this] {
        ProjectSettingsDialog dialog(*doc_, this);
        dialog.exec();
    });
    edit->addSeparator();
    edit->addAction("&Rename... (F2)", this, [this] { graph_->renameSelected(); });
    edit->addAction("&Delete nodes", QKeySequence::Delete, this, [this] { doc_->removeNodes(graph_->selectedNodes()); });

    // --- Nodes: built-ins, then OFX plugins in a submenu
    QMenu* nodes = menuBar()->addMenu("&Nodes");
    nodes->addAction("Create... (Tab)", this, [this] { graph_->showCreatePopup(); });
    nodes->addSeparator();
    QMenu* ofx = nullptr;
    for (const auto& type : comp::nodeTypes()) {
        const bool isOfx = type.rfind("ofx:", 0) == 0;
        if (isOfx && !ofx) ofx = nodes->addMenu("OFX plugins");
        QMenu* into = isOfx ? ofx : nodes;
        into->addAction(QString::fromStdString(isOfx ? type.substr(4) : type), this, [this, type] {
            const comp::Node* sel = doc_->selected();
            QPointF pos(0, 0);
            if (sel && sel->position()) pos = QPointF((*sel->position())[0], (*sel->position())[1] + 90);
            try {
                doc_->createNode(type, pos, doc_->selected());
            } catch (const comp::Error& e) {
                QMessageBox::warning(this, "Create node", QString::fromStdString(e.what()));
            }
        });
    }

    // --- Render
    render_ = new RenderDialog(*doc_, this);
    QMenu* renderMenu = menuBar()->addMenu("&Render");
    renderMenu->addAction("&Render...", QKeySequence(Qt::Key_F7), this, &MainWindow::showRenderDialog);

    // --- View
    QMenu* view = menuBar()->addMenu("&View");
    view->addAction("View selected node (1)", this, [this] {
        if (doc_->selected()) doc_->setViewed(doc_->selected());
    });
    view->addAction("Fit viewer (F)", this, [this] { viewer_->canvas()->fit(); });
    view->addAction("Fit node graph", this, [this] { graph_->fitAll(); });
    view->addSeparator();
    // Frame keys act in the viewer and timeline (the node graph and
    // properties keep their arrow keys).
    auto frameAction = [&](const QString& text, const QKeySequence& key, auto fn) {
        QAction* a = view->addAction(text, this, fn);
        a->setShortcut(key);
        a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        top->addAction(a);
        return a;
    };
    frameAction("&Play / stop", QKeySequence(Qt::Key_Space), [this] { timeline_->togglePlay(); });
    frameAction("Previous frame", QKeySequence(Qt::Key_Left), [this] { timeline_->step(-1); });
    frameAction("Next frame", QKeySequence(Qt::Key_Right), [this] { timeline_->step(1); });
    view->addAction("First frame", this, [this] { timeline_->goToStart(); });  // Home is the viewer's 1:1
    view->addAction("Last frame", this, [this] { timeline_->goToEnd(); });

    connect(doc_, &Document::dirtyChanged, this, &MainWindow::updateTitle);
    connect(doc_, &Document::pathChanged, this, &MainWindow::updateTitle);
    connect(viewer_, &Viewer::renderFinished, this, [this](bool ok) {
        const QString error = viewer_->canvas()->result().error;
        if (!ok && !error.isEmpty()) statusBar()->showMessage(error, 8000);
        else if (ok) statusBar()->clearMessage();
    });
    updateTitle();
    resize(1400, 900);
    statusBar()->showMessage("Tab in the node graph creates a node; 1 views the selected node.", 8000);
}

MainWindow::~MainWindow() {
    // The views talk to the document to the end (a scene being cleared tells
    // it the selection changed), so they go first; Qt would otherwise delete
    // the document first, as the oldest child.
    delete render_;
    delete takeCentralWidget();
    delete findChild<QDockWidget*>("propertiesDock");
}

void MainWindow::showRenderDialog() {
    if (!render_->rendering()) render_->reset();
    render_->show();
    render_->raise();
    render_->activateWindow();
}

void MainWindow::updateTitle() {
    const QString name = doc_->path().isEmpty() ? "untitled" : QFileInfo(doc_->path()).fileName();
    setWindowTitle(QString("%1%2 - comp").arg(name, doc_->dirty() ? "*" : ""));
}

bool MainWindow::openProject(const QString& path) {
    QString error;
    if (!doc_->load(path, &error)) {
        QMessageBox::critical(this, "Open project", error);
        return false;
    }
    graph_->fitAll();
    return true;
}

bool MainWindow::save() {
    if (doc_->path().isEmpty()) return saveAs();
    QString error;
    if (!doc_->save(doc_->path(), &error)) {
        QMessageBox::critical(this, "Save project", error);
        return false;
    }
    statusBar()->showMessage("Saved " + doc_->path(), 4000);
    return true;
}

bool MainWindow::saveAs() {
    QString path = QFileDialog::getSaveFileName(this, "Save project", doc_->path(), "comp projects (*.comp)");
    if (path.isEmpty()) return false;
    if (QFileInfo(path).suffix().isEmpty()) path += ".comp";
    QString error;
    if (!doc_->save(path, &error)) {
        QMessageBox::critical(this, "Save project", error);
        return false;
    }
    return true;
}

bool MainWindow::maybeSave() {
    if (!doc_->dirty()) return true;
    const auto answer = QMessageBox::question(this, "Unsaved changes", "Save changes to this project first?",
                                              QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (answer == QMessageBox::Save) return save();
    return answer == QMessageBox::Discard;
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (render_->rendering() &&
        QMessageBox::question(this, "Quit", "A render is still running. Stop it and quit?") != QMessageBox::Yes) {
        event->ignore();
        return;
    }
    if (!maybeSave()) {
        event->ignore();
        return;
    }
    render_->close();  // stops the render, after the file being written
    event->accept();
}

}  // namespace compgui

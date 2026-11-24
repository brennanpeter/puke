// Drives the real comp_gui window offscreen (QT_QPA_PLATFORM=offscreen):
// creating and connecting nodes, editing parameters through the properties
// panel, the viewer's pixels, saving and loading.
//
// With COMP_GUI_SCREENSHOT=path.png set, saves a picture of the window at
// the end.

#include <QApplication>
#include <QClipboard>
#include <QGraphicsItem>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QToolButton>
#include <QUndoStack>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>

#include "Document.h"
#include "MainWindow.h"
#include "NodeGraphView.h"
#include "PropertiesPanel.h"
#include "TestUtil.h"
#include "Viewer.h"
#include "comp/engine/Engine.h"
#include "comp/project/Project.h"

using namespace compgui;
namespace fs = std::filesystem;

namespace {

// Runs the event loop until `done` or the timeout.
bool waitFor(const std::function<bool()>& done, int ms = 10000) {
    QElapsedTimer t;
    t.start();
    while (!done()) {
        if (t.elapsed() > ms) return false;
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

// Waits for the viewer to show the result of everything asked so far.
bool waitForRender(Viewer& viewer) {
    int count = 0;
    bool ok = false;
    auto conn = QObject::connect(&viewer, &Viewer::renderFinished, [&](bool good) {
        ++count;
        ok = good;
    });
    viewer.requestRender();  // a fresh request; earlier ones are dropped
    const bool arrived = waitFor([&] { return count > 0; });
    QApplication::processEvents();
    QObject::disconnect(conn);
    return arrived && ok;
}

template <typename T>
T* find(QWidget& root, const QString& name) {
    return root.findChild<T*>(name);
}

// The n-th widget of type T inside the properties row for `param`.
template <typename T>
T* paramWidget(MainWindow& w, const char* param, int n = 0) {
    QWidget* row = find<QWidget>(w.properties(), QString("param_") + param);
    if (!row) return nullptr;
    const auto all = row->findChildren<T*>();
    return n < all.size() ? all[n] : nullptr;
}

float viewerPixel(Viewer& v, int x, int y, int c) { return v.canvas()->result().working->at(x, y, c); }

float plate(int x, int y, int c) { return c == 3 ? 1.0f : 0.004f * x + 0.006f * y + 0.2f * c; }

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    applyTheme();
    const fs::path dir = fs::current_path() / "test_gui_tmp";
    fs::remove_all(dir);
    fs::create_directories(dir);
    test::ExrOptions o;
    o.dataWindow = o.displayWindow = {0, 0, 160, 90};
    test::writeImage((dir / "plate.exr").string(), o, plate);

    MainWindow w;
    w.resize(1400, 900);
    w.show();
    Document& doc = w.document();
    Viewer& viewer = w.viewer();
    QApplication::processEvents();

    try {
        // --- A Read, viewed: the viewer shows exactly what the engine renders.
        comp::Node& read = doc.createNode("Read", {0, 0});
        doc.setParam(read, "file", (dir / "plate.exr").string());
        doc.setViewed(&read);
        CHECK(waitForRender(viewer));
        CHECK(viewer.canvas()->result().displayWindow == (comp::Box{0, 0, 160, 90}));
        CHECK(std::fabs(viewerPixel(viewer, 10, 20, 1) - plate(10, 20, 1)) < 1e-6f);
        CHECK(!viewer.canvas()->result().image.isNull());
        CHECK(w.windowTitle().startsWith("untitled*"));

        // --- A Grade, created with the Read selected: connected below it.
        comp::Node& grade = doc.createNode("Grade", {0, 90}, &read);
        CHECK(grade.input(0) == &read && doc.selected() == &grade);
        doc.setViewed(&grade);
        CHECK(waitForRender(viewer));

        // Edit through the properties panel, as a user would.
        auto* mulR = paramWidget<QDoubleSpinBox>(w, "multiply", 0);
        CHECK(mulR != nullptr);
        if (mulR) mulR->setValue(2.0);
        CHECK(grade.hasParam("multiply"));
        CHECK(waitForRender(viewer));
        CHECK(std::fabs(viewerPixel(viewer, 10, 20, 0) - 2 * plate(10, 20, 0)) < 1e-5f);
        CHECK(std::fabs(viewerPixel(viewer, 10, 20, 1) - plate(10, 20, 1)) < 1e-5f);  // only R

        auto* channels = paramWidget<QComboBox>(w, "channels");
        CHECK(channels && channels->currentText() == "rgb");
        if (channels) {
            channels->setCurrentText("rgba");
            emit channels->activated(channels->currentIndex());
        }
        CHECK(grade.hasParam("channels") && std::get<std::string>(grade.params().at("channels")) == "rgba");

        auto* reset = paramWidget<QToolButton>(w, "multiply");
        CHECK(reset && reset->isEnabled());
        if (reset) reset->click();
        CHECK(!grade.hasParam("multiply"));
        CHECK(waitForRender(viewer));
        CHECK(std::fabs(viewerPixel(viewer, 10, 20, 0) - plate(10, 20, 0)) < 1e-5f);

        // --- Transform pivots at the middle of its input's frame.
        comp::Node& xf = doc.createNode("Transform", {0, 180}, &grade);
        CHECK((std::get<comp::Vec2>(xf.params().at("center")) == comp::Vec2{80, 45}));

        // --- The Tab search creates a node connected to the selection.
        doc.setSelected(&xf);
        w.graphView().setFocus();
        QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
        QApplication::sendEvent(&w.graphView(), &tab);
        QWidget* popup = nullptr;
        CHECK(waitFor([&] { return (popup = w.graphView().findChild<QWidget*>("createNodePopup")) != nullptr; }));
        if (popup) {
            auto* edit = popup->findChild<QLineEdit*>();
            auto* list = popup->findChild<QListWidget*>();
            CHECK(list && list->count() >= 6);
            edit->setText("merg");
            emit edit->returnPressed();
        }
        comp::Node* merge = doc.graph().find("Merge1");
        CHECK(merge && merge->input(0) == &xf);

        // --- Errors show in the viewer instead of a picture.
        comp::Node& lonely = doc.createNode("Grade", {300, 0});
        doc.setViewed(&lonely);
        CHECK(!waitForRender(viewer));
        CHECK(viewer.canvas()->result().error.contains("Grade2: input 0 is not connected"));
        doc.removeNode(lonely);
        CHECK(doc.viewed() == nullptr && !waitForRender(viewer));

        // --- Editing while the viewer renders: only the last edit counts.
        doc.connectNodes(read, *merge, 1);
        doc.setViewed(merge);
        merge->setParam("operation", std::string("plus"));
        for (int i = 0; i < 50; ++i) doc.setParam(grade, "multiply", 1.0 + i);
        CHECK(waitForRender(viewer));
        comp::Engine e;
        const comp::Image expected = e.render(*merge, {}, {0, 0, 160, 90});
        CHECK(std::fabs(viewerPixel(viewer, 30, 40, 2) - expected.at(30, 40, 2)) < 1e-5f);

        // --- An OFX plugin: its parameters come from the plugin.
        comp::Node& gain = doc.createNode("ofx:org.comp.test.Gain", {0, 360}, merge);
        CHECK(gain.name() == "Gain1");
        CHECK(paramWidget<QDoubleSpinBox>(w, "gain") != nullptr);
        CHECK(paramWidget<QComboBox>(w, "mode") != nullptr);
        doc.setViewed(&gain);
        CHECK(waitForRender(viewer));

        // --- Save, then open it again.
        const QString path = QString::fromStdString((dir / "shot.comp").string());
        QString error;
        CHECK(doc.save(path, &error));
        CHECK(!doc.dirty() && w.windowTitle() == "shot.comp - comp");
        doc.newDocument();
        CHECK(doc.graph().nodes().empty() && doc.viewed() == nullptr);
        CHECK(w.openProject(path));
        CHECK(doc.graph().nodes().size() == 5);  // Read, Grade, Transform, Merge, Gain
        CHECK(doc.graph().find("Merge1")->input(1) == doc.graph().find("Read1"));
        CHECK((doc.graph().find("Grade1")->position() == comp::Vec2{0, 90}));
        CHECK(doc.viewed() == doc.graph().find("Gain1"));
        CHECK(waitForRender(viewer));

        // --- Undo and redo: every kind of edit, back to the saved project
        // exactly, then forward again exactly.
        {
            QUndoStack& undo = doc.undoStack();
            CHECK(!doc.dirty() && undo.count() == 0);  // loading starts a fresh history
            const std::string saved = comp::projectToJson(doc.graph());
            comp::Node& g1 = *doc.graph().find("Grade1");
            comp::Node& m1 = *doc.graph().find("Merge1");
            comp::Node& r1 = *doc.graph().find("Read1");

            for (int i = 0; i < 20; ++i) doc.setParam(g1, "mix", 0.05 * i);  // a slider drag...
            CHECK(undo.count() == 1);                                         // ...is one step
            doc.clearParam(g1, "multiply");
            doc.renameNode(g1, "Hero");
            CHECK_THROWS(doc.renameNode(g1, "Read1"), "a node named 'Read1' already exists");
            doc.moveNodes({{&g1, QPointF(300, 90)}, {&r1, QPointF(300, 0)}});
            doc.disconnect(m1, 1);
            CHECK_THROWS(doc.connectNodes(*doc.graph().find("Gain1"), r1, 0), "Read1");  // a cycle, and Read has no inputs
            comp::Node& extra = doc.createNode("ColorSpace", {500, 0}, &m1);
            doc.setParam(extra, "out", std::string("ACEScct"));
            doc.setViewed(&g1);
            doc.removeNodes({&g1, doc.graph().find("Transform1")});  // viewed, mid-chain
            CHECK(doc.graph().nodes().size() == 4 && doc.viewed() == nullptr);
            CHECK(doc.dirty());
            const std::string edited = comp::projectToJson(doc.graph());

            const int steps = undo.count();
            CHECK(steps == 8);
            while (undo.canUndo()) undo.undo();
            CHECK(comp::projectToJson(doc.graph()) == saved);
            CHECK(!doc.dirty() && doc.viewed() == doc.graph().find("Grade1"));  // the deleted, viewed node is back
            CHECK(doc.graph().find("Transform1")->input(0) == doc.graph().find("Grade1"));
            CHECK(waitForRender(viewer));
            while (undo.canRedo()) undo.redo();
            CHECK(comp::projectToJson(doc.graph()) == edited);
            while (undo.canUndo()) undo.undo();
            CHECK(comp::projectToJson(doc.graph()) == saved);

            // A new edit after undoing drops the redo history, as usual.
            doc.setParam(*doc.graph().find("Grade1"), "mix", 0.5);
            CHECK(!undo.canRedo());
            undo.undo();
            CHECK(!doc.dirty());
        }

        // --- Copy and paste, duplicate, rename, backdrops.
        {
            QUndoStack& undo = doc.undoStack();
            NodeGraphView& gv = w.graphView();
            auto find = [&](const char* n) { return doc.graph().find(n); };
            comp::Node& g1 = *find("Grade1");
            comp::Node& x1 = *find("Transform1");
            const std::string saved = comp::projectToJson(doc.graph());

            // Two connected nodes, pasted: renamed, still connected to each
            // other, but not to Read1, which wasn't copied.
            const auto pasted = doc.pasteNodes(doc.copyNodes({&x1, &g1}), {200, 10});
            CHECK(pasted.size() == 2 && pasted[0]->name() == "Grade2" && pasted[1]->name() == "Transform2");
            CHECK(pasted[1]->input(0) == pasted[0] && pasted[0]->input(0) == nullptr);
            CHECK(pasted[0]->params() == g1.params());
            CHECK((pasted[0]->position() == comp::Vec2{200, 100}));
            CHECK(undo.count() == 1);
            undo.undo();
            CHECK(comp::projectToJson(doc.graph()) == saved);
            CHECK_THROWS(doc.pasteNodes("not a project", {}), "pasted nodes");
            CHECK(undo.count() == 1);  // nothing recorded

            // Through the view and the clipboard.
            gv.selectNodes({&g1});
            gv.copySelection();
            CHECK(QGuiApplication::clipboard()->text().contains("\"Grade1\""));
            gv.paste();
            CHECK(find("Grade2") && doc.selected() == find("Grade2"));
            CHECK(gv.selectedNodes() == std::vector<comp::Node*>{find("Grade2")});
            gv.duplicateSelection();
            CHECK(find("Grade3") && doc.selected() == find("Grade3"));
            gv.cutSelection();
            CHECK(!find("Grade3") && QGuiApplication::clipboard()->text().contains("Grade3"));
            gv.paste();  // back, under its own name again
            CHECK(find("Grade3"));

            // Rename through the properties panel's name field.
            gv.selectNodes({&g1});
            QApplication::processEvents();
            auto* name = w.findChild<QLineEdit*>("nodeName");
            CHECK(name && name->text() == "Grade1");
            if (name) {
                name->setText("Hero");
                emit name->editingFinished();
            }
            CHECK(waitFor([&] { return g1.name() == "Hero"; }));
            CHECK(x1.input(0) == &g1);

            // A backdrop around two nodes: sized to hold them.
            doc.removeNodes({find("Grade2"), find("Grade3")});  // the copies, out of the way
            comp::Node& read1 = *find("Read1");
            gv.selectNodes({&read1, &g1});
            CHECK(gv.selectedNodes().size() == 2);
            const int stepsBefore = undo.count();
            gv.createAt("Backdrop", {});
            comp::Node* bd = find("Backdrop1");
            CHECK(bd && undo.count() == stepsBefore + 1);
            if (bd) {
                const comp::Vec2 at = *bd->position(), size = bd->vec2Param("size", {});
                for (comp::Node* n : {&read1, &g1}) {
                    const comp::Vec2 p = *n->position();
                    CHECK(p[0] > at[0] && p[0] < at[0] + size[0] && p[1] > at[1] && p[1] < at[1] + size[1]);
                }

                // Dragging it by its title bar carries the nodes on it, as
                // one undo step.
                gv.resetTransform();
                gv.centerOn(at[0] + size[0] / 2, at[1] + size[1] / 2);
                QApplication::processEvents();
                QWidget* vp = gv.viewport();
                auto mouse = [&](QEvent::Type t, QPointF scene, Qt::MouseButtons held) {
                    const QPoint p = gv.mapFromScene(scene);
                    QMouseEvent e(t, QPointF(p), QPointF(vp->mapToGlobal(p)), Qt::LeftButton, held, Qt::NoModifier);
                    QApplication::sendEvent(vp, &e);
                };
                gv.selectNodes({});
                const QPointF grab(at[0] + 30, at[1] + 10);
                const comp::Vec2 heroBefore = *g1.position();
                const int s0 = undo.count();
                mouse(QEvent::MouseButtonPress, grab, Qt::LeftButton);
                mouse(QEvent::MouseMove, grab + QPointF(20, 10), Qt::LeftButton);
                mouse(QEvent::MouseMove, grab + QPointF(60, 40), Qt::LeftButton);
                mouse(QEvent::MouseButtonRelease, grab + QPointF(60, 40), Qt::NoButton);
                CHECK((*bd->position() == comp::Vec2{at[0] + 60, at[1] + 40}));
                CHECK((*g1.position() == comp::Vec2{heroBefore[0] + 60, heroBefore[1] + 40}));
                CHECK(undo.count() == s0 + 1);

                // Its bottom-right corner resizes it.
                const QPointF corner(at[0] + 60 + size[0] - 4, at[1] + 40 + size[1] - 4);
                gv.centerOn(corner);
                QApplication::processEvents();
                mouse(QEvent::MouseButtonPress, corner, Qt::LeftButton);
                mouse(QEvent::MouseMove, corner + QPointF(50, 30), Qt::LeftButton);
                mouse(QEvent::MouseButtonRelease, corner + QPointF(50, 30), Qt::NoButton);
                CHECK((bd->vec2Param("size", {}) == comp::Vec2{size[0] + 50, size[1] + 30}));
                CHECK((*g1.position() == comp::Vec2{heroBefore[0] + 60, heroBefore[1] + 40}));  // nodes stay
                CHECK(undo.count() == s0 + 2);
            }

            while (undo.canUndo()) undo.undo();
            CHECK(comp::projectToJson(doc.graph()) == saved);
        }

        if (const char* shot = std::getenv("COMP_GUI_SCREENSHOT")) {
            doc.setParam(*doc.graph().find("Grade1"), "multiply", comp::Color{1.4, 1.1, 0.8, 1});
            doc.setParam(*doc.graph().find("Transform1"), "rotate", 12.0);
            doc.setParam(*doc.graph().find("Transform1"), "scale", comp::Vec2{0.6, 0.6});
            doc.setParam(*doc.graph().find("Merge1"), "operation", std::string("over"));
            doc.setSelected(doc.graph().find("Grade1"));
            w.graphView().selectNodes({doc.graph().find("Read1"), doc.graph().find("Grade1"), doc.graph().find("Transform1")});
            w.graphView().createAt("Backdrop", {});
            doc.setParam(*doc.graph().find("Backdrop1"), "label", std::string("Plate prep"));
            doc.setSelected(doc.graph().find("Grade1"));
            doc.setViewed(doc.graph().find("Merge1"));
            CHECK(waitForRender(viewer));
            w.graphView().fitAll();
            QApplication::processEvents();
            w.grab().save(QString::fromUtf8(shot));
        }
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "unexpected exception: %s\n", ex.what());
        ++test::failures;
    }
    if (test::failures == 0) fs::remove_all(dir);
    return test::finish("GUI");
}

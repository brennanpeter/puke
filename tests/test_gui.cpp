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
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QLabel>
#include <QSpinBox>
#include <QTabWidget>
#include <QToolButton>
#include <QUndoStack>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>

#include "Document.h"
#include "MainWindow.h"
#include "NodeGraphView.h"
#include "CurveEditor.h"
#include "ProjectSettingsDialog.h"
#include "PropertiesPanel.h"
#include "RenderDialog.h"
#include "RotoTool.h"
#include "TestUtil.h"
#include "Timeline.h"
#include "Viewer.h"
#include "comp/core/Path.h"
#include "comp/engine/Engine.h"
#include "comp/nodes/Registry.h"
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

// Runs the event loop for `ms`.
void runEventsFor(int ms) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) QApplication::processEvents(QEventLoop::AllEvents, 10);
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
    for (int f = 1001; f <= 1003; ++f)  // a short sequence: each frame a different flat value
        test::writeImage((dir / ("seq." + std::to_string(f) + ".exr")).string(), o,
                         [f](int, int, int c) { return c == 3 ? 1.0f : 0.1f * (f - 1000); });

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

        auto* reset = find<QToolButton>(w, "reset_multiply");
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

        // --- Frame range, the timeline, playback and the viewer's frame cache.
        {
            QUndoStack& undo = doc.undoStack();
            Timeline& tl = w.timeline();
            const int steps0 = undo.index();
            comp::Node& seq = doc.createNode("Read", {400, 0});
            doc.setParam(seq, "file", (dir / "seq.####.exr").string());
            doc.setViewed(&seq);

            // The range, through the timeline's fields: undoable project edits.
            auto* first = find<QSpinBox>(tl, "timelineFirst");
            auto* last = find<QSpinBox>(tl, "timelineLast");
            CHECK(first && last);
            last->setValue(1003);
            first->setValue(1001);
            CHECK((doc.settings() == comp::ProjectSettings{1001, 1003, 24}));
            CHECK(undo.index() == steps0 + 4);
            first->setValue(1005);  // past the last: the last follows
            CHECK((doc.settings() == comp::ProjectSettings{1005, 1005, 24}));
            undo.undo();
            CHECK((doc.settings() == comp::ProjectSettings{1001, 1003, 24}) && first->value() == 1001);

            tl.goToStart();
            CHECK(doc.frame() == 1001 && find<QSpinBox>(tl, "timelineFrame")->value() == 1001);
            CHECK(waitFor([&] { return !viewer.busy(); }));
            CHECK(std::fabs(viewerPixel(viewer, 5, 5, 0) - 0.1f) < 1e-6f);

            // Playback shows every frame in order, wrapping, and leaves them
            // all cached.
            std::vector<int> shown;
            auto conn = QObject::connect(&doc, &Document::frameChanged, [&](int f) { shown.push_back(f); });
            tl.play();
            CHECK(tl.playing());
            CHECK(waitFor([&] { return shown.size() >= 5; }));
            tl.stop();
            QObject::disconnect(conn);
            CHECK(shown.size() >= 5 && shown[0] == 1002 && shown[1] == 1003 && shown[2] == 1001 && shown[3] == 1002);
            CHECK(waitFor([&] { return !viewer.busy(); }));
            CHECK((viewer.cachedFrames() == std::vector<int>{1001, 1002, 1003}));

            // A cached frame shows at once, with its own pixels.
            doc.setFrame(1003);
            CHECK(!viewer.busy());
            CHECK(std::fabs(viewerPixel(viewer, 5, 5, 0) - 0.3f) < 1e-6f);
            tl.step(1);  // wraps
            CHECK(doc.frame() == 1001 && !viewer.busy());
            tl.step(-1);
            CHECK(doc.frame() == 1003);

            // Any edit empties the cache.
            comp::Node& g = doc.createNode("Grade", {400, 90}, &seq);
            doc.setViewed(&g);
            CHECK(viewer.cachedFrames().empty());
            CHECK(waitFor([&] { return !viewer.busy(); }));
            CHECK((viewer.cachedFrames() == std::vector<int>{1003}));
            doc.setParam(g, "multiply", 2.0);
            CHECK(viewer.cachedFrames().empty());
            CHECK(waitFor([&] { return !viewer.busy(); }));
            CHECK(std::fabs(viewerPixel(viewer, 5, 5, 0) - 0.6f) < 1e-6f);

            // A tiny cache limit keeps only the most recent frames.
            viewer.setCacheLimit(2 * 160 * 90 * (4 + 4 * sizeof(float)) + 1000);  // about two frames
            for (int f : {1001, 1002, 1003}) {
                doc.setFrame(f);
                CHECK(waitFor([&] { return !viewer.busy(); }));
            }
            CHECK((viewer.cachedFrames() == std::vector<int>{1002, 1003}));
            viewer.setCacheLimit(std::size_t(1) << 30);

            // Saved and loaded with the project; a loaded project starts at
            // its first frame.
            const QString path = QString::fromStdString((dir / "ranged.comp").string());
            QString error;
            CHECK(doc.save(path, &error));
            doc.newDocument();
            CHECK((doc.settings() == comp::ProjectSettings{}) && doc.frame() == 1);
            CHECK(first->value() == 1 && last->value() == 1);
            CHECK(w.openProject(path));
            CHECK((doc.settings() == comp::ProjectSettings{1001, 1003, 24}) && doc.frame() == 1001);
            CHECK(last->value() == 1003);
        }

        // --- Rendering Write nodes from the GUI.
        {
            RenderDialog& rd = w.renderDialog();
            int finishedCount = 0, lastFailures = -1;
            bool lastCancelled = false;
            QObject::connect(&rd, &RenderDialog::renderFinished, [&](int failures, bool cancelled) {
                ++finishedCount;
                lastFailures = failures;
                lastCancelled = cancelled;
            });
            auto renderAndWait = [&] {
                const int before = finishedCount;
                rd.startRender();
                return waitFor([&] { return finishedCount > before; }, 60000);
            };
            auto* log = find<QPlainTextEdit>(rd, "renderLog");
            auto* writes = find<QListWidget>(rd, "renderWrites");

            // Read2 (the sequence) -> Grade2 (x2) -> Write. The dialog offers
            // the selected Write and the project's range.
            comp::Node& g2 = *doc.graph().find("Grade2");
            comp::Node& write = doc.createNode("Write", {400, 180}, &g2);
            doc.setParam(write, "file", (dir / "out/r.####.exr").string());
            doc.setParam(write, "datatype", std::string("float"));
            comp::Node& other = doc.createNode("Write", {600, 180}, &g2);
            doc.setParam(other, "file", (dir / "out/other.####.exr").string());
            doc.setSelected(&write);
            w.showRenderDialog();
            CHECK(rd.isVisible() && writes && writes->count() == 2);
            CHECK(writes->item(0)->checkState() == Qt::Checked && writes->item(1)->checkState() == Qt::Unchecked);
            CHECK(find<QSpinBox>(rd, "renderFirst")->value() == 1001 && find<QSpinBox>(rd, "renderLast")->value() == 1003);

            // Edits made once it has started don't change what's written.
            const int before = finishedCount;
            rd.startRender();
            CHECK(rd.rendering() && !find<QPushButton>(rd, "renderStart")->isEnabled());
            doc.setParam(g2, "multiply", 10.0);
            doc.setParam(write, "file", (dir / "out/moved.####.exr").string());
            CHECK(waitFor([&] { return finishedCount > before; }, 60000));
            CHECK(lastFailures == 0 && !lastCancelled && !rd.rendering());
            CHECK(find<QProgressBar>(rd, "renderProgress")->value() == 3);
            CHECK(log && log->toPlainText().contains("frame 1002 Write1 -> "));
            CHECK(find<QLabel>(rd, "renderStatus")->text() == "Finished: 3 files written.");
            comp::Engine e;
            for (int f = 1001; f <= 1003; ++f) {
                comp::Graph check;
                comp::Node& r = check.add(comp::createNode("Read", "R"));
                r.setParam("file", (dir / ("out/r." + std::to_string(f) + ".exr")).string());
                CHECK(std::fabs(e.render(r, {}, {5, 5, 6, 6}).pixel(5, 5)[0] - 0.2f * (f - 1000)) < 1e-6f);
            }
            CHECK(!fs::exists(dir / "out/moved.1001.exr") && !fs::exists(dir / "out/other.1001.exr"));

            // Failures are reported and don't stop the rest: frame 1004
            // doesn't exist.
            find<QSpinBox>(rd, "renderLast")->setValue(1004);
            CHECK(renderAndWait());
            CHECK(lastFailures == 1 && log->toPlainText().contains("frame 1004 Write1 failed: "));
            CHECK(fs::exists(dir / "out/moved.1003.exr"));  // a new render sees the edits

            // Cancelling stops before the next file and leaves no partial one.
            doc.setParam(write, "file", (dir / "many/m.####.exr").string());
            doc.setParam(*doc.graph().find("Read2"), "file", (dir / "plate.exr").string());  // a still: any frame
            rd.reset();
            find<QSpinBox>(rd, "renderFirst")->setValue(1);
            find<QSpinBox>(rd, "renderLast")->setValue(2000);
            QObject::connect(find<QProgressBar>(rd, "renderProgress"), &QProgressBar::valueChanged, &rd,
                             [&](int v) {
                                 if (v == 1) rd.cancelRender();
                             });
            CHECK(renderAndWait());
            CHECK(lastCancelled);
            int written = 0;
            for (const auto& entry : fs::directory_iterator(dir / "many")) {
                CHECK(entry.path().extension() == ".exr");  // no .partial files
                ++written;
            }
            CHECK(written >= 1 && written < 2000);
            CHECK(find<QLabel>(rd, "renderStatus")->text().startsWith("Cancelled after"));

            // Nothing to render: said in the log, nothing starts.
            for (int i = 0; i < writes->count(); ++i) writes->item(i)->setCheckState(Qt::Unchecked);
            rd.startRender();
            CHECK(!rd.rendering() && log->toPlainText().contains("no Write nodes to render"));
            rd.close();
        }

        // --- The viewer renders only what's on screen.
        {
            test::ExrOptions big;
            big.dataWindow = big.displayWindow = {0, 0, 1600, 900};
            test::writeImage((dir / "big.exr").string(), big, plate);
            comp::Node& r = doc.createNode("Read", {800, 0});
            doc.setParam(r, "file", (dir / "big.exr").string());
            ViewerCanvas* canvas = viewer.canvas();
            doc.setViewed(&r);
            CHECK(waitFor([&] { return !viewer.busy(); }));
            const comp::Box full = canvas->result().full;
            CHECK((full == comp::Box{0, 0, 1600, 900}));
            canvas->fit();  // the whole frame on screen: all of it rendered
            CHECK(waitFor([&] { return !viewer.busy() && canvas->result().region == full; }));

            // Zooming in on a picture that's all there renders nothing new.
            int renders = 0;
            auto conn = QObject::connect(&viewer, &Viewer::renderFinished, [&](bool) { ++renders; });
            canvas->oneToOne();
            canvas->centerOn({800, 450});
            runEventsFor(100);
            CHECK(renders == 0);
            // But the next render (an edit, another frame) does only what's
            // on screen, plus a margin.
            viewer.requestRender();
            CHECK(waitFor([&] { return renders > 0 && !viewer.busy(); }));
            auto visible = canvas->visibleRegion();
            CHECK(visible && visible->width() < 1600);
            const comp::Box region = canvas->result().region;
            CHECK(region.area() < full.area() && visible->padded(200).intersect(region) == region);
            CHECK(region.intersect(*visible) == visible->intersect(full));  // all of what's on screen
            CHECK(std::fabs(viewerPixel(viewer, 800, 450, 1) - plate(800, 450, 1)) < 1e-6f);
            CHECK(canvas->visibleRegion() == visible);  // and the view stayed where it was

            // A small pan stays inside the margin: no new render.
            const int before = renders;
            canvas->centerOn({810, 452});
            QApplication::processEvents();
            runEventsFor(100);
            CHECK(renders == before);
            // A big one renders what's newly on screen.
            canvas->centerOn({100, 100});
            CHECK(waitFor([&] { return renders > before && !viewer.busy(); }));
            visible = canvas->visibleRegion();
            CHECK(canvas->result().region.intersect(*visible) == visible->intersect(full));
            CHECK(std::fabs(viewerPixel(viewer, 50, 60, 0) - plate(50, 60, 0)) < 1e-6f);

            // Cached frames that don't cover the screen are rendered again;
            // fitting brings the whole frame back.
            canvas->fit();
            CHECK(waitFor([&] { return !viewer.busy() && canvas->result().region == full; }));
            QObject::disconnect(conn);
            doc.removeNode(r);
        }

        // --- Project format: the settings dialog, and generators following it.
        {
            QUndoStack& undo = doc.undoStack();
            comp::Node& c = doc.createNode("Constant", {1000, 0});
            doc.setParam(c, "color", comp::Color{0.5, 0.25, 0.125, 1});
            doc.setViewed(&c);
            CHECK(waitFor([&] { return !viewer.busy(); }));
            CHECK((viewer.canvas()->result().full == comp::Box{0, 0, 1920, 1080}));

            const comp::ProjectSettings before = doc.settings();
            ProjectSettingsDialog dlg(doc);
            auto* preset = find<QComboBox>(dlg, "settingsPreset");
            auto* width = find<QSpinBox>(dlg, "settingsWidth");
            auto* height = find<QSpinBox>(dlg, "settingsHeight");
            CHECK(preset && width && height && preset->currentText().startsWith("HD"));
            preset->setCurrentIndex(1);  // a preset fills the size
            CHECK(width->value() == 3840 && height->value() == 2160);
            width->setValue(2048);
            height->setValue(858);  // a size that is a preset selects it
            CHECK(preset->currentText().startsWith("2K Scope"));
            width->setValue(200);
            height->setValue(100);
            CHECK(preset->currentText() == "Custom");

            const int steps = undo.index();
            CHECK(dlg.apply());
            CHECK((doc.settings().format == comp::Format{200, 100}) && undo.index() == steps + 1);
            CHECK(doc.settings().firstFrame == before.firstFrame && doc.settings().fps == before.fps);
            // The viewer renders again in the new format.
            CHECK(waitFor([&] { return !viewer.busy() && viewer.canvas()->result().full == comp::Box{0, 0, 200, 100}; }));
            CHECK(std::fabs(viewerPixel(viewer, 199, 99, 1) - 0.25f) < 1e-6f);
            // Nodes that start from their input's frame see the project format.
            comp::Node& crop = doc.createNode("Crop", {1000, 90}, &c);
            CHECK((crop.vec2Param("box_min", {}) == comp::Vec2{0, 0}) && (crop.vec2Param("box_max", {}) == comp::Vec2{200, 100}));
            comp::Node& xf2 = doc.createNode("Transform", {1100, 90}, &c);
            CHECK((xf2.vec2Param("center", {}) == comp::Vec2{100, 50}));
            undo.undo();
            undo.undo();
            undo.undo();
            CHECK(doc.settings() == before);
            CHECK(waitFor([&] { return !viewer.busy() && viewer.canvas()->result().full == comp::Box{0, 0, 1920, 1080}; }));
            // Range and fps alone don't need a render.
            comp::ProjectSettings s = doc.settings();
            s.fps = 30;
            doc.setSettings(s);
            CHECK(!viewer.busy() && !viewer.cachedFrames().empty());
            undo.undo();
            doc.removeNode(c);
        }

        // --- Drawing and editing a Roto shape in the viewer.
        {
            QUndoStack& undo = doc.undoStack();
            ViewerCanvas* canvas = viewer.canvas();
            RotoTool* tool = viewer.rotoTool();
            auto* draw = find<QToolButton>(viewer, "rotoDraw");
            comp::Node& roto = doc.createNode("Roto", {1200, 0});
            doc.setSelected(nullptr);
            CHECK(draw && draw->isHidden() && !tool->node());
            doc.setSelected(&roto);
            doc.setViewed(&roto);
            CHECK(!draw->isHidden() && tool->node() == &roto);
            CHECK(waitFor([&] { return !viewer.busy(); }));
            canvas->oneToOne();
            canvas->centerOn({50, 40});
            QApplication::processEvents();

            // Mouse events at image positions.
            auto mouse = [&](QEvent::Type t, QPointF image, Qt::MouseButtons held) {
                const QPointF w = canvas->toWidget(image);
                QMouseEvent e(t, w, canvas->mapToGlobal(w), Qt::LeftButton, held, Qt::NoModifier);
                QApplication::sendEvent(canvas, &e);
            };
            auto click = [&](QPointF p) {
                mouse(QEvent::MouseButtonPress, p, Qt::LeftButton);
                mouse(QEvent::MouseButtonRelease, p, Qt::NoButton);
            };
            auto dragTo = [&](QPointF from, QPointF to) {
                mouse(QEvent::MouseButtonPress, from, Qt::LeftButton);
                mouse(QEvent::MouseMove, (from + to) / 2, Qt::LeftButton);
                mouse(QEvent::MouseMove, to, Qt::LeftButton);
                mouse(QEvent::MouseButtonRelease, to, Qt::NoButton);
            };
            auto shape = [&] { return roto.param<std::string>("shape", ""); };

            // Draw: three clicks and a click-drag (a smooth point), then
            // click the first point to finish. One undo step per point.
            draw->click();
            CHECK(tool->drawing());
            const int steps = undo.index();
            click({20, 20});
            click({80, 20});
            dragTo({80, 60}, {90, 60});
            click({20, 60});
            CHECK(undo.index() == steps + 4);
            click({20, 20});  // finishes; changes nothing
            CHECK(undo.index() == steps + 4);
            CHECK(shape() == "M 20 20 L 80 20 C 80 20 70 60 80 60 C 90 60 20 60 20 60 Z");
            // The viewer shows the matte.
            CHECK(waitFor([&] { return !viewer.busy(); }));
            CHECK(viewerPixel(viewer, 50, 40, 3) == 1.0f && viewerPixel(viewer, 50, 10, 3) == 0.0f);
            draw->click();
            CHECK(!tool->drawing());

            // Drag a point; then the smooth point's handle, which turns the
            // other handle too (keeping its length).
            dragTo({80, 20}, {85, 25});
            CHECK(shape().starts_with("M 20 20 L 85 25 C"));
            click({80, 60});
            CHECK((tool->selection() == std::pair{0, 2}));
            dragTo({90, 60}, {80, 70});
            const comp::Path p = comp::Path::parse(shape());
            const auto& k = p.curves()[0][2];
            CHECK((k.out == comp::Path::Point{80, 70}) && (k.in == comp::Path::Point{80, 50}));
            CHECK(undo.index() == steps + 6);

            // Double-click: smooth <-> corner.
            mouse(QEvent::MouseButtonDblClick, {20, 20}, Qt::LeftButton);
            mouse(QEvent::MouseButtonRelease, {20, 20}, Qt::NoButton);
            CHECK(comp::Path::parse(shape()).curves()[0][0].out != comp::Path::Point({20, 20}));
            CHECK(undo.index() == steps + 7);

            // Delete removes the selected point, and the viewer claims the key
            // from the window's Delete-node shortcut.
            QKeyEvent over(QEvent::ShortcutOverride, Qt::Key_Delete, Qt::NoModifier);
            QApplication::sendEvent(canvas, &over);
            CHECK(over.isAccepted());
            QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
            QApplication::sendEvent(canvas, &del);
            CHECK(comp::Path::parse(shape()).curves()[0].size() == 3 && doc.graph().find("Roto1"));
            CHECK(!tool->selection());
            QKeyEvent over2(QEvent::ShortcutOverride, Qt::Key_Delete, Qt::NoModifier);
            QApplication::sendEvent(canvas, &over2);
            CHECK(!over2.isAccepted());  // nothing selected: Delete is the window's again

            // Undo goes back point by point.
            undo.undo();
            CHECK(comp::Path::parse(shape()).curves()[0].size() == 4);
            while (undo.index() > steps) undo.undo();
            CHECK(shape().empty());

            // A second shape in the same node, finished with Enter.
            draw->click();
            for (QPointF q : {QPointF(30, 30), QPointF(40, 30), QPointF(40, 40)}) click(q);
            QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(canvas, &enter);
            click({60, 60});  // a new curve
            click({70, 60});
            click({70, 70});
            CHECK(comp::Path::parse(shape()).curves().size() == 2);
            doc.setSelected(nullptr);  // leaving the node ends drawing
            CHECK(!tool->drawing() && draw->isHidden());
            doc.removeNode(roto);
        }

        // --- Animation: keys from the properties panel, values per frame.
        {
            QUndoStack& undo = doc.undoStack();
            comp::Node& g1 = *doc.graph().find("Grade1");
            doc.setSelected(&g1);
            doc.setViewed(&g1);
            doc.setFrame(1001);
            const std::string before = comp::projectToJson(doc.graph());
            doc.setParam(g1, "multiply", 2.0);  // something for mix to blend
            QApplication::processEvents();
            auto* key = find<QToolButton>(w.properties(), "key_mix");
            auto* mix = paramWidget<QDoubleSpinBox>(w, "mix");
            CHECK(key && mix && key->text() == QString::fromUtf8("◇"));
            const int steps = undo.index();

            // The key button animates at this frame, with the value shown.
            mix->setValue(0.2);
            key->click();
            CHECK(g1.animated("mix") && g1.curve("mix")->keys().size() == 1 && key->text() == QString::fromUtf8("◆"));
            CHECK(std::get<double>(g1.curve("mix")->keys()[0].value) == 0.2);
            // At another frame, editing the value sets a key there.
            doc.setFrame(1003);
            CHECK(key->text() == QString::fromUtf8("◇"));
            mix->setValue(1.0);
            CHECK(g1.curve("mix")->keys().size() == 2 && undo.index() == steps + 3);
            // Between keys the panel shows the curve's value; linear makes it exact.
            doc.setFrame(1001);
            doc.setInterp(g1, "mix", comp::Curve::Interp::Linear);
            doc.setFrame(1002);
            CHECK(std::fabs(mix->value() - 0.6) < 1e-9);
            // The timeline marks the keys.
            auto* bar = w.findChild<TimeBar*>("timeBar");
            CHECK(bar && (bar->keyFrames() == std::vector<double>{1001, 1003}));

            // The viewer renders each frame's value (mix blends Grade1's x2 on
            // R back towards the input).
            for (int f : {1001, 1002, 1003}) {
                doc.setFrame(f);
                CHECK(waitFor([&] { return !viewer.busy(); }));
                const double m = 0.2 + 0.4 * (f - 1001);
                CHECK(std::fabs(viewerPixel(viewer, 10, 20, 0) - plate(10, 20, 0) * (1 + m)) < 1e-5f);
            }

            // Removing keys with the button; the last one leaves its value.
            doc.setFrame(1003);
            key->click();
            CHECK(g1.curve("mix")->keys().size() == 1);
            doc.setFrame(1001);
            key->click();
            CHECK(!g1.animated("mix") && std::get<double>(g1.params().at("mix")) == 0.2);
            while (comp::projectToJson(doc.graph()) != before && undo.canUndo()) undo.undo();
            CHECK(comp::projectToJson(doc.graph()) == before && undo.index() >= steps - 1);

            // An animated Roto shape: dragging a point at another frame keys
            // the shape there, and frames between move the point part way.
            comp::Node& roto = doc.createNode("Roto", {1300, 0});
            doc.setParam(roto, "shape", std::string("M 20 20 L 60 20 L 60 60 L 20 60 Z"));
            doc.setSelected(&roto);
            doc.setViewed(&roto);
            QApplication::processEvents();
            find<QToolButton>(w.properties(), "key_shape")->click();
            CHECK(roto.animated("shape"));
            doc.setFrame(1003);
            RotoTool* tool = viewer.rotoTool();
            CHECK(tool->press({60, 60}, 1));
            tool->move({70, 80});
            tool->release({70, 80});
            CHECK(roto.curve("shape")->keys().size() == 2);
            doc.setFrame(1002);
            const comp::Path mid = comp::Path::parse(std::get<std::string>(*roto.valueAt("shape", 1002)));
            const auto p2 = mid.curves()[0][2].point;
            CHECK(p2[0] > 60 && p2[0] < 70 && p2[1] > 60 && p2[1] < 80);
            // The tool edits what's on screen at this frame.
            CHECK(tool->press({p2[0], p2[1]}, 0.5));
            tool->release({p2[0], p2[1]});
            doc.removeNode(roto);
        }

        // --- The curve editor.
        {
            QUndoStack& undo = doc.undoStack();
            comp::Node& g1 = *doc.graph().find("Grade1");
            const std::string before = comp::projectToJson(doc.graph());
            doc.setSelected(&g1);
            doc.setFrame(1001);
            doc.setKey(g1, "mix", 0.0);
            doc.setFrame(1003);
            doc.setKey(g1, "mix", 1.0);
            doc.setKey(g1, "gain", comp::Color{1, 1, 1, 1});
            w.bottomTabs().setCurrentWidget(&w.curveEditor());
            QApplication::processEvents();
            CurveEditor& ce = w.curveEditor();
            CurveView* view = ce.view();
            // A row per component: gain.r .. gain.a, then mix.
            CHECK(ce.list()->count() == 5 && ce.list()->item(0)->text() == "gain.r" && ce.list()->item(4)->text() == "mix");
            CHECK(view->curves().size() == 5);
            // Unchecking hides a curve.
            for (int i = 0; i < 4; ++i) ce.list()->item(i)->setCheckState(Qt::Unchecked);
            CHECK((view->curves() == std::vector<CurveRef>{{"mix", 0}}));
            view->frameAll();

            auto mouse = [&](QEvent::Type t, QPointF at, Qt::MouseButtons held, Qt::KeyboardModifiers mods = {}) {
                QMouseEvent e(t, at, view->mapToGlobal(at), Qt::LeftButton, held, mods);
                QApplication::sendEvent(view, &e);
            };
            auto drag = [&](QPointF from, QPointF to) {
                mouse(QEvent::MouseButtonPress, from, Qt::LeftButton);
                mouse(QEvent::MouseMove, (from + to) / 2, Qt::LeftButton);
                mouse(QEvent::MouseMove, to, Qt::LeftButton);
                mouse(QEvent::MouseButtonRelease, to, Qt::NoButton);
            };
            const int steps = undo.index();

            // Click a key to select it; drag it later in time and up.
            const QPointF k1 = view->toWidget(1003, 1.0);
            mouse(QEvent::MouseButtonPress, k1, Qt::LeftButton);
            mouse(QEvent::MouseButtonRelease, k1, Qt::NoButton);
            CHECK(view->selection().size() == 1 && view->selection()[0].frame == 1003);
            CHECK(undo.index() == steps);  // selecting isn't an edit
            const QPointF to = view->toWidget(1005, 1.5);
            drag(k1, to);
            const comp::Curve* c = g1.curve("mix");
            CHECK(c->keys().size() == 2 && c->keys()[1].frame == 1005);  // whole frames
            CHECK(std::fabs(std::get<double>(c->keys()[1].value) - 1.5) < 0.05);
            CHECK(undo.index() == steps + 1 && view->selection()[0].frame == 1005);

            // Its tangent handle sets a slope; flat and automatic from the menu's actions.
            const QPointF handle = view->toWidget(1005, std::get<double>(c->keys()[1].value)) + QPointF(40, 0);
            drag(handle, handle + QPointF(0, -40));  // up: a positive slope
            CHECK(!g1.curve("mix")->keys()[1].slope.empty() && g1.curve("mix")->keys()[1].slope[0] > 0);
            view->setTangents(true);
            CHECK((g1.curve("mix")->keys()[1].slope == std::vector<double>{0}));
            view->setTangents(false);
            CHECK(g1.curve("mix")->keys()[1].slope.empty());
            view->setInterp(comp::Curve::Interp::Constant);
            CHECK(g1.curve("mix")->keys()[1].interp == comp::Curve::Interp::Constant);
            CHECK(undo.index() == steps + 5);

            // Shift-click adds the other key; dragging moves both.
            mouse(QEvent::MouseButtonPress, view->toWidget(1001, 0.0), Qt::LeftButton, Qt::ShiftModifier);
            mouse(QEvent::MouseButtonRelease, view->toWidget(1001, 0.0), Qt::NoButton, Qt::ShiftModifier);
            CHECK(view->selection().size() == 2);
            drag(view->toWidget(1001, 0.0), view->toWidget(1000, 0.0));
            CHECK(g1.curve("mix")->keys()[0].frame == 1000 && g1.curve("mix")->keys()[1].frame == 1004);

            // Double-click on the curve adds a key; Delete removes the selected
            // ones, taking the key from the window's delete-node shortcut.
            const double mid = std::get<double>(g1.curve("mix")->valueAt(1002));
            mouse(QEvent::MouseButtonDblClick, view->toWidget(1002, mid), Qt::LeftButton);
            mouse(QEvent::MouseButtonRelease, view->toWidget(1002, mid), Qt::NoButton);
            CHECK(g1.curve("mix")->keys().size() == 3 && view->selection().size() == 1);
            QKeyEvent over(QEvent::ShortcutOverride, Qt::Key_Delete, Qt::NoModifier);
            QApplication::sendEvent(view, &over);
            CHECK(over.isAccepted());
            QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
            QApplication::sendEvent(view, &del);
            CHECK(g1.curve("mix")->keys().size() == 2 && view->selection().empty() && doc.graph().find("Grade1"));

            // Undo takes it all back, step by step.
            while (comp::projectToJson(doc.graph()) != before && undo.canUndo()) undo.undo();
            CHECK(comp::projectToJson(doc.graph()) == before);
            QApplication::processEvents();
            CHECK(ce.list()->count() == 0 && view->curves().empty());
            w.bottomTabs().setCurrentIndex(0);
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
            // A Roto over the result, selected, so its shape shows in the viewer.
            comp::Node& roto = doc.createNode("Roto", {0, 400}, doc.graph().find("Merge1"));
            doc.setParam(roto, "shape", std::string("M 30 20 C 70 5 120 20 125 45 L 95 80 C 60 85 20 70 30 20 Z"));
            doc.setParam(roto, "feather", 6.0);
            doc.setParam(roto, "output", std::string("rgba"));
            doc.setParam(roto, "color", comp::Color{0.9, 0.5, 0.2, 0.6});
            doc.setSelected(&roto);
            doc.setViewed(&roto);
            CHECK(waitForRender(viewer));
            // Animated: keys on the timeline and filled key buttons.
            doc.setFrame(1001);
            doc.setKey(roto, "opacity", 1.0);
            doc.setFrame(1003);
            doc.setKey(roto, "opacity", 0.5);
            doc.setFrame(1002);
            doc.setKey(roto, "feather", 6.0);
            CHECK(waitForRender(viewer));
            viewer.rotoTool()->press({125, 45}, 2);  // select a point, to show its handles
            viewer.rotoTool()->release({125, 45});
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

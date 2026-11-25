#pragma once

#include <QImage>
#include <QWidget>

#include <condition_variable>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "comp/core/Image.h"
#include "comp/engine/Snapshot.h"

class QComboBox;
class QToolButton;
class QTimer;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;

namespace compgui {

class RotoTool;

class Document;

// What the viewer asks for, and gets back.
struct ViewRequest {
    std::shared_ptr<comp::Snapshot> snapshot;
    double frame = 1;
    comp::Format format;  // the project's
    std::string display, view;
    double exposure = 0;  // stops
    int channel = 0;      // 0 RGB, 1 R, 2 G, 3 B, 4 A
    // The part of the image on screen (pixel coordinates), if known: only
    // that (plus a margin) is rendered.
    std::optional<comp::Box> visible;
    quint64 serial = 0;
};

struct ViewResult {
    quint64 serial = 0;
    double frame = 1;
    QImage image;                             // display-ready, covering `region`
    std::shared_ptr<const comp::Image> working;  // working-space pixels, for the readout
    comp::Box region;      // what was rendered
    comp::Box full;        // what would be rendered with all of it on screen
    comp::Box displayWindow;
    QString error;
    double milliseconds = 0;
};

// Renders on its own thread with its own Engine (tile cache on), so the GUI
// never waits. Only the newest request matters: older pending ones are
// dropped. Results arrive on the GUI thread via `deliver`.
class RenderWorker {
public:
    explicit RenderWorker(std::function<void(ViewResult)> deliver);
    ~RenderWorker();
    void request(ViewRequest r);

private:
    void run();

    std::function<void(ViewResult)> deliver_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<ViewRequest> pending_;
    bool stopping_ = false;
    std::thread thread_;
};

// Shows the latest result: pan, zoom, display window outline, pixel readout.
// Only the part on screen is rendered (with a margin), so a big plate
// viewed zoomed in costs what's visible; zooming and panning render what's
// newly uncovered.
class ViewerCanvas : public QWidget {
    Q_OBJECT
public:
    explicit ViewerCanvas(QWidget* parent = nullptr);
    void setResult(const ViewResult& r);
    void setMessage(const QString& message);  // e.g. "nothing viewed", an error
    void fit();
    void oneToOne();
    // Puts image pixel `p` in the middle of the canvas, keeping the zoom.
    void centerOn(QPointF p);
    const ViewResult& result() const { return result_; }
    // The image pixels on screen, once the image has been placed (after
    // the first fit); before that, nothing is known.
    std::optional<comp::Box> visibleRegion() const;
    // Image pixel coordinates -> widget coordinates, and back.
    QPointF toWidget(QPointF image) const { return image * zoom_ + offset_; }
    QPointF toImage(QPointF widget) const;
    double zoom() const { return zoom_; }
    // Takes mouse and key events first, and paints over the picture.
    void setTool(RotoTool* tool);

signals:
    void viewChanged();  // zoom, pan or size changed

protected:
    void paintEvent(QPaintEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    bool event(QEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    ViewResult result_;
    RotoTool* tool_ = nullptr;
    bool toolDragging_ = false;
    QString message_;
    double zoom_ = 1;
    QPointF offset_;  // widget position of pixel (0, 0)
    bool fitted_ = false;
    bool placed_ = false;
    bool panning_ = false;
    QPoint lastMouse_;
    QString readout_;
};

// The viewer panel: toolbar (display, view, exposure, channels) and canvas.
// Shows the document's current frame of its viewed node, re-rendering
// whenever the document says something changed.
//
// Frames it has shown are kept (up to a memory limit), so going back to
// one, or playing a range a second time, is instant. Any edit or viewer
// setting change empties this cache; changing frames doesn't.
class Viewer : public QWidget {
    Q_OBJECT
public:
    Viewer(Document& doc, QWidget* parent = nullptr);
    ~Viewer() override;
    ViewerCanvas* canvas() const { return canvas_; }
    RotoTool* rotoTool() const { return roto_; }

    // Waiting for a render of what should be on screen.
    bool busy() const { return shown_ != serial_; }
    bool isCached(int frame) const { return cache_.count(frame) > 0; }
    std::vector<int> cachedFrames() const;
    void setCacheLimit(std::size_t bytes);

public slots:
    // Something changed: forget cached frames and render the current one.
    void requestRender();

signals:
    void renderFinished(bool ok);  // after each result is shown
    void cacheChanged();

private:
    void showFrame();  // the document's frame changed
    // What has to be rendered for `r` to fill the screen.
    bool covers(const ViewResult& r) const;
    void viewChanged();
    void render();
    void deliver(ViewResult r);
    void takeReady();  // on the GUI thread: picks up what the worker left
    void fillViews();
    void clearCache();
    void addToCache(const ViewResult& r);

    Document& doc_;
    ViewerCanvas* canvas_;
    QLabel* title_;
    QComboBox* display_;
    QComboBox* view_;
    QDoubleSpinBox* exposure_;
    QComboBox* channel_;
    quint64 serial_ = 0;
    comp::Format format_;  // the project format the latest render used
    quint64 shown_ = 0;  // serial of what's on screen

    struct Cached {
        ViewResult result;
        std::size_t bytes;
        std::list<int>::iterator lru;
    };
    std::map<int, Cached> cache_;
    std::list<int> lru_;  // least recently shown first
    std::size_t cacheBytes_ = 0, cacheLimit_ = std::size_t(1) << 30;
    // The worker's newest result waits here for the GUI thread. A plain
    // mutex (not just Qt's event queue) guards the hand-over, so race
    // checkers can see it.
    std::mutex readyMutex_;
    std::optional<ViewResult> ready_;
    std::unique_ptr<RenderWorker> worker_;
    QTimer* roiTimer_;
    RotoTool* roto_;  // gathers zooming and panning into one render
};

}  // namespace compgui

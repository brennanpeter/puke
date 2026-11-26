#pragma once

#include <QImage>
#include <QWidget>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "comp/core/Image.h"
#include "comp/engine/Snapshot.h"
#include "comp/ofx/Overlay.h"
#include "OverlayRenderer.h"

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
    // The part of the image on screen (full-size pixel coordinates), if
    // known: only that (plus a margin) is rendered.
    std::optional<comp::Box> visible;
    double scale = 1;  // proxy: render at this fraction of full size
    quint64 serial = 0;
    // Caching ahead: not for the screen, only for the frame cache. The
    // worker takes any request for the screen first.
    bool background = false;
    std::uint64_t key = 0;  // the frame cache's key for this picture (see Viewer)
};

struct ViewResult {
    quint64 serial = 0;
    double frame = 1;
    QImage image;                             // display-ready, covering `region`
    std::shared_ptr<const comp::Image> working;  // working-space pixels, for the readout
    // In pixels at `scale` (a proxy render is smaller); the canvas draws it
    // at full size.
    double scale = 1;
    comp::Box region;      // what was rendered
    comp::Box full;        // what would be rendered with all of it on screen
    comp::Box displayWindow;
    comp::Box fullFrame;   // the display window at full size (for drawing)
    QString error;
    double milliseconds = 0;
    bool background = false;
    std::uint64_t key = 0;
};

// Renders on its own thread with its own Engine (tile cache on), so the GUI
// never waits. It holds at most one request for the screen and one for the
// background; a newer one of either kind replaces an older one not started
// yet, and the screen's always goes first. Results arrive, in order, on the
// worker thread via `deliver`.
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
    std::optional<ViewRequest> pending_, pendingBackground_;
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

    // An OFX overlay: drawn over the picture, given mouse and key events
    // before anything else.
    struct OverlayHooks {
        comp::ofx::Overlay* overlay = nullptr;
        std::function<comp::ofx::Overlay::View()> view;  // time, scale, format, flip; the canvas adds the rest
        comp::ofx::Overlay::Edit edit;                   // a value the plugin set
        std::function<void()> done;                      // a gesture ended: close its undo step
    };
    void setOverlay(OverlayHooks hooks);
    // The OFX view for the overlay now (the hooks' view plus zoom and size).
    comp::ofx::Overlay::View overlayView() const;
    OverlayRenderer& overlayRenderer() { return overlayRenderer_; }

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
    OverlayHooks overlay_;
    bool overlayDragging_ = false;
    OverlayRenderer overlayRenderer_;
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
// one, or playing a range a second time, is instant. Each is kept with a
// key: the engine's render key for the viewed node at that frame, plus the
// viewer's display settings. After an edit only the frames whose key
// changed are dropped: editing a node that doesn't feed the viewed one
// keeps them all, and moving a key keeps the frames it doesn't change.
//
// Caching ahead (on by default): while nothing else is being rendered, the
// frames of the project's range not yet cached are rendered in the
// background, from the current frame onwards, until the cache is full.
// After an edit it waits a moment, so dragging a slider isn't slowed down.
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
    bool cacheAhead() const { return cacheAhead_; }
    void setCacheAhead(bool on);
    // Caching ahead has a frame on its way.
    bool cachingAhead() const { return backgroundFrame_.has_value(); }
    int backgroundRequests() const { return backgroundRequests_; }  // so far

public slots:
    // Something changed: forget cached frames and render the current one.
    void requestRender();

signals:
    void renderFinished(bool ok);  // after each result is shown
    void cacheChanged();
    void cacheAheadChanged(bool on);

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
    // Takes a new snapshot of the viewed node and drops the cached frames
    // whose key changed.
    void refreshKeys();
    std::uint64_t keyFor(int frame) const;  // 0: none (nothing viewed, or an error)
    ViewRequest makeRequest(int frame) const;
    void fillAhead();  // asks for the next frame to cache, if there is one

    Document& doc_;
    ViewerCanvas* canvas_;
    QLabel* title_;
    QComboBox* display_;
    QComboBox* view_;
    QDoubleSpinBox* exposure_;
    QComboBox* channel_;
    QComboBox* proxy_;
    quint64 serial_ = 0;
    // What the keys are made from, as of the last refreshKeys().
    std::shared_ptr<comp::Snapshot> current_;
    QString snapshotError_;
    comp::Format format_;
    double scale_ = 1;
    std::uint64_t settingsKey_ = 0;
    quint64 shown_ = 0;  // serial of what's on screen

    struct Cached {
        ViewResult result;  // result.key is its key
        std::size_t bytes;
        std::list<int>::iterator lru;
    };
    std::map<int, Cached> cache_;
    std::list<int> lru_;  // least recently shown first
    std::size_t cacheBytes_ = 0, cacheLimit_ = std::size_t(1) << 30;
    std::size_t lastBytes_ = 0;  // the size of the latest frame cached
    bool cacheAhead_ = true;
    std::optional<int> backgroundFrame_;
    int backgroundRequests_ = 0;  // asked for in the background, not back yet
    std::map<int, std::uint64_t> failed_;  // frames whose render failed, by key: not tried again
    QTimer* aheadTimer_;
    // The worker's newest result waits here for the GUI thread. A plain
    // mutex (not just Qt's event queue) guards the hand-over, so race
    // checkers can see it.
    std::mutex readyMutex_;
    std::deque<ViewResult> ready_;
    std::unique_ptr<RenderWorker> worker_;
    QTimer* roiTimer_;
    RotoTool* roto_;
    // The selected OFX node's overlay, if its plugin has one.
    std::unique_ptr<comp::ofx::Overlay> overlay_;
    bool overlayEditOpen_ = false;
    void updateOverlay();  // gathers zooming and panning into one render
};

}  // namespace compgui

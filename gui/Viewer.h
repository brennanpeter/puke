#pragma once

#include <QImage>
#include <QWidget>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "comp/core/Image.h"
#include "comp/engine/Snapshot.h"

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;

namespace compgui {

class Document;

// What the viewer asks for, and gets back.
struct ViewRequest {
    std::shared_ptr<comp::Snapshot> snapshot;
    double frame = 1;
    std::string display, view;
    double exposure = 0;  // stops
    int channel = 0;      // 0 RGB, 1 R, 2 G, 3 B, 4 A
    quint64 serial = 0;
};

struct ViewResult {
    quint64 serial = 0;
    QImage image;                             // display-ready, covering `region`
    std::shared_ptr<const comp::Image> working;  // working-space pixels, for the readout
    comp::Box region, displayWindow;
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
class ViewerCanvas : public QWidget {
    Q_OBJECT
public:
    explicit ViewerCanvas(QWidget* parent = nullptr);
    void setResult(const ViewResult& r);
    void setMessage(const QString& message);  // e.g. "nothing viewed", an error
    void fit();
    void oneToOne();
    const ViewResult& result() const { return result_; }

protected:
    void paintEvent(QPaintEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    QPointF toImage(QPointF widget) const;  // widget -> pixel coordinates

    ViewResult result_;
    QString message_;
    double zoom_ = 1;
    QPointF offset_;  // widget position of pixel (0, 0)
    bool fitted_ = false;
    bool panning_ = false;
    QPoint lastMouse_;
    QString readout_;
};

// The viewer panel: toolbar (frame, display, view, exposure, channels) and
// canvas. Re-renders whenever the document says something changed.
class Viewer : public QWidget {
    Q_OBJECT
public:
    Viewer(Document& doc, QWidget* parent = nullptr);
    ~Viewer() override;
    ViewerCanvas* canvas() const { return canvas_; }

public slots:
    void requestRender();

signals:
    void renderFinished(bool ok);  // after each result is shown

private:
    void deliver(ViewResult r);
    void takeReady();  // on the GUI thread: picks up what the worker left
    void fillViews();

    Document& doc_;
    ViewerCanvas* canvas_;
    QLabel* title_;
    QSpinBox* frame_;
    QComboBox* display_;
    QComboBox* view_;
    QDoubleSpinBox* exposure_;
    QComboBox* channel_;
    quint64 serial_ = 0;
    // The worker's newest result waits here for the GUI thread. A plain
    // mutex (not just Qt's event queue) guards the hand-over, so race
    // checkers can see it.
    std::mutex readyMutex_;
    std::optional<ViewResult> ready_;
    std::unique_ptr<RenderWorker> worker_;
};

}  // namespace compgui

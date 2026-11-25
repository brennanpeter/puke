#pragma once

#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>

#include <vector>

class QDoubleSpinBox;
class QLabel;
class QSpinBox;
class QToolButton;

namespace compgui {

class Document;
class Viewer;

// The scrub bar: the project's frame range with the current frame, and a
// line under the frames the viewer has cached. Click or drag to change frame.
class TimeBar : public QWidget {
    Q_OBJECT
public:
    TimeBar(Document& doc, Viewer& viewer, QWidget* parent = nullptr);
    QSize sizeHint() const override { return {400, 34}; }
    // The frame under x (clamped to the range), and back.
    int frameAt(double x) const;
    double xOf(double frame) const;
    // Frames with a key on any parameter of the selected node, in order.
    std::vector<double> keyFrames() const;

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;

private:
    Document& doc_;
    Viewer& viewer_;
};

// Under the viewer: first frame, transport buttons, current frame, last
// frame and fps, around the scrub bar.
//
// Playback shows every frame in order, never skipping: each one waits until
// the viewer has it, so the first pass plays at render speed and later
// passes (from the viewer's cache) at the project's fps. The measured rate
// is shown while playing.
class Timeline : public QWidget {
    Q_OBJECT
public:
    Timeline(Document& doc, Viewer& viewer, QWidget* parent = nullptr);

    bool playing() const { return timer_.isActive(); }
    void play();
    void stop();
    void togglePlay() { playing() ? stop() : play(); }
    // Moves `delta` frames, wrapping around the range.
    void step(int delta);
    void goToStart();
    void goToEnd();

signals:
    void playingChanged(bool playing);

private:
    void tick();
    void syncFromDoc();
    void applySettings();  // the range or fps fields changed

    Document& doc_;
    Viewer& viewer_;
    TimeBar* bar_;
    QSpinBox* first_;
    QSpinBox* last_;
    QSpinBox* frame_;
    QDoubleSpinBox* fps_;
    QToolButton* play_;
    QLabel* rate_;
    QTimer timer_;
    QElapsedTimer clock_;  // for the measured rate
    int shownSincePlay_ = 0;
};

}  // namespace compgui

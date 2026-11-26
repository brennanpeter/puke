#include "Timeline.h"

#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>

#include <algorithm>
#include <cmath>

#include "Document.h"
#include "Viewer.h"

namespace compgui {

namespace {
constexpr double kMargin = 12;
}

// ================================================================ bar

TimeBar::TimeBar(Document& doc, Viewer& viewer, QWidget* parent) : QWidget(parent), doc_(doc), viewer_(viewer) {
    setObjectName("timeBar");
    setMinimumHeight(30);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(&doc_, &Document::frameChanged, this, qOverload<>(&QWidget::update));
    connect(&doc_, &Document::settingsChanged, this, qOverload<>(&QWidget::update));
    connect(&viewer_, &Viewer::cacheChanged, this, qOverload<>(&QWidget::update));
    connect(&doc_, &Document::selectionChanged, this, qOverload<>(&QWidget::update));
    connect(&doc_, &Document::paramChanged, this, qOverload<>(&QWidget::update));
}

std::vector<double> TimeBar::keyFrames() const {
    std::vector<double> out;
    if (comp::Node* n = doc_.selected())
        for (const auto& [key, curve] : n->curves())
            for (const auto& k : curve.keys()) out.push_back(k.frame);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

double TimeBar::xOf(double frame) const {
    const auto& s = doc_.settings();
    const double span = std::max(1, s.lastFrame - s.firstFrame);
    return kMargin + (frame - s.firstFrame) / span * (width() - 2 * kMargin);
}

int TimeBar::frameAt(double x) const {
    const auto& s = doc_.settings();
    const double span = std::max(1, s.lastFrame - s.firstFrame);
    const int f = s.firstFrame + static_cast<int>(std::lround((x - kMargin) / (width() - 2 * kMargin) * span));
    return std::clamp(f, s.firstFrame, s.lastFrame);
}

void TimeBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(34, 34, 36));
    const auto& s = doc_.settings();
    const int h = height();

    // Ticks, labelled about every 70 pixels at a round step.
    const double perFrame = (width() - 2 * kMargin) / std::max(1, s.lastFrame - s.firstFrame);
    int step = 1;
    for (int c : {1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 5000, 10000})
        if (c * perFrame >= 70 || c == 10000) {
            step = c;
            break;
        }
    QFont f = font();
    f.setPointSizeF(7.5);
    p.setFont(f);
    const int startTick = static_cast<int>(std::ceil(s.firstFrame / double(step))) * step;
    for (int t = startTick; t <= s.lastFrame; t += step) {
        const double x = xOf(t);
        p.setPen(QColor(90, 90, 94));
        p.drawLine(QPointF(x, h - 12), QPointF(x, h - 4));
        p.setPen(QColor(150, 150, 150));
        p.drawText(QRectF(x - 30, 1, 60, 12), Qt::AlignCenter, QString::number(t));
    }
    p.setPen(QColor(70, 70, 74));
    p.drawLine(QPointF(kMargin, h - 4), QPointF(width() - kMargin, h - 4));

    // Cached frames: a green line along the bottom.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(80, 170, 90));
    const double w = std::max(1.0, perFrame);
    for (int c : viewer_.cachedFrames())
        if (c >= s.firstFrame && c <= s.lastFrame) p.drawRect(QRectF(xOf(c) - w / 2, h - 3, w, 3));

    // The selected node's keys: small diamonds along the top.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(240, 170, 60));
    for (double k : keyFrames()) {
        if (k < s.firstFrame || k > s.lastFrame) continue;
        const double x = xOf(k);
        const QPointF d[4] = {{x, 13}, {x + 4, 17}, {x, 21}, {x - 4, 17}};
        p.drawPolygon(d, 4);
    }

    // The current frame.
    const int cur = doc_.frame();
    if (cur >= s.firstFrame && cur <= s.lastFrame) {
        const double x = xOf(cur);
        p.setPen(QPen(QColor(240, 170, 50), 2));
        p.drawLine(QPointF(x, 13), QPointF(x, h - 1));
    }
}

void TimeBar::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) doc_.setFrame(frameAt(e->position().x()));
}

void TimeBar::mouseMoveEvent(QMouseEvent* e) {
    if (e->buttons() & Qt::LeftButton) doc_.setFrame(frameAt(e->position().x()));
}

// ================================================================ timeline

Timeline::Timeline(Document& doc, Viewer& viewer, QWidget* parent) : QWidget(parent), doc_(doc), viewer_(viewer) {
    setObjectName("timeline");
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(6, 2, 6, 4);
    row->setSpacing(4);

    auto spin = [](const char* name, const QString& tip) {
        auto* s = new QSpinBox;
        s->setObjectName(name);
        s->setRange(-1000000, 1000000);
        s->setKeyboardTracking(false);
        s->setToolTip(tip);
        s->setButtonSymbols(QAbstractSpinBox::NoButtons);
        s->setAlignment(Qt::AlignCenter);
        s->setFixedWidth(64);
        return s;
    };
    auto button = [this](const char* name, const QString& text, const QString& tip, auto fn) {
        auto* b = new QToolButton;
        b->setObjectName(name);
        b->setText(text);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        connect(b, &QToolButton::clicked, this, fn);
        return b;
    };

    first_ = spin("timelineFirst", "First frame of the project");
    last_ = spin("timelineLast", "Last frame of the project");
    frame_ = spin("timelineFrame", "Current frame");
    fps_ = new QDoubleSpinBox;
    fps_->setObjectName("timelineFps");
    fps_->setRange(1, 240);
    fps_->setDecimals(3);
    fps_->setSuffix(" fps");
    fps_->setKeyboardTracking(false);
    fps_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    fps_->setToolTip("Playback speed");
    play_ = button("timelinePlay", "▶", "Play / stop", [this] { togglePlay(); });
    auto* ahead = button("timelineCacheAhead", "Cache ahead",
                         "Render the range's frames into the viewer's cache while nothing else is rendering",
                         [this](bool on) { viewer_.setCacheAhead(on); });
    ahead->setCheckable(true);
    ahead->setChecked(viewer_.cacheAhead());
    connect(&viewer_, &Viewer::cacheAheadChanged, ahead, &QToolButton::setChecked);
    rate_ = new QLabel;
    rate_->setObjectName("timelineRate");
    rate_->setMinimumWidth(64);
    rate_->setStyleSheet("color: #999;");
    bar_ = new TimeBar(doc_, viewer_);

    row->addWidget(first_);
    row->addWidget(button("timelineStart", "⏮", "First frame", [this] { goToStart(); }));
    row->addWidget(button("timelineBack", "◀", "Previous frame", [this] { step(-1); }));
    row->addWidget(play_);
    row->addWidget(button("timelineForward", "▶▶", "Next frame", [this] { step(1); }));
    row->addWidget(button("timelineEnd", "⏭", "Last frame", [this] { goToEnd(); }));
    row->addWidget(frame_);
    row->addWidget(bar_, 1);
    row->addWidget(last_);
    row->addWidget(fps_);
    row->addWidget(ahead);
    row->addWidget(rate_);

    connect(frame_, &QSpinBox::valueChanged, this, [this](int f) { doc_.setFrame(f); });
    connect(first_, &QSpinBox::valueChanged, this, [this] {
        if (last_->value() < first_->value()) {
            QSignalBlocker b(last_);
            last_->setValue(first_->value());
        }
        applySettings();
    });
    connect(last_, &QSpinBox::valueChanged, this, [this] {
        if (first_->value() > last_->value()) {
            QSignalBlocker b(first_);
            first_->setValue(last_->value());
        }
        applySettings();
    });
    connect(fps_, &QDoubleSpinBox::valueChanged, this, &Timeline::applySettings);
    connect(&doc_, &Document::settingsChanged, this, &Timeline::syncFromDoc);
    connect(&doc_, &Document::frameChanged, this, &Timeline::syncFromDoc);
    connect(&timer_, &QTimer::timeout, this, &Timeline::tick);
    syncFromDoc();
}

void Timeline::syncFromDoc() {
    const auto& s = doc_.settings();
    for (auto [box, v] : {std::pair{first_, s.firstFrame}, {last_, s.lastFrame}, {frame_, doc_.frame()}}) {
        QSignalBlocker b(box);
        box->setValue(v);
    }
    QSignalBlocker b(fps_);
    fps_->setValue(s.fps);
    if (playing()) timer_.setInterval(static_cast<int>(std::lround(1000.0 / s.fps)));
}

void Timeline::applySettings() {
    comp::ProjectSettings s = doc_.settings();
    s.firstFrame = first_->value();
    s.lastFrame = last_->value();
    s.fps = fps_->value();
    try {
        doc_.setSettings(s);
    } catch (const comp::Error& e) {
        syncFromDoc();
        QMessageBox::warning(this, "Project settings", QString::fromStdString(e.what()));
    }
}

void Timeline::step(int delta) {
    const auto& s = doc_.settings();
    const int len = s.lastFrame - s.firstFrame + 1;
    int f = doc_.frame();
    if (f < s.firstFrame || f > s.lastFrame) f = delta > 0 ? s.firstFrame - 1 : s.lastFrame + 1;  // outside: come in
    const int off = ((f - s.firstFrame + delta) % len + len) % len;
    doc_.setFrame(s.firstFrame + off);
}

void Timeline::goToStart() { doc_.setFrame(doc_.settings().firstFrame); }
void Timeline::goToEnd() { doc_.setFrame(doc_.settings().lastFrame); }

void Timeline::play() {
    if (playing()) return;
    shownSincePlay_ = 0;
    clock_.start();
    timer_.start(static_cast<int>(std::lround(1000.0 / doc_.settings().fps)));
    play_->setText("■");
    emit playingChanged(true);
}

void Timeline::stop() {
    if (!playing()) return;
    timer_.stop();
    play_->setText("▶");
    rate_->clear();
    emit playingChanged(false);
}

void Timeline::tick() {
    if (viewer_.busy()) return;  // the current frame isn't up yet: never skip one
    step(1);
    ++shownSincePlay_;
    if (clock_.elapsed() >= 1000) {  // the measured rate, about once a second
        rate_->setText(QString::number(shownSincePlay_ * 1000.0 / clock_.elapsed(), 'f', 1) + " fps");
        shownSincePlay_ = 0;
        clock_.restart();
    }
}

}  // namespace compgui

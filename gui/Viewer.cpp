#include "Viewer.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "Document.h"
#include "comp/color/ColorConfig.h"
#include "comp/engine/Engine.h"

namespace compgui {

// ================================================================ worker

namespace {

// Working-space pixels -> an 8-bit image for the screen: pick channels,
// apply exposure, then the OCIO display and view.
QImage toDisplay(const comp::Image& working, const ViewRequest& req) {
    const comp::Box& r = working.region();
    comp::Image show(r, {"R", "G", "B"});
    const int ch[4] = {working.channelIndex("R"), working.channelIndex("G"), working.channelIndex("B"),
                       std::max(working.channelIndex("A"), working.channelIndex("alpha"))};
    const float gain = static_cast<float>(std::exp2(req.exposure));
    for (int y = r.y0; y < r.y1; ++y) {
        const float* in = working.pixel(r.x0, y);
        float* out = show.pixel(r.x0, y);
        for (int x = 0; x < r.width(); ++x, in += working.numChannels(), out += 3) {
            if (req.channel == 0) {
                for (int c = 0; c < 3; ++c) out[c] = (ch[c] >= 0 ? in[ch[c]] : 0.0f) * gain;
            } else {
                const int src = ch[req.channel - 1];
                const float v = src >= 0 ? in[src] : 0.0f;
                out[0] = out[1] = out[2] = req.channel == 4 ? v : v * gain;  // alpha: as is
            }
        }
    }
    if (req.channel != 4) comp::ColorConfig::current()->convertToDisplay(show, req.display, req.view);

    QImage img(r.width(), r.height(), QImage::Format_RGB32);
    for (int y = r.y0; y < r.y1; ++y) {
        const float* in = show.pixel(r.x0, y);
        auto* line = reinterpret_cast<QRgb*>(img.scanLine(y - r.y0));
        for (int x = 0; x < r.width(); ++x, in += 3) {
            auto b = [](float v) { return std::isfinite(v) ? int(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f) : 0; };
            line[x] = qRgb(b(in[0]), b(in[1]), b(in[2]));
        }
    }
    return img;
}

}  // namespace

RenderWorker::RenderWorker(std::function<void(ViewResult)> deliver)
    : deliver_(std::move(deliver)), thread_([this] { run(); }) {}

RenderWorker::~RenderWorker() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

void RenderWorker::request(ViewRequest r) {
    {
        std::lock_guard lock(mutex_);
        pending_ = std::move(r);  // replaces anything not started yet
    }
    wake_.notify_one();
}

void RenderWorker::run() {
    comp::Engine engine(comp::EngineOptions{.cacheBytes = std::size_t(1) << 30});
    for (;;) {
        ViewRequest req;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || pending_; });
            if (stopping_) return;
            req = std::move(*pending_);
            pending_.reset();
        }
        ViewResult res;
        res.serial = req.serial;
        res.frame = req.frame;
        QElapsedTimer timer;
        timer.start();
        try {
            const comp::RenderContext ctx{req.frame, req.format};
            const comp::ImageInfo info = engine.info(*req.snapshot, ctx);
            res.displayWindow = info.displayWindow;
            // The frame, plus whatever overscan exists around it (up to a
            // quarter of the frame size on each side).
            const comp::Box& f = info.displayWindow;
            const int pad = std::max(f.width(), f.height()) / 4;
            comp::Box region = f.unite(info.dataWindow.intersect(f.padded(pad)));
            if (f.empty()) region = info.dataWindow.intersect({-4096, -4096, 4096, 4096});
            res.full = region;
            if (req.visible) {  // only what's on screen, with a margin for small pans
                const comp::Box& v = *req.visible;
                region = region.intersect(v.padded(std::max(32, std::max(v.width(), v.height()) / 8)));
            }
            res.region = region;
            auto working = std::make_shared<comp::Image>(engine.render(*req.snapshot, ctx, region));
            res.image = toDisplay(*working, req);
            res.working = std::move(working);
        } catch (const std::exception& e) {
            res.error = QString::fromStdString(e.what());
        }
        res.milliseconds = timer.nsecsElapsed() / 1e6;
        deliver_(std::move(res));
    }
}

// ================================================================ canvas

ViewerCanvas::ViewerCanvas(QWidget* parent) : QWidget(parent) {
    setObjectName("viewerCanvas");
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(320, 200);
}

void ViewerCanvas::setResult(const ViewResult& r) {
    result_ = r;
    message_.clear();
    if (!placed_ && !r.full.empty()) fit();  // the first picture: fit it; after that, keep the user's view
    update();
}

std::optional<comp::Box> ViewerCanvas::visibleRegion() const {
    if (!placed_ || width() <= 0 || height() <= 0) return std::nullopt;
    const QPointF a = toImage(QPointF(0, 0)), b = toImage(QPointF(width(), height()));
    return comp::Box{int(std::floor(a.x())), int(std::floor(a.y())), int(std::ceil(b.x())), int(std::ceil(b.y()))};
}

void ViewerCanvas::centerOn(QPointF p) {
    offset_ = QPointF(width() / 2.0, height() / 2.0) - p * zoom_;
    fitted_ = false;
    placed_ = true;
    update();
    emit viewChanged();
}

void ViewerCanvas::setMessage(const QString& m) {
    message_ = m;
    update();
}

void ViewerCanvas::fit() {
    const comp::Box f = result_.displayWindow.empty() ? result_.full : result_.displayWindow;
    if (f.empty()) return;
    zoom_ = std::min((width() - 40.0) / f.width(), (height() - 40.0) / f.height());
    zoom_ = std::max(zoom_, 0.01);
    offset_ = QPointF(width() / 2.0 - (f.x0 + f.width() / 2.0) * zoom_, height() / 2.0 - (f.y0 + f.height() / 2.0) * zoom_);
    fitted_ = true;
    placed_ = true;
    update();
    emit viewChanged();
}

void ViewerCanvas::oneToOne() {
    const QPointF centre = toImage(QPointF(width() / 2.0, height() / 2.0));
    zoom_ = 1;
    offset_ = QPointF(width() / 2.0, height() / 2.0) - centre;
    fitted_ = false;
    update();
    emit viewChanged();
}

QPointF ViewerCanvas::toImage(QPointF w) const { return (w - offset_) / zoom_; }

void ViewerCanvas::resizeEvent(QResizeEvent*) {
    if (fitted_) fit();
    else emit viewChanged();
}

void ViewerCanvas::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(30, 30, 32));
    if (!result_.image.isNull()) {
        const comp::Box& r = result_.region;
        const QRectF target(offset_.x() + r.x0 * zoom_, offset_.y() + r.y0 * zoom_, r.width() * zoom_, r.height() * zoom_);
        p.setRenderHint(QPainter::SmoothPixmapTransform, zoom_ < 1);
        p.drawImage(target, result_.image);
    }
    if (!result_.full.empty()) {
        const comp::Box& f = result_.displayWindow;
        p.setPen(QPen(QColor(150, 150, 150), 1, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(offset_.x() + f.x0 * zoom_, offset_.y() + f.y0 * zoom_, f.width() * zoom_, f.height() * zoom_));
    }
    p.setPen(QColor(200, 200, 200));
    if (!result_.full.empty()) {
        const comp::Box& f = result_.displayWindow;
        p.drawText(rect().adjusted(8, 6, -8, -6), Qt::AlignTop | Qt::AlignRight,
                   QString("%1x%2   %3%   %4 ms").arg(f.width()).arg(f.height()).arg(int(zoom_ * 100)).arg(result_.milliseconds, 0, 'f', 0));
    }
    if (!readout_.isEmpty()) p.drawText(rect().adjusted(8, 6, -8, -6), Qt::AlignBottom | Qt::AlignLeft, readout_);
    const QString msg = !message_.isEmpty() ? message_ : result_.error;
    if (!msg.isEmpty()) {
        p.setPen(message_.isEmpty() ? QColor(255, 110, 110) : QColor(170, 170, 170));
        p.drawText(rect().adjusted(20, 20, -20, -20), Qt::AlignCenter | Qt::TextWordWrap, msg);
    }
}

void ViewerCanvas::wheelEvent(QWheelEvent* e) {
    const QPointF at = e->position();
    const QPointF before = toImage(at);
    zoom_ = std::clamp(zoom_ * (e->angleDelta().y() > 0 ? 1.2 : 1 / 1.2), 0.01, 64.0);
    offset_ = at - before * zoom_;
    fitted_ = false;
    update();
    emit viewChanged();
}

void ViewerCanvas::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && (e->modifiers() & Qt::AltModifier))) {
        panning_ = true;
        lastMouse_ = e->pos();
        fitted_ = false;
    }
    setFocus();
}

void ViewerCanvas::mouseMoveEvent(QMouseEvent* e) {
    if (panning_) {
        offset_ += e->pos() - lastMouse_;
        lastMouse_ = e->pos();
        update();
        emit viewChanged();
        return;
    }
    readout_.clear();
    if (result_.working) {
        const QPointF p = toImage(e->position());
        const int x = int(std::floor(p.x())), y = int(std::floor(p.y()));
        const comp::Image& img = *result_.working;
        if (img.region().contains(x, y)) {
            QStringList values;
            for (int c = 0; c < img.numChannels(); ++c)
                values << QString("%1 %2").arg(QString::fromStdString(img.channels()[c])).arg(img.at(x, y, c), 0, 'f', 4);
            readout_ = QString("x %1  y %2    %3").arg(x).arg(y).arg(values.join("  "));
        }
    }
    update();
}

void ViewerCanvas::mouseReleaseEvent(QMouseEvent*) { panning_ = false; }

void ViewerCanvas::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_F) fit();
    else if (e->key() == Qt::Key_Home) oneToOne();
    else QWidget::keyPressEvent(e);
}

// ================================================================ panel

Viewer::Viewer(Document& doc, QWidget* parent) : QWidget(parent), doc_(doc) {
    setObjectName("viewer");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    auto* bar = new QHBoxLayout;
    bar->setContentsMargins(6, 4, 6, 0);
    title_ = new QLabel;
    title_->setObjectName("viewerTitle");
    display_ = new QComboBox;
    display_->setObjectName("viewerDisplay");
    view_ = new QComboBox;
    view_->setObjectName("viewerView");
    exposure_ = new QDoubleSpinBox;
    exposure_->setObjectName("viewerExposure");
    exposure_->setRange(-20, 20);
    exposure_->setSingleStep(0.5);
    exposure_->setPrefix("exp ");
    exposure_->setSuffix(" stops");
    exposure_->setKeyboardTracking(false);
    channel_ = new QComboBox;
    channel_->setObjectName("viewerChannel");
    channel_->addItems({"RGB", "R", "G", "B", "A"});
    auto* fit = new QToolButton;
    fit->setText("Fit");
    bar->addWidget(title_, 1);
    for (QWidget* w : std::initializer_list<QWidget*>{channel_, exposure_, display_, view_, fit}) bar->addWidget(w);
    layout->addLayout(bar);
    canvas_ = new ViewerCanvas;
    layout->addWidget(canvas_, 1);

    try {
        const auto config = comp::ColorConfig::current();
        for (const auto& d : config->displays()) display_->addItem(QString::fromStdString(d));
        display_->setCurrentText(QString::fromStdString(config->defaultDisplay()));
    } catch (const comp::Error&) {
    }
    fillViews();

    connect(display_, &QComboBox::currentTextChanged, this, [this] {
        fillViews();
        requestRender();
    });
    connect(view_, &QComboBox::currentTextChanged, this, &Viewer::requestRender);
    connect(exposure_, &QDoubleSpinBox::valueChanged, this, &Viewer::requestRender);
    connect(channel_, &QComboBox::currentIndexChanged, this, &Viewer::requestRender);
    connect(&doc_, &Document::frameChanged, this, &Viewer::showFrame);
    // A new project format changes what generators make; range and fps don't.
    connect(&doc_, &Document::settingsChanged, this, [this] {
        if (doc_.settings().format != format_) requestRender();
    });
    roiTimer_ = new QTimer(this);
    roiTimer_->setSingleShot(true);
    roiTimer_->setInterval(30);
    connect(canvas_, &ViewerCanvas::viewChanged, roiTimer_, qOverload<>(&QTimer::start));
    connect(roiTimer_, &QTimer::timeout, this, &Viewer::viewChanged);
    connect(fit, &QToolButton::clicked, canvas_, &ViewerCanvas::fit);
    connect(&doc_, &Document::renderNeeded, this, &Viewer::requestRender);

    worker_ = std::make_unique<RenderWorker>([this](ViewResult r) {
        // On the worker thread: leave the result, and nudge the GUI thread.
        {
            std::lock_guard lock(readyMutex_);
            ready_ = std::move(r);
        }
        QMetaObject::invokeMethod(this, &Viewer::takeReady, Qt::QueuedConnection);
    });
    requestRender();
}

Viewer::~Viewer() { worker_.reset(); }  // stop the thread before the widgets go

void Viewer::fillViews() {
    QSignalBlocker block(view_);
    const QString keep = view_->currentText();
    view_->clear();
    try {
        const auto config = comp::ColorConfig::current();
        const std::string display = display_->currentText().toStdString();
        if (!display.empty()) {
            for (const auto& v : config->views(display)) view_->addItem(QString::fromStdString(v));
            const int i = view_->findText(keep);
            view_->setCurrentText(i >= 0 ? keep : QString::fromStdString(config->defaultView(display)));
        }
    } catch (const comp::Error&) {
    }
}

void Viewer::requestRender() {
    clearCache();
    render();
}

bool Viewer::covers(const ViewResult& r) const {
    const auto visible = canvas_->visibleRegion();
    const comp::Box needed = visible ? r.full.intersect(*visible) : r.full;
    return needed.empty() || r.region.intersect(needed) == needed;
}

void Viewer::viewChanged() {
    // Render again only if the picture on screen doesn't cover it (and no
    // render is already on its way, which will).
    const ViewResult& r = canvas_->result();
    if (busy() || !doc_.viewed() || !r.error.isEmpty() || covers(r)) return;
    render();
}

void Viewer::showFrame() {
    auto it = cache_.find(doc_.frame());
    if (it == cache_.end() || !doc_.viewed() || !covers(it->second.result)) {
        render();
        return;
    }
    ++serial_;  // drop anything still on its way
    shown_ = serial_;
    lru_.splice(lru_.end(), lru_, it->second.lru);
    canvas_->setResult(it->second.result);
    emit renderFinished(true);
}

void Viewer::render() {
    comp::Node* node = doc_.viewed();
    title_->setText(node ? QString("<b>%1</b>").arg(QString::fromStdString(node->name())) : QString());
    ++serial_;
    if (!node) {
        shown_ = serial_;
        canvas_->setResult(ViewResult{});
        canvas_->setMessage("Nothing to view.\nSelect a node and press 1, or double-click it.");
        emit renderFinished(false);
        return;
    }
    ViewRequest req;
    try {
        req.snapshot = std::make_shared<comp::Snapshot>(comp::Snapshot::of(*node));  // here, on the GUI thread
    } catch (const comp::Error& e) {
        ViewResult r;
        r.serial = serial_;
        r.error = QString::fromStdString(e.what());
        deliver(std::move(r));
        return;
    }
    req.frame = doc_.frame();
    req.format = format_ = doc_.settings().format;
    req.visible = canvas_->visibleRegion();
    req.display = display_->currentText().toStdString();
    req.view = view_->currentText().toStdString();
    req.exposure = exposure_->value();
    req.channel = channel_->currentIndex();
    req.serial = serial_;
    worker_->request(std::move(req));
}

void Viewer::takeReady() {
    std::optional<ViewResult> r;
    {
        std::lock_guard lock(readyMutex_);
        r.swap(ready_);
    }
    if (r) deliver(std::move(*r));
}

void Viewer::deliver(ViewResult r) {
    if (r.serial != serial_) return;  // something newer is on its way
    shown_ = serial_;
    const bool ok = r.error.isEmpty();
    if (ok) {
        addToCache(r);
        canvas_->setResult(r);
    } else {
        ViewResult keep = canvas_->result();  // keep showing the last good picture under the error
        keep.error = r.error;
        canvas_->setResult(keep);
    }
    emit renderFinished(ok);
}

// --- frame cache

std::vector<int> Viewer::cachedFrames() const {
    std::vector<int> out;
    for (const auto& [f, c] : cache_) out.push_back(f);
    return out;
}

void Viewer::setCacheLimit(std::size_t bytes) {
    cacheLimit_ = bytes;
    clearCache();
}

void Viewer::clearCache() {
    if (cache_.empty()) return;
    cache_.clear();
    lru_.clear();
    cacheBytes_ = 0;
    emit cacheChanged();
}

void Viewer::addToCache(const ViewResult& r) {
    const int frame = static_cast<int>(r.frame);
    std::size_t bytes = static_cast<std::size_t>(r.image.sizeInBytes());
    if (r.working)
        bytes += static_cast<std::size_t>(r.working->region().area()) * r.working->numChannels() * sizeof(float);
    if (bytes > cacheLimit_) return;
    if (auto it = cache_.find(frame); it != cache_.end()) {
        cacheBytes_ -= it->second.bytes;
        lru_.erase(it->second.lru);
        cache_.erase(it);
    }
    while (cacheBytes_ + bytes > cacheLimit_ && !lru_.empty()) {  // make room: least recently shown go first
        auto old = cache_.find(lru_.front());
        cacheBytes_ -= old->second.bytes;
        cache_.erase(old);
        lru_.pop_front();
    }
    lru_.push_back(frame);
    cache_[frame] = Cached{r, bytes, std::prev(lru_.end())};
    cacheBytes_ += bytes;
    emit cacheChanged();
}

}  // namespace compgui

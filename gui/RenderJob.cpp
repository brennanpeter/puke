#include "RenderJob.h"

#include "puke/engine/Engine.h"
#include "puke/engine/Snapshot.h"
#include "puke/nodes/WriteNode.h"

namespace pukegui {

RenderJob::RenderJob(const std::vector<puke::WriteNode*>& writes, int first, int last, const puke::Format& format,
                     QObject* parent)
    : QObject(parent), first_(first), last_(last), format_(format) {
    if (last < first) throw puke::Error("the last frame is before the first");
    if (writes.empty()) throw puke::Error("no Write nodes to render");
    for (puke::WriteNode* w : writes)
        tasks_.push_back({w->name(), w, std::make_shared<const puke::Snapshot>(puke::Snapshot::of(*w))});
    total_ = (last - first + 1) * static_cast<int>(tasks_.size());
}

RenderJob::~RenderJob() {
    cancel_ = true;
    if (thread_.joinable()) thread_.join();
}

void RenderJob::start() {
    if (running_ || thread_.joinable()) return;  // once only
    running_ = true;
    thread_ = std::thread([this] { run(); });
}

void RenderJob::run() {
    puke::Engine engine;  // every core; no tile cache: each frame is rendered once
    int done = 0;
    for (int f = first_; f <= last_; ++f) {
        for (const Task& t : tasks_) {
            if (cancel_) {
                post({done, {}, false, true, true});
                return;
            }
            Event e;
            const QString prefix = QString("frame %1 %2").arg(f).arg(QString::fromStdString(t.name));
            try {
                const std::string path = t.write->execute(engine, *t.snapshot, {static_cast<double>(f), format_});
                e.message = prefix + " -> " + QString::fromStdString(path);
            } catch (const std::exception& ex) {
                e.message = prefix + " failed: " + QString::fromStdString(ex.what());
                e.failed = true;
            }
            e.done = ++done;
            post(std::move(e));
        }
    }
    post({done, {}, false, true, false});
}

void RenderJob::post(Event e) {
    {
        std::lock_guard lock(mutex_);
        events_.push_back(std::move(e));
    }
    QMetaObject::invokeMethod(this, &RenderJob::takeEvents, Qt::QueuedConnection);
}

void RenderJob::takeEvents() {
    std::deque<Event> events;
    {
        std::lock_guard lock(mutex_);
        events.swap(events_);
    }
    for (const Event& e : events) {
        if (e.last) {
            if (thread_.joinable()) thread_.join();  // it has nothing left to do
            running_ = false;
            emit finished(failures_, e.cancelled);
            return;
        }
        if (e.failed) ++failures_;
        emit progress(e.done, total_, e.message, e.failed);
    }
}

}  // namespace pukegui

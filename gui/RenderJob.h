#pragma once

#include <QObject>
#include <QString>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "comp/core/Format.h"

namespace comp {
class Snapshot;
class WriteNode;
}  // namespace comp

namespace compgui {

// Runs Write nodes over a frame range on its own thread, like comp_render:
// every Write for each frame in turn, a failure doesn't stop the rest.
//
// What gets written is the graph as it was when the job was made (each Write
// is snapshotted then), so editing can carry on during the render.
//
// cancel() stops before the next file; the one being written finishes (it's
// written to a .partial name first, so nothing half-written is left either
// way). Destroying a running job cancels it and waits.
class RenderJob : public QObject {
    Q_OBJECT
public:
    // Snapshots the Writes here, on the editing thread. Throws comp::Error if
    // one can't be rendered (e.g. an unconnected input) or last < first.
    RenderJob(const std::vector<comp::WriteNode*>& writes, int first, int last, const comp::Format& format,
              QObject* parent = nullptr);
    ~RenderJob() override;

    void start();
    void cancel() { cancel_ = true; }
    bool running() const { return running_; }
    int total() const { return total_; }

signals:
    // After each file: how many are done of total(), and what happened
    // ("frame 1001 Write1 -> out.1001.exr", or the error).
    void progress(int done, int total, const QString& message, bool failed);
    void finished(int failures, bool cancelled);

private:
    struct Task {
        std::string name;
        const comp::WriteNode* write;
        std::shared_ptr<const comp::Snapshot> snapshot;
    };
    struct Event {
        int done = 0;
        QString message;
        bool failed = false;
        bool last = false;
        bool cancelled = false;
    };

    void run();
    void post(Event e);
    void takeEvents();  // on the GUI thread

    std::vector<Task> tasks_;
    int first_, last_, total_;
    comp::Format format_;
    int failures_ = 0;
    bool running_ = false;
    std::atomic<bool> cancel_{false};
    // Events from the render thread wait here for the GUI thread (a plain
    // mutex, so race checkers can see the hand-over).
    std::mutex mutex_;
    std::deque<Event> events_;
    std::thread thread_;
};

}  // namespace compgui

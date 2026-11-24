#include "RenderDialog.h"

#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "Document.h"
#include "RenderJob.h"
#include "comp/nodes/WriteNode.h"

namespace compgui {

RenderDialog::RenderDialog(Document& doc, QWidget* parent) : QDialog(parent), doc_(doc) {
    setObjectName("renderDialog");
    setWindowTitle("Render");
    resize(520, 440);
    auto* layout = new QVBoxLayout(this);

    writes_ = new QListWidget;
    writes_->setObjectName("renderWrites");
    writes_->setMaximumHeight(120);
    first_ = new QSpinBox;
    first_->setObjectName("renderFirst");
    last_ = new QSpinBox;
    last_->setObjectName("renderLast");
    for (QSpinBox* s : {first_, last_}) s->setRange(-1000000, 1000000);
    auto* range = new QHBoxLayout;
    range->addWidget(first_);
    range->addWidget(new QLabel("to"));
    range->addWidget(last_);
    range->addStretch();
    auto* form = new QFormLayout;
    form->addRow("Write nodes", writes_);
    form->addRow("Frames", range);
    layout->addLayout(form);

    progress_ = new QProgressBar;
    progress_->setObjectName("renderProgress");
    progress_->setValue(0);
    status_ = new QLabel;
    status_->setObjectName("renderStatus");
    log_ = new QPlainTextEdit;
    log_->setObjectName("renderLog");
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(10000);
    layout->addWidget(progress_);
    layout->addWidget(status_);
    layout->addWidget(log_, 1);

    auto* buttons = new QDialogButtonBox;
    start_ = buttons->addButton("Render", QDialogButtonBox::AcceptRole);
    start_->setObjectName("renderStart");
    cancel_ = buttons->addButton("Cancel render", QDialogButtonBox::ActionRole);
    cancel_->setObjectName("renderCancel");
    QPushButton* close = buttons->addButton(QDialogButtonBox::Close);
    layout->addWidget(buttons);
    connect(start_, &QPushButton::clicked, this, &RenderDialog::startRender);
    connect(cancel_, &QPushButton::clicked, this, &RenderDialog::cancelRender);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    // The list follows the graph while nothing is rendering.
    connect(&doc_, &Document::structureChanged, this, [this] {
        if (!rendering()) reset();
    });
    reset();
}

RenderDialog::~RenderDialog() = default;

bool RenderDialog::rendering() const { return job_ && job_->running(); }

void RenderDialog::reset() {
    writes_->clear();
    auto* selected = dynamic_cast<comp::WriteNode*>(doc_.selected());
    for (const auto& n : doc_.graph().nodes()) {
        auto* w = dynamic_cast<comp::WriteNode*>(n.get());
        if (!w) continue;
        auto* item = new QListWidgetItem(QString::fromStdString(w->name()), writes_);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(!selected || selected == w ? Qt::Checked : Qt::Unchecked);
        item->setToolTip(QString::fromStdString(w->param<std::string>("file", "")));
    }
    first_->setValue(doc_.settings().firstFrame);
    last_->setValue(doc_.settings().lastFrame);
    updateButtons();
}

void RenderDialog::startRender() {
    if (rendering()) return;
    std::vector<comp::WriteNode*> writes;
    for (int i = 0; i < writes_->count(); ++i) {
        QListWidgetItem* item = writes_->item(i);
        if (item->checkState() != Qt::Checked) continue;
        if (auto* w = dynamic_cast<comp::WriteNode*>(doc_.graph().find(item->text().toStdString())))
            writes.push_back(w);
    }
    log_->clear();
    try {
        job_ = std::make_unique<RenderJob>(writes, first_->value(), last_->value());
    } catch (const comp::Error& e) {
        job_.reset();
        status_->setText("Can't render: " + QString::fromStdString(e.what()));
        log(status_->text(), true);
        updateButtons();
        return;
    }
    progress_->setRange(0, job_->total());
    progress_->setValue(0);
    status_->setText(QString("Rendering %1 file%2...").arg(job_->total()).arg(job_->total() == 1 ? "" : "s"));
    connect(job_.get(), &RenderJob::progress, this, [this](int done, int, const QString& message, bool failed) {
        progress_->setValue(done);
        log(message, failed);
    });
    connect(job_.get(), &RenderJob::finished, this, [this](int failures, bool cancelled) {
        const int done = progress_->value(), total = progress_->maximum();
        if (cancelled) status_->setText(QString("Cancelled after %1 of %2 files.").arg(done).arg(total));
        else if (failures) status_->setText(QString("Finished: %1 of %2 files failed.").arg(failures).arg(total));
        else status_->setText(QString("Finished: %1 file%2 written.").arg(total).arg(total == 1 ? "" : "s"));
        updateButtons();
        emit renderFinished(failures, cancelled);
    });
    job_->start();
    updateButtons();
}

void RenderDialog::cancelRender() {
    if (!rendering()) return;
    job_->cancel();
    status_->setText("Cancelling after the current file...");
}

void RenderDialog::closeEvent(QCloseEvent* event) {
    if (rendering()) {
        cancelRender();
        job_.reset();  // waits for the file being written
    }
    QDialog::closeEvent(event);
}

void RenderDialog::log(const QString& line, bool error) {
    log_->appendHtml(error ? "<span style='color:#e66'>" + line.toHtmlEscaped() + "</span>" : line.toHtmlEscaped());
}

void RenderDialog::updateButtons() {
    const bool busy = rendering();
    start_->setEnabled(!busy && writes_->count() > 0);
    cancel_->setEnabled(busy);
    writes_->setEnabled(!busy);
    first_->setEnabled(!busy);
    last_->setEnabled(!busy);
}

}  // namespace compgui

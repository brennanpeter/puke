#pragma once

#include <QDialog>

#include <memory>

class QLabel;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;

namespace pukegui {

class Document;
class RenderJob;

// Render > Render... : pick Write nodes and a frame range, render them in
// the background with progress, and cancel. Not modal: the graph can be
// edited meanwhile (the render writes the graph as it was when it started).
class RenderDialog : public QDialog {
    Q_OBJECT
public:
    RenderDialog(Document& doc, QWidget* parent = nullptr);
    ~RenderDialog() override;

    // Fills the Write list and frame range from the document: the selected
    // Write if one is selected, otherwise all of them; the project's range.
    void reset();
    // Starts rendering the checked Writes. Reports problems in the log.
    void startRender();
    void cancelRender();
    bool rendering() const;

signals:
    void renderFinished(int failures, bool cancelled);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void log(const QString& line, bool error = false);
    void updateButtons();

    Document& doc_;
    QListWidget* writes_;
    QSpinBox* first_;
    QSpinBox* last_;
    QProgressBar* progress_;
    QLabel* status_;
    QPlainTextEdit* log_;
    QPushButton* start_;
    QPushButton* cancel_;
    std::unique_ptr<RenderJob> job_;
};

}  // namespace pukegui

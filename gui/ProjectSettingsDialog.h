#pragma once

#include <QDialog>

class QComboBox;
class QDoubleSpinBox;
class QSpinBox;

namespace pukegui {

class Document;

// Edit > Project Settings (S in the node graph): the project's format, frame
// range and fps. OK applies them as one undoable edit.
class ProjectSettingsDialog : public QDialog {
    Q_OBJECT
public:
    ProjectSettingsDialog(Document& doc, QWidget* parent = nullptr);

    // Applies the fields to the document. Returns false (and says why) if
    // they're not valid.
    bool apply();

    void accept() override;

private:
    void presetChosen(int index);
    void sizeEdited();  // selects the matching preset, or Custom

    Document& doc_;
    QComboBox* preset_;
    QSpinBox* width_;
    QSpinBox* height_;
    QSpinBox* first_;
    QSpinBox* last_;
    QDoubleSpinBox* fps_;
};

}  // namespace pukegui

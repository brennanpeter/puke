#include "ProjectSettingsDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include "Document.h"

namespace compgui {

namespace {

struct Preset {
    const char* name;
    int width, height;
};
// Common formats; anything else is Custom (the last entry).
constexpr Preset kPresets[] = {
    {"HD", 1920, 1080},     {"UHD", 3840, 2160},     {"2K DCI", 2048, 1080}, {"4K DCI", 4096, 2160},
    {"2K Scope", 2048, 858}, {"2K Flat", 1998, 1080}, {"PAL", 720, 576},      {"NTSC", 720, 486},
    {"Square 1K", 1024, 1024},
};
constexpr int kCustom = static_cast<int>(std::size(kPresets));

}  // namespace

ProjectSettingsDialog::ProjectSettingsDialog(Document& doc, QWidget* parent) : QDialog(parent), doc_(doc) {
    setObjectName("projectSettings");
    setWindowTitle("Project Settings");
    const comp::ProjectSettings& s = doc_.settings();

    preset_ = new QComboBox;
    preset_->setObjectName("settingsPreset");
    for (const Preset& p : kPresets) preset_->addItem(QString("%1  %2x%3").arg(p.name).arg(p.width).arg(p.height));
    preset_->addItem("Custom");
    width_ = new QSpinBox;
    width_->setObjectName("settingsWidth");
    height_ = new QSpinBox;
    height_->setObjectName("settingsHeight");
    for (QSpinBox* b : {width_, height_}) b->setRange(1, 65536);
    width_->setValue(s.format.width);
    height_->setValue(s.format.height);
    auto* size = new QHBoxLayout;
    size->addWidget(width_);
    size->addWidget(new QLabel("x"));
    size->addWidget(height_);
    size->addStretch();

    first_ = new QSpinBox;
    first_->setObjectName("settingsFirst");
    last_ = new QSpinBox;
    last_->setObjectName("settingsLast");
    for (QSpinBox* b : {first_, last_}) b->setRange(-1000000, 1000000);
    first_->setValue(s.firstFrame);
    last_->setValue(s.lastFrame);
    auto* range = new QHBoxLayout;
    range->addWidget(first_);
    range->addWidget(new QLabel("to"));
    range->addWidget(last_);
    range->addStretch();
    fps_ = new QDoubleSpinBox;
    fps_->setObjectName("settingsFps");
    fps_->setRange(1, 240);
    fps_->setDecimals(3);
    fps_->setValue(s.fps);

    auto* form = new QFormLayout;
    form->addRow("Format", preset_);
    form->addRow("", size);
    form->addRow("Frame range", range);
    form->addRow("Frames per second", fps_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);

    sizeEdited();
    connect(preset_, &QComboBox::currentIndexChanged, this, &ProjectSettingsDialog::presetChosen);
    connect(width_, &QSpinBox::valueChanged, this, &ProjectSettingsDialog::sizeEdited);
    connect(height_, &QSpinBox::valueChanged, this, &ProjectSettingsDialog::sizeEdited);
}

void ProjectSettingsDialog::presetChosen(int index) {
    if (index < 0 || index >= kCustom) return;  // Custom: keep the numbers
    const QSignalBlocker bw(width_), bh(height_);
    width_->setValue(kPresets[index].width);
    height_->setValue(kPresets[index].height);
}

void ProjectSettingsDialog::sizeEdited() {
    int match = kCustom;
    for (int i = 0; i < kCustom; ++i)
        if (kPresets[i].width == width_->value() && kPresets[i].height == height_->value()) match = i;
    const QSignalBlocker b(preset_);
    preset_->setCurrentIndex(match);
}

bool ProjectSettingsDialog::apply() {
    comp::ProjectSettings s = doc_.settings();
    s.format = {width_->value(), height_->value()};
    s.firstFrame = first_->value();
    s.lastFrame = last_->value();
    s.fps = fps_->value();
    try {
        doc_.setSettings(s);
    } catch (const comp::Error& e) {
        QMessageBox::warning(this, "Project Settings", QString::fromStdString(e.what()));
        return false;
    }
    return true;
}

void ProjectSettingsDialog::accept() {
    if (apply()) QDialog::accept();
}

}  // namespace compgui

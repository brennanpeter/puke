#include "PropertiesPanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

#include "Document.h"

namespace compgui {

namespace {

using Kind = comp::ParamSpec::Kind;

// Reading a stored value leniently, the way the nodes themselves do: an int
// where a double is expected, one number for every component, and so on.
double asNumber(const comp::ParamValue& v) {
    if (auto* d = std::get_if<double>(&v)) return *d;
    if (auto* i = std::get_if<int>(&v)) return *i;
    if (auto* b = std::get_if<bool>(&v)) return *b;
    return 0;
}
std::array<double, 4> asComponents(const comp::ParamValue& v) {
    if (auto* c = std::get_if<comp::Color>(&v)) return *c;
    if (auto* p = std::get_if<comp::Vec2>(&v)) return {(*p)[0], (*p)[1], 0, 0};
    const double n = asNumber(v);
    return {n, n, n, n};
}
QString asText(const comp::ParamValue& v) {
    return std::visit(
        [](const auto& x) -> QString {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::string>) return QString::fromStdString(x);
            else if constexpr (std::is_same_v<T, bool>) return x ? "true" : "false";
            else if constexpr (std::is_same_v<T, comp::Vec2> || std::is_same_v<T, comp::Color>) {
                QStringList parts;
                for (double d : x) parts << QString::number(d);
                return parts.join(", ");
            } else {
                return QString::number(x);
            }
        },
        v);
}

QDoubleSpinBox* numberBox(double min, double max, bool compact = false) {
    auto* box = new QDoubleSpinBox;
    box->setRange(-1e9, 1e9);
    box->setDecimals(compact ? 3 : 4);
    box->setSingleStep(std::max(0.001, (max - min) / 100));
    box->setKeyboardTracking(false);  // one edit per Enter, not per keystroke
    if (compact) {  // several in a row (Vec2, Color): no arrows, drag or type instead
        box->setButtonSymbols(QAbstractSpinBox::NoButtons);
        box->setMinimumWidth(48);
        box->setMaximumWidth(64);
    } else {
        box->setMinimumWidth(70);
    }
    return box;
}

}  // namespace

PropertiesPanel::PropertiesPanel(Document& doc, QWidget* parent) : QScrollArea(parent), doc_(doc) {
    setObjectName("properties");
    setWidgetResizable(true);
    setMinimumWidth(360);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(&doc_, &Document::selectionChanged, this, &PropertiesPanel::rebuild);
    connect(&doc_, &Document::structureChanged, this, &PropertiesPanel::rebuild);
    connect(&doc_, &Document::paramChanged, this, [this](comp::Node* n) {
        if (n == node_) refresh();
    });
    rebuild();
}

void PropertiesPanel::refresh() {
    for (auto& f : refreshers_) f();
}

void PropertiesPanel::rebuild() {
    refreshers_.clear();
    node_ = doc_.selected();
    auto* page = new QWidget;
    auto* outer = new QVBoxLayout(page);

    if (!node_) {
        auto* hint = new QLabel("Select a node to edit its parameters.\n\n"
                                "Tab in the node graph creates a node.");
        hint->setAlignment(Qt::AlignCenter);
        hint->setStyleSheet("color: #888;");
        outer->addWidget(hint);
        outer->addStretch();
        setWidget(page);
        return;
    }

    // The node's name, editable (Enter renames), and its type.
    auto* titleRow = new QHBoxLayout;
    auto* nameEdit = new QLineEdit(QString::fromStdString(node_->name()));
    nameEdit->setObjectName("nodeName");
    QFont bold = nameEdit->font();
    bold.setBold(true);
    nameEdit->setFont(bold);
    auto* typeLabel = new QLabel(QString::fromStdString(node_->typeName()));
    typeLabel->setStyleSheet("color: #999;");
    titleRow->addWidget(nameEdit, 1);
    titleRow->addWidget(typeLabel);
    outer->addLayout(titleRow);
    connect(nameEdit, &QLineEdit::editingFinished, this, [this, nameEdit] {
        comp::Node* node = node_;
        const std::string name = nameEdit->text().trimmed().toStdString();
        if (!node || name == node->name()) return;
        // Renaming rebuilds this panel, deleting the field whose signal
        // we're in: do it once the signal is over.
        QMetaObject::invokeMethod(this, [this, node, name, nameEdit] {
            if (node_ != node) return;
            try {
                doc_.renameNode(*node, name);
            } catch (const comp::Error& e) {
                nameEdit->setText(QString::fromStdString(node->name()));
                QMessageBox::warning(this, "Rename", QString::fromStdString(e.what()));
            }
        }, Qt::QueuedConnection);
    });

    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    outer->addLayout(form);
    comp::Node& node = *node_;

    std::vector<std::string> described;
    std::vector<comp::ParamSpec> specs;
    try {
        specs = node.paramSpecs();
    } catch (const comp::Error&) {
    }

    for (const comp::ParamSpec& spec : specs) {
        described.push_back(spec.name);
        const std::string key = spec.name;
        auto* label = new QLabel(QString::fromStdString(spec.label.empty() ? spec.name : spec.label));
        label->setToolTip(QString::fromStdString(spec.hint));
        auto* reset = new QToolButton;
        reset->setText(QString::fromUtf8("↺"));
        reset->setToolTip("Reset to default");
        reset->setAutoRaise(true);
        connect(reset, &QToolButton::clicked, this, [this, &node, key] { doc_.clearParam(node, key); });

        auto* row = new QWidget;
        row->setObjectName(QString::fromStdString("param_" + key));
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);

        // The value shown: the node's, or the default.
        auto current = [&node, key, def = spec.defaultValue]() -> comp::ParamValue {
            auto it = node.params().find(key);
            return it != node.params().end() ? it->second : def;
        };
        std::function<void()> refreshValue;

        switch (spec.kind) {
            case Kind::Bool: {
                auto* box = new QCheckBox;
                h->addWidget(box);
                h->addStretch();
                connect(box, &QCheckBox::toggled, this, [this, &node, key](bool on) { doc_.setParam(node, key, on); });
                refreshValue = [box, current] {
                    QSignalBlocker b(box);
                    box->setChecked(asNumber(current()) != 0);
                };
                break;
            }
            case Kind::Int: {
                auto* box = new QSpinBox;
                box->setRange(-1000000, 1000000);
                box->setKeyboardTracking(false);
                if (spec.optional) {
                    box->setMinimum(-1000001);
                    box->setSpecialValueText("(unset)");
                }
                h->addWidget(box);
                h->addStretch();
                connect(box, &QSpinBox::valueChanged, this, [this, &node, key, box](int v) {
                    if (v == box->minimum() && !box->specialValueText().isEmpty()) doc_.clearParam(node, key);
                    else doc_.setParam(node, key, v);
                });
                const bool optional = spec.optional;
                refreshValue = [box, current, &node, key, optional] {
                    QSignalBlocker b(box);
                    if (optional && !node.hasParam(key)) box->setValue(box->minimum());
                    else box->setValue(static_cast<int>(asNumber(current())));
                };
                break;
            }
            case Kind::Double: {
                auto* box = numberBox(spec.min, spec.max);
                auto* slider = new QSlider(Qt::Horizontal);
                slider->setRange(0, 1000);
                h->addWidget(box);
                h->addWidget(slider, 1);
                const double lo = spec.min, hi = spec.max > spec.min ? spec.max : spec.min + 1;
                connect(box, &QDoubleSpinBox::valueChanged, this, [this, &node, key](double v) { doc_.setParam(node, key, v); });
                connect(slider, &QSlider::valueChanged, this, [box, lo, hi](int s) { box->setValue(lo + (hi - lo) * s / 1000.0); });
                refreshValue = [box, slider, current, lo, hi] {
                    QSignalBlocker b1(box), b2(slider);
                    const double v = asNumber(current());
                    box->setValue(v);
                    slider->setValue(static_cast<int>(std::lround(std::clamp((v - lo) / (hi - lo), 0.0, 1.0) * 1000)));
                };
                break;
            }
            case Kind::Vec2:
            case Kind::Color: {
                const int n = spec.kind == Kind::Vec2 ? 2 : 4;
                std::vector<QDoubleSpinBox*> boxes;
                for (int i = 0; i < n; ++i) {
                    auto* box = numberBox(spec.min, spec.max, true);
                    box->setToolTip(spec.kind == Kind::Vec2 ? (i ? "y" : "x") : QString("rgba"[i]));
                    h->addWidget(box);
                    boxes.push_back(box);
                }
                QLabel* swatch = nullptr;
                if (spec.kind == Kind::Color) {
                    swatch = new QLabel;
                    swatch->setFixedSize(16, 16);
                    h->addWidget(swatch);
                }
                const bool isColor = spec.kind == Kind::Color;
                for (auto* box : boxes)
                    connect(box, &QDoubleSpinBox::valueChanged, this, [this, &node, key, boxes, isColor](double) {
                        if (isColor)
                            doc_.setParam(node, key, comp::Color{boxes[0]->value(), boxes[1]->value(), boxes[2]->value(), boxes[3]->value()});
                        else
                            doc_.setParam(node, key, comp::Vec2{boxes[0]->value(), boxes[1]->value()});
                    });
                refreshValue = [boxes, swatch, current] {
                    const auto c = asComponents(current());
                    for (std::size_t i = 0; i < boxes.size(); ++i) {
                        QSignalBlocker b(boxes[i]);
                        boxes[i]->setValue(c[i]);
                    }
                    if (swatch) {  // a rough preview: clamped, with a 2.2 gamma
                        auto ch = [](double v) { return int(std::pow(std::clamp(v, 0.0, 1.0), 1 / 2.2) * 255); };
                        swatch->setStyleSheet(QString("background: rgb(%1,%2,%3); border: 1px solid #222;")
                                                  .arg(ch(c[0])).arg(ch(c[1])).arg(ch(c[2])));
                    }
                };
                break;
            }
            case Kind::Choice: {
                auto* box = new QComboBox;
                box->setEditable(spec.openChoice);
                for (const auto& o : spec.options) box->addItem(QString::fromStdString(o));
                h->addWidget(box, 1);
                auto apply = [this, &node, key](const QString& text) {
                    doc_.setParam(node, key, text.toStdString());
                };
                connect(box, &QComboBox::activated, this, [box, apply](int) { apply(box->currentText()); });
                if (spec.openChoice && box->lineEdit())
                    connect(box->lineEdit(), &QLineEdit::editingFinished, this, [box, apply] { apply(box->currentText()); });
                // (A choice stored as an index, e.g. from a script, shows by index.)
                refreshValue = [box, current] {
                    QSignalBlocker b(box);
                    const comp::ParamValue v = current();
                    if (auto* i = std::get_if<int>(&v)) {
                        box->setCurrentIndex(*i);
                        return;
                    }
                    const QString text = asText(v);
                    const int i = box->findText(text);
                    if (i >= 0) box->setCurrentIndex(i);
                    else if (box->isEditable()) box->setEditText(text);
                };
                break;
            }
            case Kind::String:
            case Kind::Path:  // edited by drawing in the viewer; the text is there to read or paste
            case Kind::File: {
                auto* edit = new QLineEdit;
                h->addWidget(edit, 1);
                connect(edit, &QLineEdit::editingFinished, this, [this, &node, key, edit] {
                    doc_.setParam(node, key, edit->text().toStdString());
                });
                if (spec.kind == Kind::File) {
                    auto* browse = new QToolButton;
                    browse->setText("...");
                    h->addWidget(browse);
                    const bool saving = std::string(node.typeName()) == "Write";
                    connect(browse, &QToolButton::clicked, this, [this, &node, key, edit, saving] {
                        const QString f = saving ? QFileDialog::getSaveFileName(this, "Output file", edit->text())
                                                 : QFileDialog::getOpenFileName(this, "File", edit->text());
                        if (!f.isEmpty()) {
                            edit->setText(f);
                            doc_.setParam(node, key, f.toStdString());
                        }
                    });
                }
                refreshValue = [edit, current] {
                    QSignalBlocker b(edit);
                    edit->setText(asText(current()));
                };
                break;
            }
        }
        h->addWidget(reset);
        form->addRow(label, row);

        refreshers_.push_back([refreshValue, label, reset, &node, key] {
            refreshValue();
            const bool set = node.hasParam(key);
            QFont f = label->font();
            f.setBold(set);
            label->setFont(f);
            reset->setEnabled(set);
        });
    }

    // Parameters the node type doesn't describe (set by a script, say):
    // shown so nothing is hidden, but not editable here.
    QStringList extra;
    for (const auto& [key, value] : node.params())
        if (std::find(described.begin(), described.end(), key) == described.end())
            extra << QString("%1 = %2").arg(QString::fromStdString(key), asText(value));
    if (!extra.isEmpty()) {
        auto* other = new QLabel("Other parameters:\n" + extra.join("\n"));
        other->setStyleSheet("color: #999;");
        outer->addWidget(other);
    }

    outer->addStretch();
    setWidget(page);
    refresh();
}

}  // namespace compgui

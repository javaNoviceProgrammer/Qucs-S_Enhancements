/***************************************************************************
                               exportdialog.cpp
                              ------------------
    begin                : Thu Nov 28 2013
    copyright            : (C) 2013 by Vadim Kuznetzov
    email                : <ra3xdh@gmail.com>
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/
#include "exportdialog.h"
#include "settings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include <cmath>

using namespace qucs_s::graphicsexport;

namespace {

constexpr double MinScale = 0.1;
constexpr double MaxScale = 20.0;

QSpinBox* pixels()
{
    auto* box = new QSpinBox;
    box->setRange(1, 64000);
    box->setSuffix(QObject::tr(" px"));
    box->setKeyboardTracking(false);
    return box;
}

// Shows or hides a row of the form: its field and its label.
void showRow(QFormLayout* form, QWidget* field, bool visible)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    form->setRowVisible(field, visible);
#else
    field->setVisible(visible);
    if (QWidget* label = form->labelForField(field))
        label->setVisible(visible);
#endif
}

} // namespace

ExportDialog::ExportDialog(QSize size, QSize selectionSize, const QString& fileName, QWidget* parent)
    : QDialog(parent), m_size(size), m_selectionSize(selectionSize)
{
    setWindowTitle(tr("Export Graphics"));

    m_form = new QFormLayout;

    m_file = new QLineEdit(fileName);
    m_file->setMinimumWidth(380);
    auto* browseButton = new QPushButton(tr("Browse..."));
    auto* fileRow = new QHBoxLayout;
    fileRow->addWidget(m_file, 1);
    fileRow->addWidget(browseButton);
    m_form->addRow(tr("File:"), fileRow);

    m_format = new QComboBox;
    for (const Format f : formats())
        m_format->addItem(nameFilter(f), int(f));
    m_form->addRow(tr("Format:"), m_format);

    m_selection = new QCheckBox(tr("Selected elements only"));
    m_selection->setEnabled(!selectionSize.isEmpty());
    m_form->addRow(QString(), m_selection);

    m_scale = new QDoubleSpinBox;
    m_scale->setRange(MinScale, MaxScale);
    m_scale->setDecimals(2);
    m_scale->setSingleStep(0.25);
    m_scale->setKeyboardTracking(false);
    m_dpi = new QSpinBox;
    m_dpi->setRange(int(std::ceil(MinScale * 96)), int(MaxScale * 96));
    m_dpi->setSuffix(tr(" dpi"));
    m_dpi->setKeyboardTracking(false);
    m_scaleRow = new QWidget;
    auto* scaleLayout = new QHBoxLayout(m_scaleRow);
    scaleLayout->setContentsMargins(0, 0, 0, 0);
    scaleLayout->addWidget(m_scale);
    scaleLayout->addWidget(new QLabel(QStringLiteral("=")));
    scaleLayout->addWidget(m_dpi);
    scaleLayout->addStretch();
    m_form->addRow(tr("Scale:"), m_scaleRow);

    m_width = pixels();
    m_height = pixels();
    m_sizeRow = new QWidget;
    auto* sizeLayout = new QHBoxLayout(m_sizeRow);
    sizeLayout->setContentsMargins(0, 0, 0, 0);
    sizeLayout->addWidget(m_width);
    sizeLayout->addWidget(new QLabel(QStringLiteral("×")));
    sizeLayout->addWidget(m_height);
    sizeLayout->addStretch();
    m_form->addRow(tr("Size:"), m_sizeRow);

    m_physical = new QLabel;
    m_form->addRow(tr("Size:"), m_physical);

    m_colours = new QComboBox;
    m_colours->addItem(tr("Colour"), int(Colours::Colour));
    m_colours->addItem(tr("Grayscale"), int(Colours::Grayscale));
    m_colours->addItem(tr("Black and white"), int(Colours::Monochrome));
    m_form->addRow(tr("Colours:"), m_colours);

    m_transparent = new QCheckBox(tr("Transparent background"));
    m_form->addRow(QString(), m_transparent);

    m_quality = new QSpinBox;
    m_quality->setRange(1, 100);
    m_form->addRow(tr("Quality:"), m_quality);

    m_outlines = new QCheckBox(tr("Text as outlines"));
    m_outlines->setToolTip(tr("The outlines of the letters instead of text: the file looks the same "
                              "everywhere, without the font, but its text cannot be edited or searched."));
    m_form->addRow(QString(), m_outlines);

    m_note = new QLabel;
    m_note->setWordWrap(true);
    m_form->addRow(m_note);

    auto* buttons = new QDialogButtonBox;
    buttons->addButton(tr("Export"), QDialogButtonBox::AcceptRole)->setDefault(true);
    m_copy = buttons->addButton(tr("Copy to Clipboard"), QDialogButtonBox::ActionRole);
    m_copy->setToolTip(tr("An image, an SVG and a PDF of the drawing, for another program to paste"));
    buttons->addButton(QDialogButtonBox::Cancel);

    auto* top = new QVBoxLayout(this);
    top->addLayout(m_form);
    top->addWidget(buttons);
    top->setSizeConstraint(QLayout::SetFixedSize);

    load();
    if (const auto f = formatOf(fileName)) {
        const int index = m_format->findData(int(*f));
        if (index >= 0)
            m_format->setCurrentIndex(index);
    }

    connect(browseButton, &QPushButton::clicked, this, &ExportDialog::browse);
    connect(m_file, &QLineEdit::textEdited, this, &ExportDialog::fileEdited);
    connect(m_format, &QComboBox::currentIndexChanged, this, &ExportDialog::formatChosen);
    connect(m_selection, &QCheckBox::toggled, this, &ExportDialog::selectionToggled);
    connect(m_scale, &QDoubleSpinBox::valueChanged, this, [this](double v) { setScale(v, m_scale); });
    connect(m_dpi, &QSpinBox::valueChanged, this, [this](int v) { setScale(v / 96.0, m_dpi); });
    connect(m_width, &QSpinBox::valueChanged, this,
            [this](int v) { setScale(double(v) / std::max(1, drawnSize().width()), m_width); });
    connect(m_height, &QSpinBox::valueChanged, this,
            [this](int v) { setScale(double(v) / std::max(1, drawnSize().height()), m_height); });
    connect(m_outlines, &QCheckBox::toggled, this, &ExportDialog::outlinesToggled);
    connect(buttons, &QDialogButtonBox::accepted, this, &ExportDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &ExportDialog::reject);
    connect(m_copy, &QPushButton::clicked, this, [this] {
        store();
        done(Copied);
    });

    setScale(m_scaleValue, nullptr);
    updateControls();
}

void ExportDialog::setDiagram()
{
    m_selection->setChecked(true);
    m_selection->setEnabled(false);
    setWindowTitle(tr("Export Diagram"));
}

QString ExportDialog::fileName() const
{
    const QString text = m_file->text().trimmed();
    return text.isEmpty() ? text : withSuffix(text, format());
}

Format ExportDialog::format() const
{
    return Format(m_format->currentData().toInt());
}

Options ExportDialog::options() const
{
    Options o;
    o.selectionOnly = m_selection->isChecked();
    o.scale = m_scaleValue;
    o.colours = Colours(m_colours->currentData().toInt());
    o.transparent = m_transparent->isEnabled() && m_transparent->isChecked();
    o.quality = m_quality->value();
    switch (format()) {
    case Format::Eps:    o.textAsOutlines = true; break;
    case Format::PdfTex: o.textAsOutlines = false; break;
    default:             o.textAsOutlines = m_outlines->isChecked(); break;
    }
    return o;
}

void ExportDialog::accept()
{
    const QString name = fileName();
    if (name.isEmpty()) {
        QMessageBox::warning(this, windowTitle(), tr("Give the file to export to."));
        return;
    }
    // A suffix of a format this build cannot write (TIFF, WebP without
    // the qtimageformats plugins) is not quietly changed into another.
    if (const auto typed = formatOf(m_file->text().trimmed()); typed && m_format->findData(int(*typed)) < 0) {
        QMessageBox::warning(this, windowTitle(),
                             tr("This build of Qucs-S cannot write a %1.").arg(description(*typed)));
        return;
    }
    const QString folder = QFileInfo(name).absolutePath();
    if (!QFileInfo(folder).isDir()) {
        QMessageBox::warning(this, windowTitle(),
                             tr("The folder %1 does not exist.").arg(QDir::toNativeSeparators(folder)));
        return;
    }
    store();
    QDialog::accept();
}

void ExportDialog::browse()
{
    QString filter = nameFilter(format());
    QString name = QFileDialog::getSaveFileName(this, tr("Export to File"), fileName(), nameFilters(),
                                                &filter, QFileDialog::DontConfirmOverwrite);
    if (name.isEmpty())
        return;
    // A name without the suffix of a format takes the one of the filter.
    std::optional<Format> chosen = formatOf(name);
    if (!chosen) {
        for (const Format f : formats())
            if (nameFilter(f) == filter)
                chosen = f;
        if (!chosen)
            chosen = format();
        name = withSuffix(name, *chosen);
    }
    const int index = m_format->findData(int(*chosen));
    if (index >= 0) {
        const QSignalBlocker blocker(m_format);
        m_format->setCurrentIndex(index);
    }
    m_file->setText(name);
    updateControls();
}

void ExportDialog::fileEdited(const QString& text)
{
    if (const auto f = formatOf(text)) {
        const int index = m_format->findData(int(*f));
        if (index >= 0 && index != m_format->currentIndex()) {
            const QSignalBlocker blocker(m_format);
            m_format->setCurrentIndex(index);
        }
    }
    updateControls();
}

void ExportDialog::formatChosen()
{
    const QString text = m_file->text().trimmed();
    if (!text.isEmpty())
        m_file->setText(withSuffix(text, format()));
    updateControls();
}

void ExportDialog::selectionToggled()
{
    setScale(m_scaleValue, nullptr);
}

void ExportDialog::outlinesToggled(bool on)
{
    if (format() == Format::Svg)
        m_outlinesSvg = on;
    else if (format() == Format::Pdf)
        m_outlinesPdf = on;
}

QSize ExportDialog::drawnSize() const
{
    return m_selection->isChecked() ? m_selectionSize : m_size;
}

// The scale, its resolution and the pixels it makes follow each other;
// \a from is the one edited.
void ExportDialog::setScale(double scale, const QObject* from)
{
    m_scaleValue = qBound(MinScale, scale, MaxScale);
    const QSize drawn = drawnSize();
    const QSignalBlocker b1(m_scale), b2(m_dpi), b3(m_width), b4(m_height);
    if (from != m_scale)
        m_scale->setValue(m_scaleValue);
    if (from != m_dpi)
        m_dpi->setValue(int(std::lround(m_scaleValue * 96)));
    if (from != m_width)
        m_width->setValue(std::max(1, int(std::lround(drawn.width() * m_scaleValue))));
    if (from != m_height)
        m_height->setValue(std::max(1, int(std::lround(drawn.height() * m_scaleValue))));
    // A unit of the schematic is 1/96 inch in a vector file.
    m_physical->setText(tr("%1 × %2 cm")
                            .arg(drawn.width() * 2.54 / 96.0, 0, 'f', 1)
                            .arg(drawn.height() * 2.54 / 96.0, 0, 'f', 1));
}

void ExportDialog::updateControls()
{
    const Format f = format();
    const bool raster = !isVector(f);
    showRow(m_form, m_scaleRow, raster);
    showRow(m_form, m_sizeRow, raster);
    showRow(m_form, m_physical, !raster);
    showRow(m_form, m_quality, hasQuality(f));
    showRow(m_form, m_outlines, !raster);
    m_transparent->setEnabled(hasTransparency(f));
    {
        const QSignalBlocker blocker(m_outlines);
        switch (f) {
        case Format::Svg:
            m_outlines->setEnabled(true);
            m_outlines->setChecked(m_outlinesSvg);
            break;
        case Format::Pdf:
            m_outlines->setEnabled(true);
            m_outlines->setChecked(m_outlinesPdf);
            break;
        case Format::Eps:
            m_outlines->setEnabled(false);
            m_outlines->setChecked(true);
            break;
        case Format::PdfTex:
            m_outlines->setEnabled(false);
            m_outlines->setChecked(false);
            break;
        default:
            break;
        }
    }
    QString note;
    if (f == Format::Eps) {
        note = tr("EPS has the text as outlines.");
    } else if (f == Format::PdfTex) {
        const QString name = fileName();
        const QString texName = name.isEmpty() ? QStringLiteral("NAME.pdf_tex") : QFileInfo(name).fileName();
        const QString pdfName = QFileInfo(pdfOf(texName)).fileName();
        note = tr("Writes the drawing to %1 and its text to %2, for LaTeX to set: "
                  "\\input{%2} in the document, with graphicx and xcolor loaded.")
                   .arg(pdfName, texName);
    }
    m_note->setText(note);
    showRow(m_form, m_note, !note.isEmpty());
}

void ExportDialog::load()
{
    QucsSettingsFile settings;
    settings.beginGroup(QStringLiteral("Export"));
    m_scaleValue = qBound(MinScale, settings.value(QStringLiteral("Scale"), 1.0).toDouble(), MaxScale);
    const int colours = m_colours->findData(settings.value(QStringLiteral("Colours"), 0).toInt());
    m_colours->setCurrentIndex(std::max(0, colours));
    m_transparent->setChecked(settings.value(QStringLiteral("Transparent"), false).toBool());
    m_quality->setValue(settings.value(QStringLiteral("Quality"), 90).toInt());
    m_outlinesSvg = settings.value(QStringLiteral("SvgTextAsOutlines"), true).toBool();
    m_outlinesPdf = settings.value(QStringLiteral("PdfTextAsOutlines"), false).toBool();
    settings.endGroup();
}

void ExportDialog::store() const
{
    QucsSettingsFile settings;
    settings.beginGroup(QStringLiteral("Export"));
    settings.setValue(QStringLiteral("Scale"), m_scaleValue);
    settings.setValue(QStringLiteral("Colours"), m_colours->currentData().toInt());
    settings.setValue(QStringLiteral("Transparent"), m_transparent->isChecked());
    settings.setValue(QStringLiteral("Quality"), m_quality->value());
    settings.setValue(QStringLiteral("SvgTextAsOutlines"), m_outlinesSvg);
    settings.setValue(QStringLiteral("PdfTextAsOutlines"), m_outlinesPdf);
    settings.endGroup();
}

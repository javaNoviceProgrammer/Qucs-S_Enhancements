/*
 * dataexportpanel.cpp - the Export tab of a diagram's dialog
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "dataexportpanel.h"

#include "dataimport.h"
#include "misc.h"

#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSet>
#include <QTableWidget>
#include <QShowEvent>
#include <QVBoxLayout>

namespace de = qucs_s::dataexport;
namespace ds = qucs_s::dataset;

namespace {

// A dataset's name without .dat and its simulator ("rc" of rc.dat.ngspice).
QString baseOf(const QString& file)
{
    const QString name = QFileInfo(file).fileName();
    const qsizetype dot = name.toLower().lastIndexOf(QLatin1String(".dat"));
    return dot > 0 ? name.left(dot) : QFileInfo(file).completeBaseName();
}

} // namespace

DataExportPanel::DataExportPanel(const QString& folder, QWidget* parent) : QWidget(parent), a_folder(folder)
{
    setObjectName(QStringLiteral("dataExportPanel"));
    auto* layout = new QVBoxLayout(this);
    auto* about = new QLabel(this);
    about->setWordWrap(true);
    about->setText(folder.isEmpty()
                       ? tr("Save the schematic first: its datasets are kept beside it.")
                       : tr("A dataset's variables written to a file for another program - a spreadsheet, a script, a "
                            "report: CSV, TSV, Excel (.xlsx), text in columns, NumPy (.npz) or a Qucs-S dataset. Variables "
                            "over the same sweep make a table, a row for each of its points; a complex one takes two "
                            "columns."));
    layout->addWidget(about);

    auto* top = new QHBoxLayout;
    top->addWidget(new QLabel(tr("Dataset:"), this));
    a_dataset = new QComboBox(this);
    a_dataset->setObjectName(QStringLiteral("exportDataset"));
    top->addWidget(a_dataset, 1);
    layout->addLayout(top);

    a_list = new QTableWidget(0, 4, this);
    a_list->setObjectName(QStringLiteral("exportVariables"));
    a_list->setHorizontalHeaderLabels({tr("Variable"), tr("Kind"), tr("Over"), tr("Points")});
    a_list->verticalHeader()->setVisible(false);
    a_list->setSelectionBehavior(QAbstractItemView::SelectRows);
    a_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    a_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    a_list->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    a_list->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    layout->addWidget(a_list, 1);

    auto* buttons = new QHBoxLayout;
    a_all = new QPushButton(tr("All"), this);
    a_all->setToolTip(tr("Check every variable"));
    a_none = new QPushButton(tr("None"), this);
    a_none->setToolTip(tr("Uncheck every variable"));
    a_diagramTraces = new QPushButton(tr("The Diagram's Traces"), this);
    a_diagramTraces->setObjectName(QStringLiteral("exportDiagramTraces"));
    a_diagramTraces->setToolTip(tr("Check the variables this diagram's traces show"));
    buttons->addWidget(a_all);
    buttons->addWidget(a_none);
    buttons->addWidget(a_diagramTraces);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    auto* options = new QGroupBox(tr("Write as"), this);
    auto* grid = new QGridLayout(options);
    grid->addWidget(new QLabel(tr("Format:"), options), 0, 0);
    a_format = new QComboBox(options);
    a_format->setObjectName(QStringLiteral("exportFormat"));
    for (de::Format f : de::formats()) a_format->addItem(QStringLiteral("%1 (.%2)").arg(de::formatName(f), de::suffixOf(f)), int(f));
    grid->addWidget(a_format, 0, 1);
    a_complexLabel = new QLabel(tr("Complex values as:"), options);
    grid->addWidget(a_complexLabel, 1, 0);
    a_complex = new QComboBox(options);
    a_complex->setObjectName(QStringLiteral("exportComplex"));
    a_complex->addItem(tr("Real and imaginary parts"), int(ds::Form::RealImaginary));
    a_complex->addItem(tr("Magnitude and phase (degrees)"), int(ds::Form::MagnitudePhase));
    a_complex->addItem(tr("dB and phase (degrees)"), int(ds::Form::DbPhase));
    a_complex->setToolTip(tr("A complex variable's two columns in a table (NumPy and a dataset keep it complex)"));
    grid->addWidget(a_complex, 1, 1);
    grid->setColumnStretch(1, 1);
    layout->addWidget(options);

    auto* bottom = new QHBoxLayout;
    a_summary = new QLabel(this);
    a_summary->setObjectName(QStringLiteral("exportSummary"));
    a_summary->setWordWrap(true);
    bottom->addWidget(a_summary, 1);
    a_export = new QPushButton(tr("Export..."), this);
    a_export->setObjectName(QStringLiteral("exportButton"));
    a_export->setToolTip(tr("Write the variables checked to a file"));
    bottom->addWidget(a_export, 0, Qt::AlignTop);
    layout->addLayout(bottom);

    a_status = new QLabel(this);
    a_status->setObjectName(QStringLiteral("exportStatus"));
    a_status->setWordWrap(true);
    a_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(a_status);

    const bool can = !folder.isEmpty();
    a_dataset->setEnabled(can);
    a_list->setEnabled(can);
    options->setEnabled(can);
    connect(a_dataset, &QComboBox::activated, this, [this] {
        a_chosenHere = true;
        readDataset();
    });
    connect(a_list, &QTableWidget::itemChanged, this, [this] {
        if (!a_filling) updateSummary();
    });
    connect(a_all, &QPushButton::clicked, this, [this] {
        QStringList all;
        for (int row = 0; row < a_list->rowCount(); ++row) all << a_list->item(row, 0)->text();
        choose(all);
    });
    connect(a_none, &QPushButton::clicked, this, [this] { choose({}); });
    connect(a_diagramTraces, &QPushButton::clicked, this, &DataExportPanel::checkTraces);
    connect(a_format, &QComboBox::currentIndexChanged, this, &DataExportPanel::updateSummary);
    connect(a_complex, &QComboBox::currentIndexChanged, this, &DataExportPanel::updateSummary);
    connect(a_export, &QPushButton::clicked, this, &DataExportPanel::exportFile);
    refresh();
}

void DataExportPanel::refresh()
{
    const QString before = dataset();
    a_filling = true;
    a_dataset->clear();
    if (!a_folder.isEmpty()) {
        const QDir dir(a_folder);
        QSet<QString> imported;
        for (const qucs_s::dataimport::Imported& i : qucs_s::dataimport::importedIn(a_folder)) imported.insert(i.name);
        const QStringList files =
            dir.entryList({QStringLiteral("*.dat"), QStringLiteral("*.dat.ngspice"), QStringLiteral("*.dat.xyce"), QStringLiteral("*.dat.spopus")},
                          QDir::Files, QDir::Name);
        for (const QString& f : files) {
            const bool own = f.endsWith(QLatin1String(".dat")) && imported.contains(f.chopped(4));
            a_dataset->addItem(own ? tr("%1  (imported)").arg(f) : f, dir.absoluteFilePath(f));
        }
    }
    const int at = a_dataset->findData(before);
    if (at >= 0) a_dataset->setCurrentIndex(at);
    a_filling = false;
    if (dataset() != before || !a_read) load();
    else updateSummary();
}

void DataExportPanel::load()
{
    if (isVisible()) {
        readDataset();
        return;
    }
    a_stale = true;
    a_read = false;
    a_data = ds::Dataset();
    a_filling = true;
    a_list->setRowCount(0);
    a_filling = false;
    a_summary->clear();
    a_export->setEnabled(false);
}

void DataExportPanel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (a_stale) readDataset();
}

void DataExportPanel::follow(const QString& file)
{
    if (a_chosenHere || file.isEmpty()) return;
    const int at = a_dataset->findData(QFileInfo(file).absoluteFilePath());
    if (at < 0 || at == a_dataset->currentIndex()) return;
    a_dataset->setCurrentIndex(at);
    load();
}

QString DataExportPanel::dataset() const
{
    return a_dataset->currentData().toString();
}

bool DataExportPanel::chooseDataset(const QString& file)
{
    const int at = a_dataset->findData(QFileInfo(file).absoluteFilePath());
    if (at < 0) return false;
    a_chosenHere = true;
    if (at != a_dataset->currentIndex() || !a_read) {
        a_dataset->setCurrentIndex(at);
        readDataset();
    }
    return true;
}

void DataExportPanel::readDataset()
{
    a_stale = false;
    a_data = ds::Dataset();
    a_read = !dataset().isEmpty() && a_data.read(dataset());
    a_filling = true;
    a_list->setRowCount(0);
    int row = 0;
    for (const ds::Variable& v : a_data.variables()) {
        if (v.name.startsWith(QLatin1Char('_'))) continue;   // (the Data tab hides them too)
        a_list->setRowCount(row + 1);
        auto* name = new QTableWidgetItem(v.name);
        name->setFlags((name->flags() | Qt::ItemIsUserCheckable) & ~Qt::ItemIsEditable);
        name->setCheckState(Qt::Unchecked);
        a_list->setItem(row, 0, name);
        QString kind = ds::isOperatingPointValue(a_data, v) ? tr("value") : v.independent ? tr("independent") : tr("dependent");
        if (v.isComplex()) kind += tr(", complex");
        a_list->setItem(row, 1, new QTableWidgetItem(kind));
        a_list->setItem(row, 2, new QTableWidgetItem(v.dependencies.join(QStringLiteral(", "))));
        a_list->setItem(row, 3, new QTableWidgetItem(QString::number(v.size())));
        ++row;
    }
    a_filling = false;
    a_status->clear();
    updateSummary();
}

QStringList DataExportPanel::chosen() const
{
    QStringList names;
    for (int row = 0; row < a_list->rowCount(); ++row)
        if (a_list->item(row, 0)->checkState() == Qt::Checked) names << a_list->item(row, 0)->text();
    return names;
}

void DataExportPanel::choose(const QStringList& names)
{
    if (a_stale) readDataset();
    a_filling = true;
    for (int row = 0; row < a_list->rowCount(); ++row)
        a_list->item(row, 0)->setCheckState(names.contains(a_list->item(row, 0)->text()) ? Qt::Checked : Qt::Unchecked);
    a_filling = false;
    updateSummary();
}

de::Options DataExportPanel::options() const
{
    de::Options o;
    o.format = de::Format(a_format->currentData().toInt());
    o.complex = ds::Form(a_complex->currentData().toInt());
    return o;
}

void DataExportPanel::updateSummary()
{
    const de::Options o = options();
    const QStringList names = chosen();
    bool anyComplex = false;
    for (const QString& n : names)
        if (const ds::Variable* v = a_data.find(n); v != nullptr && v->isComplex()) anyComplex = true;
    a_complexLabel->setEnabled(de::isTable(o.format) && anyComplex);
    a_complex->setEnabled(de::isTable(o.format) && anyComplex);

    QStringList notes;
    const QList<de::Table> tables = de::tablesOf(a_data, names, &notes);
    QString text;
    bool can = false;
    if (a_folder.isEmpty()) {
    } else if (a_dataset->count() == 0) {
        text = tr("No dataset here yet: simulate, or import one.");
    } else if (!a_read) {
        text = tr("%1 cannot be read.").arg(QFileInfo(dataset()).fileName());
    } else if (names.isEmpty()) {
        text = tr("Check the variables to export.");
    } else if (tables.isEmpty()) {
        text = notes.join(QLatin1Char(' '));
    } else if (de::oneTable(o.format) && tables.size() > 1) {
        QStringList over;
        for (const de::Table& t : tables) over << t.name();
        text = tr("These are over %1, and %2 holds one table: check variables of one of them, or choose an Excel workbook (a "
                  "sheet each) or NumPy.")
                   .arg(over.join(tr("; ")), de::formatName(o.format));
    } else {
        can = true;
        if (de::isTable(o.format)) {
            QStringList parts;
            for (const de::Table& t : tables) {
                int columns = 0;
                for (const QString& n : t.independents + t.dependents) columns += int(de::columnsOf(*a_data.find(n), o.complex).size());
                parts << tr("over %1, %2 rows of %3 columns").arg(t.name()).arg(t.rows).arg(columns);
            }
            text = (tables.size() > 1 ? tr("A sheet for each: %1.") : tr("A table %1.")).arg(parts.join(tr("; ")));
        } else {
            QStringList all;
            for (const de::Table& t : tables)
                for (const QString& n : t.independents + t.dependents)
                    if (!all.contains(n)) all << n;
            const QString shown = all.mid(0, 8).join(QStringLiteral(", ")) + (all.size() > 8 ? tr(" and %1 more").arg(all.size() - 8) : QString());
            text = o.format == de::Format::Npz ? tr("An array for each: %1.").arg(shown)
                                               : tr("The variables %1, as the dataset has them.").arg(shown);
        }
        if (!notes.isEmpty()) text += QLatin1Char(' ') + notes.join(QLatin1Char(' '));
    }
    a_summary->setText(text);
    a_export->setEnabled(can);
}

void DataExportPanel::checkTraces()
{
    if (!a_traces) return;
    if (a_stale) readDataset();
    const QList<QPair<QString, QString>> traces = a_traces();
    // Those of the dataset shown; else those of the first dataset the
    // traces read that is here.
    const auto of = [&traces](const QString& file) {
        QStringList names;
        for (const auto& [f, v] : traces)
            if (!file.isEmpty() && misc::isSameFile(f, file) && !names.contains(v)) names << v;
        return names;
    };
    QStringList names = of(dataset());
    if (names.isEmpty())
        for (const auto& [f, v] : traces)
            if (a_dataset->findData(QFileInfo(f).absoluteFilePath()) >= 0 && chooseDataset(f)) {
                names = of(dataset());
                break;
            }
    QStringList found;
    for (const QString& n : std::as_const(names))
        if (a_data.find(n) != nullptr) found << n;
    if (found.isEmpty()) {
        a_status->setText(tr("No trace of this diagram shows a variable of a dataset here."));
        return;
    }
    choose(found);
    a_status->setText(tr("The diagram's traces: %1.").arg(found.join(QStringLiteral(", "))));
}

bool DataExportPanel::exportTo(const QString& path, QString* error)
{
    const auto fail = [this, error](const QString& why) {
        a_status->setText(why);
        if (error != nullptr) *error = why;
        return false;
    };
    if (a_stale) readDataset();
    if (!a_read) return fail(tr("Choose a dataset."));
    if (misc::isSameFile(path, dataset())) return fail(tr("%1 is the dataset itself: choose another file.").arg(QFileInfo(path).fileName()));
    // Read again: a simulation may have written it since.
    ds::Dataset now;
    QString why;
    if (!now.read(dataset(), &why)) return fail(why);
    de::Written w;
    if (!de::write(path, now, chosen(), options(), &w, &why)) return fail(why);
    QString what;
    switch (options().format) {
    case de::Format::Xlsx:
        what = w.tables > 1 ? tr("%1 sheets, %2 columns in all").arg(w.tables).arg(w.columns)
                            : tr("%1 rows of %2 columns").arg(w.rows).arg(w.columns);
        break;
    case de::Format::Npz: what = tr("%1 arrays").arg(w.arrays); break;
    case de::Format::Dataset: what = tr("%1 variables").arg(w.arrays); break;
    default: what = tr("%1 rows of %2 columns").arg(w.rows).arg(w.columns); break;
    }
    QString said = tr("Wrote %1: %2.").arg(QDir::toNativeSeparators(path), what);
    if (!w.notes.isEmpty()) said += QLatin1Char(' ') + w.notes.join(QLatin1Char(' '));
    a_status->setText(said);
    // A dataset written here is one to plot, and to export, too.
    if (options().format == de::Format::Dataset && misc::isSameFile(QFileInfo(path).absolutePath(), a_folder)) refresh();
    return true;
}

void DataExportPanel::exportFile()
{
    const de::Format f = options().format;
    QString base = baseOf(dataset());
    // (A dataset beside it by the same name would be written over.)
    if (f == de::Format::Dataset) base += QStringLiteral("_export");
    const QString proposed = QDir(a_folder).filePath(base + QLatin1Char('.') + de::suffixOf(f));
    QString path = QFileDialog::getSaveFileName(this, tr("Export Data"), proposed, de::filterOf(f));
    if (path.isEmpty()) return;
    if (QFileInfo(path).suffix().isEmpty()) path += QLatin1Char('.') + de::suffixOf(f);
    exportTo(path);
}

/*
 * dataimportpanel.cpp - the Import tab of a diagram's dialog
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "dataimportpanel.h"

#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace di = qucs_s::dataimport;

DataImportPanel::DataImportPanel(const QString& folder, QWidget* parent) : QWidget(parent), a_folder(folder)
{
    setObjectName(QStringLiteral("dataImportPanel"));
    auto* layout = new QVBoxLayout(this);
    auto* about = new QLabel(this);
    about->setWordWrap(true);
    about->setText(folder.isEmpty()
                       ? tr("Save the schematic first: the data you import is kept beside it, as datasets of their own.")
                       : tr("Data files - measurements, a script's results - read into datasets of their own beside the "
                            "schematic, for any diagram to plot next to a simulation's: choose the dataset in the Data tab. "
                            "CSV, TSV, Excel (.xlsx), text in columns, NumPy (.npy, .npz), Touchstone (.s1p ... .snp) and "
                            "Qucs-S datasets."));
    layout->addWidget(about);

    a_list = new QTableWidget(0, 5, this);
    a_list->setObjectName(QStringLiteral("importedDatasets"));
    a_list->setHorizontalHeaderLabels({tr("Dataset"), tr("File"), tr("Format"), tr("Variables"), tr("Points")});
    a_list->verticalHeader()->setVisible(false);
    a_list->setSelectionBehavior(QAbstractItemView::SelectRows);
    a_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    a_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    a_list->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    a_list->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    layout->addWidget(a_list, 1);

    auto* buttons = new QHBoxLayout;
    a_add = new QPushButton(tr("Add Files..."), this);
    a_add->setObjectName(QStringLiteral("importAdd"));
    a_add->setToolTip(tr("Read data files into datasets - as many as you choose"));
    a_reload = new QPushButton(tr("Reload"), this);
    a_reload->setToolTip(tr("Read the datasets chosen again from their files (after they changed)"));
    a_remove = new QPushButton(tr("Remove"), this);
    a_remove->setToolTip(tr("Delete the datasets chosen (not the files they came from)"));
    buttons->addWidget(a_add);
    buttons->addWidget(a_reload);
    buttons->addWidget(a_remove);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    auto* options = new QGroupBox(tr("Read as"), this);
    auto* grid = new QGridLayout(options);
    a_optionsTitle = new QLabel(options);
    grid->addWidget(a_optionsTitle, 0, 0, 1, 2);
    a_sheetLabel = new QLabel(tr("Sheet:"), options);
    a_sheet = new QComboBox(options);
    a_sheet->setObjectName(QStringLiteral("importSheet"));
    grid->addWidget(a_sheetLabel, 1, 0);
    grid->addWidget(a_sheet, 1, 1);
    grid->addWidget(new QLabel(tr("x (independent):"), options), 2, 0);
    a_x = new QComboBox(options);
    a_x->setObjectName(QStringLiteral("importX"));
    a_x->setToolTip(tr("The column the others are plotted over"));
    grid->addWidget(a_x, 2, 1);
    grid->setColumnStretch(1, 1);
    layout->addWidget(options);

    a_status = new QLabel(this);
    a_status->setObjectName(QStringLiteral("importStatus"));
    a_status->setWordWrap(true);
    a_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(a_status);

    const bool can = !folder.isEmpty();
    a_list->setEnabled(can);
    a_add->setEnabled(can);
    options->setEnabled(can);
    connect(a_add, &QPushButton::clicked, this, &DataImportPanel::addFiles);
    connect(a_reload, &QPushButton::clicked, this, [this] { reload(selectedNames()); });
    connect(a_remove, &QPushButton::clicked, this, [this] {
        const QStringList names = selectedNames();
        if (names.isEmpty()) return;
        if (QMessageBox::question(this, tr("Remove Imported Data"),
                                  tr("Delete %1? The traces that show it will show nothing; the file it came from stays.")
                                      .arg(names.join(QStringLiteral(", "))))
            != QMessageBox::Yes)
            return;
        remove(names);
    });
    connect(a_list, &QTableWidget::itemSelectionChanged, this, &DataImportPanel::showOptions);
    const auto optionChanged = [this] {
        if (a_filling) return;
        const QStringList names = selectedNames();
        if (names.size() != 1) return;
        di::Options o;
        o.sheet = a_sheet->isEnabled() ? a_sheet->currentData().toString() : QString();
        o.x = a_x->currentData().toString();
        setOptions(names.first(), o);
    };
    connect(a_sheet, &QComboBox::activated, this, optionChanged);
    connect(a_x, &QComboBox::activated, this, optionChanged);
    refresh();
}

void DataImportPanel::addFiles()
{
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Import Data Files"), a_folder, di::fileFilter());
    if (!files.isEmpty()) importFiles(files);
}

void DataImportPanel::importFiles(const QStringList& files)
{
    if (a_folder.isEmpty()) return;
    QStringList said;
    QString last;
    for (const QString& file : files) {
        di::Imported imported;
        QString error;
        QStringList notes;
        const QString shown = QFileInfo(file).fileName();
        if (!di::importFile(a_folder, file, {}, &imported, &error, &notes)) {
            said << tr("%1: %2").arg(shown, error);
            continue;
        }
        last = imported.name;
        QString line = tr("%1 is the dataset %2: %3 variables, %4 points.")
                           .arg(shown, imported.name)
                           .arg(imported.origin.variables)
                           .arg(imported.origin.points);
        if (!notes.isEmpty()) line += QLatin1Char(' ') + notes.join(QLatin1Char(' '));
        said << line;
    }
    a_status->setText(said.join(QLatin1Char('\n')));
    refresh(last);
    if (!last.isEmpty()) emit datasetsChanged(last);
}

void DataImportPanel::reload(const QStringList& names)
{
    QStringList said;
    for (const QString& name : names)
        for (const di::Imported& i : std::as_const(a_imported)) {
            if (i.name != name) continue;
            di::Imported again;
            QString error;
            QStringList notes;
            if (!QFileInfo::exists(i.origin.source)) {
                said << tr("%1: its file %2 is not there any more.").arg(name, QDir::toNativeSeparators(i.origin.source));
            } else if (!di::importFile(a_folder, i.origin.source, i.origin.options, &again, &error, &notes, name)) {
                said << tr("%1: %2").arg(name, error);
            } else {
                said << tr("%1 read again: %2 variables, %3 points.").arg(name).arg(again.origin.variables).arg(again.origin.points)
                            + (notes.isEmpty() ? QString() : QLatin1Char(' ') + notes.join(QLatin1Char(' ')));
            }
        }
    a_status->setText(said.join(QLatin1Char('\n')));
    refresh(names.value(0));
    emit datasetsChanged(names.value(0));
}

void DataImportPanel::remove(const QStringList& names)
{
    QStringList said;
    for (const QString& name : names)
        for (const di::Imported& i : std::as_const(a_imported))
            if (i.name == name) said << (QFile::remove(i.path) ? tr("%1 removed.").arg(name) : tr("%1 could not be removed.").arg(name));
    a_status->setText(said.join(QLatin1Char('\n')));
    refresh();
    emit datasetsChanged(QString());
}

void DataImportPanel::setOptions(const QString& name, const di::Options& options)
{
    for (const di::Imported& i : std::as_const(a_imported)) {
        if (i.name != name || i.origin.options == options) continue;
        di::Imported again;
        QString error;
        QStringList notes;
        if (!di::importFile(a_folder, i.origin.source, options, &again, &error, &notes, name)) {
            a_status->setText(tr("%1: %2").arg(name, error));
            showOptions();   // (as it was)
            return;
        }
        a_status->setText(tr("%1 read again, x %2: %3 variables, %4 points.")
                              .arg(name, options.x == di::rowX() ? tr("the row") : options.x.isEmpty() ? tr("chosen by itself") : options.x)
                              .arg(again.origin.variables)
                              .arg(again.origin.points)
                          + (notes.isEmpty() ? QString() : QLatin1Char(' ') + notes.join(QLatin1Char(' '))));
        refresh(name);
        emit datasetsChanged(name);
        return;
    }
}

QStringList DataImportPanel::selectedNames() const
{
    QStringList names;
    for (int row = 0; row < a_list->rowCount(); ++row)
        if (a_list->item(row, 0) != nullptr && a_list->item(row, 0)->isSelected()) names << a_list->item(row, 0)->text();
    return names;
}

void DataImportPanel::refresh(const QString& select)
{
    const QStringList selected = select.isEmpty() ? selectedNames() : QStringList{select};
    a_imported = di::importedIn(a_folder);
    a_filling = true;
    a_list->clearSelection();
    a_list->setRowCount(int(a_imported.size()));
    for (int row = 0; row < a_imported.size(); ++row) {
        const di::Imported& i = a_imported.at(row);
        const QStringList cells{i.name, QFileInfo(i.origin.source).fileName(), i.origin.format, QString::number(i.origin.variables),
                                QString::number(i.origin.points)};
        for (int c = 0; c < cells.size(); ++c) {
            auto* item = new QTableWidgetItem(cells.at(c));
            if (c == 1) item->setToolTip(QDir::toNativeSeparators(i.origin.source));
            a_list->setItem(row, c, item);
        }
    }
    for (int row = 0; row < a_imported.size(); ++row)
        if (selected.contains(a_imported.at(row).name)) a_list->selectRow(row);
    a_filling = false;
    showOptions();
}

void DataImportPanel::showOptions()
{
    if (a_filling) return;
    a_filling = true;
    const QStringList names = selectedNames();
    a_reload->setEnabled(!names.isEmpty());
    a_remove->setEnabled(!names.isEmpty());
    a_sheet->clear();
    a_x->clear();
    a_sheet->setEnabled(false);
    a_x->setEnabled(false);
    a_sheetLabel->setVisible(false);
    a_sheet->setVisible(false);
    const di::Imported* chosen = nullptr;
    for (const di::Imported& i : std::as_const(a_imported))
        if (names.size() == 1 && i.name == names.first()) chosen = &i;
    if (chosen == nullptr) {
        a_optionsTitle->setText(names.size() > 1 ? tr("%1 datasets chosen.").arg(names.size())
                                                 : a_imported.isEmpty() ? tr("Nothing imported yet.") : tr("Choose a dataset."));
        a_filling = false;
        return;
    }
    a_optionsTitle->setText(tr("%1, from %2").arg(chosen->name, QDir::toNativeSeparators(chosen->origin.source)));
    // What the file holds now: its sheets, its columns.
    di::Data data;
    QString error;
    if (!QFileInfo::exists(chosen->origin.source) || !di::read(chosen->origin.source, chosen->origin.options, &data, &error)) {
        a_x->addItem(tr("(the file cannot be read now)"));
        a_filling = false;
        return;
    }
    if (!data.sheets.isEmpty()) {
        a_sheetLabel->setVisible(data.sheets.size() > 1);
        a_sheet->setVisible(data.sheets.size() > 1);
        a_sheet->setEnabled(data.sheets.size() > 1);
        for (const QString& s : std::as_const(data.sheets)) a_sheet->addItem(s, s);
        const int at = a_sheet->findData(chosen->origin.options.sheet);
        a_sheet->setCurrentIndex(at >= 0 ? at : 0);
    }
    if (data.columns.isEmpty()) {
        a_x->addItem(tr("%1 (the file's own)").arg(data.x), QString());
    } else {
        a_x->setEnabled(true);
        a_x->addItem(tr("Automatic"), QString());
        a_x->setItemData(0, tr("The first column when it rises or falls steadily, else the row"), Qt::ToolTipRole);
        a_x->addItem(tr("The row (1, 2, 3, ...)"), di::rowX());
        for (const QString& c : std::as_const(data.columns)) a_x->addItem(c, c);
        const int at = a_x->findData(chosen->origin.options.x);
        a_x->setCurrentIndex(at >= 0 ? at : 0);
    }
    a_filling = false;
}

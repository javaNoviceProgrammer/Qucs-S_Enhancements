/*
 * dataimportpanel.h - the Import tab of a diagram's dialog: data files
 *                     read into datasets beside the schematic
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_DATAIMPORTPANEL_H
#define QUCS_DATAIMPORTPANEL_H

#include "dataimport.h"

#include <QHash>
#include <QWidget>

class QComboBox;
class QLabel;
class QPushButton;
class QTableWidget;

/*!
 * The datasets imported into the schematic's folder (dataimport.h), a row
 * each - its name, the file it came from, the file's format, how many
 * variables and points it holds - and Add Files... (any number at once),
 * Reload (read again from their files, which may have changed) and
 * Remove. For the one chosen, its sheet (a workbook's) and which column
 * is x, each read again at once. The Data tab then lists them among the
 * datasets, a trace of one named name:variable - next to the
 * simulation's, in the same diagram.
 *
 * A schematic not saved yet has no folder: nothing is imported, and it
 * says so.
 */
class DataImportPanel : public QWidget
{
    Q_OBJECT

public:
    explicit DataImportPanel(const QString& folder, QWidget* parent = nullptr);

    QString folder() const { return a_folder; }
    /// Imports \a files, each a dataset (one imported before from the same
    /// file is read again); what came of each is said below the list.
    void importFiles(const QStringList& files);
    /// Reads the datasets \a names again from their files.
    void reload(const QStringList& names);
    /// Removes the datasets \a names (their files, not the ones they came
    /// from).
    void remove(const QStringList& names);
    /// The one chosen's options: read again with them.
    void setOptions(const QString& name, const qucs_s::dataimport::Options& options);
    QList<qucs_s::dataimport::Imported> imported() const { return a_imported; }

    // For the tests.
    QTableWidget* list() const { return a_list; }
    QComboBox* xBox() const { return a_x; }
    QComboBox* sheetBox() const { return a_sheet; }
    QLabel* status() const { return a_status; }
    QPushButton* addButton() const { return a_add; }

signals:
    /// Datasets were imported, read again or removed: \a select the one to
    /// show in the Data tab (empty: as it was).
    void datasetsChanged(const QString& select);

private:
    void refresh(const QString& select = QString());
    /// The chosen one's options, as its file has them now.
    void showOptions();
    QStringList selectedNames() const;
    void addFiles();

    QString a_folder;
    QList<qucs_s::dataimport::Imported> a_imported;
    QTableWidget* a_list = nullptr;
    QPushButton* a_add = nullptr;
    QPushButton* a_reload = nullptr;
    QPushButton* a_remove = nullptr;
    QLabel* a_optionsTitle = nullptr;
    QLabel* a_sheetLabel = nullptr;
    QComboBox* a_sheet = nullptr;
    QComboBox* a_x = nullptr;
    QLabel* a_status = nullptr;
    bool a_filling = false;
};

#endif // QUCS_DATAIMPORTPANEL_H

/*
 * dataexportpanel.h - the Export tab of a diagram's dialog: a dataset's
 *                     variables written to a file for other programs
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_DATAEXPORTPANEL_H
#define QUCS_DATAEXPORTPANEL_H

#include "dataexport.h"

#include <QList>
#include <QPair>
#include <QWidget>

#include <functional>

class QComboBox;
class QLabel;
class QPushButton;
class QTableWidget;

class DataExportPanel : public QWidget
{
    Q_OBJECT

public:
    /// The datasets of \a folder (the schematic's: its simulations', those
    /// imported); empty for a schematic not saved, which has none.
    explicit DataExportPanel(const QString& folder, QWidget* parent = nullptr);

    QString folder() const { return a_folder; }
    /// The datasets listed again (one imported, one removed).
    void refresh();
    /// The dataset file the Data tab chose: shown here, until one is
    /// chosen here.
    void follow(const QString& file);
    /// Where "The Diagram's Traces" finds them: each trace's dataset file
    /// and variable.
    void setTraces(std::function<QList<QPair<QString, QString>>()> traces) { a_traces = std::move(traces); }

    /// The dataset file chosen.
    QString dataset() const;
    /// Chooses the dataset \a file (one listed); false when it is not.
    bool chooseDataset(const QString& file);
    /// The variables checked, in the list's order.
    QStringList chosen() const;
    /// Checks \a names (and unchecks the others).
    void choose(const QStringList& names);
    /// The options chosen.
    qucs_s::dataexport::Options options() const;
    /// What is chosen written to \a path (as Export... does after asking
    /// for the file); false and why in \a error, also said below.
    bool exportTo(const QString& path, QString* error = nullptr);

    // For the tests.
    QComboBox* datasetBox() const { return a_dataset; }
    QTableWidget* list() const { return a_list; }
    QComboBox* formatBox() const { return a_format; }
    QComboBox* complexBox() const { return a_complex; }
    QLabel* summary() const { return a_summary; }
    QLabel* status() const { return a_status; }
    QPushButton* exportButton() const { return a_export; }
    QPushButton* tracesButton() const { return a_diagramTraces; }
    /// The file Export... proposes: the dataset's name, beside the
    /// schematic, with the format's suffix - name_export for an imported
    /// one (bench.csv is where bench came from) or a dataset.
    QString proposedFile() const;
    /// An export that takes longer than this shows how far it is, with
    /// Stop (a test makes it 0: at once).
    void setProgressAfter(int milliseconds) { a_progressAfter = milliseconds; }

signals:
    /// A dataset was written beside the schematic: for the Data tab to list.
    void datasetsChanged(const QString& select);

protected:
    void showEvent(QShowEvent* event) override;

private:
    /// Reads the dataset chosen now if the tab is shown, else when it is
    /// shown or used: read in full at once, it slowed the opening of every
    /// diagram dialog, most of which never show the tab.
    void load();
    void readDataset();
    void updateSummary();
    void checkTraces();
    void exportFile();

    QString a_folder;
    std::function<QList<QPair<QString, QString>>()> a_traces;
    qucs_s::dataset::Dataset a_data;
    bool a_read = false;       // a_data is the chosen dataset's
    bool a_stale = true;       // the chosen dataset is still to be read
    bool a_chosenHere = false; // a dataset was chosen in this tab: the Data tab's is not followed
    bool a_filling = false;
    int a_progressAfter = 250;
    QComboBox* a_dataset = nullptr;
    QTableWidget* a_list = nullptr;
    QPushButton* a_all = nullptr;
    QPushButton* a_none = nullptr;
    QPushButton* a_diagramTraces = nullptr;
    QComboBox* a_format = nullptr;
    QLabel* a_complexLabel = nullptr;
    QComboBox* a_complex = nullptr;
    QLabel* a_summary = nullptr;
    QPushButton* a_export = nullptr;
    QLabel* a_status = nullptr;
};

#endif // QUCS_DATAEXPORTPANEL_H

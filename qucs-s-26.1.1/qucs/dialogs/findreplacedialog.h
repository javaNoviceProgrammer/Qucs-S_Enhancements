/*
 * findreplacedialog.h - find component property values in a schematic, the
 * open schematics or the whole project, and replace them
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FINDREPLACEDIALOG_H
#define QUCS_FINDREPLACEDIALOG_H

#include "componentsearch.h"

#include <QDialog>
#include <QPointer>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QTreeWidget;
class QTreeWidgetItem;
class QucsApp;
class Schematic;

/*!
 * \brief Find and Replace for component properties (Edit > Replace, F7).
 *
 * Looks for a text in the property values of the components - of the
 * schematic in front, of every open schematic, or of every schematic of
 * the open project - narrowed to a type of component, to names that fit a
 * wildcard and to one property, and lists every hit with the value it
 * would get. The checked hits are replaced: in an open schematic as one
 * undo step, and a schematic of the project that is not open is opened
 * first and left for the user to save. With no text to find, the
 * property is set whatever its value is (what this dialog's predecessor
 * did). A double click shows the component.
 */
class FindReplaceDialog : public QDialog
{
    Q_OBJECT
public:
    enum Scope { ThisSchematic, OpenSchematics, ProjectSchematics };

    explicit FindReplaceDialog(QucsApp* app);

    void setScope(Scope scope);
    Scope scope() const;
    /// The search the fields describe.
    qucs_s::search::Query query() const;

    /// Prepares the dialog for the schematic in front: its component
    /// types and property names in the lists, the scope enabled as fits.
    void prepare();

    QLineEdit* findEdit() const { return m_find; }
    QLineEdit* replaceEdit() const { return m_replace; }
    QLineEdit* namesEdit() const { return m_names; }
    QComboBox* typeCombo() const { return m_type; }
    QComboBox* propertyCombo() const { return m_property; }
    QCheckBox* caseBox() const { return m_case; }
    QCheckBox* wholeBox() const { return m_whole; }
    QCheckBox* regexBox() const { return m_regex; }
    QTreeWidget* results() const { return m_results; }
    QPushButton* replaceButton() const { return m_replaceButton; }
    QLabel* statusLabel() const { return m_status; }

public slots:
    void search();
    void replaceChecked();

private:
    struct Hit {
        QString file;   // the schematic's file name (empty for an unsaved one)
        QPointer<Schematic> doc;   // an open document, or null for a file on disk
        qucs_s::search::Match match;
    };

    void refreshProperties();   // the property list for the component type chosen
    QList<Schematic*> openSchematics() const;
    QString shownName(const QString& file) const;
    Schematic* openDocument(const Hit& hit);   // the hit's schematic, opened if it has to be
    void reveal(QTreeWidgetItem* item);
    void updatePreview();
    void updateState();

    QucsApp* m_app;
    QLineEdit* m_find;
    QLineEdit* m_replace;
    QCheckBox* m_case;
    QCheckBox* m_whole;
    QCheckBox* m_regex;
    QRadioButton* m_this;
    QRadioButton* m_open;
    QRadioButton* m_project;
    QComboBox* m_type;
    QLineEdit* m_names;
    QComboBox* m_property;
    QTreeWidget* m_results;
    QPushButton* m_replaceButton;
    QLabel* m_status;

    QList<Hit> m_hits;
    qucs_s::search::Query m_searched;   // what the hits were found with
    Scope m_searchedScope = ThisSchematic;
    bool m_haveSearched = false;
};

#endif

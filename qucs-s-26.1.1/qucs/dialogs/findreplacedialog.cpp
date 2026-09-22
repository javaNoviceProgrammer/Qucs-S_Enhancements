/*
 * findreplacedialog.cpp - find component property values in a schematic,
 * the open schematics or the whole project, and replace them
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "findreplacedialog.h"

#include "main.h"
#include "misc.h"
#include "qucs.h"
#include "schematic.h"
#include "components/component.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QGridLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSet>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <memory>
#include <set>

using qucs_s::search::Match;
using qucs_s::search::Query;

namespace {
enum Column { FileColumn, ComponentColumn, TypeColumn, PropertyColumn, ValueColumn, NewValueColumn };
constexpr int HitRole = Qt::UserRole + 1;

// "1 schematic", "3 schematics": no English catalogue is loaded, so Qt's
// %n plural forms would show as written.
QString count(int n, const QString& one, const QString& many)
{
    return (n == 1 ? one : many).arg(n);
}
} // namespace

FindReplaceDialog::FindReplaceDialog(QucsApp* app)
    : QDialog(app), m_app(app)
{
    setWindowTitle(tr("Find and Replace in Components"));
    setModal(false);

    auto* all = new QVBoxLayout(this);
    auto* grid = new QGridLayout;
    all->addLayout(grid);

    int row = 0;
    grid->addWidget(new QLabel(tr("Find:"), this), row, 0);
    m_find = new QLineEdit(this);
    m_find->setPlaceholderText(tr("Text in a property value (empty: any value of the property below)"));
    grid->addWidget(m_find, row++, 1, 1, 5);

    grid->addWidget(new QLabel(tr("Replace with:"), this), row, 0);
    m_replace = new QLineEdit(this);
    m_replace->setPlaceholderText(tr("New text (with a regular expression, \\1 is its first group)"));
    grid->addWidget(m_replace, row++, 1, 1, 5);

    auto* options = new QHBoxLayout;
    m_case = new QCheckBox(tr("Match case"), this);
    m_whole = new QCheckBox(tr("Whole value"), this);
    m_whole->setToolTip(tr("The value is the text, rather than contains it"));
    m_regex = new QCheckBox(tr("Regular expression"), this);
    options->addWidget(m_case);
    options->addWidget(m_whole);
    options->addWidget(m_regex);
    options->addStretch(1);
    grid->addLayout(options, row++, 1, 1, 5);

    grid->addWidget(new QLabel(tr("Look in:"), this), row, 0);
    auto* scopes = new QHBoxLayout;
    m_this = new QRadioButton(tr("This schematic"), this);
    m_open = new QRadioButton(tr("Open schematics"), this);
    m_project = new QRadioButton(tr("All schematics of the project"), this);
    auto* scopeGroup = new QButtonGroup(this);
    for (QRadioButton* b : {m_this, m_open, m_project}) {
        scopeGroup->addButton(b);
        scopes->addWidget(b);
    }
    scopes->addStretch(1);
    m_this->setChecked(true);
    grid->addLayout(scopes, row++, 1, 1, 5);

    grid->addWidget(new QLabel(tr("Component type:"), this), row, 0);
    m_type = new QComboBox(this);
    m_type->setEditable(true);
    m_type->setInsertPolicy(QComboBox::NoInsert);
    m_type->setToolTip(tr("The model of the components to look at (R, C, L, Diode, ...)"));
    grid->addWidget(m_type, row, 1);
    grid->addWidget(new QLabel(tr("Names:"), this), row, 2);
    m_names = new QLineEdit(QStringLiteral("*"), this);
    m_names->setToolTip(tr("Only components whose name fits this pattern (* and ? are wildcards)"));
    grid->addWidget(m_names, row, 3);
    grid->addWidget(new QLabel(tr("Property:"), this), row, 4);
    m_property = new QComboBox(this);
    m_property->setEditable(true);
    m_property->setInsertPolicy(QComboBox::NoInsert);
    grid->addWidget(m_property, row++, 5);
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(3, 1);
    grid->setColumnStretch(5, 1);

    auto* findRow = new QHBoxLayout;
    auto* findButton = new QPushButton(tr("Find"), this);
    findButton->setDefault(true);
    findRow->addWidget(findButton);
    findRow->addStretch(1);
    all->addLayout(findRow);

    m_results = new QTreeWidget(this);
    m_results->setRootIsDecorated(false);
    m_results->setUniformRowHeights(true);
    m_results->setHeaderLabels({tr("Schematic"), tr("Component"), tr("Type"), tr("Property"), tr("Value"),
                                tr("New value")});
    m_results->header()->setStretchLastSection(true);
    m_results->setToolTip(tr("Checked rows are replaced; a double click shows the component"));
    all->addWidget(m_results, 1);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    all->addWidget(m_status);

    auto* buttons = new QDialogButtonBox(this);
    m_replaceButton = buttons->addButton(tr("Replace Checked"), QDialogButtonBox::ActionRole);
    buttons->addButton(QDialogButtonBox::Close);
    all->addWidget(buttons);

    connect(findButton, &QPushButton::clicked, this, &FindReplaceDialog::search);
    connect(m_find, &QLineEdit::returnPressed, this, &FindReplaceDialog::search);
    connect(m_replaceButton, &QPushButton::clicked, this, &FindReplaceDialog::replaceChecked);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    connect(m_results, &QTreeWidget::itemDoubleClicked, this, &FindReplaceDialog::reveal);
    connect(m_results, &QTreeWidget::itemChanged, this, [this] { updateState(); });
    connect(m_replace, &QLineEdit::textChanged, this, &FindReplaceDialog::updatePreview);
    for (QLineEdit* e : {m_find, m_names}) connect(e, &QLineEdit::textChanged, this, [this] { updateState(); });
    for (QCheckBox* b : {m_case, m_whole, m_regex}) connect(b, &QCheckBox::toggled, this, [this] { updateState(); });
    connect(scopeGroup, &QButtonGroup::buttonToggled, this, [this] { updateState(); });
    connect(m_property, &QComboBox::currentTextChanged, this, [this] { updateState(); });
    connect(m_type, &QComboBox::currentTextChanged, this, &FindReplaceDialog::refreshProperties);

    resize(820, 520);
    prepare();
}

void FindReplaceDialog::refreshProperties()
{
    // The properties of the type chosen, from the open schematics.
    const QString keep = m_property->currentText();
    const QString model = query().model;
    std::set<QString> names;
    for (Schematic* doc : openSchematics())
        for (Component* c : doc->a_DocComps)
            if (model.isEmpty() || c->Model == model)
                for (const Property* p : c->Props) names.insert(p->Name);
    {
        QSignalBlocker block(m_property);
        m_property->clear();
        m_property->addItem(tr("Any property"));
        for (const QString& n : names) m_property->addItem(n);
        m_property->setCurrentText(keep.isEmpty() ? m_property->itemText(0) : keep);
    }
    updateState();
}

QList<Schematic*> FindReplaceDialog::openSchematics() const
{
    QList<Schematic*> docs;
    for (QucsDoc* d : m_app->allDocuments())
        if (auto* s = qobject_cast<Schematic*>(QucsApp::documentWidget(d))) docs << s;
    return docs;
}

void FindReplaceDialog::prepare()
{
    // The types of component there are in the open schematics.
    const QString keepType = m_type->currentText();
    std::set<QString> models;
    for (Schematic* doc : openSchematics())
        for (Component* c : doc->a_DocComps) models.insert(c->Model);
    {
        QSignalBlocker block(m_type);
        m_type->clear();
        m_type->addItem(tr("Any type"));
        for (const QString& m : models) m_type->addItem(m);
        m_type->setCurrentText(keepType.isEmpty() ? m_type->itemText(0) : keepType);
    }
    refreshProperties();

    m_this->setEnabled(m_app->currentSchematic() != nullptr);
    m_project->setEnabled(!m_app->ProjName.isEmpty());
    if (!m_this->isEnabled() && m_this->isChecked()) m_open->setChecked(true);
    if (!m_project->isEnabled() && m_project->isChecked()) m_open->setChecked(true);
    updateState();
}

void FindReplaceDialog::setScope(Scope scope)
{
    (scope == ThisSchematic ? m_this : scope == OpenSchematics ? m_open : m_project)->setChecked(true);
}

FindReplaceDialog::Scope FindReplaceDialog::scope() const
{
    return m_this->isChecked() ? ThisSchematic : m_open->isChecked() ? OpenSchematics : ProjectSchematics;
}

Query FindReplaceDialog::query() const
{
    Query q;
    q.text = m_find->text();
    q.names = false;
    q.values = true;
    q.matchCase = m_case->isChecked();
    q.wholeValue = m_whole->isChecked();
    q.regex = m_regex->isChecked();
    const QString type = m_type->currentText().trimmed();
    q.model = type == m_type->itemText(0) ? QString() : type;
    const QString names = m_names->text().trimmed();
    q.namePattern = names == QLatin1String("*") ? QString() : names;
    const QString property = m_property->currentText().trimmed();
    q.property = property == m_property->itemText(0) ? QString() : property;
    return q;
}

QString FindReplaceDialog::shownName(const QString& file) const
{
    if (file.isEmpty()) return tr("(unsaved)");
    if (!m_app->ProjName.isEmpty()) {
        const QString relative = QucsSettings.QucsWorkDir.relativeFilePath(file);
        if (!relative.startsWith(QLatin1String(".."))) return relative;
    }
    return QFileInfo(file).fileName();
}

void FindReplaceDialog::search()
{
    m_hits.clear();
    m_haveSearched = false;
    {
        QSignalBlocker block(m_results);
        m_results->clear();
    }

    const Query q = query();
    QString error;
    if (!q.isValid(&error)) {
        m_status->setText(tr("The regular expression is not valid: %1").arg(error));
        updateState();
        return;
    }
    if (q.text.isEmpty() && q.property.isEmpty()) {
        m_status->setText(tr("Say which property to set, or give a text to find."));
        updateState();
        return;
    }

    // The schematics to look in: open ones as they are (unsaved changes
    // included), the others of the project as they are on disk.
    QList<Hit> hits;
    int documents = 0, symbolMode = 0, unreadable = 0;
    auto look = [&](Schematic* doc, const QString& file, bool open) {
        if (doc->getSymbolMode()) {
            ++symbolMode;
            return;
        }
        ++documents;
        for (const Match& m : qucs_s::search::find(doc, q))
            hits << Hit{file, open ? QPointer<Schematic>(doc) : QPointer<Schematic>(), m};
    };
    switch (scope()) {
    case ThisSchematic:
        if (Schematic* doc = m_app->currentSchematic()) look(doc, doc->getDocName(), true);
        break;
    case OpenSchematics:
        for (Schematic* doc : openSchematics()) look(doc, doc->getDocName(), true);
        break;
    case ProjectSchematics: {
        const QDir root(QucsSettings.QucsWorkDir);
        for (const QString& relative : misc::projectFiles(root, {QStringLiteral("*.sch")})) {
            if (relative.startsWith(QLatin1String(misc::ScratchFolder) + QLatin1Char('/'))) continue;
            const QString file = QDir::cleanPath(root.absoluteFilePath(relative));
            if (auto* open = qobject_cast<Schematic*>(QucsApp::documentWidget(m_app->findDoc(file)))) {
                look(open, file, true);
                continue;
            }
            std::unique_ptr<Schematic> loaded(new Schematic(nullptr, file));
            if (!loaded->load()) {
                ++unreadable;
                continue;
            }
            look(loaded.get(), file, false);
        }
        break;
    }
    }

    m_hits = hits;
    m_searched = q;
    m_searchedScope = scope();
    m_haveSearched = true;
    {
        QSignalBlocker block(m_results);
        for (int i = 0; i < m_hits.size(); ++i) {
            const Hit& h = m_hits.at(i);
            auto* item = new QTreeWidgetItem(m_results);
            item->setText(FileColumn, shownName(h.file));
            item->setToolTip(FileColumn, h.file);
            item->setText(ComponentColumn, h.match.component);
            item->setText(TypeColumn, h.match.model);
            item->setText(PropertyColumn, h.match.property);
            item->setText(ValueColumn, h.match.value);
            item->setData(FileColumn, HitRole, i);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(FileColumn, Qt::Checked);
        }
        for (int c = FileColumn; c < NewValueColumn; ++c) m_results->resizeColumnToContents(c);
    }

    QSet<QString> files;
    for (const Hit& h : m_hits) files.insert(h.file);
    QString status = m_hits.isEmpty()
        ? tr("No matches in %1.").arg(count(documents, tr("%1 schematic"), tr("%1 schematics")))
        : tr("%1 in %2.").arg(count(m_hits.size(), tr("%1 match"), tr("%1 matches")),
                              count(files.size(), tr("%1 schematic"), tr("%1 schematics")));
    if (symbolMode > 0)
        status += QLatin1Char(' ') + count(symbolMode, tr("%1 schematic in symbol editing was not searched."),
                                           tr("%1 schematics in symbol editing were not searched."));
    if (unreadable > 0)
        status += QLatin1Char(' ') + count(unreadable, tr("%1 schematic could not be read."),
                                           tr("%1 schematics could not be read."));
    m_status->setText(status);
    updatePreview();
}

void FindReplaceDialog::updatePreview()
{
    QSignalBlocker block(m_results);
    for (int i = 0; i < m_results->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = m_results->topLevelItem(i);
        const Hit& h = m_hits.at(item->data(FileColumn, HitRole).toInt());
        const QString value = qucs_s::search::replaced(h.match.value, m_searched, m_replace->text());
        QString problem;
        if (value.isEmpty()) problem = tr("a value cannot be empty");
        else if (value.contains(QLatin1Char('"'))) problem = tr("a value cannot hold a double quote");
        item->setText(NewValueColumn, problem.isEmpty() ? value : tr("(left alone: %1)").arg(problem));
        item->setForeground(NewValueColumn, problem.isEmpty() ? palette().text() : palette().placeholderText());
    }
    updateState();
}

void FindReplaceDialog::updateState()
{
    int checked = 0;
    for (int i = 0; i < m_results->topLevelItemCount(); ++i)
        if (m_results->topLevelItem(i)->checkState(FileColumn) == Qt::Checked) ++checked;
    const bool current = m_haveSearched && query() == m_searched && scope() == m_searchedScope;
    m_replaceButton->setEnabled(current && checked > 0);
    m_replaceButton->setToolTip(!m_haveSearched || current ? QString()
                                                           : tr("The search has changed: press Find again"));
}

Schematic* FindReplaceDialog::openDocument(const Hit& hit)
{
    if (hit.doc) return hit.doc;
    if (hit.file.isEmpty()) return nullptr;
    if (auto* open = qobject_cast<Schematic*>(QucsApp::documentWidget(m_app->findDoc(hit.file)))) return open;
    if (!m_app->gotoPage(hit.file)) return nullptr;
    return qobject_cast<Schematic*>(QucsApp::documentWidget(m_app->findDoc(hit.file)));
}

void FindReplaceDialog::replaceChecked()
{
    if (!m_replaceButton->isEnabled()) return;
    // The checked hits, schematic by schematic, in the order found.
    QStringList order;
    QHash<QString, QList<Hit>> byFile;
    for (int i = 0; i < m_results->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = m_results->topLevelItem(i);
        if (item->checkState(FileColumn) != Qt::Checked) continue;
        const Hit& h = m_hits.at(item->data(FileColumn, HitRole).toInt());
        const QString key = h.file.isEmpty() ? QString::number(quintptr(h.doc.data())) : h.file;
        if (!byFile.contains(key)) order << key;
        byFile[key] << h;
    }

    int values = 0, documents = 0, opened = 0, left = 0;
    for (const QString& key : order) {
        const QList<Hit>& hits = byFile.value(key);
        const bool wasOpen = hits.first().doc != nullptr
            || m_app->findDoc(hits.first().file) != nullptr;
        Schematic* doc = openDocument(hits.first());
        if (doc == nullptr || doc->getSymbolMode()) {
            left += hits.size();
            continue;
        }
        if (!wasOpen) ++opened;
        QList<Match> matches;
        for (const Hit& h : hits) matches << h.match;
        int skipped = 0;
        const int n = qucs_s::search::replace(doc, matches, m_searched, m_replace->text(), &skipped);
        left += skipped;
        if (n > 0) {
            doc->setChanged(true, true);   // one undo step for the schematic
            doc->viewport()->update();
            values += n;
            ++documents;
        }
    }

    QString summary = tr("Replaced %1 in %2.").arg(count(values, tr("%1 value"), tr("%1 values")),
                                                   count(documents, tr("%1 schematic"), tr("%1 schematics")));
    if (left > 0)
        summary += QLatin1Char(' ') + count(left, tr("%1 left alone (changed since the search, or the new value is not allowed)."),
                                            tr("%1 left alone (changed since the search, or the new values are not allowed)."));
    if (opened > 0)
        summary += QLatin1Char(' ') + count(opened, tr("%1 schematic was opened for this; save it to keep the change."),
                                            tr("%1 schematics were opened for this; save them to keep the change."));
    search();   // what is there now
    m_status->setText(summary + QLatin1Char('\n') + m_status->text());
    raise();
    activateWindow();
}

void FindReplaceDialog::reveal(QTreeWidgetItem* item)
{
    if (item == nullptr) return;
    const int index = item->data(FileColumn, HitRole).toInt();
    if (index < 0 || index >= m_hits.size()) return;
    Schematic* doc = openDocument(m_hits.at(index));
    if (doc == nullptr) return;
    m_hits[index].doc = doc;   // open now: later clicks and replacements use it
    m_app->showDocument(doc);
    qucs_s::search::reveal(doc, m_hits.at(index).match);
}

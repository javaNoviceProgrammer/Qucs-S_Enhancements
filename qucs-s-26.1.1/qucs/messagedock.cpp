/***************************************************************************
                            messagedock.cpp
                            ---------------
    begin                : Tue Mar 11 2014
    copyright            : (C) 2014 by Guilherme Brondani Torri
    email                : guitorri AT gmail DOT com
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "messagedock.h"

#include "main.h"
#include "qucsdoc.h"
#include "textdoc.h"

#include <QApplication>
#include <QClipboard>
#include <QCollator>
#include <QDir>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QHeaderView>
#include <QSet>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>
#include <QFileInfo>
#include <QStyle>
#include "schematic.h"
#include <QTextBlock>
#include <QDebug>

/*!
 * \file messagedock.cpp
 * \brief Definition of the MessageDock class.
 */

/*!
 * \brief MessageDock::MessageDock constructor
 * \param App_ is the parent class
 * It creates two docked text fields to hold the output of make called
 * over admsXml and the C++ compiler used to build the Verilog-A
 * dynamic loaded libraries.
 * \see QucsApp::slotBuildModule() for the make output assignment.
 */
MessageDock::MessageDock(QucsApp *App_): QWidget()
{

    builderTabs = new QTabWidget();
    // Tabs at the top: the dock shares the bottom of the main window with
    // the simulation console, and the tab bar that switches between the two
    // docks sits below - tabs at the bottom here would stack two tab bars.
    builderTabs->setTabPosition(QTabWidget::North);

    // 1) add a dock for the adms messages
    admsOutput = new QPlainTextEdit();
    admsOutput->setReadOnly(true);

    builderTabs->insertTab(0,admsOutput,tr("admsXml"));


    // 2) add a dock for the cpp compiler messages
    cppOutput = new QPlainTextEdit();
    cppOutput->setReadOnly(true);

    builderTabs->insertTab(1,cppOutput,tr("Compiler"));

    // 3) what the electrical rule check found
    problems = new QListWidget();
    problems->setToolTip(tr("What the check of the schematic found; click a row to see the place."));
    connect(problems, &QListWidget::itemClicked, this, &MessageDock::slotProblemChosen);
    connect(problems, &QListWidget::itemActivated, this, &MessageDock::slotProblemChosen);
    builderTabs->insertTab(2, problems, tr("Problems"));

    // 4) the operating point of every device after a DC bias run
    auto *opPage = new QWidget();
    auto *opLayout = new QVBoxLayout(opPage);
    opLayout->setContentsMargins(0, 0, 0, 0);
    auto *opBar = new QHBoxLayout();
    operatingPointFilter = new QLineEdit();
    operatingPointFilter->setObjectName("operatingPointFilter");
    operatingPointFilter->setPlaceholderText(tr("Filter: a component, a device or a parameter (T1, gm, vth)"));
    operatingPointFilter->setClearButtonEnabled(true);
    auto *copy = new QPushButton(tr("Copy"));
    copy->setObjectName("operatingPointCopy");
    copy->setToolTip(tr("Copies the rows shown, tab-separated, to paste into a spreadsheet"));
    opBar->addWidget(operatingPointFilter, 1);
    opBar->addWidget(copy);
    operatingPoint = new QTreeWidget();
    operatingPoint->setObjectName("operatingPoint");
    operatingPoint->setColumnCount(2);
    operatingPoint->setHeaderLabels({tr("Component / device / parameter"), tr("Value")});
    operatingPoint->setUniformRowHeights(true);
    operatingPoint->setAlternatingRowColors(true);
    operatingPoint->header()->setStretchLastSection(false);
    operatingPoint->setToolTip(tr("The operating point of every device after a DC bias run with ngspice; "
                                  "click a component to see it in the schematic."));
    opLayout->addLayout(opBar);
    opLayout->addWidget(operatingPoint);
    a_operatingPointTab = builderTabs->insertTab(3, opPage, tr("Operating Point"));
    connect(operatingPointFilter, &QLineEdit::textChanged, this, &MessageDock::slotFilterOperatingPoint);
    connect(copy, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(operatingPointText());
    });
    connect(operatingPoint, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) {
        const QString component = item->data(0, Qt::UserRole).toString();
        if (!component.isEmpty()) emit componentRequested(component);
    });
    showOperatingPoint(nullptr, false);

    // 5) where a Python name is used (Find All References)
    auto *refPage = new QWidget();
    auto *refLayout = new QVBoxLayout(refPage);
    refLayout->setContentsMargins(0, 0, 0, 0);
    a_referencesTitle = new QLabel(tr("Find All References (Shift+F12) on a name of a Python script lists where it is used."));
    a_referencesTitle->setObjectName("referencesTitle");
    a_referencesTitle->setTextInteractionFlags(Qt::TextSelectableByMouse);
    references = new QTreeWidget();
    references->setObjectName("references");
    references->setHeaderHidden(true);
    references->setUniformRowHeights(true);
    references->setToolTip(tr("Where the name is used; click a row to go there."));
    refLayout->addWidget(a_referencesTitle);
    refLayout->addWidget(references, 1);
    a_referencesTab = builderTabs->insertTab(4, refPage, tr("References"));
    const auto goTo = [this](QTreeWidgetItem *item) {
      if (item == nullptr || !item->data(0, Qt::UserRole + 1).isValid()) return;
      const QString file = item->data(0, Qt::UserRole).toString();
      const int line = item->data(0, Qt::UserRole + 1).toInt();
      const int column = item->data(0, Qt::UserRole + 2).toInt() + 1;
      if (file.isEmpty()) {
        if (auto *doc = qobject_cast<QWidget *>(a_referencesDoc.data())) emit lineRequested(doc, line, column);
      } else {
        emit placeRequested(file, line, column);
      }
    };
    connect(references, &QTreeWidget::itemClicked, this, goTo);
    connect(references, &QTreeWidget::itemActivated, this, goTo);

    msgDock = new QDockWidget(tr("admsXml Dock"));
    msgDock->setObjectName(QStringLiteral("MessagesDock"));
    msgDock->setWidget(builderTabs);
    App_->addDockWidget(Qt::BottomDockWidgetArea, msgDock);

    // start hidden
    msgDock->hide();

    // monitor the amds output
    connect(admsOutput,SIGNAL(textChanged()), this, SLOT(slotAdmsChanged()));
    // monitor the compiler output
    connect(cppOutput,SIGNAL(textChanged()), this, SLOT(slotCppChanged()));
    // check out if cursor over 'fail' line
    connect(admsOutput, SIGNAL(cursorPositionChanged()), this, SLOT(slotCursor()));
}

/*!
 * \brief MessageDock::reset clear the text and tab icons
 */
void MessageDock::reset()
{
    admsOutput->clear();
    cppOutput->clear();

    builderTabs->setTabIcon(0,QPixmap());
    builderTabs->setTabIcon(1,QPixmap());
}

/*!
 * \brief MessageDock::slotAdmsChanged monitors the adms log, update tab icon
 */
void MessageDock::slotAdmsChanged()
{
    // look for [fatal..] output of admsXml
    // get line from either
    //  * [fatal..] ./3ph_vs.va:34:42: analog function '$abstime' is undefined
    //  * [fatal..] ./mypotentiometer.va: during lexical analysis syntax error at line 33 --
    //  * [fatal..] [./mypotentiometer.va:63:1]: at 'Rad_Angle':

    // \todo can we change the mouse cursor over the highlighted lines?
    // A Qt::PointingHandCursor would be nice.
    QString logContents = admsOutput->toPlainText();
    QStringList lines = logContents.split("\n");

    bool error = false;

    QList<QTextEdit::ExtraSelection> extraSelections;
    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines[i];

        if (line.contains("[fatal..]",Qt::CaseSensitive)) {
            // get cursor for the line
            int pos = admsOutput->document()->findBlockByLineNumber(i).position();
            QTextCursor cursor = admsOutput->textCursor();
            cursor.setPosition(pos);

            // highlight 'fatal' lines on the log
            QTextEdit::ExtraSelection selection;
            QColor lineColor = QColor(Qt::yellow).lighter(160);
            selection.format.setBackground(lineColor);
            selection.format.setProperty(QTextFormat::FullWidthSelection, true);
            selection.cursor = cursor;
            extraSelections.append(selection);

            error = true;
        }
        else if (line.contains("[error..]",Qt::CaseSensitive)) {
            // Do something with error?
             error = true;
        }
        else if (line.contains("*** No rule to make target",Qt::CaseSensitive)) {
            // Do something with error?
             error = true;
        }
    }

    // highlight all the fatal warnings
    admsOutput->setExtraSelections(extraSelections);

    // Change adms tab icon
    if (error)
         builderTabs->setTabIcon(0,QPixmap(":/bitmaps/svg/error.svg"));
    else
         builderTabs->setTabIcon(0,QPixmap(":/bitmaps/svg/ok_apply.svg"));
}

/*!
 * \brief MessageDock::slotCppChanged monitors the compiler log, update tab icon
 */
void MessageDock::slotCppChanged()
{
    QString logContents = cppOutput->toPlainText();

    bool error = false;

    if (logContents.contains("*** No rule to make target")) {
        error = true;
    }
    else if (logContents.contains("error",Qt::CaseInsensitive)) {
        error = true;
    }

    // Change compiler tab icon
    if (error)
         builderTabs->setTabIcon(1,QPixmap(":/bitmaps/svg/error.svg"));
    else
         builderTabs->setTabIcon(1,QPixmap(":/bitmaps/svg/ok_apply.svg"));
}

/*!
 * \brief MessageDock::slotCursor
 */
void MessageDock::slotCursor()
{
    int gotoLine = -1;
    QString line =  admsOutput->textCursor().block().text();
    if (line.contains("[fatal..]",Qt::CaseSensitive)) {
        // \todo improve the parsing of line
        // try to find line number: ":34:"
        if (line.contains(":",Qt::CaseSensitive)) {
            int a,b;
            a = line.indexOf(":")+1;
            b = line.indexOf(":",a);
            gotoLine = line.mid(a,b-a).trimmed().toInt();
            qDebug() << "goto line " << gotoLine;
        }

        // try to find line number: "syntax error at line 33 --"
        if (line.contains("syntax error ",Qt::CaseSensitive)) {
            int a,b;
            a = line.indexOf("at line");
            b = line.indexOf("--",a);
            gotoLine = line.mid(a+7,b-a-7).trimmed().toInt();
            qDebug() << "goto line " << gotoLine;
        }
    }

  // \todo set highlight in QucsDoc Verilog-A file?
  // move cursor? addt line number? highliht line number? set in focus

    /*
     * add slot to TextDoc
     * it takes the gotoLine
     * hightlings the line
     * QucsApp::getDoc() //current should be a TextDoc
     */
//    QucsDoc *foo =  QucsMain->getDoc();
//    qWarning() << foo->DocName;

    if (gotoLine >= 0) {

        // \todo it will mark whatever document is open. parse the model file
        // name from the fatal message and ->findDoc instead of ->getDoc?

        // grab active text document
        TextDoc * d = dynamic_cast<TextDoc*>(QucsMain->getDoc());
        if (d == nullptr) return;   // not a text document in front

        QTextCursor cursor = d->textCursor();
        int pos = d->document()->findBlockByLineNumber(gotoLine-1).position();
        cursor.setPosition(pos);

        // Highlight a give line
        QList<QTextEdit::ExtraSelection> extraSelections;
        QTextEdit::ExtraSelection selection;
        QColor lineColor = QColor(Qt::yellow).lighter(160);
        selection.format.setBackground(lineColor);
        selection.format.setProperty(QTextFormat::FullWidthSelection, true);
        selection.cursor = cursor;

        // get existing
        extraSelections.append(d->extraSelections());

        // append new
        extraSelections.append(selection);

        // color the selections on the active document
        d->setExtraSelections(extraSelections);

        //move focus to VA code
        d->setFocus();
        //move cursor to highlighted line
//        d->setCursor(d->document()->);
        d->setTextCursor(cursor);
    }

    /// \todo add line numbers to TextDoc, highlight as the cursor moves
    /// problem that now the cursor paints over the failed line.
    /// can we have multiple selections?


}




void MessageDock::showProblems(Schematic* doc, const QList<qucs_s::erc::Issue>& issues, bool raise, bool hierarchy)
{
    QObject::disconnect(a_problemsTextGone);
    a_problemsText = nullptr;
    a_textProblems.clear();
    a_problemsDoc = doc;
    a_problemsHierarchy = hierarchy;
    a_issues = issues;
    problems->clear();
    const QIcon error = style()->standardIcon(QStyle::SP_MessageBoxCritical);
    const QIcon warning = style()->standardIcon(QStyle::SP_MessageBoxWarning);
    const QString docName = doc != nullptr ? doc->getDocName() : QString();
    for (const qucs_s::erc::Issue& issue : issues) {
        // An issue of a subcircuit names its file.
        const QString text = issue.file.isEmpty() || issue.file == docName
                                 ? issue.message
                                 : QFileInfo(issue.file).fileName() + QStringLiteral(": ") + issue.message;
        auto* item = new QListWidgetItem(issue.severity == qucs_s::erc::Severity::Error ? error : warning, text, problems);
        item->setToolTip(tr("at %1, %2").arg(issue.where.x()).arg(issue.where.y())
                         + (issue.file.isEmpty() ? QString() : QStringLiteral("\n") + issue.file));
    }
    if (issues.isEmpty()) {
        auto* item = new QListWidgetItem(style()->standardIcon(QStyle::SP_DialogApplyButton),
                                         tr("No problems found."), problems);
        item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
    }
    titleProblems(qucs_s::erc::errorCount(issues), int(issues.size()));
    if (raise) {
        builderTabs->setCurrentWidget(problems);
        msgDock->show();
        msgDock->raise();
    }
}

Schematic* MessageDock::problemsDocument() const
{
    return a_problemsDoc.data();
}

void MessageDock::titleProblems(int errors, int count)
{
    const QIcon error = style()->standardIcon(QStyle::SP_MessageBoxCritical);
    const QIcon warning = style()->standardIcon(QStyle::SP_MessageBoxWarning);
    builderTabs->setTabText(2, count == 0 ? tr("Problems") : tr("Problems (%1)").arg(count));
    builderTabs->setTabIcon(2, errors > 0 ? error : (count == 0 ? QIcon() : warning));
}

void MessageDock::showTextProblems(TextDoc* doc, const QList<TextDoc::Diagnostic>& list, const QString& checkedBy, bool raise,
                                   const QString& failure)
{
    a_problemsDoc = nullptr;
    a_problemsHierarchy = false;
    a_issues.clear();
    if (a_problemsText.data() != doc) {
        QObject::disconnect(a_problemsTextGone);
        // The rows go with the document.
        a_problemsTextGone = connect(doc, &QObject::destroyed, this, [this] {
            a_problemsText = nullptr;
            a_textProblems.clear();
            problems->clear();
            titleProblems(0, 0);
        });
    }
    a_problemsText = doc;
    a_textProblems = list;
    problems->clear();
    const QIcon error = style()->standardIcon(QStyle::SP_MessageBoxCritical);
    const QIcon warning = style()->standardIcon(QStyle::SP_MessageBoxWarning);
    const QString file = doc->getDocName().isEmpty() ? tr("untitled") : QFileInfo(doc->getDocName()).fileName();
    int errors = 0;
    for (const TextDoc::Diagnostic& d : list) {
        errors += d.error ? 1 : 0;
        auto* item = new QListWidgetItem(d.error ? error : warning, tr("%1, line %2: %3").arg(file).arg(d.line).arg(d.message), problems);
        item->setToolTip((d.column > 0 ? tr("%1, line %2, column %3").arg(doc->getDocName()).arg(d.line).arg(d.column)
                                        : tr("%1, line %2").arg(doc->getDocName()).arg(d.line))
                         + QStringLiteral("\n") + checkedBy);
    }
    if (list.isEmpty()) {
        auto* item = failure.isEmpty()
                         ? new QListWidgetItem(style()->standardIcon(QStyle::SP_DialogApplyButton),
                                               tr("No problems found in %1.").arg(file), problems)
                         : new QListWidgetItem(warning, tr("%1 was not checked: %2").arg(file, failure), problems);
        item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
        item->setToolTip(failure.isEmpty() ? checkedBy : failure);
    }
    titleProblems(errors, int(list.size()));
    if (raise) {
        builderTabs->setCurrentWidget(problems);
        msgDock->show();
        msgDock->raise();
    }
}

TextDoc* MessageDock::textProblemsDocument() const
{
    return qobject_cast<TextDoc*>(a_problemsText.data());
}

namespace {
// Data of the Operating Point tab's rows: the component (every row), and
// for a parameter its number and unit (operatingPointText()).
constexpr int ValueRole = Qt::UserRole + 1;
constexpr int UnitRole = Qt::UserRole + 2;
constexpr int DeviceRole = Qt::UserRole + 3;

QString deviceText(const qucs_s::oppoint::Device &d)
{
    return d.model.isEmpty() ? d.type : d.type + QStringLiteral(", ") + d.model;
}

// Resistors, capacitors, inductors and sources: listed after the devices
// whose operating point is the point (transistors, diodes, OSDI devices).
bool isCircuitElement(const qucs_s::oppoint::Device &d)
{
    static const QStringList elements = {"resistor", "capacitor", "inductor", "mutual", "vsource", "isource",
                                         "vcvs", "vccs", "ccvs", "cccs", "asrc"};
    return elements.contains(d.type.toLower());
}
} // namespace

void MessageDock::showOperatingPoint(Schematic* doc, bool raise)
{
    // The rows go with their document - without touching it while it is
    // being destroyed.
    if (a_operatingPointDoc.data() != doc) {
        QObject::disconnect(a_operatingPointGone);
        a_operatingPointGone = QMetaObject::Connection();
        if (doc != nullptr)
            a_operatingPointGone = connect(doc, &QObject::destroyed, this, [this] {
                a_operatingPointDoc = nullptr;
                showOperatingPoint(nullptr, false);
            });
    }
    a_operatingPointDoc = doc;
    operatingPoint->clear();
    const QList<qucs_s::oppoint::Device> devices = doc != nullptr ? doc->operatingPoint()
                                                                  : QList<qucs_s::oppoint::Device>();
    if (devices.isEmpty()) {
        auto *item = new QTreeWidgetItem(operatingPoint, {
            doc == nullptr ? tr("Calculate DC bias (F8) with ngspice to see the operating point of every device.")
                           : tr("No device reported an operating point.")});
        item->setFlags(Qt::ItemIsEnabled);
        item->setFirstColumnSpanned(true);
    }

    // A row per component - in the order of their names, numbers counted
    // (T2 before T10) - with its device, or the devices of a subcircuit,
    // and their parameters.
    QHash<QString, QTreeWidgetItem *> components;
    QSet<QTreeWidgetItem *> active;   // with a device other than a circuit element
    QList<QTreeWidgetItem *> tops;
    for (const qucs_s::oppoint::Device &d : devices) {
        const QString component = d.component.isEmpty() ? d.name.toUpper() : d.component;
        QTreeWidgetItem *&top = components[component];
        if (top == nullptr) {
            top = new QTreeWidgetItem({component, QString()});
            top->setData(0, Qt::UserRole, d.component);
            tops << top;
        }
        if (!isCircuitElement(d)) active.insert(top);
        QTreeWidgetItem *device = top;
        if (d.inside.isEmpty()) {
            top->setText(1, deviceText(d));
            top->setToolTip(1, d.description);
        } else {
            device = new QTreeWidgetItem(top, {d.inside, deviceText(d)});
            device->setData(0, Qt::UserRole, d.component);
            device->setToolTip(1, d.description);
            top->setText(1, tr("subcircuit, %n device(s)", nullptr, top->childCount()));
        }
        device->setData(0, DeviceRole, d.inside.isEmpty() ? d.type : d.inside);
        for (const qucs_s::oppoint::Parameter &p : d.parameters) {
            if (std::fabs(p.value) >= 1e90) continue;   // ngspice's "not set" (bv_max 1e99)
            auto *row = new QTreeWidgetItem(device, {p.name, qucs_s::oppoint::valueText(d, p)});
            row->setData(0, Qt::UserRole, d.component);
            row->setData(0, ValueRole, p.value);
            row->setData(0, UnitRole, qucs_s::oppoint::unitOf(d, p.name));
            row->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        }
    }
    QCollator order;
    order.setNumericMode(true);
    order.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(tops.begin(), tops.end(), [&order, &active](QTreeWidgetItem *a, QTreeWidgetItem *b) {
        if (active.contains(a) != active.contains(b)) return active.contains(a);
        return order.compare(a->text(0), b->text(0)) < 0;
    });
    operatingPoint->addTopLevelItems(tops);
    // Each value next to its name, however wide the dock.
    operatingPoint->expandAll();
    operatingPoint->resizeColumnToContents(0);
    operatingPoint->resizeColumnToContents(1);
    operatingPoint->collapseAll();

    builderTabs->setTabText(a_operatingPointTab, devices.isEmpty() ? tr("Operating Point")
                                                                   : tr("Operating Point (%1)").arg(devices.size()));
    slotFilterOperatingPoint();
    if (raise) raiseOperatingPoint();
}

Schematic* MessageDock::operatingPointDocument() const
{
    return qobject_cast<Schematic*>(a_operatingPointDoc.data());
}

void MessageDock::showBuildOutput()
{
    builderTabs->setCurrentWidget(admsOutput);
    msgDock->show();
    msgDock->raise();
}

void MessageDock::raiseOperatingPoint()
{
    builderTabs->setCurrentIndex(a_operatingPointTab);
    msgDock->show();
    msgDock->raise();
}

// A row is shown when it, a row above it or a row below it has the text;
// what is found is opened up. "gm" shows every device's gm (and gmbs...),
// "T1" all of T1.
void MessageDock::slotFilterOperatingPoint()
{
    const QString text = operatingPointFilter->text().trimmed();
    const auto has = [&text](const QTreeWidgetItem *item) {
        return item->text(0).contains(text, Qt::CaseInsensitive);
    };
    // Returns whether the item is shown.
    std::function<bool(QTreeWidgetItem *, bool)> filter = [&](QTreeWidgetItem *item, bool above) {
        const bool here = above || text.isEmpty() || has(item);
        bool below = false;
        for (int i = 0; i < item->childCount(); ++i) below = filter(item->child(i), here) || below;
        const bool shown = here || below;
        item->setHidden(!shown);
        item->setExpanded(!text.isEmpty() && below);
        return shown;
    };
    for (int i = 0; i < operatingPoint->topLevelItemCount(); ++i)
        filter(operatingPoint->topLevelItem(i), false);
}

QString MessageDock::operatingPointText() const
{
    QStringList lines{QStringLiteral("component\tdevice\tparameter\tvalue\tunit")};
    for (QTreeWidgetItemIterator it(operatingPoint, QTreeWidgetItemIterator::NotHidden); *it; ++it) {
        const QTreeWidgetItem *row = *it;
        if (!row->data(0, ValueRole).isValid()) continue;   // a parameter's row
        const QTreeWidgetItem *device = row->parent();
        const QTreeWidgetItem *top = device;
        while (top->parent() != nullptr) top = top->parent();
        lines << QStringList{top->text(0), device->data(0, DeviceRole).toString(), row->text(0),
                             QString::number(row->data(0, ValueRole).toDouble(), 'g', 9),
                             row->data(0, UnitRole).toString()}.join('\t');
    }
    return lines.join('\n') + '\n';
}

void MessageDock::showReferences(const QString &title, TextDoc *document, const QList<Reference> &list)
{
    a_referencesDoc = document;
    a_referencesTitle->setText(title);
    references->clear();
    QHash<QString, QTreeWidgetItem *> files;
    for (const Reference &r : list) {
      QTreeWidgetItem *&group = files[r.file];
      if (group == nullptr) {
        group = new QTreeWidgetItem(references);
        const QString name = r.file.isEmpty() ? (document != nullptr && !document->getDocName().isEmpty()
                                                     ? QFileInfo(document->getDocName()).fileName() : tr("the script"))
                                              : QFileInfo(r.file).fileName();
        group->setText(0, name);
        group->setToolTip(0, r.file.isEmpty() ? (document != nullptr ? QDir::toNativeSeparators(document->getDocName()) : QString())
                                              : QDir::toNativeSeparators(r.file));
        QFont bold = group->font(0);
        bold.setBold(true);
        group->setFont(0, bold);
        group->setExpanded(true);
      }
      auto *item = new QTreeWidgetItem(group);
      const QString text = r.text.trimmed();
      item->setText(0, tr("%1: %2%3").arg(r.line).arg(text, r.definition ? tr("   (defined)") : QString()));
      item->setToolTip(0, r.text);
      item->setData(0, Qt::UserRole, r.file);
      item->setData(0, Qt::UserRole + 1, r.line);
      item->setData(0, Qt::UserRole + 2, r.column);
    }
    for (QTreeWidgetItem *group : std::as_const(files)) group->setText(0, tr("%1 (%2)").arg(group->text(0)).arg(group->childCount()));
    builderTabs->setTabText(a_referencesTab, list.isEmpty() ? tr("References") : tr("References (%1)").arg(list.size()));
    builderTabs->setCurrentIndex(a_referencesTab);
    msgDock->show();
    msgDock->raise();
}

QStringList MessageDock::referenceRows() const
{
    QStringList rows;
    for (int g = 0; g < references->topLevelItemCount(); ++g) {
      const QTreeWidgetItem *group = references->topLevelItem(g);
      for (int k = 0; k < group->childCount(); ++k) {
        const QTreeWidgetItem *item = group->child(k);
        const QString file = item->data(0, Qt::UserRole).toString();
        rows << QStringLiteral("%1:%2: %3").arg(file.isEmpty() ? QString() : QFileInfo(file).fileName())
                    .arg(item->data(0, Qt::UserRole + 1).toInt()).arg(item->toolTip(0).trimmed());
      }
    }
    return rows;
}

void MessageDock::slotProblemChosen()
{
    const int row = problems->currentRow();
    if (TextDoc* doc = textProblemsDocument()) {
        if (row >= 0 && row < a_textProblems.size())
            emit lineRequested(doc, a_textProblems.at(row).line, a_textProblems.at(row).column);
        return;
    }
    if (row >= 0 && row < a_issues.size()) emit locateRequested(row);
}

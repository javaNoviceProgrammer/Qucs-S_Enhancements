/*
 * pythonviews.cpp - what the Python editor shows besides a script
 *                   (pythonviews.h)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "pythonviews.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImageWriter>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTableView>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {
QString tr(const char* text) { return QCoreApplication::translate("PythonViews", text); }

QToolButton* toolButton(QWidget* parent, const char* name, const QString& text, const QString& tip)
{
    auto* b = new QToolButton(parent);
    b->setObjectName(QLatin1String(name));
    b->setText(text);
    b->setToolTip(tip);
    b->setAutoRaise(true);
    return b;
}
} // namespace

namespace qucs_s::python {

// ----------------------------------------------------------------------
// A breakpoint's dialog

bool editBreakpointDialog(QWidget* parent, Breakpoint* b, int field, bool existing)
{
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("pythonBreakpointDialog"));
    dialog.setWindowTitle(existing ? tr("Edit Breakpoint - Line %1").arg(b->line) : tr("Add Breakpoint - Line %1").arg(b->line));
    auto* condition = new QLineEdit(b->condition, &dialog);
    condition->setObjectName(QStringLiteral("breakpointCondition"));
    condition->setPlaceholderText(tr("an expression: x > 3 and name == 'out'"));
    condition->setToolTip(tr("It stops when this holds, evaluated where the line is (empty: always)."));
    auto* hit = new QLineEdit(b->hit, &dialog);
    hit->setObjectName(QStringLiteral("breakpointHit"));
    hit->setPlaceholderText(tr("5 (from the 5th time on), == 5, > 5, % 5 (every 5th)"));
    hit->setToolTip(tr("It stops when the times the line was reached (its condition holding) are so: 5 or >= 5 from the "
                       "fifth on, == 5 the fifth alone, > 5, < 5, <= 5, % 5 every fifth (empty: every time)."));
    auto* log = new QLineEdit(b->log, &dialog);
    log->setObjectName(QStringLiteral("breakpointLog"));
    log->setPlaceholderText(tr("a message: k is {k}, total {total:.3f}"));
    log->setToolTip(tr("A logpoint: the message written in the output - each {expression} in it evaluated -, and on "
                       "without stopping (empty: it stops)."));
    auto* enabled = new QCheckBox(tr("On"), &dialog);
    enabled->setObjectName(QStringLiteral("breakpointEnabled"));
    enabled->setChecked(b->enabled);
    auto* form = new QFormLayout;
    form->addRow(tr("Condition:"), condition);
    form->addRow(tr("Hit count:"), hit);
    form->addRow(tr("Log message:"), log);
    form->addRow(QString(), enabled);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addLayout(form);
    layout->addWidget(buttons);
    (field == 1 ? hit : field == 2 ? log : condition)->setFocus();
    dialog.resize(std::max(dialog.sizeHint().width(), 460), dialog.sizeHint().height());
    if (dialog.exec() != QDialog::Accepted) return false;
    b->condition = condition->text().trimmed();
    b->hit = hit->text().trimmed();
    b->log = log->text();
    b->enabled = enabled->isChecked();
    return true;
}

// ----------------------------------------------------------------------
// A script's Run Settings

bool editRunSettingsDialog(QWidget* parent, const QString& script, RunSettings* settings)
{
    const QFileInfo info(script);
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("pythonRunSettingsDialog"));
    dialog.setWindowTitle(tr("Run Settings - %1").arg(info.fileName()));
    auto* arguments = new QLineEdit(settings->arguments, &dialog);
    arguments->setObjectName(QStringLiteral("runArguments"));
    arguments->setPlaceholderText(tr("--points 101 \"a file.dat\"  (as a shell splits them: sys.argv[1:])"));
    auto* folder = new QLineEdit(settings->folder, &dialog);
    folder->setObjectName(QStringLiteral("runFolder"));
    folder->setPlaceholderText(tr("its own folder (relative: to it)"));
    auto* browseFolder = new QPushButton(QObject::tr("Browse..."), &dialog);
    QObject::connect(browseFolder, &QPushButton::clicked, &dialog, [&] {
        const QString chosen = QFileDialog::getExistingDirectory(&dialog, tr("Working folder"), info.absolutePath());
        if (!chosen.isEmpty()) folder->setText(QDir(info.absolutePath()).relativeFilePath(chosen));
    });
    auto* environment = new QPlainTextEdit(settings->environment, &dialog);
    environment->setObjectName(QStringLiteral("runEnvironment"));
    environment->setPlaceholderText(tr("NAME=value, a line each (${OTHER}: its value)"));
    environment->setTabChangesFocus(true);
    environment->setFixedHeight(environment->fontMetrics().height() * 5 + 12);
    auto* envFile = new QLineEdit(settings->envFile, &dialog);
    envFile->setObjectName(QStringLiteral("runEnvFile"));
    envFile->setPlaceholderText(tr("a .env file of NAME=value lines (relative: to its folder)"));
    auto* browseFile = new QPushButton(QObject::tr("Browse..."), &dialog);
    QObject::connect(browseFile, &QPushButton::clicked, &dialog, [&] {
        const QString chosen = QFileDialog::getOpenFileName(&dialog, tr(".env file"), info.absolutePath(), tr("Environment files (*.env .env);;All files (*)"));
        if (!chosen.isEmpty()) envFile->setText(QDir(info.absolutePath()).relativeFilePath(chosen));
    });
    auto* folderRow = new QHBoxLayout;
    folderRow->addWidget(folder, 1);
    folderRow->addWidget(browseFolder);
    auto* fileRow = new QHBoxLayout;
    fileRow->addWidget(envFile, 1);
    fileRow->addWidget(browseFile);
    auto* form = new QFormLayout;
    form->addRow(tr("Arguments:"), arguments);
    form->addRow(tr("Working folder:"), folderRow);
    form->addRow(tr("Environment:"), environment);
    form->addRow(tr(".env file:"), fileRow);
    auto* note = new QLabel(tr("Run and Debug use these for %1; the Python Shell does not.").arg(info.fileName()), &dialog);
    note->setEnabled(false);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Reset, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons->button(QDialogButtonBox::Reset), &QPushButton::clicked, &dialog, [&] {
        arguments->clear();
        folder->clear();
        environment->clear();
        envFile->clear();
    });
    auto* layout = new QVBoxLayout(&dialog);
    layout->addLayout(form);
    layout->addWidget(note);
    layout->addWidget(buttons);
    dialog.resize(std::max(dialog.sizeHint().width(), 520), dialog.sizeHint().height());
    arguments->setFocus();
    if (dialog.exec() != QDialog::Accepted) return false;
    settings->arguments = arguments->text().trimmed();
    settings->folder = folder->text().trimmed();
    settings->environment = environment->toPlainText().trimmed();
    settings->envFile = envFile->text().trimmed();
    return true;
}

// ----------------------------------------------------------------------
// The folders

Exchange::Exchange(QObject* parent) : QObject(parent)
{
    a_watcher = new QFileSystemWatcher(this);
    for (const QString& folder : {plotsFolder(), shellFolder(), QDir(shellFolder()).filePath(QStringLiteral("answers")), requestsFolder()})
        if (!folder.isEmpty() && QFileInfo(folder).isDir()) a_watcher->addPath(folder);
    connect(a_watcher, &QFileSystemWatcher::directoryChanged, this, &Exchange::scan);
    a_poll = new QTimer(this);
    a_poll->setInterval(3000);
    connect(a_poll, &QTimer::timeout, this, &Exchange::scan);
    a_poll->start();
}

int Exchange::askShell(QJsonObject request)
{
    const QString folder = QDir(shellFolder()).filePath(QStringLiteral("requests"));
    if (shellFolder().isEmpty()) return 0;
    const int id = ++a_questions;
    request.insert(QStringLiteral("id"), id);
    // (Written whole, then named: the shell reads none half written.)
    const QString name = QStringLiteral("%1-%2.json").arg(QCoreApplication::applicationPid()).arg(id, 6, 10, QLatin1Char('0'));
    QFile out(QDir(folder).filePath(name + QStringLiteral(".part")));
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 0;
    out.write(QJsonDocument(request).toJson(QJsonDocument::Compact));
    out.close();
    QFile::remove(QDir(folder).filePath(name));
    if (!out.rename(QDir(folder).filePath(name))) return 0;
    return id;
}

void Exchange::scan()
{
    // The figures: each once, in the order written.
    QDir plots(plotsFolder());
    if (!plotsFolder().isEmpty() && plots.exists()) {
        const QFileInfoList found = plots.entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
        QList<std::pair<double, QFileInfo>> fresh;
        for (const QFileInfo& about : found) {
            if (a_plotsSeen.contains(about.fileName())) continue;
            QFile file(about.filePath());
            if (!file.open(QIODevice::ReadOnly)) continue;
            const QJsonObject o = QJsonDocument::fromJson(file.readAll()).object();
            a_plotsSeen.insert(about.fileName());
            if (o.isEmpty()) continue;
            fresh.append({o.value(QStringLiteral("time")).toDouble(), about});
        }
        std::stable_sort(fresh.begin(), fresh.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& [time, about] : fresh) {
            QFile file(about.filePath());
            if (!file.open(QIODevice::ReadOnly)) continue;
            const QJsonObject o = QJsonDocument::fromJson(file.readAll()).object();
            file.close();
            QFile::remove(about.filePath());   // (the picture stays: the pane's)
            const QString image = plots.filePath(o.value(QStringLiteral("image")).toString());
            if (QFileInfo(image).isFile()) emit plotArrived(image, o);
        }
    }
    // The shell's variables, when they changed.
    if (const QString shell = shellFolder(); !shell.isEmpty()) {
        QFile variables(QDir(shell).filePath(QStringLiteral("variables.json")));
        if (variables.open(QIODevice::ReadOnly)) {
            const QByteArray bytes = variables.readAll();
            if (bytes != a_variables) {
                const QJsonDocument doc = QJsonDocument::fromJson(bytes);
                if (doc.isObject()) {
                    a_variables = bytes;
                    emit variablesChanged(doc.object().value(QStringLiteral("variables")).toArray());
                }
            }
        }
        QDir answers(QDir(shell).filePath(QStringLiteral("answers")));
        for (const QFileInfo& answer : answers.entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
            QFile file(answer.filePath());
            if (!file.open(QIODevice::ReadOnly)) continue;
            const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
            file.close();
            QFile::remove(answer.filePath());
            if (doc.isObject()) emit shellAnswered(doc.object());
        }
    }
    // qucs.display()'s requests.
    QDir requests(requestsFolder());
    if (!requestsFolder().isEmpty() && requests.exists()) {
        for (const QFileInfo& request : requests.entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
            QFile file(request.filePath());
            if (!file.open(QIODevice::ReadOnly)) continue;
            const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
            file.close();
            QFile::remove(request.filePath());
            if (doc.isObject()) emit displayRequested(doc.object());
        }
    }
}

} // namespace qucs_s::python

// ----------------------------------------------------------------------
// Python Plots

PythonPlotsPane::PythonPlotsPane(QWidget* parent) : QWidget(parent)
{
    a_previous = toolButton(this, "pythonPlotsPrevious", tr("Previous"), tr("The figure before (Left)"));
    a_previous->setArrowType(Qt::LeftArrow);
    a_next = toolButton(this, "pythonPlotsNext", tr("Next"), tr("The figure after (Right)"));
    a_next->setArrowType(Qt::RightArrow);
    a_title = new QLabel(this);
    a_title->setObjectName(QStringLiteral("pythonPlotsTitle"));
    a_title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    a_fitButton = toolButton(this, "pythonPlotsFit", tr("Fit"), tr("Fitted to the pane, or at its own size"));
    a_fitButton->setCheckable(true);
    a_fitButton->setChecked(true);
    a_save = toolButton(this, "pythonPlotsSave", tr("Save As..."), tr("The figure saved as a picture (PNG, JPEG, BMP)"));
    a_copy = toolButton(this, "pythonPlotsCopy", tr("Copy"), tr("The figure copied, as a picture"));
    a_remove = toolButton(this, "pythonPlotsRemove", tr("Remove"), tr("This figure taken away (Delete)"));
    a_removeAll = toolButton(this, "pythonPlotsRemoveAll", tr("Remove All"), tr("Every figure taken away"));
    connect(a_previous, &QToolButton::clicked, this, [this] { choose(a_current - 1); });
    connect(a_next, &QToolButton::clicked, this, [this] { choose(a_current + 1); });
    connect(a_fitButton, &QToolButton::toggled, this, &PythonPlotsPane::setFitted);
    connect(a_save, &QToolButton::clicked, this, &PythonPlotsPane::saveAs);
    connect(a_copy, &QToolButton::clicked, this, &PythonPlotsPane::copy);
    connect(a_remove, &QToolButton::clicked, this, [this] { remove(a_current); });
    connect(a_removeAll, &QToolButton::clicked, this, &PythonPlotsPane::removeAll);
    auto* bar = new QHBoxLayout;
    bar->setContentsMargins(2, 0, 2, 0);
    bar->addWidget(a_previous);
    bar->addWidget(a_next);
    bar->addWidget(a_title, 1);
    for (QToolButton* b : {a_fitButton, a_save, a_copy, a_remove, a_removeAll}) bar->addWidget(b);

    a_view = new QLabel(this);
    a_view->setObjectName(QStringLiteral("pythonPlotsView"));
    a_view->setAlignment(Qt::AlignCenter);
    a_scroll = new QScrollArea(this);
    a_scroll->setWidget(a_view);
    a_scroll->setWidgetResizable(true);
    a_scroll->setAlignment(Qt::AlignCenter);
    a_empty = new QLabel(tr("The figures of a script's matplotlib come here - plt.show() in a script run, debugged or "
                            "run in the Python Shell (Simulation > Python > Plots in Qucs-S)."),
                         this);
    a_empty->setWordWrap(true);
    a_empty->setAlignment(Qt::AlignCenter);
    a_empty->setEnabled(false);

    a_strip = new QListWidget(this);
    a_strip->setObjectName(QStringLiteral("pythonPlotsStrip"));
    a_strip->setViewMode(QListView::IconMode);
    a_strip->setFlow(QListView::LeftToRight);
    a_strip->setWrapping(false);
    a_strip->setMovement(QListView::Static);
    a_strip->setIconSize(QSize(96, 64));
    a_strip->setFixedHeight(96);
    a_strip->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    a_strip->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(a_strip, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0 && row != a_current) choose(row);
    });

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addLayout(bar);
    layout->addWidget(a_empty, 1);
    layout->addWidget(a_scroll, 1);
    layout->addWidget(a_strip);
    setFocusPolicy(Qt::StrongFocus);
    showCurrent();
}

void PythonPlotsPane::addPlot(const QString& image, const QJsonObject& about)
{
    QPixmap thumbnail(image);
    const QIcon icon(thumbnail.scaled(a_strip->iconSize() * 2, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    // A figure kept open and drawn again (plt.pause(), an animation): its
    // picture replaced, where it is.
    const auto same = [&about](const Plot& p) {
        return p.about.value(QStringLiteral("update")).toBool()
               && p.about.value(QStringLiteral("pid")).toInteger() == about.value(QStringLiteral("pid")).toInteger()
               && p.about.value(QStringLiteral("figure")).toVariant() == about.value(QStringLiteral("figure")).toVariant();
    };
    for (qsizetype k = a_plots.size() - 1; k >= 0; --k) {
        if (!same(a_plots.at(k))) continue;
        QFile::remove(a_plots.at(k).image);
        a_plots[k] = {image, about};
        a_strip->item(int(k))->setIcon(icon);
        a_strip->item(int(k))->setToolTip(title(int(k)));
        if (a_current == k) showCurrent();
        else choose(int(k));
        return;
    }
    a_plots.append({image, about});
    auto* item = new QListWidgetItem(icon, QString(), a_strip);
    item->setToolTip(title(int(a_plots.size()) - 1));
    while (a_plots.size() > kMost) {   // the oldest go
        QFile::remove(a_plots.first().image);
        a_plots.removeFirst();
        delete a_strip->takeItem(0);
    }
    choose(int(a_plots.size()) - 1);
    emit countChanged(count());
}

QString PythonPlotsPane::title(int index) const
{
    if (index < 0 || index >= a_plots.size()) return {};
    const QJsonObject& about = a_plots.at(index).about;
    QString said = tr("%1, figure %2").arg(about.value(QStringLiteral("source")).toString(), about.value(QStringLiteral("figure")).toVariant().toString());
    const QString title = about.value(QStringLiteral("title")).toString();
    if (!title.isEmpty()) said += QStringLiteral(": ") + title;
    const double time = about.value(QStringLiteral("time")).toDouble();
    if (time > 0) said += QStringLiteral("  (%1)").arg(QDateTime::fromMSecsSinceEpoch(qint64(time * 1000)).toString(QStringLiteral("HH:mm:ss")));
    return said;
}

QString PythonPlotsPane::imagePath(int index) const
{
    return index >= 0 && index < a_plots.size() ? a_plots.at(index).image : QString();
}

void PythonPlotsPane::choose(int index)
{
    if (a_plots.isEmpty()) index = -1;
    else index = std::clamp(index, 0, int(a_plots.size()) - 1);
    a_current = index;
    {
        const QSignalBlocker block(a_strip);
        a_strip->setCurrentRow(index);
    }
    if (index >= 0) a_strip->scrollToItem(a_strip->item(index));
    showCurrent();
}

void PythonPlotsPane::setFitted(bool on)
{
    a_fit = on;
    {
        const QSignalBlocker block(a_fitButton);
        a_fitButton->setChecked(on);
    }
    showCurrent();
}

QPixmap PythonPlotsPane::shown() const
{
    return a_view->pixmap();
}

void PythonPlotsPane::showCurrent()
{
    const bool any = a_current >= 0;
    a_empty->setVisible(!any);
    a_scroll->setVisible(any);
    a_strip->setVisible(a_plots.size() > 1);
    for (QToolButton* b : {a_fitButton, a_save, a_copy, a_remove, a_removeAll}) b->setEnabled(any);
    a_previous->setEnabled(any && a_current > 0);
    a_next->setEnabled(any && a_current + 1 < a_plots.size());
    if (!any) {
        a_title->setText(tr("No figures"));
        a_view->clear();
        return;
    }
    a_title->setText(tr("%1 of %2 - %3").arg(a_current + 1).arg(a_plots.size()).arg(title(a_current)));
    QPixmap picture(a_plots.at(a_current).image);
    const double scale = std::max(1.0, a_plots.at(a_current).about.value(QStringLiteral("scale")).toDouble(1.0));
    if (a_fit) {
        // Within the pane, no larger than its own size.
        const QSize room = a_scroll->viewport()->size() - QSize(4, 4);
        const QSize own = (QSizeF(picture.size()) / scale).toSize();
        const qreal ratio = devicePixelRatioF();
        const QSize target = own.boundedTo(room).isValid() && room.width() > 16 && room.height() > 16
                                 ? own.scaled(own.boundedTo(room), Qt::KeepAspectRatio) : own;
        QPixmap fitted = picture.scaled(target * ratio, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        fitted.setDevicePixelRatio(ratio);
        a_view->setPixmap(fitted);
    } else {
        picture.setDevicePixelRatio(scale);
        a_view->setPixmap(picture);
    }
}

bool PythonPlotsPane::save(int index, const QString& path) const
{
    if (index < 0 || index >= a_plots.size()) return false;
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("png") || suffix.isEmpty()) {
        QFile::remove(path);
        return QFile::copy(a_plots.at(index).image, path);
    }
    QImage picture(a_plots.at(index).image);
    if (suffix == QLatin1String("jpg") || suffix == QLatin1String("jpeg")) {   // (no transparency: on white)
        QImage white(picture.size(), QImage::Format_RGB32);
        white.fill(Qt::white);
        QPainter painter(&white);
        painter.drawImage(0, 0, picture);
        painter.end();
        picture = white;
    }
    return picture.save(path);
}

void PythonPlotsPane::saveAs()
{
    if (a_current < 0) return;
    const QString path = QFileDialog::getSaveFileName(this, tr("Save the figure"), QStringLiteral("figure.png"),
                                                      tr("PNG picture (*.png);;JPEG picture (*.jpg *.jpeg);;BMP picture (*.bmp)"));
    if (path.isEmpty()) return;
    save(a_current, path);
}

void PythonPlotsPane::copy()
{
    if (a_current >= 0) QApplication::clipboard()->setImage(QImage(a_plots.at(a_current).image));
}

void PythonPlotsPane::remove(int index)
{
    if (index < 0 || index >= a_plots.size()) return;
    QFile::remove(a_plots.at(index).image);
    a_plots.removeAt(index);
    delete a_strip->takeItem(index);
    choose(std::min(index, int(a_plots.size()) - 1));
    emit countChanged(count());
}

void PythonPlotsPane::removeAll()
{
    for (const Plot& p : std::as_const(a_plots)) QFile::remove(p.image);
    a_plots.clear();
    a_strip->clear();
    choose(-1);
    emit countChanged(0);
}

void PythonPlotsPane::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (a_fit) QTimer::singleShot(0, this, &PythonPlotsPane::showCurrent);
}

void PythonPlotsPane::keyPressEvent(QKeyEvent* event)
{
    switch (event->key()) {
    case Qt::Key_Left:
        choose(a_current - 1);
        return;
    case Qt::Key_Right:
        choose(a_current + 1);
        return;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        remove(a_current);
        return;
    default:
        break;
    }
    if (event->matches(QKeySequence::Copy)) {
        copy();
        return;
    }
    QWidget::keyPressEvent(event);
}

// ----------------------------------------------------------------------
// Python Variables

PythonVariablesPane::PythonVariablesPane(QWidget* parent) : QWidget(parent)
{
    a_filter = new QLineEdit(this);
    a_filter->setObjectName(QStringLiteral("pythonVariablesFilter"));
    a_filter->setPlaceholderText(tr("Filter"));
    a_filter->setClearButtonEnabled(true);
    connect(a_filter, &QLineEdit::textChanged, this, &PythonVariablesPane::filter);
    auto* refresh = toolButton(this, "pythonVariablesRefresh", tr("Refresh"), tr("The Python Shell's variables read again"));
    connect(refresh, &QToolButton::clicked, this, &PythonVariablesPane::refreshRequested);
    a_view = new QTreeWidget(this);
    a_view->setObjectName(QStringLiteral("pythonVariables"));
    a_view->setHeaderLabels({tr("Name"), tr("Type"), tr("Size"), tr("Value")});
    a_view->setRootIsDecorated(false);
    a_view->setUniformRowHeights(true);
    a_view->setAlternatingRowColors(true);
    a_view->header()->setStretchLastSection(true);
    a_view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(a_view, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        if (item->data(0, Qt::UserRole).toBool()) emit tableRequested(item->text(0));
    });
    connect(a_view, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& at) {
        QTreeWidgetItem* item = a_view->itemAt(at);
        if (item == nullptr) return;
        QMenu menu(this);
        QAction* table = menu.addAction(tr("View as Table"));
        table->setEnabled(item->data(0, Qt::UserRole).toBool());
        QAction* copy = menu.addAction(tr("Copy Value"));
        QAction* chosen = menu.exec(a_view->viewport()->mapToGlobal(at));
        if (chosen == table) emit tableRequested(item->text(0));
        else if (chosen == copy) QApplication::clipboard()->setText(item->text(3));
    });
    a_note = new QLabel(tr("The Python Shell's variables, after each command (modules, functions and classes aside). "
                           "Double-click an array, a list, a dictionary or a DataFrame to see it as a table."),
                        this);
    a_note->setWordWrap(true);
    a_note->setEnabled(false);
    auto* top = new QHBoxLayout;
    top->setContentsMargins(2, 0, 2, 0);
    top->addWidget(a_filter, 1);
    top->addWidget(refresh);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addLayout(top);
    layout->addWidget(a_view, 1);
    layout->addWidget(a_note);
}

void PythonVariablesPane::setVariables(const QJsonArray& variables)
{
    const QString chosen = a_view->currentItem() != nullptr ? a_view->currentItem()->text(0) : QString();
    const int scrolled = a_view->verticalScrollBar()->value();
    a_view->clear();
    for (const QJsonValue& v : variables) {
        const QJsonObject o = v.toObject();
        auto* item = new QTreeWidgetItem(a_view);
        item->setText(0, o.value(QStringLiteral("name")).toString());
        item->setText(1, o.value(QStringLiteral("type")).toString());
        item->setText(2, o.value(QStringLiteral("size")).toString());
        item->setText(3, o.value(QStringLiteral("value")).toString());
        item->setToolTip(3, o.value(QStringLiteral("value")).toString());
        const bool table = o.value(QStringLiteral("table")).toBool();
        item->setData(0, Qt::UserRole, table);
        if (table) item->setToolTip(0, tr("Double-click: the value as a table"));
        if (item->text(0) == chosen) a_view->setCurrentItem(item);
    }
    for (int k = 0; k < 3; ++k) a_view->resizeColumnToContents(k);
    a_view->verticalScrollBar()->setValue(scrolled);
    filter();
}

void PythonVariablesPane::filter()
{
    const QString wanted = a_filter->text().trimmed();
    for (int k = 0; k < a_view->topLevelItemCount(); ++k) {
        QTreeWidgetItem* item = a_view->topLevelItem(k);
        item->setHidden(!wanted.isEmpty() && !item->text(0).contains(wanted, Qt::CaseInsensitive)
                        && !item->text(1).contains(wanted, Qt::CaseInsensitive));
    }
}

QStringList PythonVariablesPane::rows() const
{
    QStringList rows;
    for (int k = 0; k < a_view->topLevelItemCount(); ++k) {
        const QTreeWidgetItem* item = a_view->topLevelItem(k);
        if (!item->isHidden())
            rows << QStringLiteral("%1: %2 [%3] = %4").arg(item->text(0), item->text(1), item->text(2), item->text(3));
    }
    return rows;
}

// ----------------------------------------------------------------------
// The Data Viewer

void PythonTableModel::answer(const QJsonObject& table)
{
    const QJsonArray shape = table.value(QStringLiteral("shape")).toArray();
    const int rows = shape.size() == 2 ? shape.at(0).toInt() : 0;
    const int columns = shape.size() == 2 ? shape.at(1).toInt() : 0;
    const int start = table.value(QStringLiteral("start")).toInt();
    if (rows != a_rows || columns != a_columns) {   // (the first answer - or the value changed: again from it)
        beginResetModel();
        a_rows = rows;
        a_columns = columns;
        a_names.clear();
        for (const QJsonValue& v : table.value(QStringLiteral("columns")).toArray()) a_names << v.toString();
        a_blocks.clear();
        a_index.clear();
        a_asked.clear();
        endResetModel();
        emit shapeKnown();
    }
    const int block = start / kBlock;
    a_blocks.insert(block, table.value(QStringLiteral("rows")).toArray());
    a_index.insert(block, table.value(QStringLiteral("index")).toArray());
    a_asked.remove(block);
    const int first = block * kBlock;
    const int last = std::min(a_rows, first + kBlock) - 1;
    if (last >= first) {
        emit dataChanged(index(first, 0), index(last, std::max(0, a_columns - 1)));
        emit headerDataChanged(Qt::Vertical, first, last);
    }
    if (a_wantAll) {
        if (complete()) {
            a_wantAll = false;
            emit allFetched();
        } else {
            fetchAll();
        }
    }
}

bool PythonTableModel::complete() const
{
    const int blocks = (a_rows + kBlock - 1) / kBlock;
    for (int b = 0; b < blocks; ++b)
        if (!a_blocks.contains(b)) return false;
    return true;
}

void PythonTableModel::fetchAll()
{
    a_wantAll = true;
    if (complete()) {
        a_wantAll = false;
        emit allFetched();
        return;
    }
    // A few blocks at a time.
    const int blocks = (a_rows + kBlock - 1) / kBlock;
    int asked = int(a_asked.size());
    for (int b = 0; b < blocks && asked < 4; ++b)
        if (!a_blocks.contains(b) && !a_asked.contains(b)) {
            ask(b);
            ++asked;
        }
}

void PythonTableModel::ask(int block) const
{
    if (a_asked.contains(block) || !a_fetch) return;
    a_asked.insert(block);
    a_fetch(block * kBlock, kBlock);
}

int PythonTableModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : a_rows; }

int PythonTableModel::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : a_columns; }

QString PythonTableModel::text(int row, int column) const
{
    const auto found = a_blocks.constFind(row / kBlock);
    if (found == a_blocks.constEnd()) return {};
    const QJsonValue cell = found->at(row % kBlock).toArray().at(column);
    if (cell.isDouble()) {
        const double v = cell.toDouble();
        if (std::floor(v) == v && std::fabs(v) < 1e15) return QString::number(qint64(v));
        return QString::number(v, 'g', 17);
    }
    if (cell.isNull() || cell.isUndefined()) return {};
    return cell.toVariant().toString();
}

QVariant PythonTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid()) return {};
    const int block = index.row() / kBlock;
    if (!a_blocks.contains(block)) {
        if (role == Qt::DisplayRole) {
            ask(block);
            return QStringLiteral("...");
        }
        return {};
    }
    const QJsonValue cell = a_blocks.value(block).at(index.row() % kBlock).toArray().at(index.column());
    if (role == Qt::DisplayRole) {
        if (cell.isDouble()) {
            const double v = cell.toDouble();
            if (std::floor(v) == v && std::fabs(v) < 1e15) return QString::number(qint64(v));
            return QString::number(v, 'g', 10);
        }
        return text(index.row(), index.column());
    }
    if (role == Qt::ToolTipRole) return text(index.row(), index.column());
    if (role == Qt::TextAlignmentRole) return QVariant::fromValue(cell.isDouble() ? Qt::AlignRight | Qt::AlignVCenter : Qt::AlignLeft | Qt::AlignVCenter);
    return {};
}

QVariant PythonTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole) return {};
    if (orientation == Qt::Horizontal) return a_names.value(section, QString::number(section));
    const auto found = a_index.constFind(section / kBlock);
    if (found == a_index.constEnd()) return QString::number(section);
    return found->at(section % kBlock).toVariant().toString();
}

PythonDataViewer::PythonDataViewer(const QString& name, PythonTableModel::Fetch fetch, QWidget* parent)
    : QWidget(parent, Qt::Window), a_name(name)
{
    setObjectName(QStringLiteral("pythonDataViewer"));
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tr("%1 - Data Viewer").arg(name));
    a_model = new PythonTableModel(this);
    a_model->setFetch(std::move(fetch));
    a_about = new QLabel(tr("%1: reading...").arg(name), this);
    a_about->setObjectName(QStringLiteral("pythonDataAbout"));
    a_about->setTextInteractionFlags(Qt::TextSelectableByMouse);
    a_about->setWordWrap(true);
    a_view = new QTableView(this);
    a_view->setObjectName(QStringLiteral("pythonDataTable"));
    a_view->setModel(a_model);
    a_view->setAlternatingRowColors(true);
    a_view->horizontalHeader()->setDefaultSectionSize(110);
    a_view->verticalHeader()->setDefaultSectionSize(a_view->fontMetrics().height() + 6);
    auto* copy = new QPushButton(tr("Copy"), this);
    copy->setToolTip(tr("The cells selected - all of them when none is - as tab-separated text"));
    connect(copy, &QPushButton::clicked, this, &PythonDataViewer::copy);
    auto* save = new QPushButton(tr("Export as CSV..."), this);
    save->setToolTip(tr("Every row written to a CSV file"));
    connect(save, &QPushButton::clicked, this, &PythonDataViewer::exportAs);
    auto* row = new QHBoxLayout;
    row->addWidget(a_about, 1);
    row->addWidget(copy);
    row->addWidget(save);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(row);
    layout->addWidget(a_view, 1);
    resize(720, 480);
    connect(a_model, &PythonTableModel::allFetched, this, [this] {
        if (a_exportPath.isEmpty()) return;
        const QString path = a_exportPath;
        a_exportPath.clear();
        exportTo(path);
    });
}

void PythonDataViewer::answer(const QJsonObject& table)
{
    if (table.contains(QStringLiteral("error"))) {
        a_about->setText(tr("%1: %2").arg(a_name, table.value(QStringLiteral("error")).toString()));
        return;
    }
    a_model->answer(table);
    const QJsonArray shape = table.value(QStringLiteral("shape")).toArray();
    QString said = tr("%1: %2, %3 rows x %4 columns").arg(a_name, table.value(QStringLiteral("type")).toString())
                       .arg(shape.at(0).toInt()).arg(shape.at(1).toInt());
    if (const QString note = table.value(QStringLiteral("note")).toString(); !note.isEmpty()) said += QStringLiteral(" - ") + note;
    a_about->setText(said);
}

QString PythonDataViewer::selectedText() const
{
    QModelIndexList chosen = a_view->selectionModel() != nullptr ? a_view->selectionModel()->selectedIndexes() : QModelIndexList();
    int top = 0, left = 0, bottom = a_model->rowCount() - 1, right = a_model->columnCount() - 1;
    if (!chosen.isEmpty()) {
        top = bottom = chosen.first().row();
        left = right = chosen.first().column();
        for (const QModelIndex& i : chosen) {
            top = std::min(top, i.row());
            bottom = std::max(bottom, i.row());
            left = std::min(left, i.column());
            right = std::max(right, i.column());
        }
    }
    QStringList lines;
    for (int r = top; r <= bottom; ++r) {
        QStringList cells;
        for (int c = left; c <= right; ++c) cells << a_model->text(r, c);
        lines << cells.join(QLatin1Char('\t'));
    }
    return lines.join(QLatin1Char('\n'));
}

void PythonDataViewer::copy()
{
    QApplication::clipboard()->setText(selectedText());
}

void PythonDataViewer::exportAs()
{
    const QString path = QFileDialog::getSaveFileName(this, tr("Export %1").arg(a_name), a_name + QStringLiteral(".csv"),
                                                      tr("CSV file (*.csv)"));
    if (path.isEmpty()) return;
    a_exportPath = path;
    a_model->fetchAll();   // (written when every row is there)
}

bool PythonDataViewer::exportTo(const QString& path)
{
    if (!a_model->complete()) {
        a_exportPath = path;
        a_model->fetchAll();
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        a_about->setText(tr("%1 could not be written: %2").arg(QDir::toNativeSeparators(path), file.errorString()));
        return false;
    }
    const auto quoted = [](const QString& text) {
        if (!text.contains(QLatin1Char(',')) && !text.contains(QLatin1Char('"')) && !text.contains(QLatin1Char('\n'))) return text;
        return QStringLiteral("\"%1\"").arg(QString(text).replace(QLatin1Char('"'), QStringLiteral("\"\"")));
    };
    QStringList header;
    for (int c = 0; c < a_model->columnCount(); ++c) header << quoted(a_model->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString());
    file.write(header.join(QLatin1Char(',')).toUtf8() + '\n');
    for (int r = 0; r < a_model->rowCount(); ++r) {
        QStringList cells;
        for (int c = 0; c < a_model->columnCount(); ++c) cells << quoted(a_model->text(r, c));
        file.write(cells.join(QLatin1Char(',')).toUtf8() + '\n');
    }
    emit exported();
    return true;
}

// ----------------------------------------------------------------------
// Go to Symbol

PythonSymbolPicker::PythonSymbolPicker(const QList<Entry>& entries, QWidget* parent)
    : QFrame(parent, Qt::Popup), a_entries(entries)
{
    setObjectName(QStringLiteral("pythonSymbolPicker"));
    setAttribute(Qt::WA_DeleteOnClose);
    setFrameStyle(QFrame::Box | QFrame::Plain);
    a_filter = new QLineEdit(this);
    a_filter->setObjectName(QStringLiteral("pythonSymbolFilter"));
    a_filter->setPlaceholderText(tr("Go to a class, a function or a variable: type its letters"));
    a_list = new QListWidget(this);
    a_list->setObjectName(QStringLiteral("pythonSymbols"));
    a_list->setUniformItemSizes(true);
    connect(a_filter, &QLineEdit::textChanged, this, &PythonSymbolPicker::filter);
    connect(a_list, &QListWidget::itemActivated, this, &PythonSymbolPicker::choose);
    a_filter->installEventFilter(this);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(2);
    layout->addWidget(a_filter);
    layout->addWidget(a_list, 1);
    filter();
}

void PythonSymbolPicker::filter()
{
    const QString wanted = a_filter->text().trimmed();
    QList<std::pair<int, int>> ranked;   // score, entry
    for (int k = 0; k < a_entries.size(); ++k) {
        const qucs_s::python::Symbol& s = a_entries.at(k).symbol;
        const int score = qucs_s::python::symbolScore(s.name, wanted);
        if (score < 0) continue;
        // (The script's own first, when the letters do as well.)
        ranked.append({score * 2 + (a_entries.at(k).file.isEmpty() ? 1 : 0), k});
    }
    if (!wanted.isEmpty())
        std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    a_list->clear();
    for (const auto& [score, k] : std::as_const(ranked)) {
        const Entry& e = a_entries.at(k);
        const QString name = e.symbol.container.isEmpty() ? e.symbol.name : e.symbol.container + QLatin1Char('.') + e.symbol.name;
        const QString where = e.file.isEmpty() ? tr("line %1").arg(e.symbol.line) : tr("%1, line %2").arg(QFileInfo(e.file).fileName()).arg(e.symbol.line);
        auto* item = new QListWidgetItem(qucs_s::python::completionIcon(e.symbol.kind == QLatin1String("variable") ? QStringLiteral("statement")
                                                                                                                 : e.symbol.kind),
                                         QStringLiteral("%1    %2").arg(name, where), a_list);
        item->setData(Qt::UserRole, k);
        item->setData(Qt::UserRole + 1, name);
        item->setToolTip(e.file.isEmpty() ? where : QDir::toNativeSeparators(e.file));
    }
    a_list->setCurrentRow(0);
}

QStringList PythonSymbolPicker::shownNames() const
{
    QStringList names;
    for (int k = 0; k < a_list->count(); ++k) names << a_list->item(k)->data(Qt::UserRole + 1).toString();
    return names;
}

void PythonSymbolPicker::choose()
{
    const QListWidgetItem* item = a_list->currentItem();
    if (item == nullptr) return;
    const Entry e = a_entries.at(item->data(Qt::UserRole).toInt());
    close();
    emit chosen(e.file, e.symbol.line, e.symbol.column);
}

bool PythonSymbolPicker::eventFilter(QObject* watched, QEvent* event)
{
    // The list's keys while the letters are typed.
    if (watched == a_filter && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        switch (key->key()) {
        case Qt::Key_Down:
        case Qt::Key_Up:
        case Qt::Key_PageDown:
        case Qt::Key_PageUp:
            QApplication::sendEvent(a_list, event);
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            choose();
            return true;
        case Qt::Key_Escape:
            close();
            return true;
        default:
            break;
        }
    }
    return QFrame::eventFilter(watched, event);
}

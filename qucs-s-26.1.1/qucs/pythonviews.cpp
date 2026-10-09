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
#include <QComboBox>
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
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTableView>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
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

// ----------------------------------------------------------------------
// A plot of a table's columns

namespace {
// Round steps (1, 2, 5 times a power of ten) from \a low to \a high.
QVector<double> roundTicks(double low, double high, int most)
{
    QVector<double> ticks;
    const double span = high - low;
    if (!(span > 0) || !std::isfinite(span)) return ticks;
    const double rough = span / std::max(1, most);
    const double power = std::pow(10.0, std::floor(std::log10(rough)));
    double step = power;
    for (const double m : {1.0, 2.0, 5.0, 10.0})
        if (m * power >= rough) {
            step = m * power;
            break;
        }
    for (double t = std::ceil(low / step) * step; t <= high + step * 1e-9; t += step) ticks << (std::fabs(t) < step * 1e-9 ? 0.0 : t);
    return ticks;
}
} // namespace

QImage plotImage(const QString& title, const QString& xName, const QVector<double>& x,
                 const QList<std::pair<QString, QVector<double>>>& series, bool logX, qreal scale)
{
    const QSizeF size(820, 500);
    QImage image((size * scale).toSize(), QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(scale);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    QFont font = painter.font();
    font.setPixelSize(12);
    painter.setFont(font);
    const QFontMetricsF metrics(font);
    const QRectF plot(78, 40, size.width() - 78 - 24, size.height() - 40 - 56);

    // The ranges of what can be drawn.
    const auto usableX = [logX](double v) { return std::isfinite(v) && (!logX || v > 0); };
    double x0 = qInf(), x1 = -qInf(), y0 = qInf(), y1 = -qInf();
    for (int k = 0; k < x.size(); ++k) {
        if (!usableX(x.at(k))) continue;
        for (const auto& s : series) {
            if (k >= s.second.size() || !std::isfinite(s.second.at(k))) continue;
            x0 = std::min(x0, x.at(k));
            x1 = std::max(x1, x.at(k));
            y0 = std::min(y0, s.second.at(k));
            y1 = std::max(y1, s.second.at(k));
        }
    }
    painter.setPen(QColor(0x22, 0x22, 0x22));
    QFont bold = font;
    bold.setPixelSize(14);
    bold.setBold(true);
    painter.setFont(bold);
    painter.drawText(QRectF(0, 6, size.width(), 28), Qt::AlignCenter, title);
    painter.setFont(font);
    if (!std::isfinite(x0) || !std::isfinite(y0)) {
        painter.drawText(plot, Qt::AlignCenter, QCoreApplication::translate("PythonViews", "No numbers to plot."));
        return image;
    }
    if (x1 <= x0) {
        x0 = logX ? x0 / 2 : x0 - 1;
        x1 = logX ? x1 * 2 : x1 + 1;
    }
    if (y1 <= y0) {
        y0 -= std::max(1.0, std::fabs(y0) * 0.1);
        y1 += std::max(1.0, std::fabs(y1) * 0.1);
    } else {
        const double pad = (y1 - y0) * 0.05;
        y0 -= pad;
        y1 += pad;
    }
    const double l0 = logX ? std::log10(x0) : x0, l1 = logX ? std::log10(x1) : x1;
    const auto px = [&](double v) { return plot.left() + ((logX ? std::log10(v) : v) - l0) / (l1 - l0) * plot.width(); };
    const auto py = [&](double v) { return plot.bottom() - (v - y0) / (y1 - y0) * plot.height(); };

    // The grid, and the numbers along the axes.
    const QColor grid(0xe3, 0xe6, 0xea), minor(0xf1, 0xf3, 0xf5), ink(0x44, 0x48, 0x50);
    const auto label = [](double v) { return QString::number(v, 'g', 4); };
    for (const double t : roundTicks(y0, y1, 7)) {
        painter.setPen(QPen(grid, 1));
        painter.drawLine(QPointF(plot.left(), py(t)), QPointF(plot.right(), py(t)));
        painter.setPen(ink);
        painter.drawText(QRectF(0, py(t) - 8, plot.left() - 6, 16), Qt::AlignRight | Qt::AlignVCenter, label(t));
    }
    if (logX) {
        for (int d = int(std::floor(l0)); d <= int(std::ceil(l1)); ++d)
            for (int m = 1; m <= 9; ++m) {
                const double v = m * std::pow(10.0, d);
                if (v < x0 || v > x1) continue;
                painter.setPen(QPen(m == 1 ? grid : minor, 1));
                painter.drawLine(QPointF(px(v), plot.top()), QPointF(px(v), plot.bottom()));
                if (m == 1) {
                    painter.setPen(ink);
                    painter.drawText(QRectF(px(v) - 40, plot.bottom() + 4, 80, 16), Qt::AlignCenter, label(v));
                }
            }
    } else {
        for (const double t : roundTicks(x0, x1, 8)) {
            painter.setPen(QPen(grid, 1));
            painter.drawLine(QPointF(px(t), plot.top()), QPointF(px(t), plot.bottom()));
            painter.setPen(ink);
            painter.drawText(QRectF(px(t) - 40, plot.bottom() + 4, 80, 16), Qt::AlignCenter, label(t));
        }
    }
    painter.setPen(QPen(ink, 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(plot);
    painter.drawText(QRectF(plot.left(), plot.bottom() + 24, plot.width(), 20), Qt::AlignCenter, xName + (logX ? QStringLiteral(" (log)") : QString()));
    if (series.size() == 1) {
        painter.save();
        painter.translate(16, plot.center().y());
        painter.rotate(-90);
        painter.drawText(QRectF(-plot.height() / 2, -10, plot.height(), 20), Qt::AlignCenter, series.first().first);
        painter.restore();
    }

    // The series: lines broken where there is no number; dots when few.
    static const QList<QColor> colours{QColor(0x1f, 0x77, 0xb4), QColor(0xff, 0x7f, 0x0e), QColor(0x2c, 0xa0, 0x2c), QColor(0xd6, 0x27, 0x28),
                                       QColor(0x94, 0x67, 0xbd), QColor(0x8c, 0x56, 0x4b), QColor(0xe3, 0x77, 0xc2), QColor(0x7f, 0x7f, 0x7f),
                                       QColor(0xbc, 0xbd, 0x22), QColor(0x17, 0xbe, 0xcf)};
    painter.save();
    painter.setClipRect(plot.adjusted(-1, -1, 1, 1));
    for (int s = 0; s < series.size(); ++s) {
        const QColor colour = colours.at(s % colours.size());
        QPainterPath path;
        bool open = false;
        int points = 0;
        for (int k = 0; k < x.size() && k < series.at(s).second.size(); ++k) {
            const double v = series.at(s).second.at(k);
            if (!usableX(x.at(k)) || !std::isfinite(v)) {
                open = false;
                continue;
            }
            const QPointF at(px(x.at(k)), py(v));
            if (open) path.lineTo(at);
            else path.moveTo(at);
            open = true;
            ++points;
        }
        painter.setPen(QPen(colour, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawPath(path);
        if (points <= 60) {
            painter.setBrush(colour);
            for (int k = 0; k < x.size() && k < series.at(s).second.size(); ++k)
                if (usableX(x.at(k)) && std::isfinite(series.at(s).second.at(k)))
                    painter.drawEllipse(QPointF(px(x.at(k)), py(series.at(s).second.at(k))), 2.6, 2.6);
            painter.setBrush(Qt::NoBrush);
        }
    }
    painter.restore();
    if (series.size() > 1) {   // a legend, at the top right
        qreal wide = 0;
        for (const auto& s : series) wide = std::max(wide, metrics.horizontalAdvance(s.first));
        const QRectF box(plot.right() - wide - 46, plot.top() + 8, wide + 38, 18.0 * series.size() + 8);
        painter.setPen(QPen(grid, 1));
        painter.setBrush(QColor(255, 255, 255, 230));
        painter.drawRect(box);
        for (int s = 0; s < series.size(); ++s) {
            const qreal y = box.top() + 13 + 18.0 * s;
            painter.setPen(QPen(colours.at(s % colours.size()), 2));
            painter.drawLine(QPointF(box.left() + 6, y), QPointF(box.left() + 24, y));
            painter.setPen(ink);
            painter.drawText(QPointF(box.left() + 30, y + metrics.ascent() / 2 - 1), series.at(s).first);
        }
    }
    return image;
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
                            "run in the Python Shell (Python > Plots in Qucs-S)."),
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
    a_view->setUniformRowHeights(true);
    a_view->setAlternatingRowColors(true);
    a_view->header()->setStretchLastSection(true);
    a_view->setContextMenuPolicy(Qt::CustomContextMenu);
    const auto expression = [](const QTreeWidgetItem* item) { return item->data(0, Qt::UserRole + 1).toString(); };
    connect(a_view, &QTreeWidget::itemDoubleClicked, this, [this, expression](QTreeWidgetItem* item) {
        if (item->data(0, Qt::UserRole).toBool() && !expression(item).isEmpty()) emit tableRequested(expression(item));
    });
    // A value opened: its insides asked for (once; and again after a
    // command, as it stays open).
    connect(a_view, &QTreeWidget::itemExpanded, this, [this, expression](QTreeWidgetItem* item) {
        const QString e = expression(item);
        if (e.isEmpty()) return;
        a_opened.insert(e);
        if (item->childCount() == 0) emit childrenRequested(e);
    });
    connect(a_view, &QTreeWidget::itemCollapsed, this, [this, expression](QTreeWidgetItem* item) { a_opened.remove(expression(item)); });
    connect(a_view, &QTreeWidget::customContextMenuRequested, this, [this, expression](const QPoint& at) {
        QTreeWidgetItem* item = a_view->itemAt(at);
        if (item == nullptr) return;
        const bool table = item->data(0, Qt::UserRole).toBool() && !expression(item).isEmpty();
        QMenu menu(this);
        QAction* asTable = menu.addAction(tr("View as Table"));
        asTable->setEnabled(table);
        QAction* display = menu.addAction(tr("Show in a Data Display"));
        display->setObjectName(QStringLiteral("pythonVariablesShowInDisplay"));
        display->setEnabled(table);
        QAction* copy = menu.addAction(tr("Copy Value"));
        QAction* chosen = menu.exec(a_view->viewport()->mapToGlobal(at));
        if (chosen == asTable) emit tableRequested(expression(item));
        else if (chosen == display) emit displayRequested(expression(item));
        else if (chosen == copy) QApplication::clipboard()->setText(item->text(3));
    });
    a_note = new QLabel(tr("The Python Shell's variables, after each command (modules, functions and classes aside). "
                           "Open one to see inside it; double-click an array, a list, a dictionary or a DataFrame to see "
                           "it as a table; its menu shows it in a data display of Qucs-S."),
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

void PythonVariablesPane::fill(QTreeWidgetItem* parent, const QJsonArray& variables)
{
    for (const QJsonValue& v : variables) {
        const QJsonObject o = v.toObject();
        auto* item = parent != nullptr ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(a_view);
        item->setText(0, o.value(QStringLiteral("name")).toString());
        item->setText(1, o.value(QStringLiteral("type")).toString());
        item->setText(2, o.value(QStringLiteral("size")).toString());
        item->setText(3, o.value(QStringLiteral("value")).toString());
        item->setToolTip(3, o.value(QStringLiteral("value")).toString());
        const bool table = o.value(QStringLiteral("table")).toBool();
        item->setData(0, Qt::UserRole, table);
        // Its expression: the name, or what the shell said it is.
        const QString expression = parent == nullptr ? item->text(0) : o.value(QStringLiteral("expression")).toString();
        item->setData(0, Qt::UserRole + 1, expression);
        if (table) item->setToolTip(0, tr("Double-click: the value as a table"));
        if (o.value(QStringLiteral("inside")).toBool() && !expression.isEmpty()) {
            item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
            if (a_opened.contains(expression)) item->setExpanded(true);   // (asks for its insides again)
        }
    }
}

void PythonVariablesPane::setVariables(const QJsonArray& variables)
{
    const QString chosen = a_view->currentItem() != nullptr ? a_view->currentItem()->text(0) : QString();
    const int scrolled = a_view->verticalScrollBar()->value();
    a_view->clear();
    fill(nullptr, variables);
    for (int k = 0; k < a_view->topLevelItemCount(); ++k)
        if (a_view->topLevelItem(k)->text(0) == chosen) a_view->setCurrentItem(a_view->topLevelItem(k));
    for (int k = 0; k < 3; ++k) a_view->resizeColumnToContents(k);
    a_view->verticalScrollBar()->setValue(scrolled);
    filter();
}

QTreeWidgetItem* PythonVariablesPane::rowOf(const QString& expression) const
{
    for (QTreeWidgetItemIterator it(a_view); *it; ++it)
        if ((*it)->data(0, Qt::UserRole + 1).toString() == expression) return *it;
    return nullptr;
}

void PythonVariablesPane::showChildren(const QString& expression, const QJsonArray& items)
{
    QTreeWidgetItem* item = rowOf(expression);
    if (item == nullptr) return;
    qDeleteAll(item->takeChildren());
    fill(item, items);
    if (items.isEmpty()) item->setChildIndicatorPolicy(QTreeWidgetItem::DontShowIndicator);
    a_view->resizeColumnToContents(0);   // (its names, set in, whole)
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
    for (QTreeWidgetItemIterator it(a_view, QTreeWidgetItemIterator::NotHidden); *it; ++it) {
        int depth = 0;
        for (const QTreeWidgetItem* up = (*it)->parent(); up != nullptr; up = up->parent()) ++depth;
        if ((*it)->parent() != nullptr && !(*it)->parent()->isExpanded()) continue;
        rows << QStringLiteral("%1%2: %3 [%4] = %5").arg(QString(depth * 2, QLatin1Char(' ')), (*it)->text(0), (*it)->text(1), (*it)->text(2), (*it)->text(3));
    }
    return rows;
}

// ----------------------------------------------------------------------
// The Data Viewer

void PythonTableModel::setView(int sort, bool descending, const QJsonObject& filters)
{
    a_sort = sort;
    a_descending = descending;
    a_filters = filters;
    ++a_token;
    beginResetModel();
    a_rows = 0;
    a_blocks.clear();
    a_index.clear();
    a_asked.clear();
    endResetModel();
    fetchFirst();
}

void PythonTableModel::setComplexMode(int mode)
{
    a_complex = mode;
    if (a_rows > 0 && a_columns > 0) emit dataChanged(index(0, 0), index(a_rows - 1, a_columns - 1));
}

void PythonTableModel::answer(const QJsonObject& table)
{
    if (table.contains(QStringLiteral("token")) && table.value(QStringLiteral("token")).toInt(-1) != a_token) return;   // (of a view gone)
    const QJsonArray shape = table.value(QStringLiteral("shape")).toArray();
    const int rows = shape.size() == 2 ? shape.at(0).toInt() : 0;
    const int columns = shape.size() == 2 ? shape.at(1).toInt() : 0;
    const int start = table.value(QStringLiteral("start")).toInt();
    a_total = table.value(QStringLiteral("total")).toInt(rows);
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
    QJsonObject request{{QStringLiteral("start"), block * kBlock}, {QStringLiteral("count"), kBlock}, {QStringLiteral("token"), a_token}};
    if (a_sort >= 0) {
        request.insert(QStringLiteral("sort"), a_sort);
        request.insert(QStringLiteral("descending"), a_descending);
    }
    if (!a_filters.isEmpty()) request.insert(QStringLiteral("filters"), a_filters);
    a_fetch(request);
}

namespace {
// A complex number as _qucs_data writes one: 1.5+2.0j, -0.0-1e-05j.
bool readComplex(const QString& text, double* re, double* im)
{
    static const QRegularExpression complex(QStringLiteral("^\\(?([-+]?(?:[0-9.]+(?:[eE][-+]?[0-9]+)?|nan|inf))"
                                                           "([-+](?:[0-9.]+(?:[eE][-+]?[0-9]+)?|nan|inf))j\\)?$"));
    const QRegularExpressionMatch m = complex.match(text);
    if (!m.hasMatch()) return false;
    bool a = false, b = false;
    *re = m.captured(1).toDouble(&a);
    *im = m.captured(2).toDouble(&b);
    if (!a) *re = m.captured(1).contains(QLatin1String("nan")) ? qQNaN() : (m.captured(1).startsWith(QLatin1Char('-')) ? -qInf() : qInf());
    if (!b) *im = m.captured(2).contains(QLatin1String("nan")) ? qQNaN() : (m.captured(2).startsWith(QLatin1Char('-')) ? -qInf() : qInf());
    return true;
}

QString fullDigits(double v) { return QString::number(v, 'g', 17); }
} // namespace

QString PythonTableModel::rowLabel(int row) const
{
    const auto found = a_index.constFind(row / kBlock);
    return found == a_index.constEnd() ? QString::number(row) : found->at(row % kBlock).toVariant().toString();
}

double PythonTableModel::number(int row, int column) const
{
    const auto found = a_blocks.constFind(row / kBlock);
    if (found == a_blocks.constEnd()) return qQNaN();
    const QJsonValue cell = found->at(row % kBlock).toArray().at(column);
    if (cell.isDouble()) return cell.toDouble();
    double re = 0, im = 0;
    if (!readComplex(cell.toString(), &re, &im)) {
        bool ok = false;
        const double v = cell.toString().toDouble(&ok);
        return ok ? v : qQNaN();
    }
    const double magnitude = std::hypot(re, im);
    switch (a_complex) {
    case Real: return re;
    case Imaginary: return im;
    case DbPhase:
    case Decibel: return 20.0 * std::log10(magnitude);
    case Phase: return std::atan2(im, re) * 180.0 / M_PI;
    default: return magnitude;
    }
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
    // A complex number as the mode has it.
    double re = 0, im = 0;
    if (a_complex != AsIs && cell.isString() && readComplex(cell.toString(), &re, &im)) {
        const double magnitude = std::hypot(re, im);
        const double phase = std::atan2(im, re) * 180.0 / M_PI;
        switch (a_complex) {
        case MagnitudePhase: return QStringLiteral("%1 \u2220 %2\u00b0").arg(fullDigits(magnitude), fullDigits(phase));
        case DbPhase: return QStringLiteral("%1 dB \u2220 %2\u00b0").arg(fullDigits(20.0 * std::log10(magnitude)), fullDigits(phase));
        case Real: return fullDigits(re);
        case Imaginary: return fullDigits(im);
        case Magnitude: return fullDigits(magnitude);
        case Decibel: return fullDigits(20.0 * std::log10(magnitude));
        case Phase: return fullDigits(phase);
        default: break;
        }
    }
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
        // (A complex part: ten digits as a float has them.)
        double re = 0, im = 0;
        if (a_complex != AsIs && a_complex != MagnitudePhase && a_complex != DbPhase && cell.isString()
            && readComplex(cell.toString(), &re, &im))
            return QString::number(number(index.row(), index.column()), 'g', 10);
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
    auto* plot = new QPushButton(tr("Plot"), this);
    plot->setObjectName(QStringLiteral("pythonDataPlot"));
    plot->setToolTip(tr("The columns selected plotted in the Python Plots pane: the first the x of the rest when two or "
                        "more are, else over the rows - complex values as they are shown"));
    connect(plot, &QPushButton::clicked, this, &PythonDataViewer::plotSelected);
    auto* row = new QHBoxLayout;
    row->addWidget(a_about, 1);
    row->addWidget(plot);
    row->addWidget(copy);
    row->addWidget(save);

    // A column's filter, the complex numbers' form.
    a_filterColumn = new QComboBox(this);
    a_filterColumn->setObjectName(QStringLiteral("pythonDataFilterColumn"));
    a_filterRule = new QLineEdit(this);
    a_filterRule->setObjectName(QStringLiteral("pythonDataFilterRule"));
    a_filterRule->setPlaceholderText(tr("Filter: > 5, <= 1e-3, != 0, or text it has - Return"));
    a_filterRule->setClearButtonEnabled(true);
    connect(a_filterRule, &QLineEdit::returnPressed, this, [this] {
        setFilter(a_filterColumn->currentIndex(), a_filterRule->text());
    });
    connect(a_filterColumn, &QComboBox::activated, this, [this](int column) {
        a_filterRule->setText(a_model->filters().value(QString::number(column)).toString());
    });
    auto* clear = new QPushButton(tr("Clear Filters"), this);
    connect(clear, &QPushButton::clicked, this, [this] {
        a_filterRule->clear();
        a_model->setView(a_model->sortColumn(), a_model->descending(), {});
        showFilters();
    });
    a_complexMode = new QComboBox(this);
    a_complexMode->setObjectName(QStringLiteral("pythonDataComplex"));
    a_complexMode->addItems({tr("a+bj"), tr("magnitude \u2220 phase"), tr("dB \u2220 phase"), tr("real part"), tr("imaginary part"),
                             tr("magnitude"), tr("dB (20 log10)"), tr("phase (\u00b0)")});
    a_complexMode->setToolTip(tr("How complex numbers are shown, copied, exported and plotted"));
    connect(a_complexMode, &QComboBox::currentIndexChanged, this, [this](int mode) { a_model->setComplexMode(mode); });
    a_filtersShown = new QLabel(this);
    a_filtersShown->setObjectName(QStringLiteral("pythonDataFilters"));
    a_filtersShown->setEnabled(false);
    auto* filters = new QHBoxLayout;
    filters->addWidget(a_filterColumn);
    filters->addWidget(a_filterRule, 1);
    filters->addWidget(clear);
    filters->addWidget(new QLabel(tr("Complex:"), this));
    filters->addWidget(a_complexMode);
    connect(a_model, &PythonTableModel::shapeKnown, this, [this] {
        // (The columns to filter, as the table has them.)
        const int chosen = a_filterColumn->currentIndex();
        a_filterColumn->clear();
        for (int c = 0; c < a_model->columnCount(); ++c) a_filterColumn->addItem(a_model->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString());
        a_filterColumn->setCurrentIndex(std::max(0, std::min(chosen, a_filterColumn->count() - 1)));
    });

    // Sorted by a click on a column's header: up, down, as it was.
    a_view->horizontalHeader()->setSectionsClickable(true);
    a_view->horizontalHeader()->setSortIndicatorShown(false);
    connect(a_view->horizontalHeader(), &QHeaderView::sectionClicked, this, [this](int column) {
        if (a_model->sortColumn() != column) sortBy(column, false);
        else if (!a_model->descending()) sortBy(column, true);
        else sortBy(-1, false);
    });

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(row);
    layout->addLayout(filters);
    layout->addWidget(a_filtersShown);
    layout->addWidget(a_view, 1);
    resize(760, 520);
    connect(a_model, &PythonTableModel::allFetched, this, [this] {
        if (!a_plotColumns.isEmpty()) {
            const QList<int> columns = a_plotColumns;
            a_plotColumns.clear();
            plotColumns(columns);
        }
        if (a_exportPath.isEmpty()) return;
        const QString path = a_exportPath;
        a_exportPath.clear();
        exportTo(path);
    });
    showFilters();
}

void PythonDataViewer::setFilter(int column, const QString& rule)
{
    if (column < 0) return;
    QJsonObject filters = a_model->filters();
    if (rule.trimmed().isEmpty()) filters.remove(QString::number(column));
    else filters.insert(QString::number(column), rule.trimmed());
    a_model->setView(a_model->sortColumn(), a_model->descending(), filters);
    showFilters();
}

void PythonDataViewer::sortBy(int column, bool descending)
{
    a_model->setView(column, descending, a_model->filters());
    QHeaderView* header = a_view->horizontalHeader();
    header->setSortIndicatorShown(column >= 0);
    if (column >= 0) header->setSortIndicator(column, descending ? Qt::DescendingOrder : Qt::AscendingOrder);
}

void PythonDataViewer::showFilters()
{
    QStringList said;
    const QJsonObject filters = a_model->filters();
    for (auto it = filters.begin(); it != filters.end(); ++it) {
        const int column = it.key().toInt();
        const QString name = a_model->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString();
        said << QStringLiteral("%1: %2").arg(name.isEmpty() ? it.key() : name, it.value().toString());
    }
    a_filtersShown->setText(said.isEmpty() ? QString() : tr("Filtered - %1").arg(said.join(QStringLiteral("; "))));
    a_filtersShown->setVisible(!said.isEmpty());
}

void PythonDataViewer::plotSelected()
{
    QList<int> columns;
    if (a_view->selectionModel() != nullptr)
        for (const QModelIndex& i : a_view->selectionModel()->selectedIndexes())
            if (!columns.contains(i.column())) columns << i.column();
    std::sort(columns.begin(), columns.end());
    if (columns.isEmpty())   // (none chosen: every column, over the rows)
        for (int c = 0; c < a_model->columnCount(); ++c) columns << c;
    plotColumns(columns);
}

void PythonDataViewer::plotColumns(const QList<int>& columns)
{
    if (columns.isEmpty() || a_model->rowCount() == 0) return;
    if (!a_model->complete()) {   // (every row first)
        a_plotColumns = columns;
        a_about->setText(tr("%1: reading every row to plot...").arg(a_name));
        a_model->fetchAll();
        return;
    }
    const int rows = a_model->rowCount();
    const auto header = [this](int c) { return a_model->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString(); };
    QVector<double> x(rows);
    QString xName;
    QList<int> ys = columns;
    if (columns.size() >= 2) {
        xName = header(columns.first());
        ys.removeFirst();
        for (int r = 0; r < rows; ++r) x[r] = a_model->number(r, columns.first());
    } else {
        // Over the rows' labels when they are numbers, else their places.
        bool labels = true;
        for (int r = 0; r < rows && labels; ++r) a_model->rowLabel(r).toDouble(&labels);
        xName = labels ? tr("index") : tr("row");
        for (int r = 0; r < rows; ++r) x[r] = labels ? a_model->rowLabel(r).toDouble() : r;
    }
    QList<std::pair<QString, QVector<double>>> series;
    for (const int c : std::as_const(ys)) {
        QVector<double> y(rows);
        for (int r = 0; r < rows; ++r) y[r] = a_model->number(r, c);
        QString name = header(c);
        if (a_model->complexMode() != PythonTableModel::AsIs) name += QStringLiteral(" (%1)").arg(a_complexMode->currentText());
        series.append({name, y});
    }
    // A log x when it is positive over two decades or more (a frequency).
    double lowest = qInf(), highest = -qInf();
    bool positive = true;
    for (const double v : std::as_const(x)) {
        if (!std::isfinite(v)) continue;
        positive = positive && v > 0;
        lowest = std::min(lowest, v);
        highest = std::max(highest, v);
    }
    const bool logX = positive && lowest > 0 && highest / lowest >= 100.0;
    const QString title = ys.size() == 1 ? QStringLiteral("%1: %2").arg(a_name, series.first().first) : a_name;
    emit plotted(qucs_s::python::plotImage(title, xName, x, series, logX), title);
    a_about->setText(tr("%1: plotted in the Python Plots pane.").arg(a_name));
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
                       .arg(table.value(QStringLiteral("total")).toInt(shape.at(0).toInt())).arg(shape.at(1).toInt());
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

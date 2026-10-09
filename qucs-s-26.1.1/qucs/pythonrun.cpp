/*
 * pythonrun.cpp - a Python script run as a program of its own, its output
 *                 in a console (the Python toolbar's Run)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "pythonrun.h"

#include "main.h"

#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTimer>
#include <QVBoxLayout>

namespace {

// A traceback's place: File "/path/x.py", line 3
const QRegularExpression& placePattern()
{
    static const QRegularExpression pattern(QStringLiteral("File \"([^\"]+)\", line (\\d+)"));
    return pattern;
}

} // namespace

PythonRunConsole::PythonRunConsole(QWidget* parent)
    : QWidget(parent), a_decoder(QStringDecoder::Utf8)
{
    a_output = new QPlainTextEdit(this);
    a_output->setObjectName(QStringLiteral("pythonRunOutput"));
    a_output->setReadOnly(true);
    a_output->setUndoRedoEnabled(false);
    a_output->setMaximumBlockCount(kMostLines);
    a_output->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    // Fixed-width, as a terminal's: a traceback's marks under their code.
    QFont font = QucsSettings.textFont;
    if (!QFontInfo(font).fixedPitch()) font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    a_output->setFont(font);
    a_output->viewport()->installEventFilter(this);
    a_output->viewport()->setMouseTracking(true);

    a_status = new QLabel(tr("Run a Python script with Run on the Python toolbar (F2)."), this);
    a_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    a_stop = new QPushButton(tr("Stop"), this);
    a_stop->setObjectName(QStringLiteral("pythonRunStop"));
    a_stop->setEnabled(false);
    a_stop->setToolTip(tr("Ends the script that is running."));
    a_clear = new QPushButton(tr("Clear"), this);
    a_clear->setToolTip(tr("Clears the output."));
    connect(a_stop, &QPushButton::clicked, this, &PythonRunConsole::stop);
    connect(a_clear, &QPushButton::clicked, this, &PythonRunConsole::clear);

    auto* row = new QHBoxLayout;
    row->setContentsMargins(4, 0, 4, 2);
    row->addWidget(a_status, 1);
    row->addWidget(a_stop);
    row->addWidget(a_clear);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addWidget(a_output, 1);
    layout->addLayout(row);
}

PythonRunConsole::~PythonRunConsole()
{
    if (a_process != nullptr) {
        disconnect(a_process, nullptr, this, nullptr);
        a_process->kill();
        a_process->waitForFinished(2000);
    }
}

bool PythonRunConsole::isRunning() const
{
    return a_process != nullptr;
}

QString PythonRunConsole::outputText() const
{
    return a_output->toPlainText();
}

bool PythonRunConsole::run(const QString& interpreter, const QString& script)
{
    if (a_process != nullptr) {   // the one going, ended at once
        disconnect(a_process, nullptr, this, nullptr);
        a_process->kill();
        a_process->waitForFinished(2000);
        a_process->deleteLater();
        a_process = nullptr;
    }
    clear();
    a_script = script;
    a_interpreter = interpreter;
    a_exitCode = -1;
    a_stopped = false;
    a_decoder.resetState();
    const QFileInfo info(script);
    note(tr("%1 %2   (in %3)").arg(QFileInfo(interpreter).fileName(), info.fileName(), QDir::toNativeSeparators(info.absolutePath())));

    a_process = new QProcess(this);
    a_process->setProcessChannelMode(QProcess::MergedChannels);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    environment.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    a_process->setProcessEnvironment(environment);
    a_process->setWorkingDirectory(info.absolutePath());
    connect(a_process, &QProcess::readyReadStandardOutput, this, &PythonRunConsole::readOutput);
    connect(a_process, &QProcess::finished, this, &PythonRunConsole::runFinished);
    a_clock.start();
    a_process->start(interpreter, {QStringLiteral("-u"), info.absoluteFilePath()});
    if (!a_process->waitForStarted(10000)) {
        note(tr("%1 could not be started: %2").arg(interpreter, a_process->errorString()));
        setStatus(tr("%1 was not run.").arg(info.fileName()));
        disconnect(a_process, nullptr, this, nullptr);
        a_process->deleteLater();
        a_process = nullptr;
        emit finished(-1);
        return false;
    }
    a_process->closeWriteChannel();   // input() reads the end of the file, not nothing for ever
    a_stop->setEnabled(true);
    setStatus(tr("Running %1...").arg(info.fileName()));
    emit started();
    return true;
}

void PythonRunConsole::stop()
{
    if (a_process == nullptr) return;
    a_stopped = true;
#ifdef Q_OS_WIN
    a_process->kill();   // (a console program takes no polite request)
#else
    a_process->terminate();
    QPointer<QProcess> process = a_process;
    QTimer::singleShot(2000, this, [process] {
        if (process != nullptr && process->state() != QProcess::NotRunning) process->kill();
    });
#endif
}

void PythonRunConsole::clear()
{
    a_output->clear();
    a_lineOpen = false;
    a_overwrite = false;
}

void PythonRunConsole::readOutput()
{
    if (a_process == nullptr) return;
    const QByteArray bytes = a_process->readAllStandardOutput();
    if (!bytes.isEmpty()) append(a_decoder.decode(bytes));
}

void PythonRunConsole::runFinished()
{
    if (a_process == nullptr) return;
    readOutput();
    if (a_lineOpen) endLine();
    QProcess* process = a_process;
    a_process = nullptr;
    disconnect(process, nullptr, this, nullptr);
    process->deleteLater();
    const QString seconds = QString::number(a_clock.elapsed() / 1000.0, 'f', 2);
    const QString name = QFileInfo(a_script).fileName();
    if (a_stopped) {
        a_exitCode = -1;
        note(tr("Stopped after %1 s.").arg(seconds));
        setStatus(tr("%1 was stopped after %2 s.").arg(name, seconds));
    } else if (process->exitStatus() != QProcess::NormalExit) {
        a_exitCode = -1;
        note(tr("Ended abnormally after %1 s.").arg(seconds));
        setStatus(tr("%1 ended abnormally after %2 s.").arg(name, seconds));
    } else {
        a_exitCode = process->exitCode();
        note(tr("Exit code %1 after %2 s.").arg(a_exitCode).arg(seconds));
        setStatus(tr("%1: exit code %2 after %3 s.").arg(name).arg(a_exitCode).arg(seconds));
    }
    a_stop->setEnabled(false);
    emit finished(a_exitCode);
}

void PythonRunConsole::append(const QString& text)
{
    QTextCursor cursor(a_output->document());
    cursor.movePosition(QTextCursor::End);
    QString pending;
    const auto flush = [&] {
        if (pending.isEmpty()) return;
        if (a_overwrite) {   // after a carriage return: the line begins again
            cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::KeepAnchor);
            cursor.removeSelectedText();
            a_overwrite = false;
        }
        cursor.insertText(pending, QTextCharFormat());
        pending.clear();
        a_lineOpen = true;
    };
    for (qsizetype k = 0; k < text.size(); ++k) {
        const QChar c = text.at(k);
        if (c == QLatin1Char('\r')) {
            flush();
            if (k + 1 < text.size() && text.at(k + 1) == QLatin1Char('\n')) continue;   // CR LF: the LF ends it
            a_overwrite = true;
        } else if (c == QLatin1Char('\n')) {
            flush();
            a_overwrite = false;
            endLine();
            cursor.movePosition(QTextCursor::End);
        } else {
            pending += c;
        }
    }
    flush();
    a_output->verticalScrollBar()->setValue(a_output->verticalScrollBar()->maximum());
}

void PythonRunConsole::endLine()
{
    QTextDocument* document = a_output->document();
    QTextBlock block = document->lastBlock();
    // A traceback's place: a link to it, when the file is there.
    const QRegularExpressionMatch m = placePattern().match(block.text());
    if (m.hasMatch()) {
        QString file = m.captured(1);
        if (QFileInfo(file).isRelative() && !a_script.isEmpty()) file = QFileInfo(a_script).absoluteDir().filePath(file);
        if (QFileInfo(file).isFile()) {
            QTextCursor link(block);
            link.setPosition(block.position() + m.capturedStart(0));
            link.setPosition(block.position() + m.capturedEnd(0), QTextCursor::KeepAnchor);
            QTextCharFormat format;
            format.setAnchor(true);
            format.setAnchorHref(QStringLiteral("%1#%2").arg(QFileInfo(file).absoluteFilePath(), m.captured(2)));
            format.setForeground(palette().link());
            format.setFontUnderline(true);
            link.mergeCharFormat(format);
        }
    }
    QTextCursor cursor(document);
    cursor.movePosition(QTextCursor::End);
    cursor.insertBlock(QTextBlockFormat(), QTextCharFormat());
    a_lineOpen = false;
}

void PythonRunConsole::note(const QString& text)
{
    if (a_lineOpen) endLine();
    QTextCursor cursor(a_output->document());
    cursor.movePosition(QTextCursor::End);
    QTextCharFormat format;
    format.setForeground(palette().placeholderText());
    format.setFontItalic(true);
    cursor.insertText(text, format);
    cursor.insertBlock(QTextBlockFormat(), QTextCharFormat());
    a_output->verticalScrollBar()->setValue(a_output->verticalScrollBar()->maximum());
}

void PythonRunConsole::setStatus(const QString& text)
{
    a_status->setText(text);
}

bool PythonRunConsole::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == a_output->viewport()) {
        if (event->type() == QEvent::MouseMove) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            a_output->viewport()->setCursor(a_output->anchorAt(mouse->position().toPoint()).isEmpty() ? Qt::IBeamCursor : Qt::PointingHandCursor);
        } else if (event->type() == QEvent::MouseButtonRelease) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            const QString href = a_output->anchorAt(mouse->position().toPoint());
            if (mouse->button() == Qt::LeftButton && !href.isEmpty() && !a_output->textCursor().hasSelection()) {
                const qsizetype hash = href.lastIndexOf(QLatin1Char('#'));
                emit locationRequested(href.left(hash), href.mid(hash + 1).toInt());
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

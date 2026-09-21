/*
 * processconsole.cpp - a console around an interactive program, for the
 *                      Terminal and Python Shell docks
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "processconsole.h"

#include "main.h"
#include "qucs.h"

#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QStandardPaths>
#include <QTextCursor>
#include <QTimer>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
#include <QProcess>
#else
#include <QSocketNotifier>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#ifdef Q_OS_MACOS
#include <util.h>
#else
#include <pty.h>
#endif
#endif

// ---------------------------------------------------------------------
// ConsoleProcess

ConsoleProcess::ConsoleProcess(QObject* parent) : QObject(parent) {}

ConsoleProcess::~ConsoleProcess()
{
    endProcess();
}

#ifdef Q_OS_WIN

bool ConsoleProcess::start(const QString& program, const QStringList& args,
                           const QString& workDir, const QStringList& extraEnv)
{
    stop();
    a_error.clear();
    a_process = new QProcess(this);
    a_process->setProcessChannelMode(QProcess::MergedChannels);
    if (!workDir.isEmpty()) a_process->setWorkingDirectory(workDir);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const QString& entry : extraEnv) {
        const int eq = entry.indexOf(QLatin1Char('='));
        if (eq > 0) env.insert(entry.left(eq), entry.mid(eq + 1));
    }
    a_process->setProcessEnvironment(env);
    connect(a_process, &QProcess::readyRead, this, [this] { emit output(a_process->readAll()); });
    connect(a_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        const int exitCode = status == QProcess::NormalExit ? code : -1;
        cleanUp();
        emit finished(exitCode);
    });
    a_process->start(program, args);
    if (!a_process->waitForStarted(5000)) {
        a_error = a_process->errorString();
        cleanUp();
        return false;
    }
    return true;
}

bool ConsoleProcess::isRunning() const
{
    return a_process != nullptr && a_process->state() != QProcess::NotRunning;
}

qint64 ConsoleProcess::processId() const
{
    return a_process != nullptr ? a_process->processId() : -1;
}

void ConsoleProcess::write(const QByteArray& data)
{
    if (isRunning()) a_process->write(data);
}

void ConsoleProcess::interrupt()
{
    // No console to send a Ctrl-C through on pipes; nothing to do.
}

void ConsoleProcess::stop()
{
    if (a_process == nullptr) return;
    endProcess();
    emit finished(-1);
}

void ConsoleProcess::endProcess()
{
    if (a_process == nullptr) return;
    QProcess* p = a_process;
    a_process = nullptr;
    p->disconnect(this);
    if (p->state() != QProcess::NotRunning) {
        p->closeWriteChannel();
        if (!p->waitForFinished(500)) {
            p->kill();
            p->waitForFinished(1000);
        }
    }
    p->deleteLater();
}

void ConsoleProcess::cleanUp()
{
    if (a_process != nullptr) {
        a_process->deleteLater();
        a_process = nullptr;
    }
}

#else // Unix: a pseudo-terminal

bool ConsoleProcess::start(const QString& program, const QStringList& args,
                           const QString& workDir, const QStringList& extraEnv)
{
    stop();
    a_error.clear();
    const QString path = QFileInfo(program).isAbsolute() ? program : QStandardPaths::findExecutable(program);
    if (path.isEmpty() || !QFileInfo(path).isExecutable()) {
        a_error = tr("%1: not found or not executable").arg(program);
        return false;
    }

    // Everything the child needs, prepared before the fork: after it only
    // async-signal-safe calls are made until exec.
    QByteArrayList argvBytes;
    argvBytes << QFile::encodeName(path);
    for (const QString& arg : args) argvBytes << arg.toLocal8Bit();
    std::vector<char*> argv;
    for (QByteArray& b : argvBytes) argv.push_back(b.data());
    argv.push_back(nullptr);

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("TERM"), QStringLiteral("dumb"));   // plain output, few escape sequences
    for (const QString& entry : extraEnv) {
        const int eq = entry.indexOf(QLatin1Char('='));
        if (eq > 0) env.insert(entry.left(eq), entry.mid(eq + 1));
    }
    QByteArrayList envBytes;
    const QStringList keys = env.keys();
    for (const QString& key : keys) envBytes << (key + QLatin1Char('=') + env.value(key)).toLocal8Bit();
    std::vector<char*> envp;
    for (QByteArray& b : envBytes) envp.push_back(b.data());
    envp.push_back(nullptr);

    const QByteArray dir = QFile::encodeName(workDir);

    struct winsize ws;
    std::memset(&ws, 0, sizeof ws);
    ws.ws_row = 40;
    ws.ws_col = 120;

    int master = -1;
    const pid_t pid = forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0) {
        a_error = QString::fromLocal8Bit(std::strerror(errno));
        return false;
    }
    if (pid == 0) {
        // The child: its own session on the pseudo-terminal (forkpty did
        // that), default signal dispositions, the working directory, exec.
        ::signal(SIGINT, SIG_DFL);
        ::signal(SIGQUIT, SIG_DFL);
        ::signal(SIGTSTP, SIG_DFL);
        ::signal(SIGTTIN, SIG_DFL);
        ::signal(SIGTTOU, SIG_DFL);
        ::signal(SIGPIPE, SIG_DFL);
        ::signal(SIGHUP, SIG_DFL);
        if (!dir.isEmpty() && ::chdir(dir.constData()) != 0) { /* start where we are */ }
        ::execve(argv[0], argv.data(), envp.data());
        ::_exit(127);
    }

    a_pid = pid;
    a_master = master;
    ::fcntl(a_master, F_SETFL, ::fcntl(a_master, F_GETFL) | O_NONBLOCK);
    ::fcntl(a_master, F_SETFD, FD_CLOEXEC);
    a_notifier = new QSocketNotifier(a_master, QSocketNotifier::Read, this);
    connect(a_notifier, &QSocketNotifier::activated, this, &ConsoleProcess::readFromPty);
    // A program that exits without closing the terminal (a stopped job
    // holding it, say) is noticed by this.
    a_reaper = new QTimer(this);
    a_reaper->setInterval(500);
    connect(a_reaper, &QTimer::timeout, this, [this] { reap(false); });
    a_reaper->start();
    return true;
}

bool ConsoleProcess::isRunning() const
{
    return a_pid > 0;
}

qint64 ConsoleProcess::processId() const
{
    return a_pid;
}

void ConsoleProcess::write(const QByteArray& data)
{
    if (a_master < 0) return;
    const char* p = data.constData();
    qsizetype left = data.size();
    while (left > 0) {
        const ssize_t n = ::write(a_master, p, static_cast<size_t>(left));
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) { ::usleep(1000); continue; }
            return;
        }
        p += n;
        left -= n;
    }
}

void ConsoleProcess::interrupt()
{
    // The terminal turns it into SIGINT for the foreground job.
    write(QByteArray(1, '\003'));
}

void ConsoleProcess::readFromPty()
{
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(a_master, buf, sizeof buf);
        if (n > 0) {
            emit output(QByteArray(buf, static_cast<int>(n)));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) return;   // drained
        // End of file (macOS) or EIO (Linux): the other side is gone.
        reap(true);
        return;
    }
}

// Collects the child when it has exited - waiting a little for it when
// \a wait, as the terminal already reported the end. True once it is gone.
bool ConsoleProcess::reap(bool wait)
{
    if (a_pid <= 0) return true;
    int status = 0;
    pid_t r = ::waitpid(static_cast<pid_t>(a_pid), &status, WNOHANG);
    for (int i = 0; r == 0 && wait && i < 50; ++i) {   // up to half a second
        ::usleep(10000);
        r = ::waitpid(static_cast<pid_t>(a_pid), &status, WNOHANG);
    }
    if (r == 0) {
        if (!wait) return false;
        // The terminal closed but the process lingers: end it.
        ::kill(static_cast<pid_t>(a_pid), SIGKILL);
        r = ::waitpid(static_cast<pid_t>(a_pid), &status, 0);
    }
    // Whatever it wrote last.
    if (a_master >= 0) {
        char buf[4096];
        for (;;) {
            const ssize_t n = ::read(a_master, buf, sizeof buf);
            if (n <= 0) break;
            emit output(QByteArray(buf, static_cast<int>(n)));
        }
    }
    int exitCode = -1;
    if (r > 0 && WIFEXITED(status)) exitCode = WEXITSTATUS(status);
    cleanUp();
    emit finished(exitCode);
    return true;
}

void ConsoleProcess::stop()
{
    if (a_pid <= 0) return;
    endProcess();
    emit finished(-1);
}

void ConsoleProcess::endProcess()
{
    if (a_pid <= 0) return;
    const pid_t pid = static_cast<pid_t>(a_pid);
    // A hang-up, as a closing terminal window gives; the shell passes it
    // on to its jobs. Then a kill if that was not enough.
    ::kill(pid, SIGHUP);
    if (a_master >= 0) { ::close(a_master); a_master = -1; }
    int status = 0;
    pid_t r = ::waitpid(pid, &status, WNOHANG);
    for (int i = 0; r == 0 && i < 50; ++i) {
        ::usleep(10000);
        r = ::waitpid(pid, &status, WNOHANG);
    }
    if (r == 0) {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, 0);
    }
    cleanUp();
}

void ConsoleProcess::cleanUp()
{
    if (a_notifier != nullptr) {
        a_notifier->setEnabled(false);
        a_notifier->deleteLater();
        a_notifier = nullptr;
    }
    if (a_reaper != nullptr) {
        a_reaper->stop();
        a_reaper->deleteLater();
        a_reaper = nullptr;
    }
    if (a_master >= 0) { ::close(a_master); a_master = -1; }
    a_pid = -1;
}

#endif // Q_OS_WIN

// ---------------------------------------------------------------------
// ProcessConsole

ProcessConsole::~ProcessConsole()
{
    // The program goes with the console - quietly, the widgets being on
    // their way out too.
    a_process->disconnect(this);
    a_process->stop();
}

ProcessConsole::ProcessConsole(QWidget* parent)
    : QWidget(parent),
      a_process(new ConsoleProcess(this)),
      a_output(new QPlainTextEdit(this)),
      a_input(new QLineEdit(this)),
      a_status(new QLabel(this)),
      a_buttonInterrupt(new QPushButton(tr("Interrupt"), this)),
      a_buttonRestart(new QPushButton(tr("Restart"), this)),
      a_buttonClear(new QPushButton(tr("Clear"), this)),
      a_buttonProjectDir(new QPushButton(tr("Project dir"), this)),
      a_pendingFlush(new QTimer(this)),
      a_decoder(QStringDecoder::Utf8)
{
    a_pendingFlush->setSingleShot(true);
    a_pendingFlush->setInterval(3000);   // a program that says nothing gets its input then
    connect(a_pendingFlush, &QTimer::timeout, this, &ProcessConsole::flushPendingInput);
    QFont font;
    font.setFamily("monospace");
    font.setStyleHint(QFont::Monospace);
    font.setPointSize(10);
    a_output->setFont(font);
    a_output->setReadOnly(true);
    a_output->setLineWrapMode(QPlainTextEdit::NoWrap);
    a_output->setMaximumBlockCount(10000);
    a_output->setUndoRedoEnabled(false);
    a_input->setFont(font);
    a_input->setClearButtonEnabled(true);
    a_input->setPlaceholderText(tr("Command - Enter runs it, Up/Down browse the history, Ctrl-C interrupts, Ctrl-D ends the input"));
    a_input->installEventFilter(this);
    a_buttonInterrupt->setToolTip(tr("Ctrl-C: interrupt what the program is doing"));
    a_buttonRestart->setToolTip(tr("Start the program afresh"));
    a_buttonProjectDir->setToolTip(tr("Change the program's directory to the project directory"));
    a_status->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto* bottom = new QHBoxLayout;
    bottom->addWidget(a_input, 1);
    bottom->addWidget(a_buttonInterrupt);
    bottom->addWidget(a_buttonProjectDir);
    bottom->addWidget(a_buttonRestart);
    bottom->addWidget(a_buttonClear);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(3);
    layout->addWidget(a_output, 1);
    layout->addLayout(bottom);
    layout->addWidget(a_status);

    connect(a_process, &ConsoleProcess::output, this, &ProcessConsole::slotOutput);
    connect(a_process, &ConsoleProcess::finished, this, &ProcessConsole::slotFinished);
    connect(a_input, &QLineEdit::returnPressed, this, &ProcessConsole::slotSend);
    connect(a_buttonInterrupt, &QPushButton::clicked, this, &ProcessConsole::interrupt);
    connect(a_buttonRestart, &QPushButton::clicked, this, &ProcessConsole::restart);
    connect(a_buttonClear, &QPushButton::clicked, this, &ProcessConsole::clear);
    connect(a_buttonProjectDir, &QPushButton::clicked, this, &ProcessConsole::changeToProjectDirectory);
    setChangeDirectoryCommand(QString());
    setStatus(tr("not started"));
}

void ProcessConsole::setProgram(const QString& program, const QStringList& args, const QStringList& extraEnv)
{
    a_program = program;
    a_args = args;
    a_env = extraEnv;
}

void ProcessConsole::setWorkingDirectory(const QString& dir)
{
    a_workDir = dir;
}

void ProcessConsole::setChangeDirectoryCommand(const QString& command, Quoting quoting)
{
    a_cdCommand = command;
    a_cdQuoting = quoting;
    a_buttonProjectDir->setVisible(!command.isEmpty());
}

bool ProcessConsole::isRunning() const
{
    return a_process->isRunning();
}

QString ProcessConsole::outputText() const
{
    return a_output->toPlainText();
}

QString ProcessConsole::startDirectory() const
{
    if (!a_workDir.isEmpty()) return a_workDir;
    if (QucsMain != nullptr && !QucsMain->ProjName.isEmpty()) return QucsSettings.QucsWorkDir.absolutePath();
    return QDir::homePath();
}

QString ProcessConsole::quotedForShell(const QString& path)
{
#ifdef Q_OS_WIN
    return QLatin1Char('"') + QString(path).replace(QLatin1Char('"'), QStringLiteral("`\"")) + QLatin1Char('"');
#else
    return QLatin1Char('\'') + QString(path).replace(QLatin1Char('\''), QStringLiteral("'\\''")) + QLatin1Char('\'');
#endif
}

QString ProcessConsole::quotedForPython(const QString& path)
{
    QString s = path;
    s.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    s.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QLatin1Char('"') + s + QLatin1Char('"');
}

bool ProcessConsole::start()
{
    if (isRunning()) return true;
    if (a_program.isEmpty()) {
        appendNote(tr("No program to run."));
        return false;
    }
    a_decoder.resetState();
    a_escape = Plain;
    a_overwriteLine = false;
    a_seenOutput = false;
    a_pendingInput.clear();
    const QString dir = startDirectory();
    if (!a_process->start(a_program, a_args, dir, a_env)) {
        appendNote(tr("Could not start %1: %2").arg(a_program, a_process->errorString()));
        setStatus(tr("%1: could not be started").arg(a_program));
        return false;
    }
    setStatus(tr("%1 (pid %2) in %3").arg(QDir::toNativeSeparators(a_program)).arg(a_process->processId()).arg(QDir::toNativeSeparators(dir)));
    emit started();
    return true;
}

void ProcessConsole::stop()
{
    if (isRunning()) a_process->stop();
}

void ProcessConsole::restart()
{
    stop();
    if (!a_output->document()->isEmpty()) appendNote(tr("--- restarted ---"));
    start();
    a_input->setFocus();
}

void ProcessConsole::interrupt()
{
    if (isRunning()) a_process->interrupt();
    a_input->setFocus();
}

void ProcessConsole::clear()
{
    a_output->clear();
    a_overwriteLine = false;
    a_input->setFocus();
}

void ProcessConsole::sendLine(const QString& line)
{
    if (!isRunning() && !start()) return;
#ifdef Q_OS_WIN
    appendOutput(line + QLatin1Char('\n'));   // no terminal to echo it
#endif
    const QByteArray bytes = (line + QLatin1Char('\n')).toUtf8();
    if (!a_seenOutput) {
        // Not before the program is up and listening (see a_pendingInput).
        a_pendingInput += bytes;
        if (!a_pendingFlush->isActive()) a_pendingFlush->start();
        return;
    }
    a_process->write(bytes);
}

void ProcessConsole::flushPendingInput()
{
    a_pendingFlush->stop();
    a_seenOutput = true;
    if (a_pendingInput.isEmpty() || !isRunning()) return;
    const QByteArray bytes = a_pendingInput;
    a_pendingInput.clear();
    a_process->write(bytes);
}

void ProcessConsole::changeToProjectDirectory()
{
    if (a_cdCommand.isEmpty()) return;
    const QString dir = (QucsMain != nullptr && !QucsMain->ProjName.isEmpty())
                            ? QucsSettings.QucsWorkDir.absolutePath()
                            : QDir::homePath();
    const QString native = QDir::toNativeSeparators(dir);
    sendLine(a_cdCommand.arg(a_cdQuoting == PythonQuoting ? quotedForPython(native) : quotedForShell(native)));
    a_input->setFocus();
}

void ProcessConsole::slotSend()
{
    const QString line = a_input->text();
    a_input->clear();
    if (!line.trimmed().isEmpty() && (a_history.isEmpty() || a_history.last() != line))
        a_history << line;
    a_historyPos = a_history.size();
    a_historyDraft.clear();
    sendLine(line);
}

void ProcessConsole::historyStep(int direction)
{
    if (a_history.isEmpty()) return;
    if (a_historyPos == a_history.size()) a_historyDraft = a_input->text();
    const int pos = qBound(0, a_historyPos + direction, a_history.size());
    if (pos == a_historyPos) return;
    a_historyPos = pos;
    a_input->setText(pos == a_history.size() ? a_historyDraft : a_history.at(pos));
}

bool ProcessConsole::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == a_input && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        const bool ctrl = key->modifiers().testFlag(Qt::ControlModifier);
        if (key->key() == Qt::Key_Up && !ctrl) { historyStep(-1); return true; }
        if (key->key() == Qt::Key_Down && !ctrl) { historyStep(+1); return true; }
        if (ctrl && key->key() == Qt::Key_C && !a_input->hasSelectedText()) { interrupt(); return true; }
        if (ctrl && key->key() == Qt::Key_D && a_input->text().isEmpty()) {
            if (isRunning()) a_process->write(QByteArray(1, '\004'));   // end of file
            return true;
        }
        if (ctrl && key->key() == Qt::Key_L) { clear(); return true; }
    }
    return QWidget::eventFilter(watched, event);
}

void ProcessConsole::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (!isRunning() && !a_program.isEmpty()) start();
}

void ProcessConsole::slotOutput(const QByteArray& data)
{
    appendOutput(a_decoder.decode(data));
    if (!a_seenOutput) flushPendingInput();   // it is up: what was typed meanwhile
}

void ProcessConsole::slotFinished(int exitCode)
{
    appendNote(exitCode < 0 ? tr("--- %1 ended ---").arg(a_program)
                            : tr("--- %1 exited with code %2 ---").arg(a_program).arg(exitCode));
    setStatus(tr("%1: not running (Restart starts it again)").arg(a_program));
    emit finished(exitCode);
}

// Text from the program: escape sequences dropped, carriage return and
// backspace applied, the rest appended.
void ProcessConsole::appendOutput(const QString& text)
{
    QTextCursor cursor(a_output->document());
    cursor.movePosition(QTextCursor::End);
    cursor.beginEditBlock();
    QString run;   // plain characters waiting to be inserted
    auto flush = [&] {
        if (run.isEmpty()) return;
        if (a_overwriteLine) {
            // Back to the start of the line: what follows replaces it.
            cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::KeepAnchor);
            cursor.removeSelectedText();
            a_overwriteLine = false;
        }
        cursor.insertText(run);
        run.clear();
    };
    for (const QChar ch : text) {
        switch (a_escape) {
        case Escape:
            if (ch == QLatin1Char('[')) a_escape = Csi;
            else if (ch == QLatin1Char(']')) a_escape = Osc;
            else a_escape = Plain;   // a two-character sequence, done
            continue;
        case Csi:
            if (ch.unicode() >= 0x40 && ch.unicode() <= 0x7e) a_escape = Plain;   // the final byte
            continue;
        case Osc:
            if (ch == QChar(0x07)) a_escape = Plain;
            else if (ch == QChar(0x1b)) a_escape = OscEscape;
            continue;
        case OscEscape:
            a_escape = ch == QLatin1Char('\\') ? Plain : Osc;
            continue;
        case Plain:
            break;
        }
        if (ch == QChar(0x1b)) { a_escape = Escape; continue; }
        if (ch == QLatin1Char('\n')) {
            flush();
            a_overwriteLine = false;
            cursor.insertText(QStringLiteral("\n"));
        } else if (ch == QLatin1Char('\r')) {
            flush();
            a_overwriteLine = true;
        } else if (ch == QLatin1Char('\b')) {
            flush();
            if (!cursor.atBlockStart()) cursor.deletePreviousChar();
        } else if (ch == QChar(0x07) || (ch.unicode() < 0x20 && ch != QLatin1Char('\t'))) {
            // a bell or another control character: nothing to show
        } else {
            run += ch;
        }
    }
    flush();
    cursor.endEditBlock();
    a_output->setTextCursor(cursor);
    a_output->ensureCursorVisible();
}

void ProcessConsole::appendNote(const QString& text)
{
    QTextCursor cursor(a_output->document());
    cursor.movePosition(QTextCursor::End);
    if (!cursor.atBlockStart()) cursor.insertText(QStringLiteral("\n"));
    cursor.insertText(text + QLatin1Char('\n'));
    a_overwriteLine = false;
    a_output->setTextCursor(cursor);
    a_output->ensureCursorVisible();
}

void ProcessConsole::setStatus(const QString& text)
{
    a_status->setText(text);
}

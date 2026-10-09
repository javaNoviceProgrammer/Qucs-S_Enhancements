/*
 * pythondoc.cpp - a Python script in the text editor: checked as it is
 *                 typed, indented as Python is, run from the Python toolbar
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "pythondoc.h"

#include "main.h"
#include "qucs.h"
#include "settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTimer>

#include <algorithm>
#include <memory>

namespace qucs_s::python {

namespace {

QString tr(const char* text) { return QCoreApplication::translate("PythonDoc", text); }

// What a check is run in: a folder of its own, empty - python -c puts the
// folder it runs in first on sys.path, and a json.py or ast.py of the
// script's folder would stand in for the modules the checker imports.
QString neutralFolder()
{
    static std::unique_ptr<QTemporaryDir> folder;
    if (!folder || !folder->isValid()) folder = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/qucs-python-check-XXXXXX"));
    return folder->isValid() ? folder->path() : QDir::tempPath();
}

} // namespace

const QString& checkerProgram()
{
    static const QString program = QStringLiteral(R"PY(
import sys, json, warnings
name = sys.argv[1] if len(sys.argv) > 1 else '<script>'
data = sys.stdin.buffer.read()
try:
    src = data.decode('utf-8')
except UnicodeDecodeError:
    src = data.decode('latin-1')
out = {'python': '%d.%d.%d' % tuple(sys.version_info[:3]), 'checker': '', 'problems': []}
def add(line, col, end_line, end_col, error, message, code=''):
    out['problems'].append({'line': int(line or 1), 'column': int(col or 0), 'endLine': int(end_line or 0),
                            'endColumn': int(end_col or 0), 'error': bool(error), 'message': str(message),
                            'code': str(code or '')})
ok = True
with warnings.catch_warnings(record=True) as caught:
    warnings.simplefilter('always')
    try:
        compile(src, name, 'exec', dont_inherit=True)
    except SyntaxError as e:
        ok = False
        line, col = e.lineno or 1, e.offset or 0
        end_line, end_col = getattr(e, 'end_lineno', None) or 0, getattr(e, 'end_offset', None) or 0
        if (end_line, end_col) <= (line, col):
            end_line = end_col = 0
        add(line, col, end_line, end_col, True, e.msg)
    except (ValueError, TypeError) as e:
        ok = False
        add(1, 0, 0, 0, True, e)
seen = set()
for w in caught:
    if not issubclass(w.category, (SyntaxWarning, DeprecationWarning)):
        continue
    if (w.lineno, str(w.message)) in seen:
        continue
    seen.add((w.lineno, str(w.message)))
    add(w.lineno, 0, 0, 0, False, w.message)
if ok:
    import importlib.util, shutil, subprocess
    ruff = None
    try:
        if importlib.util.find_spec('ruff') is not None:
            ruff = [sys.executable, '-m', 'ruff']
    except Exception:
        pass
    if ruff is None and shutil.which('ruff'):
        ruff = [shutil.which('ruff')]
    done = False
    if ruff is not None:
        try:
            run = subprocess.run(ruff + ['check', '--output-format=json', '--no-cache', '--stdin-filename', name, '-'],
                                 input=src.encode('utf-8'), stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
            if run.returncode not in (0, 1):
                raise RuntimeError(run.stderr)
            found = json.loads(run.stdout.decode('utf-8') or '[]')
            version = subprocess.run(ruff + ['--version'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
            out['checker'] = version.stdout.decode('utf-8').strip() or 'ruff'
            for f in found:
                at, end = f.get('location') or {}, f.get('end_location') or {}
                add(at.get('row'), at.get('column'), end.get('row'), end.get('column'), False, f.get('message', ''), f.get('code'))
            done = True
        except Exception:
            pass
    if not done:
        try:
            import pyflakes
            from pyflakes import api
            class Reporter:
                def unexpectedError(self, filename, message):
                    pass
                def syntaxError(self, filename, message, lineno, offset, text):
                    pass
                def flake(self, m):
                    add(m.lineno, (getattr(m, 'col', 0) or 0) + 1, 0, 0, False, m.message % m.message_args)
            api.check(src, name, Reporter())
            out['checker'] = ('pyflakes ' + getattr(pyflakes, '__version__', '')).strip()
        except Exception:
            pass
sys.stdout.write(json.dumps(out))
)PY");
    return program;
}

Check readCheck(const QByteArray& output)
{
    Check check;
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(output.trimmed(), &error);
    if (!doc.isObject()) {
        check.failure = tr("The check gave no answer it should (%1).").arg(error.errorString());
        return check;
    }
    const QJsonObject o = doc.object();
    check.python = o.value(QStringLiteral("python")).toString();
    check.checker = o.value(QStringLiteral("checker")).toString();
    for (const QJsonValue& v : o.value(QStringLiteral("problems")).toArray()) {
        const QJsonObject p = v.toObject();
        Problem problem;
        problem.line = std::max(1, p.value(QStringLiteral("line")).toInt(1));
        problem.column = std::max(0, p.value(QStringLiteral("column")).toInt());
        problem.endLine = std::max(0, p.value(QStringLiteral("endLine")).toInt());
        problem.endColumn = std::max(0, p.value(QStringLiteral("endColumn")).toInt());
        problem.error = p.value(QStringLiteral("error")).toBool();
        problem.message = p.value(QStringLiteral("message")).toString();
        problem.code = p.value(QStringLiteral("code")).toString();
        check.problems.append(problem);
    }
    // In the text's order (the compiler says what its tokenizer found
    // first, then the rest).
    std::stable_sort(check.problems.begin(), check.problems.end(), [](const Problem& a, const Problem& b) {
        return a.line != b.line ? a.line < b.line : a.column < b.column;
    });
    return check;
}

QList<Interpreter> interpretersFor(const QString& script, const QString& project, const QStringList& chosen)
{
    QList<Interpreter> list;
    QSet<QString> seen;
    const auto add = [&](const QString& path, const QString& label) {
        if (path.isEmpty()) return;
        // (By its path, not where it leads: a virtual environment's python
        // is a link to the one it was made with, and runs another way.)
        const QString absolute = QFileInfo(path).absoluteFilePath();
        if (seen.contains(absolute)) return;
        seen.insert(absolute);
        list.append({absolute, label});
    };
    const auto environmentsIn = [&](const QString& folder, const QString& where) {
        for (const QString& name : {QStringLiteral(".venv"), QStringLiteral("venv")}) {
#ifdef Q_OS_WIN
            const QString python = QDir(folder).filePath(name + QStringLiteral("/Scripts/python.exe"));
#else
            const QString python = QDir(folder).filePath(name + QStringLiteral("/bin/python"));
#endif
            if (QFileInfo(python).isExecutable()) add(python, where.arg(name));
        }
    };
    if (!script.isEmpty()) environmentsIn(QFileInfo(script).absolutePath(), tr("python (%1 beside the script)"));
    if (!project.isEmpty()) environmentsIn(project, tr("python (%1 of the project)"));
    const QString set = QucsSettings.PythonExecutable.trimmed();
    if (!set.isEmpty()) {
        const QString found = QFileInfo(set).isAbsolute() ? set : QStandardPaths::findExecutable(set);
        add(found.isEmpty() ? set : found, tr("%1 (Application Settings)").arg(QFileInfo(set).fileName()));
    }
    for (const QString& name : {QStringLiteral("python3"), QStringLiteral("python")})
        add(QStandardPaths::findExecutable(name), tr("%1 (PATH)").arg(name));
    for (const QString& path : chosen)
        if (QFileInfo(path).isExecutable()) add(path, tr("%1 (chosen)").arg(QFileInfo(path).fileName()));
    return list;
}

QString indentStep(const QString& text)
{
    for (const QStringView line : QStringView(text).split(QLatin1Char('\n'))) {
        if (line.isEmpty() || (line.front() != QLatin1Char(' ') && line.front() != QLatin1Char('\t'))) continue;
        if (line.trimmed().isEmpty()) continue;
        return line.front() == QLatin1Char('\t') ? QStringLiteral("\t") : QStringLiteral("    ");
    }
    return QStringLiteral("    ");
}

namespace {

// \a line without its comment (a # outside a string).
QString withoutComment(const QString& line)
{
    QChar quote;
    for (qsizetype k = 0; k < line.size(); ++k) {
        const QChar c = line.at(k);
        if (!quote.isNull()) {
            if (c == QLatin1Char('\\')) ++k;
            else if (c == quote) quote = QChar();
        } else if (c == QLatin1Char('\'') || c == QLatin1Char('"')) {
            quote = c;
        } else if (c == QLatin1Char('#')) {
            return line.left(k);
        }
    }
    return line;
}

} // namespace

QString nextIndent(const QString& line, const QString& step)
{
    qsizetype n = 0;
    while (n < line.size() && (line.at(n) == QLatin1Char(' ') || line.at(n) == QLatin1Char('\t'))) ++n;
    const QString indent = line.left(n);
    const QString code = withoutComment(line).trimmed();
    if (code.endsWith(QLatin1Char(':'))) return indent + step;
    static const QRegularExpression ends(QStringLiteral("^(return|pass|break|continue|raise)\\b"));
    if (!ends.match(code).hasMatch()) return indent;
    if (indent.endsWith(step)) return indent.chopped(step.size());
    if (indent.endsWith(QLatin1Char('\t'))) return indent.chopped(1);
    qsizetype spaces = 0;
    while (spaces < 4 && spaces < indent.size() && indent.at(indent.size() - 1 - spaces) == QLatin1Char(' ')) ++spaces;
    return indent.chopped(spaces);
}

} // namespace qucs_s::python

namespace {

// The Python scripts open: a setting reaches each.
QSet<PythonDoc*>& openScripts()
{
    static QSet<PythonDoc*> scripts;
    return scripts;
}

constexpr int kCheckLimit = 15000;   // ms

} // namespace

PythonDoc::PythonDoc(QucsApp* app, const QString& name) : TextDoc(app, name)
{
    openScripts().insert(this);
    setDiagnosticsAtLineEnds(messagesAtLineEnds());

    a_delay = new QTimer(this);
    a_delay->setSingleShot(true);
    a_delay->setInterval(kCheckDelay);
    connect(a_delay, &QTimer::timeout, this, &PythonDoc::checkNow);

    a_limit = new QTimer(this);
    a_limit->setSingleShot(true);
    a_limit->setInterval(kCheckLimit);
    connect(a_limit, &QTimer::timeout, this, [this] {
        stopCheck();
        a_check = {};
        a_check.failure = tr("The check took longer than %1 s and was stopped.").arg(kCheckLimit / 1000);
        a_hasCheck = true;
        setDiagnostics({});
        emit checkFinished();
    });

    // Each edit (not a reformatting by the highlighter: it leaves the
    // revision as it was).
    connect(document(), &QTextDocument::contentsChanged, this, &PythonDoc::scheduleCheck);
}

PythonDoc::~PythonDoc()
{
    openScripts().remove(this);
    stopCheck();
    // TextDoc's destructor edits the text as it lets the highlighter go:
    // no check is scheduled by a PythonDoc that is no more.
    disconnect(document(), nullptr, this, nullptr);
}

bool PythonDoc::load()
{
    const bool loaded = TextDoc::load();
    if (loaded) checkNow();
    return loaded;
}

QString PythonDoc::interpreter() const
{
    if (!a_interpreter.isEmpty()) return a_interpreter;
    const QString project = a_App != nullptr && !a_App->ProjName.isEmpty() ? QucsSettings.QucsWorkDir.absolutePath() : QString();
    const QList<qucs_s::python::Interpreter> found =
        qucs_s::python::interpretersFor(getDocName(), project, _settings::Get().item<QStringList>("PythonInterpreters"));
    return found.isEmpty() ? QStringLiteral("python3") : found.first().path;
}

void PythonDoc::setInterpreter(const QString& path)
{
    if (a_interpreter == path) return;
    a_interpreter = path;
    checkNow();
}

void PythonDoc::scheduleCheck()
{
    const int revision = document()->revision();
    if (revision == a_scheduledRevision) return;
    a_scheduledRevision = revision;
    stopCheck();   // (its answer would be of a text no longer there)
    a_delay->start();
}

void PythonDoc::stopCheck()
{
    a_limit->stop();
    if (a_process == nullptr) return;
    QProcess* process = a_process;
    a_process = nullptr;
    disconnect(process, nullptr, this, nullptr);
    process->kill();
    process->waitForFinished(1000);
    process->deleteLater();
}

void PythonDoc::checkNow()
{
    a_delay->stop();
    stopCheck();
    a_scheduledRevision = a_checkedRevision = document()->revision();

    a_process = new QProcess(this);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    environment.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
    a_process->setProcessEnvironment(environment);
    a_process->setWorkingDirectory(qucs_s::python::neutralFolder());
    connect(a_process, &QProcess::finished, this, &PythonDoc::finishCheck);
    connect(a_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) finishCheck();
    });
    const QString name = getDocName().isEmpty() ? QStringLiteral("<script>") : QFileInfo(getDocName()).absoluteFilePath();
    a_process->start(interpreter(), {QStringLiteral("-c"), qucs_s::python::checkerProgram(), name});
    a_process->write(toPlainText().toUtf8());
    a_process->closeWriteChannel();
    a_limit->start();
}

void PythonDoc::finishCheck()
{
    QProcess* process = a_process;
    if (process == nullptr) return;
    a_process = nullptr;
    a_limit->stop();
    disconnect(process, nullptr, this, nullptr);
    process->deleteLater();
    if (a_checkedRevision != document()->revision()) return;   // (another is on its way)

    qucs_s::python::Check check;
    if (process->error() == QProcess::FailedToStart) {
        check.failure = tr("No Python at %1 to check it with: choose one on the Python toolbar, or set it under "
                           "Application Settings > Locations.").arg(interpreter());
    } else if (process->exitStatus() != QProcess::NormalExit) {
        check.failure = tr("The check ended before it answered (%1).").arg(interpreter());
    } else {
        check = qucs_s::python::readCheck(process->readAllStandardOutput());
        if (!check.failure.isEmpty()) {
            const QString said = QString::fromUtf8(process->readAllStandardError()).trimmed().section(QLatin1Char('\n'), -1);
            if (!said.isEmpty()) check.failure += QLatin1Char(' ') + said;
        }
    }
    a_check = check;
    a_hasCheck = true;

    QList<Diagnostic> marks;
    for (const qucs_s::python::Problem& p : std::as_const(check.problems)) {
        Diagnostic d;
        d.line = p.line;
        d.column = p.column;
        d.endLine = p.endLine;
        d.endColumn = p.endColumn;
        d.error = p.error;
        d.message = p.code.isEmpty() ? p.message : QStringLiteral("%1 (%2)").arg(p.message, p.code);
        marks.append(d);
    }
    setDiagnostics(marks);
    emit checkFinished();
}

QString PythonDoc::checkedBy() const
{
    if (checking() && !a_hasCheck) return tr("Checking with %1...").arg(interpreter());
    if (!a_hasCheck) return tr("Not checked yet.");
    if (!a_check.failure.isEmpty()) return a_check.failure;
    const QString python = tr("Python %1").arg(a_check.python);
    if (a_check.checker.isEmpty())
        return tr("%1's compiler alone: syntax errors and its warnings. With ruff or pyflakes installed for it, "
                  "names not defined, imports not used and more.").arg(python);
    return tr("%1 and %2").arg(python, a_check.checker);
}

bool PythonDoc::messagesAtLineEnds()
{
    return _settings::Get().item<bool>("PythonMessagesAtLineEnds");
}

void PythonDoc::setMessagesAtLineEnds(bool on)
{
    _settings::Get().setItem<bool>("PythonMessagesAtLineEnds", on);
    for (PythonDoc* script : std::as_const(openScripts())) script->setDiagnosticsAtLineEnds(on);
}

void PythonDoc::keyPressEvent(QKeyEvent* event)
{
    if (isReadOnly()) {
        TextDoc::keyPressEvent(event);
        return;
    }
    const Qt::KeyboardModifiers modifiers = event->modifiers() & ~Qt::KeypadModifier;
    const int key = event->key();
    if ((key == Qt::Key_Return || key == Qt::Key_Enter) && modifiers == Qt::NoModifier) {
        QTextCursor cursor = textCursor();
        QTextCursor start = cursor;   // (a selection's start: the selection goes)
        start.setPosition(cursor.selectionStart());
        const QString before = start.block().text().left(start.positionInBlock());
        const QString indent = qucs_s::python::nextIndent(before, qucs_s::python::indentStep(toPlainText()));
        cursor.beginEditBlock();
        cursor.insertText(QStringLiteral("\n") + indent);
        cursor.endEditBlock();
        setTextCursor(cursor);
        ensureCursorVisible();
        return;
    }
    if (key == Qt::Key_Tab && modifiers == Qt::NoModifier) {
        QTextCursor cursor = textCursor();
        if (cursor.hasSelection() && document()->findBlock(cursor.selectionStart()) != document()->findBlock(cursor.selectionEnd())) {
            shiftLines(1);
            return;
        }
        const QString step = qucs_s::python::indentStep(toPlainText());
        cursor.insertText(step == QLatin1String("\t") ? step : QString(4 - cursor.positionInBlock() % 4, QLatin1Char(' ')));
        setTextCursor(cursor);
        return;
    }
    if (key == Qt::Key_Backtab || (key == Qt::Key_Tab && modifiers == Qt::ShiftModifier)) {
        shiftLines(-1);
        return;
    }
    TextDoc::keyPressEvent(event);
}

void PythonDoc::shiftLines(int by)
{
    QTextCursor cursor = textCursor();
    const bool selected = cursor.hasSelection();
    QTextBlock first = document()->findBlock(cursor.selectionStart());
    QTextBlock last = document()->findBlock(cursor.selectionEnd());
    // A selection ending at the start of a line leaves that line.
    if (selected && last != first && cursor.selectionEnd() == last.position()) last = last.previous();
    const QString step = qucs_s::python::indentStep(toPlainText());
    cursor.beginEditBlock();
    for (QTextBlock block = first; block.isValid(); block = block.next()) {
        QTextCursor at(block);
        const QString text = block.text();
        if (by > 0) {
            if (!text.trimmed().isEmpty() || first == last) at.insertText(step);   // (empty lines left empty)
        } else {
            int n = 0;
            if (text.startsWith(QLatin1Char('\t'))) n = 1;
            else
                while (n < 4 && n < text.size() && text.at(n) == QLatin1Char(' ')) ++n;
            if (n > 0) {
                at.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, n);
                at.removeSelectedText();
            }
        }
        if (block == last) break;
    }
    cursor.endEditBlock();
    if (selected) {   // the lines shifted, whole, selected
        cursor.setPosition(first.position());
        cursor.setPosition(last.position() + last.length() - 1, QTextCursor::KeepAnchor);
        setTextCursor(cursor);
    }
}

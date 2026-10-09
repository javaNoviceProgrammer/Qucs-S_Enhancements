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

#include "config.h"
#include "main.h"
#include "qucs.h"
#include "settings.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QCompleter>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QProcess>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTimer>

#include <algorithm>
#include <memory>

namespace qucs_s::python {

namespace {

QString tr(const char* text) { return QCoreApplication::translate("PythonDoc", text); }

} // namespace

QString neutralFolder()
{
    static std::unique_ptr<QTemporaryDir> folder;
    if (!folder || !folder->isValid()) folder = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/qucs-python-check-XXXXXX"));
    return folder->isValid() ? folder->path() : QDir::tempPath();
}

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
def add(line, col, end_line, end_col, error, message, code='', fix=None):
    out['problems'].append({'line': int(line or 1), 'column': int(col or 0), 'endLine': int(end_line or 0),
                            'endColumn': int(end_col or 0), 'error': bool(error), 'message': str(message),
                            'code': str(code or ''), 'fix': fix})
def fix_of(f):
    fix = f.get('fix') or None
    if not fix or not fix.get('edits'):
        return None
    edits = []
    for e in fix['edits']:
        at, end = e.get('location') or {}, e.get('end_location') or {}
        edits.append({'line': at.get('row', 1), 'column': at.get('column', 1), 'endLine': end.get('row', 1),
                      'endColumn': end.get('column', 1), 'text': e.get('content') or ''})
    return {'message': fix.get('message') or '', 'applicability': fix.get('applicability') or 'safe', 'edits': edits}
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
                add(at.get('row'), at.get('column'), end.get('row'), end.get('column'), False, f.get('message', ''), f.get('code'),
                    fix_of(f))
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

const QString& completerProgram()
{
    static const QString program = programText(QStringLiteral("completer.py"));
    return program;
}

QString programText(const QString& name)
{
    QFile file(QStringLiteral(":/python/") + name);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

const QString& debuggerProgram()
{
    static const QString program = programText(QStringLiteral("debugger.py"));
    return program;
}

const QString& formatterProgram()
{
    static const QString program = programText(QStringLiteral("formatter.py"));
    return program;
}

const QString& typeCheckProgram()
{
    static const QString program = programText(QStringLiteral("typecheck.py"));
    return program;
}

QString moduleFolder()
{
    // Written once, for as long as the program runs: the module, the
    // Python Shell's runner and start-up file, the matplotlib backend and
    // the tables (each read from the resources) - and the folders scripts
    // reach Qucs-S through.
    static std::unique_ptr<QTemporaryDir> folder;
    if (folder && folder->isValid()) return folder->path();
    folder = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/qucs-python-XXXXXX"));
    if (!folder->isValid()) return {};
    for (const QString& name : {QStringLiteral("qucs.py"), QStringLiteral("_qucs_shell.py"), QStringLiteral("_qucs_startup.py"),
                                QStringLiteral("_qucs_plots.py"), QStringLiteral("_qucs_data.py")}) {
        QFile out(folder->filePath(name));
        if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) out.write(programText(name).toUtf8());
    }
    for (const QString& sub : {QStringLiteral("jobs"), QStringLiteral("plots"), QStringLiteral("shell/requests"),
                               QStringLiteral("shell/answers"), QStringLiteral("requests")})
        QDir(folder->path()).mkpath(sub);
    return folder->path();
}

namespace {
QString subfolder(const QString& name)
{
    const QString folder = moduleFolder();
    return folder.isEmpty() ? QString() : QDir(folder).filePath(name);
}
} // namespace

QString plotsFolder() { return subfolder(QStringLiteral("plots")); }

QString shellFolder() { return subfolder(QStringLiteral("shell")); }

QString requestsFolder() { return subfolder(QStringLiteral("requests")); }

bool inlinePlots() { return _settings::Get().item<bool>("PythonInlinePlots"); }

void setInlinePlots(bool on) { _settings::Get().setItem<bool>("PythonInlinePlots", on); }

namespace {
// The program qucs.simulate() runs: Qucs-S's own, when this is it (not a
// test's program), unless one is set.
QString simulatingProgram()
{
    if (qEnvironmentVariableIsSet("QUCS_S_EXECUTABLE")) return {};
    const QString self = QCoreApplication::applicationFilePath();
    return QFileInfo(self).baseName() == QLatin1String(QUCS_NAME) ? self : QString();
}

QString pythonPathWith(const QString& before)
{
    const QString folder = moduleFolder();
    if (folder.isEmpty()) return before;
    return before.isEmpty() ? folder : folder + QDir::listSeparator() + before;
}
} // namespace

namespace {
// What a script is given to reach Qucs-S: the folders of its figures and its
// requests - and matplotlib's backend, the pane's, while it shows them.
QList<std::pair<QString, QString>> exchangeVariables()
{
    QList<std::pair<QString, QString>> list;
    if (const QString plots = plotsFolder(); !plots.isEmpty()) {
        list.append({QStringLiteral("QUCS_S_PLOTS"), plots});
        if (inlinePlots()) list.append({QStringLiteral("MPLBACKEND"), QStringLiteral("module://_qucs_plots")});
    }
    if (const QString requests = requestsFolder(); !requests.isEmpty()) list.append({QStringLiteral("QUCS_S_REQUESTS"), requests});
    return list;
}
} // namespace

QProcessEnvironment scriptEnvironment()
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    environment.insert(QStringLiteral("PYTHONPATH"), pythonPathWith(environment.value(QStringLiteral("PYTHONPATH"))));
    if (const QString program = simulatingProgram(); !program.isEmpty())
        environment.insert(QStringLiteral("QUCS_S_EXECUTABLE"), program);
    for (const auto& [name, value] : exchangeVariables()) environment.insert(name, value);
    return environment;
}

QStringList shellEnvironment()
{
    QStringList entries{QStringLiteral("PYTHONPATH=") + pythonPathWith(qEnvironmentVariable("PYTHONPATH"))};
    if (const QString program = simulatingProgram(); !program.isEmpty())
        entries << QStringLiteral("QUCS_S_EXECUTABLE=") + program;
    for (const auto& [name, value] : exchangeVariables()) entries << name + QLatin1Char('=') + value;
    // Its start-up file: its variables shown (the user's own after it).
    if (const QString folder = moduleFolder(); !folder.isEmpty()) {
        entries << QStringLiteral("PYTHONSTARTUP=") + QDir(folder).filePath(QStringLiteral("_qucs_startup.py"))
                << QStringLiteral("QUCS_S_SHELL=") + shellFolder()
                << QStringLiteral("QUCS_S_PYTHONSTARTUP=") + qEnvironmentVariable("PYTHONSTARTUP");
    }
    return entries;
}

Completions readCompletions(const QByteArray& line)
{
    const Answer read = readAnswer(line);
    Completions answer;
    if (read.kind != QLatin1String("complete")) return answer;
    answer.id = read.id;
    answer.engine = read.engine;
    answer.items = read.items;
    return answer;
}

Answer readAnswer(const QByteArray& line)
{
    Answer answer;
    const QJsonDocument doc = QJsonDocument::fromJson(line.trimmed());
    if (!doc.isObject()) return answer;
    const QJsonObject o = doc.object();
    answer.id = o.value(QStringLiteral("id")).toInt(-1);
    answer.kind = o.value(QStringLiteral("kind")).toString(QStringLiteral("complete"));
    answer.engine = o.value(QStringLiteral("engine")).toString();
    for (const QJsonValue& v : o.value(QStringLiteral("items")).toArray()) {
        const QJsonObject item = v.toObject();
        const QString name = item.value(QStringLiteral("name")).toString();
        if (!name.isEmpty())
            answer.items.append({name, item.value(QStringLiteral("type")).toString(), item.value(QStringLiteral("description")).toString()});
    }
    if (const QJsonObject sig = o.value(QStringLiteral("signature")).toObject(); !sig.isEmpty()) {
        Signature& s = answer.signature;
        s.valid = true;
        s.name = sig.value(QStringLiteral("name")).toString();
        for (const QJsonValue& v : sig.value(QStringLiteral("params")).toArray()) s.params << v.toString();
        s.index = sig.value(QStringLiteral("index")).toInt(-1);
        s.doc = sig.value(QStringLiteral("doc")).toString();
        const QJsonArray open = sig.value(QStringLiteral("open")).toArray();
        s.openLine = open.size() == 2 ? open.at(0).toInt() : 0;
        s.openColumn = open.size() == 2 ? open.at(1).toInt() : 0;
    }
    if (const QJsonObject help = o.value(QStringLiteral("help")).toObject(); !help.isEmpty()) {
        answer.help.valid = true;
        answer.help.title = help.value(QStringLiteral("title")).toString();
        answer.help.type = help.value(QStringLiteral("type")).toString();
        answer.help.text = help.value(QStringLiteral("text")).toString();
    }
    for (const QJsonValue& v : o.value(QStringLiteral("imports")).toArray()) {
        const QJsonObject i = v.toObject();
        answer.imports.append({i.value(QStringLiteral("title")).toString(), i.value(QStringLiteral("line")).toInt(1),
                               i.value(QStringLiteral("text")).toString()});
    }
    for (const QString& kind : {QStringLiteral("references"), QStringLiteral("rename")}) {
        const QJsonObject found = o.value(kind).toObject();
        if (found.isEmpty()) continue;
        answer.valid = true;
        answer.name = found.value(QStringLiteral("name")).toString();
        answer.scope = found.value(QStringLiteral("scope")).toString();
        answer.refusal = found.value(QStringLiteral("refusal")).toString();
        answer.count = found.value(QStringLiteral("count")).toInt();
        for (const QJsonValue& v : found.value(QStringLiteral("references")).toArray()) {
            const QJsonObject r = v.toObject();
            answer.references.append({r.value(QStringLiteral("file")).toString(), r.value(QStringLiteral("line")).toInt(),
                                      r.value(QStringLiteral("column")).toInt(), r.value(QStringLiteral("end")).toInt(),
                                      r.value(QStringLiteral("text")).toString(), r.value(QStringLiteral("definition")).toBool()});
        }
        for (const QJsonValue& v : found.value(QStringLiteral("changes")).toArray()) {
            const QJsonObject c = v.toObject();
            answer.changes.append({c.value(QStringLiteral("file")).toString(), c.value(QStringLiteral("text")).toString()});
        }
    }
    if (const QJsonObject place = o.value(QStringLiteral("definition")).toObject(); !place.isEmpty()) {
        Place& p = answer.place;
        p.valid = true;
        p.file = place.value(QStringLiteral("file")).toString();
        p.line = place.value(QStringLiteral("line")).toInt();
        p.column = std::max(0, place.value(QStringLiteral("column")).toInt());
        p.name = place.value(QStringLiteral("name")).toString();
        p.builtin = place.value(QStringLiteral("builtin")).toBool();
        p.library = place.value(QStringLiteral("library")).toBool();
    }
    return answer;
}

bool isCellMarker(const QString& line)
{
    static const QRegularExpression marker(QStringLiteral("^\\s*#\\s*(%%|In\\s*\\[[^\\]]*\\]\\s*:|<codecell>)"));
    return marker.match(line).hasMatch();
}

std::pair<int, int> cellAround(const QString& text, int line)
{
    const QList<QStringView> lines = QStringView(text).split(QLatin1Char('\n'));
    const int count = int(lines.size());
    line = std::clamp(line, 1, std::max(1, count));
    int first = 1;
    for (int k = line; k >= 1; --k)
        if (isCellMarker(lines.at(k - 1).toString())) {
            first = k;
            break;
        }
    int last = count;
    for (int k = line + 1; k <= count; ++k)
        if (isCellMarker(lines.at(k - 1).toString())) {
            last = k - 1;
            break;
        }
    // (A file's last line break is no line of its.)
    while (last > first && lines.at(last - 1).trimmed().isEmpty() && last == count) --last;
    return {first, last};
}

QList<OutlineEntry> outlineOf(const QString& text)
{
    static const QRegularExpression head(QStringLiteral("^([ \\t]*)(async[ \\t]+def|def|class)[ \\t]+([A-Za-z_]\\w*)"));
    const QList<QStringView> lines = QStringView(text).split(QLatin1Char('\n'));
    const auto indentOf = [](QStringView line) {
        int n = 0;
        for (const QChar c : line) {
            if (c == QLatin1Char(' ')) ++n;
            else if (c == QLatin1Char('\t')) n += 8 - n % 8;
            else break;
        }
        return n;
    };
    QList<OutlineEntry> entries;
    QList<std::pair<int, int>> open;   // the entries a line may be in: their index and indentation
    int lastCode = 0;                  // the last line of code so far (from 1)
    for (int k = 0; k < lines.size(); ++k) {
        const QStringView line = lines.at(k);
        const QStringView code = line.trimmed();
        if (code.isEmpty() || code.startsWith(QLatin1Char('#'))) continue;
        const int indent = indentOf(line);
        // A line as far left as an entry's head, or further, ends its body:
        // its last line is the last of code before.
        while (!open.isEmpty() && indent <= open.last().second) {
            entries[open.last().first].lastLine = std::max(entries[open.last().first].line, lastCode);
            open.removeLast();
        }
        lastCode = k + 1;
        const QRegularExpressionMatch m = head.matchView(line);
        if (!m.hasMatch()) continue;
        OutlineEntry e;
        e.line = k + 1;
        e.lastLine = k + 1;
        e.depth = int(open.size());
        e.kind = m.captured(2).endsWith(QLatin1String("def")) ? QStringLiteral("def") : QStringLiteral("class");
        e.name = m.captured(3);
        open.append({int(entries.size()), indent});
        entries.append(e);
    }
    for (const auto& o : std::as_const(open)) entries[o.first].lastLine = std::max(entries[o.first].line, lastCode);
    return entries;
}

namespace {
bool isNameCharacter(QChar c) { return c.isLetterOrNumber() || c == QLatin1Char('_'); }
} // namespace

int wordStart(const QString& text)
{
    qsizetype start = text.size();
    while (start > 0 && isNameCharacter(text.at(start - 1))) --start;
    return int(start);
}

bool inStringOrComment(const QString& text)
{
    QChar quote;
    for (qsizetype k = 0; k < text.size(); ++k) {
        const QChar c = text.at(k);
        if (!quote.isNull()) {
            if (c == QLatin1Char('\\')) ++k;
            else if (c == quote) quote = QChar();
        } else if (c == QLatin1Char('\'') || c == QLatin1Char('"')) {
            quote = c;
        } else if (c == QLatin1Char('#')) {
            return true;
        }
    }
    return !quote.isNull();
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
        if (const QJsonObject fix = p.value(QStringLiteral("fix")).toObject(); !fix.isEmpty()) {
            problem.fixMessage = fix.value(QStringLiteral("message")).toString();
            problem.fixApplicability = fix.value(QStringLiteral("applicability")).toString();
            for (const QJsonValue& e : fix.value(QStringLiteral("edits")).toArray()) {
                const QJsonObject o = e.toObject();
                problem.fixEdits.append({o.value(QStringLiteral("line")).toInt(1), o.value(QStringLiteral("column")).toInt(1),
                                         o.value(QStringLiteral("endLine")).toInt(1), o.value(QStringLiteral("endColumn")).toInt(1),
                                         o.value(QStringLiteral("text")).toString()});
            }
        }
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

QString applyEdits(const QString& text, QList<Problem::Edit> edits)
{
    // Each at its offset in the text as it is: the last first.
    QList<qsizetype> starts{0};
    for (qsizetype k = 0; k < text.size(); ++k)
        if (text.at(k) == QLatin1Char('\n')) starts.append(k + 1);
    const auto offset = [&](int line, int column) {
        if (line < 1) return qsizetype(0);
        if (line > starts.size()) return text.size();
        const qsizetype begin = starts.at(line - 1);
        const qsizetype end = line < starts.size() ? starts.at(line) - 1 : text.size();
        return std::min(begin + std::max(0, column - 1), end);
    };
    std::sort(edits.begin(), edits.end(), [](const Problem::Edit& a, const Problem::Edit& b) {
        return a.line != b.line ? a.line > b.line : a.column > b.column;
    });
    QString out = text;
    for (const Problem::Edit& e : std::as_const(edits)) {
        const qsizetype from = offset(e.line, e.column), to = std::max(from, offset(e.endLine, e.endColumn));
        out.replace(from, to - from, e.text);
    }
    return out;
}

QString undefinedName(const QString& message)
{
    // ruff: Undefined name `np`; pyflakes: undefined name 'np'; mypy: Name
    // "np" is not defined; pyright: "np" is not defined.
    static const QRegularExpression said(QStringLiteral("(?:[Uu]ndefined name\\s+[`'\"]([A-Za-z_]\\w*)[`'\"]|"
                                                        "^(?:Name\\s+)?\"([A-Za-z_]\\w*)\" is not defined)"));
    const QRegularExpressionMatch m = said.match(message);
    if (!m.hasMatch()) return {};
    return m.captured(1).isEmpty() ? m.captured(2) : m.captured(1);
}

QString ignoredOnLine(const QString& line, const QString& code, const QString& source)
{
    // A comment of its kind there already: the code added to it.
    if (source == QLatin1String("mypy") || source == QLatin1String("pyright")) {
        const QString tag = source == QLatin1String("mypy") ? QStringLiteral("type: ignore") : QStringLiteral("pyright: ignore");
        const QRegularExpression had(QStringLiteral("#\\s*%1(\\[([^\\]]*)\\])?").arg(QRegularExpression::escape(tag)));
        const QRegularExpressionMatch m = had.match(line);
        if (m.hasMatch()) {
            if (m.captured(1).isEmpty() || code.isEmpty()) return line;   // (all ignored there)
            QStringList codes = m.captured(2).split(QLatin1Char(','), Qt::SkipEmptyParts);
            for (QString& c : codes) c = c.trimmed();
            if (codes.contains(code)) return line;
            codes << code;
            return line.left(m.capturedStart(0)) + QStringLiteral("# %1[%2]").arg(tag, codes.join(QStringLiteral(", "))) + line.mid(m.capturedEnd(0));
        }
        return line + (code.isEmpty() ? QStringLiteral("  # %1").arg(tag) : QStringLiteral("  # %1[%2]").arg(tag, code));
    }
    static const QRegularExpression noqa(QStringLiteral("#\\s*noqa(:\\s*([A-Z]+[0-9]+(?:\\s*,\\s*[A-Z]+[0-9]+)*))?"), QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = noqa.match(line);
    if (m.hasMatch()) {
        if (m.captured(1).isEmpty() || code.isEmpty()) return line;   // (a bare noqa: all of them)
        QStringList codes = m.captured(2).split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (QString& c : codes) c = c.trimmed();
        if (codes.contains(code)) return line;
        codes << code;
        return line.left(m.capturedStart(0)) + QStringLiteral("# noqa: %1").arg(codes.join(QStringLiteral(", "))) + line.mid(m.capturedEnd(0));
    }
    return line + (code.isEmpty() ? QStringLiteral("  # noqa") : QStringLiteral("  # noqa: %1").arg(code));
}

RunSettings runSettingsFor(const QString& script)
{
    const QJsonObject all = QJsonDocument::fromJson(_settings::Get().item<QString>("PythonRunSettings").toUtf8()).object();
    const QJsonObject o = all.value(QFileInfo(script).absoluteFilePath()).toObject();
    return {o.value(QStringLiteral("arguments")).toString(), o.value(QStringLiteral("folder")).toString(),
            o.value(QStringLiteral("environment")).toString(), o.value(QStringLiteral("envFile")).toString()};
}

void setRunSettingsFor(const QString& script, const RunSettings& settings)
{
    QJsonObject all = QJsonDocument::fromJson(_settings::Get().item<QString>("PythonRunSettings").toUtf8()).object();
    const QString key = QFileInfo(script).absoluteFilePath();
    if (settings.isEmpty()) all.remove(key);
    else
        all.insert(key, QJsonObject{{QStringLiteral("arguments"), settings.arguments},
                                    {QStringLiteral("folder"), settings.folder},
                                    {QStringLiteral("environment"), settings.environment},
                                    {QStringLiteral("envFile"), settings.envFile}});
    _settings::Get().setItem<QString>("PythonRunSettings", QString::fromUtf8(QJsonDocument(all).toJson(QJsonDocument::Compact)));
}

QStringList splitArguments(const QString& text)
{
    // As a shell: '...' as it is, "..." with \" \\ \$ \` escaped, a
    // backslash escaping the next character - not on Windows, where it is a
    // path's.
#ifdef Q_OS_WIN
    constexpr bool escapes = false;
#else
    constexpr bool escapes = true;
#endif
    QStringList out;
    QString word;
    bool inWord = false;
    QChar quote;
    for (qsizetype k = 0; k < text.size(); ++k) {
        const QChar c = text.at(k);
        if (quote == QLatin1Char('\'')) {
            if (c == quote) quote = QChar();
            else word += c;
            continue;
        }
        if (quote == QLatin1Char('"')) {
            if (c == quote) quote = QChar();
            else if (escapes && c == QLatin1Char('\\') && k + 1 < text.size() && QStringLiteral("\"\\$`").contains(text.at(k + 1))) word += text.at(++k);
            else word += c;
            continue;
        }
        if (c.isSpace()) {
            if (inWord) out << word;
            word.clear();
            inWord = false;
            continue;
        }
        inWord = true;
        if (c == QLatin1Char('\'') || c == QLatin1Char('"')) quote = c;
        else if (escapes && c == QLatin1Char('\\') && k + 1 < text.size()) word += text.at(++k);
        else word += c;
    }
    if (inWord) out << word;
    return out;
}

QList<std::pair<QString, QString>> readEnvironment(const QString& text, const QProcessEnvironment& base)
{
    QList<std::pair<QString, QString>> found;
    static const QRegularExpression line(QStringLiteral("^\\s*(?:export\\s+)?([A-Za-z_][A-Za-z0-9_]*)\\s*=\\s*(.*?)\\s*$"));
    static const QRegularExpression reference(QStringLiteral("\\$\\{([A-Za-z_][A-Za-z0-9_]*)\\}"));
    const auto valueOf = [&](const QString& name) {
        for (qsizetype k = found.size() - 1; k >= 0; --k)
            if (found.at(k).first == name) return found.at(k).second;
        return base.value(name);
    };
    for (const QString& raw : text.split(QLatin1Char('\n'))) {
        const QString trimmed = raw.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#'))) continue;
        const QRegularExpressionMatch m = line.match(raw);
        if (!m.hasMatch()) continue;
        QString value = m.captured(2);
        const bool single = value.size() >= 2 && value.startsWith(QLatin1Char('\'')) && value.endsWith(QLatin1Char('\''));
        const bool dbl = value.size() >= 2 && value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"'));
        if (single || dbl) {
            value = value.mid(1, value.size() - 2);
        } else {   // (a comment after it)
            const qsizetype hash = value.indexOf(QStringLiteral(" #"));
            if (hash >= 0) value = value.left(hash).trimmed();
        }
        if (!single) {   // ${NAME}: as it is before
            QString expanded;
            qsizetype at = 0;
            for (const QRegularExpressionMatch& r : reference.globalMatch(value)) {
                expanded += value.mid(at, r.capturedStart(0) - at) + valueOf(r.captured(1));
                at = r.capturedEnd(0);
            }
            value = expanded + value.mid(at);
        }
        found.append({m.captured(1), value});
    }
    return found;
}

RunPlan runPlanFor(const QString& script)
{
    RunPlan plan;
    const QFileInfo info(script);
    const RunSettings settings = runSettingsFor(script);
    plan.environment = scriptEnvironment();
    plan.arguments = splitArguments(settings.arguments);
    plan.folder = info.absolutePath();
    if (const QString folder = settings.folder.trimmed(); !folder.isEmpty()) {
        const QString path = QDir(info.absolutePath()).absoluteFilePath(folder);
        if (!QFileInfo(path).isDir()) {
            plan.failure = tr("The working folder %1 of its Run Settings is not there.").arg(QDir::toNativeSeparators(path));
            return plan;
        }
        plan.folder = QDir::cleanPath(path);
    }
    if (const QString file = settings.envFile.trimmed(); !file.isEmpty()) {
        const QString path = QDir(info.absolutePath()).absoluteFilePath(file);
        QFile env(path);
        if (!env.open(QIODevice::ReadOnly)) {
            plan.failure = tr("The .env file %1 of its Run Settings could not be read.").arg(QDir::toNativeSeparators(path));
            return plan;
        }
        for (const auto& [name, value] : readEnvironment(QString::fromUtf8(env.readAll()), plan.environment)) plan.environment.insert(name, value);
    }
    for (const auto& [name, value] : readEnvironment(settings.environment, plan.environment)) plan.environment.insert(name, value);
    return plan;
}

QJsonObject toJson(const Breakpoint& b)
{
    return {{QStringLiteral("line"), b.line},
            {QStringLiteral("condition"), b.condition.trimmed()},
            {QStringLiteral("hit"), b.hit.trimmed()},
            {QStringLiteral("log"), b.log},
            {QStringLiteral("enabled"), b.enabled}};
}

QList<Symbol> symbolsOf(const QString& text)
{
    // The outline's functions and classes - methods with their class - and
    // the module's variables: a name given a value at the top level (a, b =
    // ...; for x in ...; x: int = ...), the first time.
    QList<Symbol> symbols;
    const QList<OutlineEntry> outline = outlineOf(text);
    QList<std::pair<int, QString>> classes;   // the classes open: their depth and name
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const OutlineEntry& e : outline) {
        while (!classes.isEmpty() && classes.last().first >= e.depth) classes.removeLast();
        const QString line = lines.value(e.line - 1);
        const qsizetype at = line.indexOf(QRegularExpression(QStringLiteral("\\b%1\\b").arg(QRegularExpression::escape(e.name))),
                                          line.indexOf(e.kind == QLatin1String("class") ? QLatin1String("class") : QLatin1String("def")));
        Symbol s;
        s.line = e.line;
        s.column = int(std::max<qsizetype>(0, at));
        s.kind = e.kind == QLatin1String("class") ? QStringLiteral("class") : QStringLiteral("function");
        s.name = e.name;
        s.container = classes.isEmpty() ? QString() : classes.last().second;
        symbols.append(s);
        if (e.kind == QLatin1String("class")) classes.append({e.depth, e.name});
    }
    static const QRegularExpression assigned(QStringLiteral("^(?:for\\s+([A-Za-z_]\\w*(?:\\s*,\\s*[A-Za-z_]\\w*)*)\\s+in\\b"
                                                           "|([A-Za-z_]\\w*(?:\\s*,\\s*[A-Za-z_]\\w*)*)\\s*(?::[^=]*)?=(?!=))"));
    QSet<QString> seen;
    for (const Symbol& s : std::as_const(symbols))
        if (s.container.isEmpty()) seen.insert(s.name);
    QChar open;   // (a string left open across lines: a docstring)
    for (int k = 0; k < lines.size(); ++k) {
        const QString& line = lines.at(k);
        const qsizetype triple = line.count(QLatin1String("\"\"\"")) + line.count(QLatin1String("'''"));
        if (!open.isNull()) {
            if (triple % 2 == 1) open = QChar();
            continue;
        }
        if (triple % 2 == 1) {
            open = QLatin1Char('"');
            continue;
        }
        if (line.isEmpty() || line.front().isSpace()) continue;
        const QRegularExpressionMatch m = assigned.match(line);
        if (!m.hasMatch()) continue;
        static const QSet<QString> keywords{
            QStringLiteral("if"), QStringLiteral("elif"), QStringLiteral("else"), QStringLiteral("while"), QStringLiteral("try"),
            QStringLiteral("except"), QStringLiteral("finally"), QStringLiteral("with"), QStringLiteral("lambda"),
            QStringLiteral("return"), QStringLiteral("not"), QStringLiteral("and"), QStringLiteral("or"), QStringLiteral("in"),
            QStringLiteral("is"), QStringLiteral("assert"), QStringLiteral("del"), QStringLiteral("global"),
            QStringLiteral("nonlocal"), QStringLiteral("yield"), QStringLiteral("await"), QStringLiteral("case"),
            QStringLiteral("match"), QStringLiteral("print")};
        const QString names = m.captured(1).isEmpty() ? m.captured(2) : m.captured(1);
        for (const QString& part : names.split(QLatin1Char(','))) {
            const QString name = part.trimmed();
            if (name.isEmpty() || seen.contains(name) || name == QLatin1String("_") || keywords.contains(name)) continue;
            seen.insert(name);
            Symbol s;
            s.line = k + 1;
            s.column = int(line.indexOf(name));
            s.kind = QStringLiteral("variable");
            s.name = name;
            symbols.append(s);
        }
    }
    std::stable_sort(symbols.begin(), symbols.end(), [](const Symbol& a, const Symbol& b) { return a.line < b.line; });
    return symbols;
}

int symbolScore(const QString& name, const QString& pattern)
{
    // Its letters in order (any case): better at the start, together, and
    // at the start of a word of the name (snake_case, CamelCase).
    if (pattern.isEmpty()) return 0;
    int score = 0;
    qsizetype at = 0;
    qsizetype last = -2;
    for (const QChar c : pattern) {
        const qsizetype found = name.indexOf(c, at, Qt::CaseInsensitive);
        if (found < 0) return -1;
        if (found == last + 1) score += 5;   // together
        if (found == 0) score += 10;
        else if (name.at(found - 1) == QLatin1Char('_') || (name.at(found).isUpper() && name.at(found - 1).isLower())) score += 6;
        if (name.at(found) == c) score += 1;   // the same case
        last = found;
        at = found + 1;
    }
    if (name.startsWith(pattern, Qt::CaseInsensitive)) score += 20;
    if (name.compare(pattern, Qt::CaseInsensitive) == 0) score += 30;
    return score - int(name.size() - pattern.size()) / 4;
}

TypeCheck readTypeCheck(const QByteArray& output)
{
    TypeCheck check;
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(output.trimmed(), &error);
    if (!doc.isObject()) {
        check.failure = tr("The type check gave no answer it should (%1).").arg(error.errorString());
        return check;
    }
    const QJsonObject o = doc.object();
    check.failure = o.value(QStringLiteral("failure")).toString();
    check.tool = o.value(QStringLiteral("tool")).toString();
    for (const QJsonValue& v : o.value(QStringLiteral("problems")).toArray()) {
        const QJsonObject p = v.toObject();
        Problem problem;
        problem.line = std::max(1, p.value(QStringLiteral("line")).toInt(1));
        problem.column = std::max(0, p.value(QStringLiteral("column")).toInt());
        problem.endLine = std::max(0, p.value(QStringLiteral("endLine")).toInt());
        problem.endColumn = std::max(0, p.value(QStringLiteral("endColumn")).toInt());
        problem.message = p.value(QStringLiteral("message")).toString();
        problem.code = p.value(QStringLiteral("code")).toString();
        check.problems.append(problem);
    }
    return check;
}

QHash<int, int> bracketPairs(const QString& text)
{
    QHash<int, int> pairs;
    QList<std::pair<int, QChar>> open;
    const auto closes = [](QChar o, QChar c) {
        return (o == QLatin1Char('(') && c == QLatin1Char(')')) || (o == QLatin1Char('[') && c == QLatin1Char(']'))
               || (o == QLatin1Char('{') && c == QLatin1Char('}'));
    };
    const qsizetype n = text.size();
    for (qsizetype k = 0; k < n; ++k) {
        const QChar c = text.at(k);
        if (c == QLatin1Char('#')) {   // a comment: to the line's end
            while (k < n && text.at(k) != QLatin1Char('\n')) ++k;
            continue;
        }
        if (c == QLatin1Char('\'') || c == QLatin1Char('"')) {   // a string: past it
            const bool triple = k + 2 < n && text.at(k + 1) == c && text.at(k + 2) == c;
            qsizetype e = k + (triple ? 3 : 1);
            while (e < n) {
                const QChar d = text.at(e);
                if (d == QLatin1Char('\\')) {
                    e += 2;
                    continue;
                }
                if (triple ? (d == c && e + 2 < n && text.at(e + 1) == c && text.at(e + 2) == c) : d == c) break;
                if (!triple && d == QLatin1Char('\n')) break;   // (left open: it ends with its line)
                ++e;
            }
            k = std::min(n, e + (triple ? 3 : 1)) - 1;
            continue;
        }
        if (c == QLatin1Char('(') || c == QLatin1Char('[') || c == QLatin1Char('{')) {
            open.append({int(k), c});
        } else if (c == QLatin1Char(')') || c == QLatin1Char(']') || c == QLatin1Char('}')) {
            if (!open.isEmpty() && closes(open.last().second, c)) {
                pairs.insert(open.last().first, int(k));
                pairs.insert(int(k), open.last().first);
                open.removeLast();
            } else {
                pairs.insert(int(k), -1);   // (closes nothing - or another kind)
            }
        }
    }
    for (const auto& o : std::as_const(open)) pairs.insert(o.first, -1);
    return pairs;
}

namespace {
int indentWidth(QStringView line)
{
    int n = 0;
    for (const QChar c : line) {
        if (c == QLatin1Char(' ')) ++n;
        else if (c == QLatin1Char('\t')) n += 8 - n % 8;
        else break;
    }
    return n;
}
} // namespace

std::pair<int, int> foldRange(const QStringList& lines, int line)
{
    if (line < 1 || line > lines.size()) return {0, 0};
    const QString& head = lines.at(line - 1);
    int last = line;
    if (isCellMarker(head)) {   // a cell: to the line before the next one
        for (int k = line + 1; k <= lines.size() && !isCellMarker(lines.at(k - 1)); ++k)
            if (!lines.at(k - 1).trimmed().isEmpty()) last = k;
        return last > line ? std::pair<int, int>{line + 1, last} : std::pair<int, int>{0, 0};
    }
    if (head.trimmed().isEmpty()) return {0, 0};
    const int indent = indentWidth(head);
    for (int k = line + 1; k <= lines.size(); ++k) {
        const QString& text = lines.at(k - 1);
        if (text.trimmed().isEmpty()) continue;
        if (indentWidth(text) <= indent || isCellMarker(text)) break;
        last = k;
    }
    return last > line ? std::pair<int, int>{line + 1, last} : std::pair<int, int>{0, 0};
}

std::pair<QString, int> dottedNameAt(const QString& text, int position)
{
    const auto nameCharacter = [](QChar c) { return c.isLetterOrNumber() || c == QLatin1Char('_'); };
    int start = position, end = position;
    while (start > 0 && nameCharacter(text.at(start - 1))) --start;
    while (end < text.size() && nameCharacter(text.at(end))) ++end;
    if (start == end || text.at(start).isDigit() || inStringOrComment(text.left(start))) return {};
    // Back over what it is an attribute of: name.name.
    while (start >= 2 && text.at(start - 1) == QLatin1Char('.') && nameCharacter(text.at(start - 2))) {
        int before = start - 1;
        while (before > 0 && nameCharacter(text.at(before - 1))) --before;
        if (text.at(before).isDigit()) break;
        start = before;
    }
    return {text.mid(start, end - start), start};
}

QIcon completionIcon(const QString& type)
{
    static QHash<QString, QIcon> icons;
    const auto found = icons.constFind(type);
    if (found != icons.constEnd()) return *found;
    QString letter = QStringLiteral("v");
    QColor colour(0x2a, 0x9d, 0x8f);
    if (type == QLatin1String("function") || type == QLatin1String("method")) {
        letter = QStringLiteral("f");
        colour = QColor(0x7c, 0x4d, 0xbd);
    } else if (type == QLatin1String("class")) {
        letter = QStringLiteral("C");
        colour = QColor(0xd9, 0x82, 0x2b);
    } else if (type == QLatin1String("module")) {
        letter = QStringLiteral("m");
        colour = QColor(0x2f, 0x6f, 0xb5);
    } else if (type == QLatin1String("keyword")) {
        letter = QStringLiteral("k");
        colour = QColor(0x6b, 0x72, 0x80);
    } else if (type == QLatin1String("param")) {
        letter = QStringLiteral("p");
        colour = QColor(0xb0, 0x4a, 0x7a);
    } else if (type == QLatin1String("path")) {
        letter = QStringLiteral("/");
        colour = QColor(0x6b, 0x72, 0x80);
    }
    QIcon icon;
    for (const int scale : {1, 2}) {
        QPixmap pixmap(16 * scale, 16 * scale);
        pixmap.setDevicePixelRatio(scale);   // (drawn in a 16 x 16 square either way)
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(colour);
        painter.drawRoundedRect(QRectF(1, 1, 14, 14), 3, 3);
        QFont font = painter.font();
        font.setBold(true);
        font.setPixelSize(11);
        painter.setFont(font);
        painter.setPen(Qt::white);
        painter.drawText(QRectF(1, 1, 14, 14), Qt::AlignCenter, letter);
        painter.end();
        icon.addPixmap(pixmap);
    }
    icons.insert(type, icon);
    return icon;
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

    // Completion: a list below the cursor (the completer's answers,
    // narrowed by what is typed).
    a_completions = new QStandardItemModel(this);
    a_completer = new QCompleter(a_completions, this);
    a_completer->setWidget(this);
    a_completer->setCompletionMode(QCompleter::PopupCompletion);
    a_completer->setCaseSensitivity(Qt::CaseInsensitive);
    a_completer->setModelSorting(QCompleter::UnsortedModel);
    a_completer->setFilterMode(Qt::MatchStartsWith);
    a_completer->setMaxVisibleItems(12);
    a_completer->setWrapAround(false);
    a_completer->popup()->setObjectName(QStringLiteral("pythonCompletions"));
    connect(a_completer, QOverload<const QString&>::of(&QCompleter::activated), this, &PythonDoc::insertCompletion);
    a_completeDelay = new QTimer(this);
    a_completeDelay->setSingleShot(true);
    a_completeDelay->setInterval(kCompleteDelay);
    connect(a_completeDelay, &QTimer::timeout, this, [this] { complete(false); });

    // The call the cursor is in, above it (asked again as the cursor moves
    // in it).
    a_signatureTip = new QLabel(this, Qt::ToolTip | Qt::FramelessWindowHint);
    a_signatureTip->setObjectName(QStringLiteral("pythonSignature"));
    a_signatureTip->setForegroundRole(QPalette::ToolTipText);
    a_signatureTip->setBackgroundRole(QPalette::ToolTipBase);
    a_signatureTip->setAutoFillBackground(true);
    a_signatureTip->setFrameStyle(QFrame::Box | QFrame::Plain);
    a_signatureTip->setMargin(4);
    a_signatureTip->setTextFormat(Qt::RichText);
    a_signatureTip->setWordWrap(true);
    a_signatureTip->setMaximumWidth(640);
    a_signatureDelay = new QTimer(this);
    a_signatureDelay->setSingleShot(true);
    a_signatureDelay->setInterval(kCompleteDelay);
    connect(a_signatureDelay, &QTimer::timeout, this, [this] { showSignature(false); });
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this] {
        if (signatureShown()) a_signatureDelay->start();
    });
    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect&, int dy) {
        if (dy != 0 && signatureShown()) a_signatureTip->hide();   // (scrolled: no longer by its call)
    });

    // The outline above the text.
    a_outlineBar = new QWidget(this);
    a_outlineBar->setObjectName(QStringLiteral("pythonOutlineBar"));
    a_outlineBar->setAutoFillBackground(true);
    auto* row = new QHBoxLayout(a_outlineBar);
    row->setContentsMargins(4, 2, 4, 2);
    a_outline = new QComboBox(a_outlineBar);
    a_outline->setObjectName(QStringLiteral("pythonOutline"));
    a_outline->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    a_outline->setMinimumContentsLength(24);
    a_outline->setMaxVisibleItems(24);
    a_outline->setFocusPolicy(Qt::NoFocus);
    a_outline->setToolTip(tr("The script's functions and classes: the one the cursor is in, and any other to go to"));
    row->addWidget(a_outline);
    row->addStretch(1);
    connect(a_outline, &QComboBox::activated, this, [this](int index) {
        const int line = a_outline->itemData(index).toInt();
        if (line <= 0) return;
        const QTextBlock block = document()->findBlockByNumber(line - 1);
        QTextCursor at(block);
        const QString text = block.text();
        int indent = 0;
        while (indent < text.size() && text.at(indent).isSpace()) ++indent;
        at.setPosition(block.position() + indent);
        setTextCursor(at);
        centerCursor();
        setFocus();
    });
    a_outlineDelay = new QTimer(this);
    a_outlineDelay->setSingleShot(true);
    a_outlineDelay->setInterval(300);
    connect(a_outlineDelay, &QTimer::timeout, this, &PythonDoc::updateOutline);
    connect(document(), &QTextDocument::contentsChanged, a_outlineDelay, qOverload<>(&QTimer::start));
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &PythonDoc::chooseOutlineEntry);
    // The light bulb follows the cursor (to a line with a problem).
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this] {
        if (const int line = bulbLine(); line != a_bulbLine) {
            a_bulbLine = line;
            refreshMarks();
        }
    });

    // The bracket at the cursor and its partner; the folds kept as the
    // text changes, one the cursor goes into opened.
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &PythonDoc::markBrackets);
    connect(document(), &QTextDocument::contentsChanged, this, [this] {
        if (!a_folds.isEmpty() && !a_applyingFolds) QTimer::singleShot(0, this, &PythonDoc::applyFolds);
    });
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this] {
        if (a_folds.isEmpty() || a_applyingFolds || textCursor().block().isVisible()) return;
        const int line = textCursor().blockNumber() + 1;
        const QStringList lines = toPlainText().split(QLatin1Char('\n'));
        a_folds.removeIf([&](const Fold& f) {
            const auto [first, last] = qucs_s::python::foldRange(lines, f.at.blockNumber() + 1);
            return first <= line && line <= last;
        });
        applyFolds();
    });

    // The type check: after the check, one at a time.
    a_typeDelay = new QTimer(this);
    a_typeDelay->setSingleShot(true);
    a_typeDelay->setInterval(200);
    connect(a_typeDelay, &QTimer::timeout, this, &PythonDoc::startTypeCheck);
    updateMargins();
    placeOutline();
    updateOutline();
}

PythonDoc::~PythonDoc()
{
    openScripts().remove(this);
    stopCheck();
    stopCompleter();
    for (QProcess* process : {a_formatProcess, a_typeProcess}) {
        if (process == nullptr) continue;
        disconnect(process, nullptr, this, nullptr);
        process->kill();
        process->waitForFinished(1000);
    }
    // TextDoc's destructor edits the text as it lets the highlighter go:
    // no check is scheduled by a PythonDoc that is no more.
    disconnect(document(), nullptr, this, nullptr);
}

bool PythonDoc::load()
{
    const bool loaded = TextDoc::load();
    if (loaded) {
        updateOutline();
        checkNow();
    }
    return loaded;
}

bool PythonDoc::reload()
{
    // Read again: the breakpoints at the same lines (the text cleared
    // first would take them all to its start).
    const QList<int> kept = breakpoints();
    const bool loaded = TextDoc::reload();
    setBreakpoints(kept);
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
    a_interpreter = path;   // (the next question starts the completer with it)
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
    if (a_library) return;   // (Python's own: not the user's to fix)

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
    a_checkRevision = a_checkedRevision;

    QList<Diagnostic> marks;
    bool errors = false;
    for (const qucs_s::python::Problem& p : std::as_const(check.problems)) {
        Diagnostic d;
        d.line = p.line;
        d.column = p.column;
        d.endLine = p.endLine;
        d.endColumn = p.endColumn;
        d.error = p.error;
        d.message = p.code.isEmpty() ? p.message : QStringLiteral("%1 (%2)").arg(p.message, p.code);
        marks.append(d);
        errors = errors || p.error;
    }
    // The type check's findings kept where they are now (they move with the
    // text) until its next answer.
    QList<Diagnostic> typed;
    for (const Diagnostic& d : diagnostics())
        if (!d.source.isEmpty()) typed.append(d);
    showProblems(marks, typed);
    emit checkFinished();
    // Typed when it compiles (a syntax error is the check's to say).
    if (!errors && check.failure.isEmpty()) scheduleTypeCheck();
}

void PythonDoc::showProblems(const QList<TextDoc::Diagnostic>& basic, const QList<TextDoc::Diagnostic>& typed)
{
    QList<Diagnostic> all = basic;
    all.append(typed);
    std::stable_sort(all.begin(), all.end(), [](const Diagnostic& a, const Diagnostic& b) {
        return a.line != b.line ? a.line < b.line : a.column < b.column;
    });
    setDiagnostics(all);
}

QString PythonDoc::checkedBy() const
{
    if (checking() && !a_hasCheck) return tr("Checking with %1...").arg(interpreter());
    if (!a_hasCheck) return tr("Not checked yet.");
    if (!a_check.failure.isEmpty()) return a_check.failure;
    const QString python = tr("Python %1").arg(a_check.python);
    QString said = a_check.checker.isEmpty()
                       ? tr("%1's compiler alone: syntax errors and its warnings. With ruff or pyflakes installed for it, "
                            "names not defined, imports not used and more.").arg(python)
                       : tr("%1 and %2").arg(python, a_check.checker);
    // The type check's word.
    if (typeChecker() == QLatin1String("off")) return said;
    if (!a_typeCheck.failure.isEmpty()) said += QLatin1Char('\n') + tr("The type check failed: %1").arg(a_typeCheck.failure);
    else if (!a_typeCheck.tool.isEmpty()) said += QLatin1Char('\n') + tr("Types checked by %1.").arg(a_typeCheck.tool);
    else if (typeChecked())
        said += QLatin1Char('\n') + (typeChecker() == QLatin1String("auto")
                                          ? tr("No type checker installed for it (mypy or pyright).")
                                          : tr("No %1 installed for it.").arg(typeChecker()));
    return said;
}

bool PythonDoc::messagesAtLineEnds()
{
    return _settings::Get().item<bool>("PythonMessagesAtLineEnds");
}

QList<PythonDoc*> PythonDoc::openDocuments() { return openScripts().values(); }

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
    // The list's keys while it is shown: Return, Enter and Tab take the
    // word chosen, Escape closes it (the completer acts on a key left).
    if (completing()) {
        switch (key) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Tab:
        case Qt::Key_Backtab:
        case Qt::Key_Escape:
            event->ignore();
            return;
        default:
            break;
        }
    }
    if (key == Qt::Key_Escape && signatureShown()) {
        a_signatureTip->hide();
        return;
    }
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
    if (autoClose() && closePair(event)) {
        afterTyping(event->text());
        return;
    }
    TextDoc::keyPressEvent(event);
    afterTyping(event->text());
}

// ----------------------------------------------------------------------
// Completion

namespace {
// \a text ends in a dot after a name, a call or a subscript - not after a
// number (1.).
bool afterNameDot(const QString& text)
{
    if (!text.endsWith(QLatin1Char('.')) || text.size() < 2) return false;
    const QString before = text.chopped(1);
    const QChar last = before.back();
    if (last == QLatin1Char(')') || last == QLatin1Char(']')) return true;
    const QString name = before.mid(qucs_s::python::wordStart(before));
    return !name.isEmpty() && !name.front().isDigit();
}
} // namespace

bool PythonDoc::completing() const
{
    // (Asked by event() while it is being made, too: no completer yet.)
    return a_completer != nullptr && a_completer->popup()->isVisible();
}

namespace {
// The keys of the list of completions while it is shown: theirs, not the
// window's shortcuts' (Escape is the window's - back to selecting).
bool isListKey(const QEvent* event)
{
    if (event->type() != QEvent::ShortcutOverride) return false;
    switch (static_cast<const QKeyEvent*>(event)->key()) {
    case Qt::Key_Escape:
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
        return true;
    default:
        return false;
    }
}
} // namespace

bool PythonDoc::event(QEvent* event)
{
    // (The list hands the keyboard to the text - QCompleter makes it the
    // list's focus proxy -: the text is asked about the shortcuts.)
    if (completing() && isListKey(event)) {
        event->accept();
        return true;
    }
    if (event->type() == QEvent::ShortcutOverride) {
        const auto* key = static_cast<const QKeyEvent*>(event);
        // Escape closes the call's signature, not the window's.
        if (key->key() == Qt::Key_Escape && signatureShown()) {
            event->accept();
            return true;
        }
        // Shift+Return runs the selection or the line (Simulation > Python):
        // the action's, not a line break - which the text would take.
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
            && (key->modifiers() & ~Qt::KeypadModifier) == Qt::ShiftModifier) {
            event->ignore();
            return true;
        }
    }
    return TextDoc::event(event);
}

QStringList PythonDoc::completionNames() const
{
    QStringList names;
    const QAbstractItemModel* shown = a_completer->completionModel();
    for (int row = 0; row < shown->rowCount(); ++row) names << shown->index(row, 0).data().toString();
    return names;
}

QString PythonDoc::completedBy() const
{
    if (!a_completionEngine.isEmpty()) return tr("Completed by %1.").arg(a_completionEngine);
    return tr("Completed with the script's names, Python's keywords and builtins and the members of the modules it "
              "imports. With jedi installed for its Python: the attributes of anything, arguments and more.");
}

bool PythonDoc::completeAsYouType()
{
    return _settings::Get().item<bool>("PythonCompleteAsYouType");
}

void PythonDoc::setCompleteAsYouType(bool on)
{
    _settings::Get().setItem<bool>("PythonCompleteAsYouType", on);
}

void PythonDoc::afterTyping(const QString& typed)
{
    const QTextCursor cursor = textCursor();
    const QString before = cursor.block().text().left(cursor.positionInBlock());
    const int start = qucs_s::python::wordStart(before);
    const QString word = before.mid(start);
    if (completing()) {
        // Narrowed while the word goes on; closed when it is another.
        if (cursor.blockNumber() != a_requestBlock || start != a_requestStart || cursor.hasSelection()) {
            a_completer->popup()->hide();
        } else {
            a_completer->setCompletionPrefix(word);
            if (a_completer->completionCount() == 0) a_completer->popup()->hide();
            else placeList(word);   // (as long as its words now)
        }
        return;
    }
    // As one types: a moment after a letter of a name or a dot, complete()
    // says whether there is a name to complete.
    const bool nameOrDot = typed.size() == 1 && (typed.at(0).isLetterOrNumber() || typed.at(0) == QLatin1Char('_')
                                                 || typed.at(0) == QLatin1Char('.'));
    if (completeAsYouType() && nameOrDot) a_completeDelay->start();
    else a_completeDelay->stop();
    // The call's signature: asked for at its bracket or a comma, and
    // again as it is written (gone when the call is closed).
    const bool call = typed == QLatin1String("(") || typed == QLatin1String(",");
    if ((call && completeAsYouType()) || (signatureShown() && !typed.isEmpty())) a_signatureDelay->start();
}

void PythonDoc::complete(bool asked)
{
    a_completeDelay->stop();
    const QTextCursor cursor = textCursor();
    const QString before = cursor.block().text().left(cursor.positionInBlock());
    if (!asked) {
        // As one types: two letters of a name, or a dot after a name or a
        // bracket (not a number's) - not in a string or a comment. (The
        // word as it is now: it may have ended since the letter.)
        const QString word = before.mid(qucs_s::python::wordStart(before));
        const bool afterDot = word.isEmpty() && afterNameDot(before);
        if ((!afterDot && (word.size() < 2 || word.at(0).isDigit())) || qucs_s::python::inStringOrComment(before)) return;
    }
    const int id = ask(QStringLiteral("complete"), cursor.blockNumber() + 1, cursor.positionInBlock());
    if (id == 0) return;
    a_request = id;
    a_requestBlock = cursor.blockNumber();
    a_requestStart = qucs_s::python::wordStart(before);
}

int PythonDoc::ask(const QString& kind, int line, int column)
{
    if (!startCompleter()) return 0;
    const int id = ++a_questions;
    const QJsonObject question{{QStringLiteral("id"), id},
                               {QStringLiteral("kind"), kind},
                               {QStringLiteral("source"), toPlainText()},
                               {QStringLiteral("line"), line},
                               {QStringLiteral("column"), column},
                               {QStringLiteral("path"), getDocName().isEmpty() ? QString() : QFileInfo(getDocName()).absoluteFilePath()}};
    a_completerProcess->write(QJsonDocument(question).toJson(QJsonDocument::Compact) + '\n');
    return id;
}

bool PythonDoc::startCompleter()
{
    // Running with the script's interpreter, or started again with it.
    const QString python = interpreter();
    if (a_completerProcess != nullptr && a_completerInterpreter == python) return true;
    stopCompleter();
    a_completerProcess = new QProcess(this);
    QProcessEnvironment environment = qucs_s::python::scriptEnvironment();   // (the qucs module completed too)
    environment.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
    a_completerProcess->setProcessEnvironment(environment);
    a_completerProcess->setWorkingDirectory(qucs_s::python::neutralFolder());
    a_completerProcess->setStandardErrorFile(QProcess::nullDevice());
    connect(a_completerProcess, &QProcess::readyReadStandardOutput, this, &PythonDoc::readCompleter);
    // Gone (it crashed, or it was stopped): started again for the next one.
    QProcess* process = a_completerProcess;
    connect(a_completerProcess, &QProcess::finished, this, [this, process] {
        if (a_completerProcess != process) return;
        a_completerProcess = nullptr;
        a_completerOutput.clear();
        process->deleteLater();
    });
    a_completerProcess->start(python, {QStringLiteral("-u"), QStringLiteral("-c"), qucs_s::python::completerProgram()});
    if (!a_completerProcess->waitForStarted(5000)) {
        stopCompleter();
        return false;
    }
    a_completerInterpreter = python;
    return true;
}

void PythonDoc::stopCompleter()
{
    if (a_completerProcess == nullptr) return;
    QProcess* process = a_completerProcess;
    a_completerProcess = nullptr;
    a_completerOutput.clear();
    disconnect(process, nullptr, this, nullptr);
    process->closeWriteChannel();
    if (!process->waitForFinished(300)) {
        process->kill();
        process->waitForFinished(1000);
    }
    process->deleteLater();
}

void PythonDoc::readCompleter()
{
    if (a_completerProcess == nullptr) return;
    a_completerOutput += a_completerProcess->readAllStandardOutput();
    qsizetype end;
    while ((end = a_completerOutput.indexOf('\n')) >= 0) {
        const QByteArray line = a_completerOutput.left(end);
        a_completerOutput.remove(0, end + 1);
        const qucs_s::python::Answer answer = qucs_s::python::readAnswer(line);
        // (An earlier question's answer: overtaken.)
        if (answer.kind == QLatin1String("complete") && answer.id == a_request) {
            qucs_s::python::Completions completions;
            completions.id = answer.id;
            completions.engine = answer.engine;
            completions.items = answer.items;
            showCompletions(completions);
        } else if (answer.kind == QLatin1String("signature") && answer.id == a_signatureRequest) {
            answerSignature(answer);
        } else if (answer.kind == QLatin1String("help") && answer.id == a_helpRequest) {
            answerHelp(answer);
        } else if (answer.kind == QLatin1String("definition") && answer.id == a_definitionRequest) {
            if (!answer.engine.isEmpty()) a_completionEngine = answer.engine;
            a_definition = answer.place;
            emit definitionAnswered();
        } else if (answer.kind == QLatin1String("imports") && answer.id == a_fixRequest) {
            a_fixRequest = 0;
            if (a_fixesRevision == document()->revision()) {   // (of the text as it is: first, the likeliest first)
                qsizetype at = 0;
                for (const qucs_s::python::Answer::Import& i : std::as_const(answer.imports))
                    a_fixes.insert(at++, {tr("Import: %1").arg(i.title), {{i.line, 1, i.line, 1, i.text}}});
            }
            emit fixesAnswered();
            if (!a_fixAt.isNull()) showFixMenu();
        } else if (answer.kind == QLatin1String("references") && answer.id == a_referencesRequest) {
            a_references = answer;
            emit referencesAnswered();
        } else if (answer.kind == QLatin1String("rename") && answer.id == a_renameRequest) {
            a_rename = answer;
            // The script's own change made here - unless it changed since.
            if (answer.valid && answer.refusal.isEmpty() && a_renameRevision != document()->revision())
                a_rename.refusal = tr("The script changed while the name was being renamed: nothing done.");
            else
                for (const qucs_s::python::FileChange& c : std::as_const(answer.changes))
                    if (c.file.isEmpty() && answer.refusal.isEmpty()) replaceText(c.text);
            emit renameAnswered();
        }
    }
}

void PythonDoc::showCompletions(const qucs_s::python::Completions& answer)
{
    a_completionEngine = answer.engine;
    const QTextCursor cursor = textCursor();
    const QString before = cursor.block().text().left(cursor.positionInBlock());
    const int start = qucs_s::python::wordStart(before);
    // The cursor moved on to another word, or the keyboard to another
    // widget: not shown.
    const QWidget* focus = QApplication::focusWidget();
    const bool elsewhere = !isVisible() || (focus != nullptr && focus != this && focus != a_completer->popup());
    if (cursor.blockNumber() != a_requestBlock || start != a_requestStart || cursor.hasSelection() || elsewhere) {
        emit completionsAnswered();
        return;
    }
    a_completions->clear();
    for (const qucs_s::python::Completion& c : answer.items) {
        auto* item = new QStandardItem(qucs_s::python::completionIcon(c.type), c.name);
        item->setEditable(false);
        item->setToolTip(c.description.isEmpty() ? c.type : c.description);
        item->setData(c.type, Qt::UserRole);
        a_completions->appendRow(item);
    }
    const QString word = before.mid(start);
    a_completer->setCompletionPrefix(word);
    // Nothing to add: none, or the word as it is typed.
    const int count = a_completer->completionCount();
    if (count == 0 || (count == 1 && a_completer->currentCompletion() == word)) {
        a_completer->popup()->hide();
        emit completionsAnswered();
        return;
    }
    placeList(word);
    emit completionsAnswered();
}

void PythonDoc::placeList(const QString& word)
{
    // Below the word's start, as wide as its words, its first chosen.
    QRect at = cursorRect();
    at.translate(viewport()->pos());
    at.moveLeft(at.left() - fontMetrics().horizontalAdvance(word));
    at.setWidth(a_completer->popup()->sizeHintForColumn(0) + a_completer->popup()->verticalScrollBar()->sizeHint().width() + 8);
    a_completer->complete(at);
    a_completer->popup()->setCurrentIndex(a_completer->completionModel()->index(0, 0));
}

void PythonDoc::insertCompletion(const QString& name)
{
    QTextCursor cursor = textCursor();
    const QString before = cursor.block().text().left(cursor.positionInBlock());
    cursor.setPosition(cursor.block().position() + qucs_s::python::wordStart(before), QTextCursor::KeepAnchor);
    cursor.insertText(name);
    setTextCursor(cursor);
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

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

#include <QAbstractItemView>
#include <QApplication>
#include <QCompleter>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
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

const QString& completerProgram()
{
    static const QString program = QStringLiteral(R"PY(
import sys, json, re, os, io, ast, keyword, builtins, inspect, importlib, pkgutil, tokenize
try:
    import jedi
    ENGINE = ('jedi ' + str(getattr(jedi, '__version__', ''))).strip()
except Exception:
    jedi = None
    ENGINE = ''
STDLIB = set(getattr(sys, 'stdlib_module_names', ())) | set(sys.builtin_module_names)
NEVER_IMPORTED = {'antigravity', 'this', '__hello__', '__phello__', 'idlelib', 'turtledemo'}
MOST = 500

def kind_of(value):
    if inspect.ismodule(value):
        return 'module'
    if inspect.isclass(value):
        return 'class'
    if callable(value):
        return 'function'
    return 'instance'

KEYWORDS = {k: 'keyword' for k in keyword.kwlist + list(getattr(keyword, 'softkwlist', []))}
BUILTINS = {}
for n in dir(builtins):
    if not n.startswith('_'):
        BUILTINS[n] = kind_of(getattr(builtins, n))
for n in ('__name__', '__file__', '__doc__'):
    BUILTINS[n] = 'instance'

_modules = None
def module_names(folders):
    global _modules
    if _modules is None:
        found = set(sys.builtin_module_names)
        try:
            for m in pkgutil.iter_modules():
                found.add(m.name)
        except Exception:
            pass
        _modules = found
    found = set(_modules)
    for folder in folders:
        try:
            for m in pkgutil.iter_modules([folder]):
                found.add(m.name)
        except Exception:
            pass
    return {n: 'module' for n in found}

def source_of(name, folders, path=True):
    parts = name.split('.')
    for base in list(folders) + ([p for p in sys.path if p] if path else []):
        if not os.path.isdir(base):
            continue
        p = os.path.join(base, *parts)
        if os.path.isfile(p + '.py'):
            return p + '.py'
        if os.path.isfile(os.path.join(p, '__init__.py')):
            return os.path.join(p, '__init__.py')
    return None

def read_members(path):
    with open(path, 'rb') as f:
        tree = ast.parse(f.read())
    out = {}
    def visit(body):
        for node in body:
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                out[node.name] = 'function'
            elif isinstance(node, ast.ClassDef):
                out[node.name] = 'class'
            elif isinstance(node, ast.Import):
                for a in node.names:
                    out[a.asname or a.name.split('.')[0]] = 'module'
            elif isinstance(node, ast.ImportFrom):
                for a in node.names:
                    if a.name != '*':
                        out[a.asname or a.name] = 'instance'
            elif isinstance(node, (ast.Assign, ast.AnnAssign, ast.AugAssign)):
                targets = node.targets if isinstance(node, ast.Assign) else [node.target]
                for t in targets:
                    for n in ast.walk(t):
                        if isinstance(n, ast.Name):
                            out.setdefault(n.id, 'statement')
            elif isinstance(node, (ast.If, ast.Try, ast.With)):
                visit(node.body)
                visit(getattr(node, 'orelse', []))
                visit(getattr(node, 'finalbody', []))
                for h in getattr(node, 'handlers', []):
                    visit(h.body)
    visit(tree.body)
    if os.path.basename(path) == '__init__.py':
        for m in pkgutil.iter_modules([os.path.dirname(path)]):
            out.setdefault(m.name, 'module')
    return out

_members = {}
def module_members(name, folders):
    key = (name, tuple(folders))
    if key in _members:
        return _members[key]
    out = {}
    root = name.split('.')[0]
    try:
        if root in STDLIB and root not in NEVER_IMPORTED and source_of(root, folders, path=False) is None:
            module = importlib.import_module(name)
            for n in dir(module):
                try:
                    out[n] = kind_of(getattr(module, n))
                except Exception:
                    out[n] = 'instance'
            for m in pkgutil.iter_modules(getattr(module, '__path__', None) or []):
                out.setdefault(m.name, 'module')
        else:
            path = source_of(name, folders)
            if path:
                out = read_members(path)
    except Exception:
        pass
    _members[key] = out
    return out

def imports_of(source):
    aliases = {}
    try:
        for node in ast.walk(ast.parse(source)):
            if isinstance(node, ast.Import):
                for a in node.names:
                    aliases[a.asname or a.name.split('.')[0]] = a.name if a.asname else a.name.split('.')[0]
            elif isinstance(node, ast.ImportFrom) and node.module and not node.level:
                for a in node.names:
                    aliases.setdefault(a.asname or a.name, node.module + '.' + a.name)
    except (SyntaxError, ValueError):
        for m in re.finditer(r'^[ \t]*import[ \t]+([\w.]+)(?:[ \t]+as[ \t]+(\w+))?', source, re.M):
            aliases[m.group(2) or m.group(1).split('.')[0]] = m.group(1) if m.group(2) else m.group(1).split('.')[0]
        for m in re.finditer(r'^[ \t]*from[ \t]+([\w.]+)[ \t]+import[ \t]+(\w+)(?:[ \t]+as[ \t]+(\w+))?', source, re.M):
            aliases.setdefault(m.group(3) or m.group(2), m.group(1) + '.' + m.group(2))
    return aliases

def script_names(text):
    out = {}
    for m in re.finditer(r'^[ \t]*(?:async[ \t]+)?def[ \t]+(\w+)', text, re.M):
        out[m.group(1)] = 'function'
    for m in re.finditer(r'^[ \t]*class[ \t]+(\w+)', text, re.M):
        out[m.group(1)] = 'class'
    for m in re.finditer(r'^[ \t]*import[ \t]+([\w.]+)(?:[ \t]+as[ \t]+(\w+))?', text, re.M):
        out[m.group(2) or m.group(1).split('.')[0]] = 'module'
    whole = True
    try:
        for tok in tokenize.generate_tokens(io.StringIO(text).readline):
            if tok.type == tokenize.NAME:
                out.setdefault(tok.string, 'statement')
    except (tokenize.TokenError, IndentationError, SyntaxError):
        whole = False
    if not whole:
        for m in re.finditer(r'\b[A-Za-z_]\w*', text):
            out.setdefault(m.group(0), 'statement')
    return out

def own(source, line, column, path):
    lines = source.split('\n')
    before = lines[line - 1][:column] if 0 < line <= len(lines) else ''
    word = re.search(r'\w*$', before).group(0)
    start = sum(len(l) + 1 for l in lines[:line - 1]) + column - len(word)
    rest = source[:start] + source[start + len(word):]   # the script without the word being typed
    folders = [os.path.dirname(os.path.abspath(path))] if path else []
    m = re.match(r'^\s*from\s+([\w.]+)\s+import\s+(?:.*,\s*)?\(?\s*(\w*)$', before)
    if m:
        return module_members(m.group(1), folders), m.group(2)
    m = re.match(r'^\s*(?:import|from)\s+([\w.]*)$', before)
    if m:
        package, dot, prefix = m.group(1).rpartition('.')
        if dot:
            return {k: v for k, v in module_members(package, folders).items() if v == 'module'}, prefix
        return module_names(folders), prefix
    m = re.search(r'([A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*)\.(\w*)$', before)
    if m:
        base, prefix = m.group(1), m.group(2)
        head, _, tail = base.partition('.')
        aliases = imports_of(rest)
        if head in aliases:
            found = module_members(aliases[head] + ('.' + tail if tail else ''), folders)
            if found:
                return found, prefix
        out = {}
        for n in re.finditer(r'\b' + re.escape(base) + r'\.([A-Za-z_]\w*)', rest):
            out[n.group(1)] = 'statement'
        return out, prefix
    if (word and word[0].isdigit()) or before[:len(before) - len(word)].endswith('.'):
        return {}, word   # (a number's, or an attribute of what is no name)
    names = dict(script_names(rest))
    names.update(BUILTINS)
    names.update(KEYWORDS)
    return names, word

def answer(request):
    source, line, column, path = request.get('source', ''), int(request.get('line', 1)), int(request.get('column', 0)), request.get('path') or None
    if jedi is not None:
        try:
            items = []
            for c in jedi.Script(code=source, path=path).complete(line, column):
                items.append({'name': c.name_with_symbols, 'type': c.type, 'description': c.description})
                if len(items) >= MOST:
                    break
            return ENGINE, items
        except Exception:
            pass
    names, prefix = own(source, line, column, path)
    low = prefix.lower()
    chosen = sorted((n for n in names if n.lower().startswith(low)), key=lambda n: (n.startswith('_'), n.lower(), n))
    return '', [{'name': n, 'type': names[n], 'description': ''} for n in chosen[:MOST]]

while True:
    raw = sys.stdin.readline()
    if not raw:
        break
    if not raw.strip():
        continue
    request = {}
    try:
        request = json.loads(raw)
        engine, items = answer(request)
        out = {'id': request.get('id', -1), 'engine': engine, 'items': items}
    except Exception as e:
        out = {'id': request.get('id', -1), 'engine': '', 'items': [], 'failure': str(e)}
    sys.stdout.write(json.dumps(out) + '\n')
    sys.stdout.flush()
)PY");
    return program;
}

Completions readCompletions(const QByteArray& line)
{
    Completions answer;
    const QJsonDocument doc = QJsonDocument::fromJson(line.trimmed());
    if (!doc.isObject()) return answer;
    const QJsonObject o = doc.object();
    answer.id = o.value(QStringLiteral("id")).toInt(-1);
    answer.engine = o.value(QStringLiteral("engine")).toString();
    for (const QJsonValue& v : o.value(QStringLiteral("items")).toArray()) {
        const QJsonObject item = v.toObject();
        const QString name = item.value(QStringLiteral("name")).toString();
        if (!name.isEmpty())
            answer.items.append({name, item.value(QStringLiteral("type")).toString(), item.value(QStringLiteral("description")).toString()});
    }
    return answer;
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

// A completion's icon: a letter for what it is, on a colour of its own.
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
}

PythonDoc::~PythonDoc()
{
    openScripts().remove(this);
    stopCheck();
    stopCompleter();
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
    if (!startCompleter()) return;
    a_requestBlock = cursor.blockNumber();
    a_requestStart = qucs_s::python::wordStart(before);
    const QJsonObject question{{QStringLiteral("id"), ++a_request},
                               {QStringLiteral("source"), toPlainText()},
                               {QStringLiteral("line"), cursor.blockNumber() + 1},
                               {QStringLiteral("column"), cursor.positionInBlock()},
                               {QStringLiteral("path"), getDocName().isEmpty() ? QString() : QFileInfo(getDocName()).absoluteFilePath()}};
    a_completerProcess->write(QJsonDocument(question).toJson(QJsonDocument::Compact) + '\n');
}

bool PythonDoc::startCompleter()
{
    // Running with the script's interpreter, or started again with it.
    const QString python = interpreter();
    if (a_completerProcess != nullptr && a_completerInterpreter == python) return true;
    stopCompleter();
    a_completerProcess = new QProcess(this);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
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
        const qucs_s::python::Completions answer = qucs_s::python::readCompletions(line);
        if (answer.id == a_request) showCompletions(answer);   // (an earlier one's: overtaken)
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
        auto* item = new QStandardItem(completionIcon(c.type), c.name);
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

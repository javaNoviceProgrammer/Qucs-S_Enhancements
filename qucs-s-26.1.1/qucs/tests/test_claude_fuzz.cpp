/*
 * The Claude Code panel fuzzed: a real panel driven by the program's side -
 * the stream-json messages Claude Code writes (streamed text, tool uses and
 * their results, permission requests, an MCP server's questions, its own
 * MCP messages, turn results, rate limits, its warnings), each now and then
 * mutated or garbled - and by the user's - rendering, resizing, links
 * clicked, prompts typed and sent (the dock's commands among them), images
 * attached, prompts edited, permissions answered, conversations exported,
 * restored and imported, the theme changed, the view scrolled.
 *
 * Text selected anywhere and quoted in the composer (Reply), or drawn whole
 * under the selection.
 *
 * After each step: no dialog left open (one is closed and said), a render
 * in bounded time, the newest plain reply in the transcript, no formula's
 * mark left unplaced, the view at the conversation's end while it follows
 * it; a quote lines of a quote under all the composer held, none of the
 * characters that only lay text out. A crash or a sanitizer's report ends the
 * run; the trace file (each step, flushed) and the seed replay it.
 *
 * QUCS_FUZZ_SEED (1), QUCS_FUZZ_STEPS (400 - CI's short pass) or
 * QUCS_FUZZ_SECONDS (a long run: steps without limit until then),
 * QUCS_FUZZ_TRACE (a file for the steps; else in the test's folder).
 */

#include <QtTest>
#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPointer>
#include <QRandomGenerator>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextList>
#include <QTimer>
#include <optional>
#include <QToolButton>

#include "claudecode.h"
#include "claudecodepanel.h"
#include "claudecodetabs.h"
#include "claudejobs.h"
#include "claudejobswindow.h"
#include "isolated_settings.h"

namespace {

bool slowBuild()
{
#if defined(__SANITIZE_ADDRESS__)
    return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
    return true;
#endif
#endif
#ifndef NDEBUG
    return true;
#else
    return false;
#endif
}

// Links' openings caught: a fuzzed reply's http link opened no browser.
class Opened : public QObject
{
    Q_OBJECT
public:
    QStringList urls;
public slots:
    void open(const QUrl& url) { urls << url.toString(); }
};

// What the fuzz draws from.
class Draw
{
public:
    explicit Draw(quint32 seed) : r(seed) {}
    QRandomGenerator r;
    int below(int n) { return n <= 0 ? 0 : int(r.bounded(quint32(n))); }
    bool chance(double p) { return r.generateDouble() < p; }
    template <typename T> const T& pick(const QList<T>& list) { return list.at(below(int(list.size()))); }
    QString pickText(const QStringList& list) { return list.at(below(int(list.size()))); }
};

// The tools the dock's conversations are given: answered at once, later
// (by a step of the fuzz), or after a question to the user; with a text, a
// long text, JSON, an image, an error. Some only look, some are allowed in
// auto mode, one cannot be undone, one is asked about each time.
class FuzzHost : public qucs_s::claude::ToolHost
{
public:
    explicit FuzzHost(Draw& draw, QString picture) : d(draw), a_picture(std::move(picture)) {}
    static QStringList names()
    {
        return {"look", "change", "wipe", "type", "slow", "ask", "picture", "big", "fail", "json"};
    }
    QString serverName() const override { return QStringLiteral("qucs-s"); }
    QJsonArray tools() const override
    {
        QJsonArray out;
        for (const QString& n : names())
            out.append(QJsonObject{{"name", n}, {"description", "Does " + n}, {"inputSchema", QJsonObject{{"type", "object"}}}});
        return out;
    }
    QStringList readOnlyTools() const override { return {"look", "picture", "big", "json"}; }
    QStringList autonomousTools() const override { return {"change", "slow", "ask", "fail"}; }
    QString actionOf(const QString& tool) const override { return "use " + tool; }
    QString subjectOf(const QString&, const QJsonObject& a) const override
    {
        return QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact)).left(300);
    }
    QString instructions() const override { return QStringLiteral("The fuzz's tools."); }
    bool irreversible(const QString& tool, const QJsonObject& a) const override
    {
        return tool == QLatin1String("wipe") || a.value("what").toString() == QLatin1String("wipe");
    }
    QString askedEachTime(const QString& tool, const QJsonObject& a) const override
    {
        return tool == QLatin1String("type") || a.value("what").toString() == QLatin1String("type")
                   ? QStringLiteral("What is typed runs with your rights.")
                   : QString();
    }
    QJsonObject forDocument(const QString& tool, const QJsonObject& a, const QString& document) const override
    {
        if (tool != QLatin1String("change")) return a;
        QJsonObject with = a;
        with.insert("document", document);
        return with;
    }
    void setAsker(quint64 caller, Asker asker) override
    {
        if (asker) askers.insert(caller, std::move(asker));
        else askers.remove(caller);
    }
    void callTool(const QString& tool, const QJsonObject& a, std::function<void(const QJsonObject&)> done) override
    {
        callToolFor(0, tool, a, std::move(done));
    }
    void callToolFor(quint64 caller, const QString& tool, const QJsonObject&, std::function<void(const QJsonObject&)> done) override
    {
        ++calls;
        if (tool == QLatin1String("slow") || d.chance(0.1)) {
            waiting.append({tool, std::move(done)});
            return;
        }
        if (tool == QLatin1String("ask")) {
            if (const Asker asker = askers.value(caller)) {
                ++asked;
                asker(QStringLiteral("Write over x.sch?"),
                      QJsonObject{{"type", "object"}, {"properties", QJsonObject{{"confirm", QJsonObject{{"type", "boolean"}}}}}},
                      [done](const QJsonObject& r) { done(text("asked: " + r.value("action").toString(), false)); });
                return;
            }
        }
        done(result(tool));
    }
    // A call cancelled: stopped now and then (answered at once, as a stopped
    // solve answers), else left to end in its time.
    void cancelCall(quint64, const QString& tool) override
    {
        ++cancels;
        for (qsizetype i = 0; i < waiting.size(); ++i)
            if (waiting.at(i).first == tool && d.chance(0.6)) {
                auto [name, done] = waiting.takeAt(i);
                done(text("Stopped: " + name, true));
                return;
            }
    }
    // A waiting call answered: one, or all of them.
    void answer(bool all)
    {
        while (!waiting.isEmpty()) {
            auto [tool, done] = waiting.takeAt(d.below(int(waiting.size())));
            done(result(tool));
            if (!all) return;
        }
    }
    static QJsonObject text(const QString& t, bool error)
    {
        return {{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", t}}}}, {"isError", error}};
    }
    QJsonObject result(const QString& tool)
    {
        if (tool == QLatin1String("fail") || d.chance(0.05)) return text("It failed: " + tool, true);
        if (tool == QLatin1String("big")) return text(QString(d.below(300000) + 1, QLatin1Char('b')), false);
        if (tool == QLatin1String("json"))
            return text(QStringLiteral("{\"a\": [1, 2, {\"b\": \"%1\"}], \"n\": %2}").arg(QString(d.below(2000), QLatin1Char('j'))).arg(d.below(9)), false);
        if (tool == QLatin1String("picture")) {
            QFile f(a_picture);
            const QByteArray data = d.chance(0.8) && f.open(QIODevice::ReadOnly) ? f.readAll().toBase64() : QByteArrayLiteral("bm90IGFuIGltYWdl");
            return {{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", "A picture"}},
                                           QJsonObject{{"type", "image"}, {"mimeType", "image/png"}, {"data", QString::fromLatin1(data)}}}}};
        }
        if (d.chance(0.05)) return QJsonObject{{"content", QJsonArray{}}};
        return text("Done: " + tool, false);
    }
    Draw& d;
    QHash<quint64, Asker> askers;
    QList<std::pair<QString, std::function<void(const QJsonObject&)>>> waiting;
    int calls = 0, asked = 0, cancels = 0;

private:
    QString a_picture;
};

} // namespace

class TestClaudeFuzz : public QObject
{
    Q_OBJECT

    QTemporaryDir dir;
    QStringList files;   // the images and others a step may attach or link
    int marker = 0;

    // ---- the program's side ---------------------------------------------

    QString word(Draw& d)
    {
        static const QStringList words = {
            QStringLiteral("alpha"), QStringLiteral("β-γ"), QStringLiteral("日本語"), QStringLiteral("مرحبا"), QStringLiteral("😀🚀"),
            QStringLiteral("a\u0301e\u0300"), QStringLiteral("\u202Eright-to-left\u202C"), QStringLiteral("x"), QStringLiteral("R1"),
            QStringLiteral("10kΩ"), QStringLiteral("µm"), QStringLiteral("tab\there"), QStringLiteral("cr\r\nlf"),
            QStringLiteral("zero\u200Bwidth"), QStringLiteral("&amp;"), QStringLiteral("%1%2"), QStringLiteral("\\n"),
            QStringLiteral("C:\\path\\x.sch"), QStringLiteral("~/q.sch"), QStringLiteral("ñ"), QStringLiteral("—")};
        return d.pickText(words);
    }

    QString piece(Draw& d, bool big)
    {
        const int kind = d.below(40);
        switch (kind) {
        case 0: return QStringLiteral("# Heading %1").arg(word(d));
        case 1: return QStringLiteral("## %1\n### %2").arg(word(d), word(d));
        case 2: return QStringLiteral("- one\n- %1\n  - nested %2\n    - deeper").arg(word(d), word(d));
        case 3: return QStringLiteral("1. first\n2. %1\n10. tenth").arg(word(d));
        case 4: return QStringLiteral("> a quote %1\n>> deeper").arg(word(d));
        case 5: return QStringLiteral("```cpp\nint x = %1;\n```").arg(d.below(100));
        case 6: return QStringLiteral("```\nunclosed fence %1").arg(word(d));
        case 7: return QStringLiteral("| a | b |\n|---|---|\n| %1 | %2 |").arg(word(d), word(d));
        case 8: return QStringLiteral("| broken | table\n|---\n| 1 | 2 | 3 |");
        case 9: return QStringLiteral("**bold %1** *it* `code` ~~gone~~").arg(word(d));
        case 10: return QStringLiteral("[a link](http://example.com/%1) <https://example.org>").arg(d.below(9));
        case 11: return QStringLiteral("[a file](%1)").arg(QUrl::fromLocalFile(d.pickText(files)).toString());
        case 12: return QStringLiteral("![a picture](%1)").arg(QUrl::fromLocalFile(d.pickText(files)).toString());
        case 13: return QStringLiteral("![missing](/nonexistent/%1.png)").arg(d.below(9));
        case 14: return QStringLiteral("<b>html %1</b> <i>x</i> <img src=\"/nonexistent.png\"> <script>alert(1)</script>").arg(word(d));
        case 15: return QStringLiteral("<div><table><tr><td>%1</td></tr></table></div>").arg(word(d));
        case 16: return QStringLiteral("---");
        case 17: return QStringLiteral("$x^2 + %1$ and $$\\int_0^1 f(x)\\,dx$$").arg(d.below(9));
        case 18: return QStringLiteral("$$\n\\begin{align} a &= b \\\\ c &= d \\end{align}\n$$");
        case 19: return QStringLiteral("$\\frac{1}{ unclosed");
        case 20: return QStringLiteral("$$$$ $ $ \\( a \\) \\[ b \\]");
        case 21: return QStringLiteral("\\(\\sqrt{%1}\\) and \\[\\sum_{n=1}^{\\infty} n^{-2}\\]").arg(d.below(99));
        case 22: return QString(d.below(big ? 20000 : 600) + 1, QChar(QLatin1Char('a' + d.below(26))));   // a run with no break
        case 23: return QString(d.below(60) + 1, QLatin1Char('-')).replace(QLatin1String("-"), QLatin1String("- "));   // a deep list
        case 24: return QString(d.below(60) + 1, QLatin1Char('>')) + QStringLiteral(" deep quote");
        case 25: return QString(d.below(80) + 1, QLatin1Char('{')) + QStringLiteral("$$") + QString(d.below(80) + 1, QLatin1Char('}'));
        case 26: return QStringLiteral("line\nbreak\n\n\n\npara");
        case 27: return QString(QChar(0)) + QStringLiteral("nul ") + QString(QChar(0x1F));
        case 28: return QStringLiteral("<!-- a comment %1 -->").arg(word(d));
        case 29: return QStringLiteral("    indented code %1").arg(word(d));
        case 30: return QStringLiteral("* [ ] task\n* [x] done");
        case 31: return QStringLiteral("Footnote[^1]\n\n[^1]: the note");
        case 32: return QStringLiteral("\\begin{equation} E = mc^2 \\end{equation}");
        case 33: {
            QString t;
            for (int i = 0; i < (big ? 400 : 30); ++i) t += QStringLiteral("$x_{%1}$ ").arg(i);
            return t;
        }
        case 34: {
            QString t;
            for (int i = 0; i < (big ? 300 : 20); ++i) t += QStringLiteral("Line %1 of many.\n\n").arg(i);
            return t;
        }
        default: {
            QStringList w;
            for (int i = 0, n = d.below(12) + 1; i < n; ++i) w << word(d);
            return w.join(QLatin1Char(' '));
        }
        }
    }

    // A reply's text, and whether it is plain (no markup that may hide a
    // paragraph after it: math, HTML, code).
    QString reply(Draw& d, bool big, bool* plain)
    {
        QStringList parts;
        bool safe = true;
        for (int i = 0, n = d.below(5) + 1; i < n; ++i) {
            const QString p = piece(d, big && d.chance(0.2));
            if (p.contains(QLatin1Char('$')) || p.contains(QLatin1Char('<')) || p.contains(QLatin1String("```"))
                || p.contains(QLatin1String("\\(")) || p.contains(QLatin1String("\\[")) || p.contains(QLatin1String("\\begin"))
                || p.contains(QLatin1String("    ")) || p.contains(QChar(0)))
                safe = false;
            parts << p;
        }
        if (plain != nullptr) *plain = safe;
        return parts.join(QStringLiteral("\n\n"));
    }

    QJsonValue anyValue(Draw& d, int depth = 0)
    {
        switch (d.below(depth > 2 ? 6 : 8)) {
        case 0: return QJsonValue();
        case 1: return d.chance(0.5);
        case 2: return double(d.below(2000)) - 1000;
        case 3: return d.pickText({QStringLiteral("1e308"), QStringLiteral("-0"), QStringLiteral("NaN"), QString()}).toDouble();
        case 4: return word(d);
        case 5: return piece(d, false);
        case 6: {
            QJsonArray a;
            for (int i = 0, n = d.below(4); i < n; ++i) a.append(anyValue(d, depth + 1));
            return a;
        }
        default: {
            QJsonObject o;
            for (int i = 0, n = d.below(4); i < n; ++i)
                o.insert(d.pickText({QStringLiteral("type"), QStringLiteral("text"), QStringLiteral("id"), QStringLiteral("name"),
                                     QStringLiteral("content"), QStringLiteral("input"), word(d)}),
                         anyValue(d, depth + 1));
            return o;
        }
        }
    }

    QString toolName(Draw& d)
    {
        return d.pickText({QStringLiteral("Bash"), QStringLiteral("Write"), QStringLiteral("Edit"), QStringLiteral("MultiEdit"), QStringLiteral("Read"),
                           QStringLiteral("Grep"), QStringLiteral("Glob"), QStringLiteral("Task"), QStringLiteral("Agent"), QStringLiteral("TodoWrite"),
                           QStringLiteral("WebFetch"), QStringLiteral("WebSearch"), QStringLiteral("NotebookEdit"), QStringLiteral("ExitPlanMode"),
                           QStringLiteral("BashOutput"), QStringLiteral("KillShell"), QStringLiteral("mcp__qucs-s__get_state"),
                           QStringLiteral("mcp__qucs-s__simulate"), QStringLiteral("mcp__qucs-s__fem_solve"), QStringLiteral("mcp__other__thing"),
                           QString(), word(d)});
    }

    QJsonObject toolInput(Draw& d)
    {
        QJsonObject in;
        const QStringList paths = {dir.filePath(QStringLiteral("work/a.sch")), QStringLiteral("relative/b.txt"), QStringLiteral("/"), QString(),
                                   dir.filePath(QStringLiteral("work/") + QString(300, QLatin1Char('n'))), word(d)};
        for (int i = 0, n = d.below(5); i < n; ++i) {
            switch (d.below(10)) {
            case 0: in.insert(QStringLiteral("command"), d.chance(0.8) ? QJsonValue(QStringLiteral("ls -la ") + word(d)) : anyValue(d)); break;
            case 1: in.insert(QStringLiteral("file_path"), d.chance(0.8) ? QJsonValue(d.pickText(paths)) : anyValue(d)); break;
            case 2: in.insert(QStringLiteral("content"), reply(d, false, nullptr)); break;
            case 3:
                in.insert(QStringLiteral("old_string"), word(d));
                in.insert(QStringLiteral("new_string"), reply(d, false, nullptr));
                break;
            case 4: {
                QJsonArray todos;
                for (int k = 0, m = d.below(5); k < m; ++k)
                    todos.append(QJsonObject{{QStringLiteral("content"), word(d)},
                                             {QStringLiteral("status"), d.pickText({QStringLiteral("pending"), QStringLiteral("in_progress"),
                                                                                    QStringLiteral("completed"), word(d)})}});
                in.insert(QStringLiteral("todos"), d.chance(0.9) ? QJsonValue(todos) : anyValue(d));
                break;
            }
            case 5: in.insert(QStringLiteral("pattern"), word(d)); break;
            case 6: in.insert(QStringLiteral("url"), QStringLiteral("https://example.com/") + word(d)); break;
            case 7: in.insert(QStringLiteral("description"), word(d)); break;
            case 8: in.insert(QStringLiteral("edits"), anyValue(d)); break;
            default: in.insert(word(d), anyValue(d)); break;
            }
        }
        return in;
    }

    QString toolId(Draw& d) { return d.chance(0.8) ? QStringLiteral("t%1").arg(d.below(12)) : word(d); }

    // One message of the program's, and whether a plain reply with \a token
    // at its end was among it (to be found in the transcript).
    QJsonObject message(Draw& d, bool big, QString* token)
    {
        const auto text = [this, &d, big, token] {
            bool plain = false;
            QString t = reply(d, big, &plain);
            if (plain && token != nullptr) {
                *token = QStringLiteral("QZX%1QZX").arg(++marker);
                t += QStringLiteral("\n\n") + *token;
            }
            return t;
        };
        switch (d.below(20)) {
        case 0:
        case 1: {
            QJsonObject sys{{QStringLiteral("type"), QStringLiteral("system")},
                            {QStringLiteral("subtype"), d.pickText({QStringLiteral("init"), QStringLiteral("status"), QStringLiteral("permission_denied"),
                                                                    QStringLiteral("task_started"), QStringLiteral("task_updated"), QStringLiteral("task_progress"),
                                                                    QStringLiteral("task_notification"), QStringLiteral("compact_boundary"), word(d)})},
                            {QStringLiteral("session_id"), QStringLiteral("s-%1").arg(d.below(3))},
                            {QStringLiteral("model"), d.pickText({QStringLiteral("claude-opus-5-5"), QStringLiteral("claude-haiku-5-5"), word(d)})},
                            {QStringLiteral("permissionMode"), d.pickText({QStringLiteral("default"), QStringLiteral("auto"), QStringLiteral("plan"),
                                                                           QStringLiteral("acceptEdits"), word(d)})},
                            {QStringLiteral("task_id"), toolId(d)},
                            {QStringLiteral("tool_use_id"), toolId(d)},
                            {QStringLiteral("description"), word(d)},
                            {QStringLiteral("status"), d.pickText({QStringLiteral("completed"), QStringLiteral("failed"), word(d)})},
                            {QStringLiteral("is_backgrounded"), d.chance(0.5)},
                            {QStringLiteral("message"), word(d)}};
            // (The Background Jobs' events: claudejobs.h.)
            if (d.chance(0.5))
                sys.insert(QStringLiteral("patch"), QJsonObject{{QStringLiteral("status"), d.pickText({QStringLiteral("pending"), QStringLiteral("running"),
                                                                                                       QStringLiteral("completed"), QStringLiteral("failed"),
                                                                                                       QStringLiteral("killed"), QStringLiteral("paused"), word(d)})},
                                                                {QStringLiteral("description"), d.chance(0.5) ? QJsonValue(word(d)) : anyValue(d)},
                                                                {QStringLiteral("error"), anyValue(d)}});
            if (d.chance(0.3))
                sys.insert(QStringLiteral("usage"), QJsonObject{{QStringLiteral("total_tokens"), anyValue(d)},
                                                                {QStringLiteral("tool_uses"), anyValue(d)}});
            if (d.chance(0.3)) sys.insert(QStringLiteral("summary"), d.chance(0.5) ? QJsonValue(word(d)) : anyValue(d));
            if (d.chance(0.2)) sys.insert(QStringLiteral("output_file"), anyValue(d));
            if (d.chance(0.5)) sys.insert(QStringLiteral("slash_commands"), QJsonArray{QStringLiteral("compact"), QStringLiteral("review"), word(d)});
            return sys;
        }
        case 2:
        case 3:
        case 4: {
            // A streamed reply: its start, deltas, a stop.
            QJsonObject event;
            switch (d.below(5)) {
            case 0:
                event = {{QStringLiteral("type"), QStringLiteral("content_block_start")},
                         {QStringLiteral("content_block"), QJsonObject{{QStringLiteral("type"), d.pickText({QStringLiteral("text"), QStringLiteral("tool_use"),
                                                                                                              QStringLiteral("thinking")})},
                                                                       {QStringLiteral("name"), toolName(d)}, {QStringLiteral("text"), QString()}}}};
                break;
            case 1:
            case 2:
            case 3:
                event = {{QStringLiteral("type"), QStringLiteral("content_block_delta")},
                         {QStringLiteral("delta"), QJsonObject{{QStringLiteral("type"), d.chance(0.85) ? QStringLiteral("text_delta") : QStringLiteral("input_json_delta")},
                                                               {QStringLiteral("text"), d.chance(0.7) ? word(d) + QLatin1Char(' ') : piece(d, big && d.chance(0.1))},
                                                               {QStringLiteral("partial_json"), QStringLiteral("{\"a\":")}}}};
                break;
            default: event = {{QStringLiteral("type"), d.pickText({QStringLiteral("content_block_stop"), QStringLiteral("message_stop"), word(d)})}}; break;
            }
            QJsonObject m{{QStringLiteral("type"), QStringLiteral("stream_event")}, {QStringLiteral("event"), event}};
            if (d.chance(0.05)) m.insert(QStringLiteral("parent_tool_use_id"), toolId(d));
            return m;
        }
        case 5:
        case 6:
        case 7:
        case 8: {
            QJsonArray content;
            for (int i = 0, n = d.below(3) + 1; i < n; ++i) {
                const int kind = d.below(10);
                if (kind < 5) content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), text()}});
                else if (kind < 9)
                    content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("tool_use")}, {QStringLiteral("id"), toolId(d)},
                                               {QStringLiteral("name"), toolName(d)}, {QStringLiteral("input"), toolInput(d)}});
                else content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("thinking")}, {QStringLiteral("thinking"), word(d)}});
            }
            QJsonObject m{{QStringLiteral("type"), QStringLiteral("assistant")},
                          {QStringLiteral("uuid"), QStringLiteral("u-%1").arg(d.below(1000))},
                          {QStringLiteral("message"), QJsonObject{{QStringLiteral("content"), content}}}};
            if (d.chance(0.05)) m.insert(QStringLiteral("parent_tool_use_id"), toolId(d));
            return m;
        }
        case 9:
        case 10: {
            QJsonArray content;
            for (int i = 0, n = d.below(2) + 1; i < n; ++i) {
                QJsonValue body;
                switch (d.below(4)) {
                case 0: body = reply(d, big && d.chance(0.1), nullptr); break;
                case 1: body = QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), reply(d, false, nullptr)}}}; break;
                case 2:
                    body = QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("image")},
                                                  {QStringLiteral("source"), QJsonObject{{QStringLiteral("type"), QStringLiteral("base64")},
                                                                                         {QStringLiteral("data"), QStringLiteral("iVBORw0KGgo=")}}}}};
                    break;
                default: body = anyValue(d); break;
                }
                content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("tool_result")}, {QStringLiteral("tool_use_id"), toolId(d)},
                                           {QStringLiteral("is_error"), d.chance(0.3)}, {QStringLiteral("content"), body}});
            }
            return {{QStringLiteral("type"), QStringLiteral("user")}, {QStringLiteral("uuid"), QStringLiteral("u-%1").arg(d.below(1000))},
                    {QStringLiteral("message"), QJsonObject{{QStringLiteral("content"), content}}}};
        }
        case 11:
        case 12: {
            QJsonObject request;
            switch (d.below(5)) {
            case 0:
            case 1: {
                const QString tool = toolName(d);
                request = {{QStringLiteral("subtype"), QStringLiteral("can_use_tool")}, {QStringLiteral("tool_name"), tool},
                           {QStringLiteral("input"), toolInput(d)}};
                if (d.chance(0.5))
                    request.insert(QStringLiteral("permission_suggestions"),
                                   QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("setMode")}, {QStringLiteral("mode"), QStringLiteral("acceptEdits")},
                                                          {QStringLiteral("destination"), QStringLiteral("session")}}});
                break;
            }
            case 2: {
                QJsonObject properties;
                switch (d.below(4)) {
                case 0: properties = {{QStringLiteral("ok"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}}}; break;
                case 1: properties = {{QStringLiteral("pick"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                                                           {QStringLiteral("enum"), QJsonArray{word(d), word(d), QString()}}}}}; break;
                case 2:
                    for (int i = 0, n = d.below(5); i < n; ++i)
                        properties.insert(word(d), QJsonObject{{QStringLiteral("type"), d.pickText({QStringLiteral("string"), QStringLiteral("number"),
                                                                                                    QStringLiteral("boolean"), QStringLiteral("integer")})},
                                                               {QStringLiteral("title"), word(d)}, {QStringLiteral("default"), anyValue(d)}});
                    break;
                default: properties = anyValue(d).toObject(); break;
                }
                request = {{QStringLiteral("subtype"), QStringLiteral("elicitation")}, {QStringLiteral("mcp_server_name"), QStringLiteral("qucs-s")},
                           {QStringLiteral("message"), reply(d, false, nullptr)},
                           {QStringLiteral("requested_schema"), QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}, {QStringLiteral("properties"), properties}}}};
                break;
            }
            case 3: {
                const QJsonObject rpc{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), d.below(100)},
                                      {QStringLiteral("method"), d.pickText({QStringLiteral("tools/list"), QStringLiteral("initialize"), QStringLiteral("ping"),
                                                                             QStringLiteral("tools/call"), QStringLiteral("resources/list"), word(d)})},
                                      {QStringLiteral("params"), QJsonObject{{QStringLiteral("name"), d.pickText({QStringLiteral("get_state"), QStringLiteral("list_documents"),
                                                                                                                  word(d)})},
                                                                             {QStringLiteral("arguments"), anyValue(d)}}}};
                request = {{QStringLiteral("subtype"), QStringLiteral("mcp_message")},
                           {QStringLiteral("server_name"), d.pickText({QStringLiteral("qucs-s"), word(d)})}, {QStringLiteral("message"), rpc}};
                break;
            }
            default: request = {{QStringLiteral("subtype"), word(d)}}; break;
            }
            return {{QStringLiteral("type"), QStringLiteral("control_request")}, {QStringLiteral("request_id"), QStringLiteral("r%1").arg(d.below(8))},
                    {QStringLiteral("request"), request}};
        }
        case 13:
            return {{QStringLiteral("type"), d.pickText({QStringLiteral("control_cancel_request"), QStringLiteral("control_response")})},
                    {QStringLiteral("request_id"), QStringLiteral("r%1").arg(d.below(8))},
                    {QStringLiteral("response"), QJsonObject{{QStringLiteral("subtype"), d.pickText({QStringLiteral("success"), QStringLiteral("error")})},
                                                             {QStringLiteral("request_id"), QStringLiteral("q%1").arg(d.below(8))}, {QStringLiteral("response"), anyValue(d)}}}};
        case 14:
        case 15: {
            QJsonObject usage{{QStringLiteral("input_tokens"), d.chance(0.9) ? QJsonValue(d.below(100000)) : anyValue(d)},
                              {QStringLiteral("output_tokens"), d.below(10000)},
                              {QStringLiteral("cache_read_input_tokens"), d.chance(0.9) ? QJsonValue(d.below(1000000)) : anyValue(d)},
                              {QStringLiteral("cache_creation_input_tokens"), d.below(1000)}};
            return {{QStringLiteral("type"), QStringLiteral("result")},
                    {QStringLiteral("subtype"), d.pickText({QStringLiteral("success"), QStringLiteral("error_max_turns"), QStringLiteral("error_during_execution"),
                                                            word(d)})},
                    {QStringLiteral("is_error"), d.chance(0.2)}, {QStringLiteral("result"), reply(d, false, nullptr)},
                    {QStringLiteral("duration_ms"), d.below(100000)}, {QStringLiteral("num_turns"), d.below(9)},
                    {QStringLiteral("total_cost_usd"), d.chance(0.9) ? QJsonValue(d.r.generateDouble() * 3) : anyValue(d)},
                    {QStringLiteral("usage"), usage}, {QStringLiteral("session_id"), QStringLiteral("s-%1").arg(d.below(3))},
                    {QStringLiteral("permission_denials"), d.chance(0.8) ? QJsonValue(QJsonArray{}) : anyValue(d)}};
        }
        case 16:
            return {{QStringLiteral("type"), QStringLiteral("rate_limit_event")},
                    {QStringLiteral("rate_limit_info"), QJsonObject{{QStringLiteral("status"), d.pickText({QStringLiteral("allowed"), QStringLiteral("rejected"), word(d)})},
                                                                    {QStringLiteral("resetsAt"), d.chance(0.8) ? QJsonValue(1.8e9 + d.below(100000)) : anyValue(d)}}}};
        default: return anyValue(d).toObject();
        }
    }

    // Now and then a field of it replaced, or taken out.
    void mutate(Draw& d, QJsonObject& m)
    {
        if (m.isEmpty()) return;
        const QStringList keys = m.keys();
        const QString key = d.pickText(keys);
        if (d.chance(0.3)) m.remove(key);
        else if (m.value(key).isObject() && d.chance(0.6)) {
            QJsonObject inner = m.value(key).toObject();
            mutate(d, inner);
            m.insert(key, inner);
        } else m.insert(key, anyValue(d));
    }

    // One of Claude Code's messages about the dock's own tools (FuzzHost):
    // their MCP traffic - calls, the handshake, answers to the server's
    // questions -, permission asked for one, a use of one and its result,
    // the turn's end, a request taken back. Its request ids are "fz<n>",
    // counted in what the dock writes back.
    int requests = 0;
    QJsonObject hostMessage(Draw& d)
    {
        const QString id = QStringLiteral("fz%1").arg(++requests);
        const auto tool = [this, &d] { return d.chance(0.9) ? d.pickText(FuzzHost::names()) : word(d); };
        const auto arguments = [this, &d] {
            QJsonObject a{{"what", d.pickText({"x", "wipe", "type", "overwrite", word(d)})}};
            if (d.chance(0.1)) a.insert("max_chars", d.chance(0.5) ? QJsonValue(300) : QJsonValue(word(d)));
            if (d.chance(0.1)) a.insert(word(d), anyValue(d));
            return a;
        };
        const auto mcp = [&d, &id](const QJsonObject& rpc) {
            return QJsonObject{{"type", "control_request"},
                               {"request_id", id},
                               {"request", QJsonObject{{"subtype", "mcp_message"},
                                                       {"server_name", d.chance(0.95) ? QStringLiteral("qucs-s") : QStringLiteral("other")},
                                                       {"message", rpc}}}};
        };
        switch (d.below(12)) {
        case 0:
        case 1:
        case 2:
        case 3: {
            QJsonObject params{{"name", tool()}, {"arguments", arguments()}};
            if (d.chance(0.8)) params.insert("_meta", QJsonObject{{"claudecode/toolUseId", toolId(d)}});
            return mcp({{"jsonrpc", "2.0"}, {"id", requests}, {"method", "tools/call"}, {"params", params}});
        }
        case 4: {
            QJsonObject params{{"protocolVersion", "2025-06-18"}};
            if (d.chance(0.7)) params.insert("capabilities", QJsonObject{{"elicitation", QJsonObject{}}});
            return mcp({{"jsonrpc", "2.0"}, {"id", requests}, {"method", "initialize"}, {"params", params}});
        }
        case 5:
            return mcp({{"jsonrpc", "2.0"}, {"id", requests},
                        {"method", d.pickText({"tools/list", "ping", "resources/list", "resources/templates/list"})}});
        case 6:
            // (Now and then a call of a moment ago cancelled: the turn stopped.)
            if (d.chance(0.5))
                return mcp({{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"},
                            {"params", QJsonObject{{"requestId", std::max(1, requests - d.below(4))}, {"reason", "AbortError"}}}});
            return mcp({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
        case 7: {
            // The answer to a question of the server's (qucs-ask-<n>).
            QJsonObject rpc{{"jsonrpc", "2.0"}, {"id", QStringLiteral("qucs-ask-%1").arg(d.below(40) + 1)}};
            if (d.chance(0.8))
                rpc.insert("result", QJsonObject{{"action", d.pickText({"accept", "decline", "cancel"})},
                                                 {"content", QJsonObject{{"confirm", d.chance(0.5)}}}});
            else rpc.insert("error", QJsonObject{{"code", -1}, {"message", "no"}});
            return mcp(rpc);
        }
        case 8:
            return {{"type", "control_request"},
                    {"request_id", id},
                    {"request", QJsonObject{{"subtype", "can_use_tool"}, {"tool_name", "mcp__qucs-s__" + tool()}, {"input", arguments()}}}};
        case 9:
            return {{"type", "assistant"},
                    {"message", QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "tool_use"}, {"id", toolId(d)},
                                                                               {"name", "mcp__qucs-s__" + tool()}, {"input", arguments()}}}}}}};
        case 10:
            return {{"type", "user"},
                    {"message", QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "tool_result"}, {"tool_use_id", toolId(d)},
                                                                               {"content", word(d)}}}}}}};
        default:
            if (d.chance(0.5))
                return {{"type", "result"}, {"subtype", "success"}, {"is_error", false}, {"result", word(d)},
                        {"session_id", QStringLiteral("s-%1").arg(d.below(3))}};
            return {{"type", "control_cancel_request"}, {"request_id", QStringLiteral("fz%1").arg(std::max(1, requests - d.below(5)))}};
        }
    }

    // What a paste or a drop brings: files (images among them), an image,
    // text, HTML, or several.
    QMimeData* someMime(Draw& d)
    {
        auto* mime = new QMimeData;
        if (d.chance(0.5)) {
            QList<QUrl> urls;
            for (int i = 0, k = d.below(4) + 1; i < k; ++i)
                urls << (d.chance(0.9) ? QUrl::fromLocalFile(d.pickText(files)) : QUrl(QStringLiteral("https://example.com/x.png")));
            mime->setUrls(urls);
        }
        if (d.chance(0.3)) {
            QImage image(d.below(300) + 1, d.below(300) + 1, QImage::Format_ARGB32);
            image.fill(QColor::fromRgb(d.r.generate()));
            mime->setImageData(image);
        }
        if (d.chance(0.4)) mime->setText(reply(d, false, nullptr));
        if (d.chance(0.2)) mime->setHtml(QStringLiteral("<table><tr><td>%1</td></tr></table>").arg(word(d)));
        return mime;
    }

    // What makes \a w as wide as it is at least: the deepest shown widget of
    // its that is past \a most on its own, its class, name and text.
    static QString widest(QWidget* w, int most)
    {
        const auto least = [](QWidget* x) { return std::max(x->minimumSizeHint().width(), x->minimumWidth()); };
        QWidget* child = nullptr;
        for (QWidget* c : w->findChildren<QWidget*>(Qt::FindDirectChildrenOnly)) {
            if (c->isWindow() || (c->isHidden() && qobject_cast<QStackedWidget*>(w) == nullptr)) continue;
            if (least(c) > most && (child == nullptr || least(c) > least(child))) child = c;
        }
        if (child == nullptr) return {};
        const QString inner = widest(child, most);
        if (!inner.isEmpty()) return inner;
        QString text = child->property("text").toString();
        if (text.isEmpty()) text = child->property("title").toString();
        // (None of its own as wide: its children's widths, and its layout's least.)
        if (text.isEmpty()) {
            QStringList parts;
            for (QWidget* c : child->findChildren<QWidget*>(Qt::FindDirectChildrenOnly))
                if (!c->isHidden() && !c->isWindow() && least(c) > 40)
                    parts << QStringLiteral("%1 %2 %3").arg(QString::fromLatin1(c->metaObject()->className()), c->objectName()).arg(least(c));
            if (child->layout() != nullptr) parts << QStringLiteral("layout %1").arg(child->layout()->minimumSize().width());
            text = parts.join(QStringLiteral("; "));
        }
        return QStringLiteral("%1 %2%3 (%4): %5 characters, %6")
            .arg(QString::fromLatin1(child->metaObject()->className()), child->objectName(),
                 child->isHidden() ? QStringLiteral(" (a hidden page)") : QString())
            .arg(least(child))
            .arg(text.size())
            .arg(text.left(400));
    }

    // A transcript's place on a picture, when it shows one.
    static std::optional<QPoint> pictureIn(QTextBrowser* view)
    {
        QTextDocument* doc = view->document();
        for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
            for (QTextBlock::iterator it = b.begin(); !it.atEnd(); ++it)
                if (it.fragment().charFormat().isImageFormat()) {
                    QTextCursor c(doc);
                    c.setPosition(it.fragment().position());
                    const QRect r = view->cursorRect(c);
                    return QPoint(r.right() + 3, r.center().y());
                }
        return std::nullopt;
    }

    // ---- the user's side ---------------------------------------------------

    static QStringList anchorsOf(QTextDocument* doc)
    {
        QStringList out;
        for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
            for (QTextBlock::iterator it = b.begin(); !it.atEnd(); ++it) {
                const QString href = it.fragment().charFormat().anchorHref();
                if (!href.isEmpty() && !out.contains(href)) out << href;
            }
        return out;
    }

    // Some of the transcript selected, anywhere, as a drag or Select All
    // leaves it.
    static void selectSome(Draw& d, QTextBrowser* view)
    {
        QTextDocument* doc = view->document();
        const int end = doc->characterCount() - 1;
        const auto place = [&d, end] { return d.chance(0.15) ? (d.chance(0.5) ? 0 : end) : d.below(end + 1); };
        QTextCursor c(doc);
        c.setPosition(place());
        c.setPosition(place(), QTextCursor::KeepAnchor);
        view->setTextCursor(c);
    }

    // What is wrong with the composer after a Reply (\a before: its text until
    // then): all that kept, the rest lines of a quote and a blank line, none
    // of the characters that only lay text out or stand for something else.
    static QString quoteTrouble(const QString& before, const QString& after)
    {
        if (!after.startsWith(before)) return QStringLiteral("a reply did not keep the draft \"%1\"").arg(before.left(60));
        const QString added = after.mid(before.size());
        if (!added.endsWith(QLatin1String("\n\n"))) return QStringLiteral("a reply's quote has no line under it: \"%1\"").arg(added.right(60));
        QStringList lines = added.chopped(2).split(QLatin1Char('\n'));
        while (!lines.isEmpty() && lines.constFirst().isEmpty()) lines.removeFirst();   // (apart from the draft)
        if (lines.isEmpty()) return QStringLiteral("a reply quoted nothing");
        for (const QString& line : std::as_const(lines))
            if (line != QLatin1String(">") && !line.startsWith(QLatin1String("> "))) return QStringLiteral("a reply's line not quoted: \"%1\"").arg(line.left(60));
        for (const QChar c : added)
            if (c == QChar::ParagraphSeparator || c == QChar::LineSeparator || c == QChar::ObjectReplacementCharacter || c.unicode() == 0x200B
                || c.unicode() == 0xFDD0 || c.unicode() == 0xFDD1 || c.unicode() == 0x258D || (c.unicode() >= 0xE000 && c.unicode() < 0xE000 + 0x1800))
                return QStringLiteral("a reply's quote holds U+%1").arg(uint(c.unicode()), 4, 16, QLatin1Char('0'));
        return {};
    }

    // The transcript as drawn: each block - its frame, list, formats,
    // fragments - but the document's own numbers for its lists and frames.
    static QString drawn(QTextDocument* doc)
    {
        const auto props = [](const QTextFormat& f) {
            QMap<int, QVariant> p = f.properties();
            p.remove(QTextFormat::ObjectIndex);
            // (An image's own size, kept on its format once fitted: kept or
            // not by whether it was fitted yet - markdowndoc.cpp's kOwnWidth.)
            for (int k = 0; k < 4; ++k) p.remove(QTextFormat::UserProperty + 0x4D44 + k);
            return p;
        };
        QString out;
        QDebug dbg(&out);
        for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
            QTextFrame* frame = QTextCursor(b).currentFrame();
            int depth = 0;
            for (QTextFrame* f = frame; f != nullptr; f = f->parentFrame()) ++depth;
            dbg << "\nblock" << depth << props(frame->frameFormat()) << props(b.blockFormat()) << props(b.charFormat());
            if (QTextList* list = b.textList()) dbg << "list" << props(list->format()) << list->itemNumber(b) << list->count();
            for (QTextBlock::iterator it = b.begin(); !it.atEnd(); ++it) dbg << "\n  " << it.fragment().text() << props(it.fragment().charFormat());
        }
        return out;
    }

    // Where two transcripts as drawn() first differ: the line, the one
    // before it, each side about the place.
    static QString difference(const QString& part, const QString& whole)
    {
        const QStringList a = part.split(QLatin1Char('\n')), b = whole.split(QLatin1Char('\n'));
        qsizetype i = 0;
        while (i < a.size() && i < b.size() && a.at(i) == b.at(i)) ++i;
        const QString x = a.value(i), y = b.value(i);
        qsizetype k = 0;
        while (k < x.size() && k < y.size() && x.at(k) == y.at(k)) ++k;
        const qsizetype at = std::max<qsizetype>(0, k - 120);
        return QStringLiteral("line %1 (%2):\n  %3\n  %4").arg(i).arg(a.value(i - 1).left(160), x.mid(at, 300), y.mid(at, 300));
    }

    static QList<QToolButton*> shownButtons(QWidget* in)
    {
        QList<QToolButton*> out;
        if (in == nullptr || !in->isVisible()) return out;
        for (QToolButton* b : in->findChildren<QToolButton*>())
            if (b->isVisible() && b->isEnabled()) out << b;
        return out;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir.isValid());
        useIsolatedSettings(dir.filePath(QStringLiteral("settings")));
        qputenv("QUCS_FAKE_DIR", dir.path().toUtf8());
        qputenv("QUCS_CLAUDE", "/nonexistent/claude");
        QDir().mkpath(dir.filePath(QStringLiteral("work")));
        // Files a step attaches or a reply shows: images of several kinds,
        // a broken one, a text file, a folder, one not there.
        QImage small(64, 48, QImage::Format_RGB32);
        small.fill(Qt::darkGreen);
        QImage tall(40, 3000, QImage::Format_ARGB32);
        tall.fill(QColor(200, 30, 30, 128));
        QImage wide(2400, 30, QImage::Format_Grayscale8);
        wide.fill(Qt::gray);
        const QList<std::pair<QString, QImage>> images = {{QStringLiteral("small.png"), small}, {QStringLiteral("tall.png"), tall},
                                                          {QStringLiteral("wide.jpg"), wide}};
        for (const auto& [name, image] : images) {
            QVERIFY(image.save(dir.filePath(name)));
            files << dir.filePath(name);
        }
        QFile broken(dir.filePath(QStringLiteral("broken.png")));
        QVERIFY(broken.open(QIODevice::WriteOnly));
        broken.write("\x89PNG\r\n\x1a\nnot really");
        broken.close();
        QFile text(dir.filePath(QStringLiteral("notes.txt")));
        QVERIFY(text.open(QIODevice::WriteOnly));
        text.write("plain text\n");
        text.close();
        files << broken.fileName() << text.fileName() << dir.filePath(QStringLiteral("work")) << dir.filePath(QStringLiteral("absent.png"));
    }

    void fuzzThePanel()
    {
        const quint32 seed = quint32(qEnvironmentVariableIntValue("QUCS_FUZZ_SEED")) + (qEnvironmentVariableIsSet("QUCS_FUZZ_SEED") ? 0 : 1);
        const int seconds = qEnvironmentVariableIntValue("QUCS_FUZZ_SECONDS");
        const int steps = seconds > 0 ? std::numeric_limits<int>::max()
                                      : (qEnvironmentVariableIsSet("QUCS_FUZZ_STEPS") ? qEnvironmentVariableIntValue("QUCS_FUZZ_STEPS") : 400);
        const bool big = seconds > 0;   // (a long run: big texts too)
        Draw d(seed);
        QFile trace(qEnvironmentVariableIsSet("QUCS_FUZZ_TRACE") ? qEnvironmentVariable("QUCS_FUZZ_TRACE") : dir.filePath(QStringLiteral("trace.txt")));
        QVERIFY(trace.open(QIODevice::WriteOnly | QIODevice::Truncate));
        auto step = [&trace](int n, const QString& what) {
            trace.write(QStringLiteral("%1 %2\n").arg(n).arg(what.left(300).replace(QLatin1Char('\n'), QLatin1String("\\n"))).toUtf8());
            trace.flush();
        };
        Opened opened;
        for (const char* scheme : {"http", "https", "mailto", "file"}) QDesktopServices::setUrlHandler(QString::fromLatin1(scheme), &opened, "open");
        // A dialog or a menu left open: closed, said.
        QStringList dialogs;
        QTimer watch;
        watch.setInterval(20);
        connect(&watch, &QTimer::timeout, this, [&dialogs] {
            if (QWidget* modal = QApplication::activeModalWidget()) {
                dialogs << modal->windowTitle() + QLatin1Char(':') + QString::fromLatin1(modal->metaObject()->className());
                if (auto* dialog = qobject_cast<QDialog*>(modal)) dialog->reject();
                else modal->close();
            }
            if (QWidget* popup = QApplication::activePopupWidget()) popup->close();
        });
        watch.start();

        auto panel = std::make_unique<ClaudeCodePanel>();
        panel->setDefaultDirectory(dir.filePath(QStringLiteral("work")));
        panel->resize(440, 700);
        panel->show();
        QVERIFY(QTest::qWaitForWindowExposed(panel.get()));
        const QStringList programs = {QStringLiteral("/nonexistent/claude"), QStringLiteral("/bin/false"), QStringLiteral("/bin/cat")};

        QStringList findings;
        auto found = [&findings, &step](int n, const QString& what) {
            if (findings.size() < 200) findings << QStringLiteral("step %1: %2").arg(n).arg(what);
            step(n, QStringLiteral("FINDING ") + what);
        };
        QElapsedTimer clock;
        clock.start();
        qint64 slowest = 0;
        QString pendingToken;   // a plain reply's, to be found once rendered
        int lines = 0;
        for (int n = 0; n < steps; ++n) {
            if (seconds > 0 && clock.elapsed() > qint64(seconds) * 1000) break;
            // (Not past QtTest's 2000 messages, which hide the rest.)
            if (n % (seconds > 0 ? 2000 : 100) == 0) qInfo("step %d, %lld s, %d lines fed, %lld findings", n, clock.elapsed() / 1000, lines, qint64(findings.size()));
            const int action = d.below(100);
            QScrollBar* bar = panel->transcript()->verticalScrollBar();
            if (action < 45) {
                // The program's side: one message, or a garbled line.
                QString token;
                QJsonObject m = message(d, big, &token);
                QByteArray line;
                if (d.chance(0.08)) mutate(d, m);
                line = QJsonDocument(m).toJson(QJsonDocument::Compact);
                if (d.chance(0.03)) line = line.left(d.below(int(line.size())));                // cut short
                if (d.chance(0.02)) line = "Warning: " + piece(d, false).toUtf8();             // not the protocol
                if (d.chance(0.01)) line = QByteArray(d.below(64) + 1, char(d.below(256)));   // noise
                step(n, QStringLiteral("line %1").arg(QString::fromUtf8(line)));
                panel->session()->handleLine(line);
                ++lines;
                // (Expected where the line as fed is still a reply of
                // Claude's own with that text.)
                if (!token.isEmpty()) {
                    const QJsonObject fed = QJsonDocument::fromJson(line).object();
                    bool reply = fed.value(QLatin1String("type")).toString() == QLatin1String("assistant")
                                 && fed.value(QLatin1String("parent_tool_use_id")).toString().isEmpty();
                    bool has = false;
                    for (const QJsonValue& v : fed.value(QLatin1String("message")).toObject().value(QLatin1String("content")).toArray())
                        has = has || (v.toObject().value(QLatin1String("type")).toString() == QLatin1String("text")
                                      && v.toObject().value(QLatin1String("text")).toString().contains(token));
                    if (reply && has) {
                        pendingToken = token;
                        step(n, QStringLiteral("expect ") + token);
                    }
                }
            } else if (action < 60) {
                step(n, QStringLiteral("render"));
                // Where the reader is: a view above the end stays where it
                // is while what is drawn again is below its top.
                const bool following = panel->followsTheEnd();
                const int before = bar->value();
                const int top = panel->transcript()->cursorForPosition(QPoint(0, 0)).position();
                QElapsedTimer t;
                t.start();
                panel->renderNow();
                const qint64 ms = t.elapsed();
                if (!following && top < panel->renderedAt() && before <= bar->maximum() && bar->value() != before)
                    found(n, QStringLiteral("scrolled up, the view moved from %1 to %2 of %3 (drawn from %4, its top at %5)")
                                 .arg(before).arg(bar->value()).arg(bar->maximum()).arg(panel->renderedAt()).arg(top));
                // Drawn in part, drawn whole: the same transcript, and what
                // is selected still so.
                if (d.chance(0.3)) {
                    const QString part = drawn(panel->transcript()->document());
                    const qsizetype from = panel->renderedFrom();
                    selectSome(d, panel->transcript());
                    const QString selected = panel->transcript()->textCursor().selectedText();
                    panel->renderNow(true);
                    if (panel->transcript()->textCursor().selectedText() != selected)
                        found(n, QStringLiteral("drawn whole, the selection went from \"%1\" to \"%2\"").arg(selected.left(60), panel->transcript()->textCursor().selectedText().left(60)));
                    const QString whole = drawn(panel->transcript()->document());
                    if (part != whole) {
                        if (qEnvironmentVariableIsSet("QUCS_FUZZ_DUMPS")) {
                            QFile fa(qEnvironmentVariable("QUCS_FUZZ_DUMPS") + QStringLiteral("/part%1.txt").arg(n));
                            QFile fb(qEnvironmentVariable("QUCS_FUZZ_DUMPS") + QStringLiteral("/whole%1.txt").arg(n));
                            if (fa.open(QIODevice::WriteOnly)) fa.write(part.toUtf8());
                            if (fb.open(QIODevice::WriteOnly)) fb.write(whole.toUtf8());
                        }
                        found(n, QStringLiteral("drawn in part (from part %1) not as drawn whole, %2").arg(from).arg(difference(part, whole)));
                    }
                }
                slowest = std::max(slowest, ms);
                if (!slowBuild() && ms > 1500) found(n, QStringLiteral("a render took %1 ms (%2 characters)").arg(ms).arg(panel->transcriptText().size()));
                QTest::qWait(d.below(15));
                const QString shown = panel->transcriptText();
                if (!pendingToken.isEmpty()) {
                    if (!shown.contains(pendingToken)) found(n, QStringLiteral("a plain reply's end, %1, not in the transcript").arg(pendingToken));
                    pendingToken.clear();
                }
                for (QChar c : shown)
                    if (c.unicode() >= 0xE000 && c.unicode() < 0xE000 + 0x1800) {
                        found(n, QStringLiteral("a formula's mark left in the transcript (U+%1)").arg(uint(c.unicode()), 4, 16, QLatin1Char('0')));
                        break;
                    }
                // Following: at the end once something is drawn, near it
                // (as the panel has it: 16 pixels) else.
                const bool drew = panel->renderedAt() < panel->transcript()->document()->characterCount() - 1;
                if (panel->followsTheEnd() && bar->value() < bar->maximum() - (drew ? 2 : 16))
                    found(n, QStringLiteral("not at the conversation's end: %1 of %2").arg(bar->value()).arg(bar->maximum()));
            } else if (action < 64) {
                const QSize size(d.below(1100) + 100, d.below(900) + 100);
                step(n, QStringLiteral("resize %1x%2").arg(size.width()).arg(size.height()));
                panel->resize(size);
                QTest::qWait(1);
            } else if (action < 70) {
                const QStringList anchors = anchorsOf(panel->transcript()->document());
                if (anchors.isEmpty()) continue;
                const QString href = d.pickText(anchors);
                step(n, QStringLiteral("click %1").arg(href));
                if (href.startsWith(QLatin1String("edit:"))) pendingToken.clear();   // (back to before that prompt)
                emit panel->transcript()->anchorClicked(QUrl(href));
            } else if (action < 76) {
                // A prompt typed and sent: words, a command of the dock's or
                // the program's, an unknown one.
                QString text;
                const QList<ClaudeCodePanel::Command> commands = panel->commands();
                if (d.chance(0.4) && !commands.isEmpty()) {
                    const ClaudeCodePanel::Command& c = d.pick(commands);
                    // (Not /exit nor /clear's question, the dialogs' own: the watch closes them.)
                    text = QLatin1Char('/') + c.name + (d.chance(0.5) ? QLatin1Char(' ') + word(d) : QString());
                } else if (d.chance(0.1)) {
                    text = QLatin1Char('/') + word(d);
                } else {
                    text = reply(d, false, nullptr);
                }
                panel->session()->setProgram(d.pickText(programs));
                step(n, QStringLiteral("send %1").arg(text));
                if (text.startsWith(QLatin1Char('/'))) pendingToken.clear();   // (/new, /clear: another conversation)
                panel->composer()->setPlainText(text);
                if (d.chance(0.2)) {
                    // Through the command list: Tab, Enter.
                    QTest::keyClick(panel->composer(), d.chance(0.5) ? Qt::Key_Tab : Qt::Key_Down);
                    QTest::keyClick(panel->composer(), Qt::Key_Return);
                } else {
                    panel->sendComposer();
                }
            } else if (action < 79) {
                QStringList some;
                for (int i = 0, k = d.below(4) + 1; i < k; ++i) some << d.pickText(files);
                step(n, QStringLiteral("attach %1").arg(some.join(QLatin1String(", "))));
                panel->attachImageFiles(some);
                if (d.chance(0.4)) {
                    const qsizetype i = qsizetype(d.below(5)) - 1;
                    step(n, QStringLiteral("remove image %1").arg(i));
                    panel->removeAttachedImage(i);
                }
            } else if (action < 82) {
                const qsizetype i = qsizetype(d.below(20)) - 2;
                step(n, QStringLiteral("edit %1").arg(i));
                if (panel->editPrompt(i)) pendingToken.clear();   // (the conversation back to before it)
                if (d.chance(0.5)) {
                    step(n, QStringLiteral("cancel the edit"));
                    panel->cancelEdit();
                }
            } else if (action < 87) {
                // A permission or a question answered: one of the shown
                // cards' buttons, a field filled first.
                QList<QToolButton*> buttons = shownButtons(panel->permissionCard()) + shownButtons(panel->askCard());
                if (buttons.isEmpty()) continue;
                for (QLineEdit* e : panel->askCard()->findChildren<QLineEdit*>()) e->setText(word(d));
                for (QCheckBox* b : panel->askCard()->findChildren<QCheckBox*>()) b->setChecked(d.chance(0.5));
                QToolButton* b = buttons.at(d.below(int(buttons.size())));
                step(n, QStringLiteral("press %1 (%2)").arg(b->objectName(), b->text()));
                b->click();
            } else if (action < 89) {
                // Saved and read back, now and then changed between.
                QJsonObject saved = panel->conversationJson();
                if (d.chance(0.4)) mutate(d, saved);
                step(n, QStringLiteral("restore %1 bytes").arg(QJsonDocument(saved).toJson(QJsonDocument::Compact).size()));
                panel->restoreConversation(saved);
                pendingToken.clear();
            } else if (action < 90) {
                // A session of Claude Code's own read in: some lines of it.
                QFile f(dir.filePath(QStringLiteral("session.jsonl")));
                QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
                for (int i = 0, k = d.below(12); i < k; ++i) {
                    QJsonObject m = message(d, false, nullptr);
                    if (d.chance(0.5)) m.insert(QStringLiteral("type"), d.pickText({QStringLiteral("user"), QStringLiteral("assistant")}));
                    if (d.chance(0.3)) mutate(d, m);
                    f.write(QJsonDocument(m).toJson(QJsonDocument::Compact) + "\n");
                }
                f.close();
                step(n, QStringLiteral("import a session"));
                panel->importClaudeSession(f.fileName());
                pendingToken.clear();
            } else if (action < 92) {
                const auto format = ClaudeCodePanel::ExportFormat(d.below(3));
                step(n, QStringLiteral("export %1").arg(int(format)));
                QString why;
                panel->exportConversation(dir.filePath(QStringLiteral("export.out")), format, &why);
            } else if (action < 93) {
                selectSome(d, panel->transcript());
                const QPoint at(d.below(panel->transcript()->viewport()->width() + 1), d.below(panel->transcript()->viewport()->height() + 1));
                const std::unique_ptr<QMenu> menu = panel->transcriptMenu(at);
                auto* item = menu != nullptr ? menu->findChild<QAction*>(QStringLiteral("claudeReply")) : nullptr;
                step(n, QStringLiteral("reply to %1 characters (%2)").arg(panel->transcript()->textCursor().selectedText().size()).arg(item != nullptr));
                if (item != nullptr) {
                    if (d.chance(0.3)) panel->composer()->setPlainText(word(d));
                    const QString draft = panel->composer()->toPlainText();
                    item->trigger();
                    const QString trouble = quoteTrouble(draft, panel->composer()->toPlainText());
                    if (!trouble.isEmpty()) found(n, trouble);
                    if (d.chance(0.5)) panel->composer()->clear();
                }
            } else if (action < 94) {
                step(n, QStringLiteral("scroll"));
                bar->setValue(d.below(bar->maximum() + 1));
            } else if (action < 95) {
                step(n, QStringLiteral("stop"));
                panel->stopTurn();
            } else if (action < 96) {
                step(n, QStringLiteral("to the background"));
                panel->sendToBackground();
            } else if (action < 97) {
                step(n, QStringLiteral("a theme"));
                QPalette p = QApplication::palette();
                p.setColor(QPalette::Window, d.chance(0.5) ? QColor(30, 30, 30) : QColor(240, 240, 240));
                p.setColor(QPalette::Base, p.color(QPalette::Window));
                p.setColor(QPalette::Text, d.chance(0.5) ? Qt::white : Qt::black);
                QApplication::setPalette(p);
            } else if (action < 98) {
                step(n, QStringLiteral("name"));
                panel->setName(word(d));
            } else {
                step(n, QStringLiteral("new conversation"));
                panel->newConversation();
                pendingToken.clear();
            }
            QCoreApplication::processEvents();
            // (Kept to a size that renders in moderate time: the long run's
            // conversation started afresh now and then.)
            if (n % 300 == 299) {
                step(n, QStringLiteral("new conversation (size)"));
                panel->newConversation();
                pendingToken.clear();
            }
        }
        watch.stop();
        qInfo("%d lines fed; the slowest render %lld ms; %lld dialogs closed: %s; %lld links opened", lines, slowest, qint64(dialogs.size()),
              qPrintable(QStringList(dialogs.mid(0, 5)).join(QLatin1String("; "))), qint64(opened.urls.size()));
        for (const QString& f : findings) qWarning("%s", qPrintable(f));
        for (const char* scheme : {"http", "https", "mailto", "file"}) QDesktopServices::unsetUrlHandler(QString::fromLatin1(scheme));
        QVERIFY2(findings.isEmpty(), qPrintable(QStringLiteral("%1 findings, the first: %2").arg(findings.size()).arg(findings.value(0))));
    }

    // The whole dock fuzzed: its tabs of conversations with Qucs-S's tools
    // (FuzzHost) and a program that is running - what the dock writes to it
    // kept in a file. The program's side as above, and Claude Code's MCP
    // traffic for the dock's tools (hostMessage()): calls answered at once,
    // later or after a question, held for the dock's own question, sent to
    // the background. The user's side as above, and tabs opened, closed,
    // shown, renamed (through the editor) and through their menu, the
    // conversations kept and brought back, /resume's list, every menu's
    // actions (modes, models, the guards, pins, usage, exports), the
    // transcript's own menu (Open in Viewer, Reply), the composer's keys, pastes
    // and drops of files, images, text, the document in front and what the
    // pen drew on it, a document renamed, the dock hidden, its font.
    //
    // After each step: the tabs as many as their conversations, no tab's
    // title over a line, the dock's least width bounded (a long text never
    // widens the window); at a render, what fuzzThePanel checks of the
    // conversation in front. At the end: each of the program's requests
    // answered once at most, and every line the dock wrote JSON.
    void fuzzTheDock()
    {
        const quint32 seed = quint32(qEnvironmentVariableIntValue("QUCS_FUZZ_SEED")) + (qEnvironmentVariableIsSet("QUCS_FUZZ_SEED") ? 0 : 1);
        const int seconds = qEnvironmentVariableIntValue("QUCS_FUZZ_SECONDS");
        const int steps = seconds > 0 ? std::numeric_limits<int>::max()
                                      : (qEnvironmentVariableIsSet("QUCS_FUZZ_STEPS") ? qEnvironmentVariableIntValue("QUCS_FUZZ_STEPS") : 400);
        Draw d(seed + 7919);
        QFile trace(qEnvironmentVariableIsSet("QUCS_FUZZ_TRACE") ? qEnvironmentVariable("QUCS_FUZZ_TRACE") + QStringLiteral(".dock")
                                                                 : dir.filePath(QStringLiteral("trace-dock.txt")));
        QVERIFY(trace.open(QIODevice::WriteOnly | QIODevice::Truncate));
        auto step = [&trace](int n, const QString& what) {
            trace.write(QStringLiteral("%1 %2\n").arg(n).arg(what.left(300).replace(QLatin1Char('\n'), QLatin1String("\\n"))).toUtf8());
            trace.flush();
        };
        QStringList findings;
        auto found = [&findings, &step](int n, const QString& what) {
            if (findings.size() < 200) findings << QStringLiteral("step %1: %2").arg(n).arg(what);
            step(n, QStringLiteral("FINDING ") + what);
        };
        Opened opened;
        for (const char* scheme : {"http", "https", "mailto", "file"}) QDesktopServices::setUrlHandler(QString::fromLatin1(scheme), &opened, "open");
        QStringList dialogs;
        QTimer watch;
        watch.setInterval(20);
        connect(&watch, &QTimer::timeout, this, [&dialogs] {
            if (QWidget* modal = QApplication::activeModalWidget()) {
                dialogs << modal->windowTitle() + QLatin1Char(':') + QString::fromLatin1(modal->metaObject()->className());
                if (auto* dialog = qobject_cast<QDialog*>(modal)) dialog->reject();
                else modal->close();
            }
            if (QWidget* popup = QApplication::activePopupWidget()) popup->close();
        });
        watch.start();

        // A program that runs and keeps what the dock writes to it (each one
        // in a file of its own: written-<its pid>.jsonl).
        for (const QString& old : QDir(dir.path()).entryList({QStringLiteral("written-*.jsonl")}, QDir::Files)) QFile::remove(dir.filePath(old));
        const QString alive = dir.filePath(QStringLiteral("alive.sh"));
        {
            QFile f(alive);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write("#!/bin/sh\nexec cat > \"$QUCS_FAKE_DIR/written-$$.jsonl\"\n");
            f.close();
            QVERIFY(f.setPermissions(f.permissions() | QFileDevice::ExeOwner));
        }
        const QStringList programs = {alive, alive, alive, QStringLiteral("/nonexistent/claude"), QStringLiteral("/bin/false")};
        // Documents to have in front, open, pinned, renamed.
        QStringList documents;
        for (const QString& name : {QStringLiteral("a.sch"), QStringLiteral("b.sch"), QStringLiteral("model.qfem"), QStringLiteral("notes.md"),
                                    QStringLiteral("ünï cødé.sch")}) {
            QFile f(dir.filePath(QStringLiteral("work/") + name));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("x\n");
            documents << f.fileName();
        }
        documents << QString() << dir.filePath(QStringLiteral("work/gone.sch"));
        QString front = documents.first();
        QStringList schematics = documents.mid(0, 2), others = documents.mid(2, 2);
        quint64 drawingRevision = 0;
        QString drawing;

        FuzzHost host(d, files.first());
        auto tabs = std::make_unique<ClaudeCodeTabs>();
        tabs->setDefaultDirectory(dir.filePath(QStringLiteral("work")));
        tabs->setToolHost(&host);
        tabs->setDocumentProvider([&front] { return front; });
        tabs->setSchematicsProvider([&schematics] { return schematics; });
        tabs->setFilesProvider([&others] { return others; });
        tabs->setDrawingsProvider([&drawingRevision, &drawing](const QString&, quint64) { return std::make_pair(drawingRevision, drawing); });
        tabs->resize(440, 700);
        tabs->show();
        QVERIFY(QTest::qWaitForWindowExposed(tabs.get()));
        if (tabs->count() == 0) tabs->newConversation();
        const int leastWidth = tabs->minimumSizeHint().width();
        const qreal charWidth = QFontMetricsF(QApplication::font()).averageCharWidth();

        QElapsedTimer clock;
        clock.start();
        qint64 slowest = 0;
        QString pendingToken;   // a plain reply's, to be found in its conversation once rendered
        QPointer<ClaudeCodePanel> tokenOf;
        int lines = 0;
        for (int n = 0; n < steps; ++n) {
            if (seconds > 0 && clock.elapsed() > qint64(seconds) * 1000) break;
            if (n % (seconds > 0 ? 2000 : 100) == 0)
                qInfo("step %d, %lld s, %d lines fed, %d tabs, %d tool calls, %lld findings", n, clock.elapsed() / 1000, lines, tabs->count(),
                      host.calls, qint64(findings.size()));
            if (tabs->count() == 0) {
                step(n, QStringLiteral("(a tab: none was left)"));
                tabs->newConversation();
            }
            const QList<ClaudeCodePanel*> panels = tabs->panels();
            ClaudeCodePanel* panel = tabs->current();
            if (panel == nullptr) {
                found(n, QStringLiteral("%1 tabs and none in front").arg(tabs->count()));
                break;
            }
            // (QUCS_FUZZ_DUMP=<step>: the panel in front as it is then.)
            if (n == qEnvironmentVariableIntValue("QUCS_FUZZ_DUMP") && n > 0) {
                QPlainTextEdit* box = panel->composer();
                qInfo("step %d: composer %dx%d visible %d viewport %dx%d, font %s, %lld blocks, editing %d, commands shown %d, text %s", n,
                      box->width(), box->height(), int(box->isVisible()), box->viewport()->width(), box->viewport()->height(),
                      qPrintable(box->font().toString()), qint64(box->document()->blockCount()), int(panel->isEditing()),
                      int(panel->commandList()->isVisible()), QJsonDocument(QJsonArray{box->toPlainText()}).toJson(QJsonDocument::Compact).constData());
            }
            // The Background Jobs window over it all: opened, a job stopped, those
            // over cleared; it lists what the list holds.
            if (d.chance(0.03)) {
                using qucs_s::claude::jobs::Registry;
                step(n, QStringLiteral("the jobs window"));
                ClaudeJobsWindow* jobs = ClaudeJobsWindow::showWindow(panel);
                const QList<qucs_s::claude::jobs::Job> listed = Registry::instance().jobs();
                if (!listed.isEmpty() && d.chance(0.5)) Registry::instance().stop(listed.at(d.below(int(listed.size()))).id);
                if (d.chance(0.2)) Registry::instance().clearFinished();
                if (jobs->table()->rowCount() != Registry::instance().count())
                    found(n, QStringLiteral("the jobs window shows %1 rows of %2 jobs").arg(jobs->table()->rowCount()).arg(Registry::instance().count()));
                if (d.chance(0.5)) jobs->close();
            }
            const int action = d.below(100);
            if (action < 38) {
                // A line of the program's: to the conversation in front, now
                // and then another.
                ClaudeCodePanel* to = d.chance(0.85) ? panel : panels.at(d.below(int(panels.size())));
                QString token;
                QJsonObject m = d.chance(0.5) ? hostMessage(d) : message(d, false, &token);
                if (d.chance(0.06)) mutate(d, m);
                QByteArray line = QJsonDocument(m).toJson(QJsonDocument::Compact);
                if (d.chance(0.02)) line = line.left(d.below(int(line.size())));
                step(n, QStringLiteral("line to %1: %2").arg(panels.indexOf(to)).arg(QString::fromUtf8(line)));
                to->session()->handleLine(line);
                ++lines;
                if (to == panel && !token.isEmpty()) {
                    const QJsonObject fed = QJsonDocument::fromJson(line).object();
                    bool has = false;
                    for (const QJsonValue& v : fed.value(QLatin1String("message")).toObject().value(QLatin1String("content")).toArray())
                        has = has || v.toObject().value(QLatin1String("text")).toString().contains(token);
                    if (fed.value(QLatin1String("type")).toString() == QLatin1String("assistant")
                        && fed.value(QLatin1String("parent_tool_use_id")).toString().isEmpty() && has)
                        pendingToken = token;
                        tokenOf = panel;
                }
            } else if (action < 48) {
                step(n, QStringLiteral("render"));
                QScrollBar* bar = panel->transcript()->verticalScrollBar();
                QElapsedTimer t;
                t.start();
                panel->renderNow();
                const qint64 ms = t.elapsed();
                slowest = std::max(slowest, ms);
                if (!slowBuild() && ms > 1500) found(n, QStringLiteral("a render took %1 ms").arg(ms));
                if (d.chance(0.3)) {
                    const QString part = drawn(panel->transcript()->document());
                    const qsizetype from = panel->renderedFrom();
                    panel->renderNow(true);
                    const QString whole = drawn(panel->transcript()->document());
                    if (part != whole)
                        found(n, QStringLiteral("drawn in part (from part %1) not as drawn whole, %2").arg(from).arg(difference(part, whole)));
                }
                QTest::qWait(d.below(10));
                const QString shown = panel->transcriptText();
                if (!pendingToken.isEmpty() && tokenOf == panel && !shown.contains(pendingToken))
                    found(n, QStringLiteral("a plain reply's end, %1, not in the transcript").arg(pendingToken));
                pendingToken.clear();
                for (QChar c : shown)
                    if (c.unicode() >= 0xE000 && c.unicode() < 0xE000 + 0x1800) {
                        found(n, QStringLiteral("a formula's mark left in the transcript"));
                        break;
                    }
                const bool drew = panel->renderedAt() < panel->transcript()->document()->characterCount() - 1;
                if (panel->followsTheEnd() && bar->value() < bar->maximum() - (drew ? 2 : 16))
                    found(n, QStringLiteral("not at the conversation's end: %1 of %2").arg(bar->value()).arg(bar->maximum()));
            } else if (action < 51) {
                const QSize size(d.chance(0.3) ? d.below(160) + 40 : d.below(1100) + 100, d.below(900) + 100);
                step(n, QStringLiteral("resize %1x%2").arg(size.width()).arg(size.height()));
                tabs->resize(size);
                QTest::qWait(1);
            } else if (action < 55) {
                const bool all = d.chance(0.3);
                step(n, QStringLiteral("the host answers %1 of %2 waiting").arg(all ? QStringLiteral("all") : QStringLiteral("one")).arg(host.waiting.size()));
                host.answer(all);
            } else if (action < 58) {
                step(n, QStringLiteral("new tab"));
                tabs->newConversation();
                pendingToken.clear();
            } else if (action < 60) {
                ClaudeCodePanel* which = panels.at(d.below(int(panels.size())));
                const bool ask = d.chance(0.3);
                step(n, QStringLiteral("close tab %1%2").arg(panels.indexOf(which)).arg(ask ? QStringLiteral(" (asked)") : QString()));
                if (which == panel) pendingToken.clear();
                tabs->closeConversation(which, ask);
            } else if (action < 62) {
                ClaudeCodePanel* which = panels.at(d.below(int(panels.size())));
                step(n, QStringLiteral("show tab %1").arg(panels.indexOf(which)));
                if (which != panel) pendingToken.clear();
                tabs->showConversation(which);
            } else if (action < 64) {
                ClaudeCodePanel* which = panels.at(d.below(int(panels.size())));
                step(n, QStringLiteral("rename tab %1").arg(panels.indexOf(which)));
                tabs->renameConversation(which);
                if (QLineEdit* editor = tabs->renameEditor()) {
                    editor->setText(d.chance(0.1) ? QString() : reply(d, false, nullptr));
                    switch (d.below(5)) {
                    case 0: QTest::keyClick(editor, Qt::Key_Return); break;
                    case 1: QTest::keyClick(editor, Qt::Key_Escape); break;
                    case 2: panel->composer()->setFocus(); break;
                    case 3: tabs->closeConversation(which, false); break;
                    default: tabs->newConversation(); break;
                    }
                }
            } else if (action < 65) {
                const int index = d.below(tabs->count());
                std::unique_ptr<QMenu> menu(tabs->tabMenu(index));
                const QList<QAction*> actions = menu ? menu->actions() : QList<QAction*>();
                if (actions.isEmpty()) continue;
                QAction* a = actions.at(d.below(int(actions.size())));
                step(n, QStringLiteral("tab %1's menu: %2").arg(index).arg(a->text()));
                if (a->isEnabled() && !a->isSeparator()) a->trigger();
                pendingToken.clear();   // (renamed, or closed: another in front)
                if (QLineEdit* editor = tabs->renameEditor()) QTest::keyClick(editor, d.chance(0.5) ? Qt::Key_Return : Qt::Key_Escape);
            } else if (action < 68) {
                // One of the panel's menus' actions (the ⋯ menu and its own).
                QList<QMenu*> menus = panel->findChildren<QMenu*>();
                if (menus.isEmpty()) continue;
                QMenu* menu = menus.at(d.below(int(menus.size())));
                emit menu->aboutToShow();
                QList<QAction*> actions;
                for (QAction* a : menu->actions())
                    if (a->isEnabled() && !a->isSeparator() && a->menu() == nullptr) actions << a;
                if (actions.isEmpty()) continue;
                QAction* a = actions.at(d.below(int(actions.size())));
                step(n, QStringLiteral("menu %1: %2").arg(menu->title(), a->text()));
                a->trigger();
                pendingToken.clear();   // (another folder: another conversation)
            } else if (action < 70) {
                // The transcript's own menu: at a picture, now and then.
                const std::optional<QPoint> picture = d.chance(0.5) ? pictureIn(panel->transcript()) : std::nullopt;
                const QPoint at = picture ? *picture
                                          : QPoint(d.below(panel->transcript()->viewport()->width() + 1),
                                                   d.below(panel->transcript()->viewport()->height() + 1));
                if (d.chance(0.5)) selectSome(d, panel->transcript());
                std::unique_ptr<QMenu> menu = panel->transcriptMenu(at);
                const QList<QAction*> actions = menu ? menu->actions() : QList<QAction*>();
                QList<QAction*> usable;
                for (QAction* a : actions)
                    if (a->isEnabled() && !a->isSeparator()) usable << a;
                if (usable.isEmpty()) continue;
                QAction* a = usable.at(d.below(int(usable.size())));
                step(n, QStringLiteral("transcript menu at %1,%2: %3").arg(at.x()).arg(at.y()).arg(a->text()));
                const QString draft = panel->composer()->toPlainText();
                a->trigger();
                if (a->objectName() == QLatin1String("claudeReply")) {
                    const QString trouble = quoteTrouble(draft, panel->composer()->toPlainText());
                    if (!trouble.isEmpty()) found(n, trouble);
                }
            } else if (action < 74) {
                QString text;
                const QList<ClaudeCodePanel::Command> commands = panel->commands();
                if (d.chance(0.3) && !commands.isEmpty()) {
                    const ClaudeCodePanel::Command& c = d.pick(commands);
                    text = QLatin1Char('/') + c.name + (d.chance(0.5) ? QLatin1Char(' ') + word(d) : QString());
                } else {
                    text = reply(d, false, nullptr);
                }
                const QString program = d.pickText(programs);
                panel->session()->setProgram(program);
                step(n, QStringLiteral("send (%1) %2").arg(QFileInfo(program).fileName(), text));
                if (text.startsWith(QLatin1Char('/'))) pendingToken.clear();
                panel->composer()->setPlainText(text);
                panel->sendComposer();
            } else if (action < 77) {
                // The composer's keys; a paste of what the clipboard holds.
                static const QList<std::pair<int, Qt::KeyboardModifiers>> keys = {
                    {Qt::Key_Up, Qt::NoModifier},     {Qt::Key_Down, Qt::NoModifier},       {Qt::Key_Escape, Qt::NoModifier},
                    {Qt::Key_Return, Qt::NoModifier}, {Qt::Key_Return, Qt::ShiftModifier}, {Qt::Key_Tab, Qt::NoModifier},
                    {Qt::Key_V, Qt::ControlModifier}, {Qt::Key_Slash, Qt::NoModifier},     {Qt::Key_Backspace, Qt::NoModifier},
                    {Qt::Key_A, Qt::NoModifier}};
                const auto& [key, modifiers] = keys.at(d.below(int(keys.size())));
                if (key == Qt::Key_V) QApplication::clipboard()->setMimeData(someMime(d));
                step(n, QStringLiteral("key %1").arg(QKeySequence(QKeyCombination(modifiers, Qt::Key(key))).toString()));
                panel->composer()->setFocus();
                QTest::keyClick(panel->composer(), Qt::Key(key), modifiers);
                if (key == Qt::Key_Return && !(modifiers & Qt::ShiftModifier)) pendingToken.clear();
            } else if (action < 79) {
                // A drop on the composer or the transcript.
                std::unique_ptr<QMimeData> mime(someMime(d));
                QWidget* target = d.chance(0.5) ? panel->composer()->viewport() : panel->transcript()->viewport();
                QStringList urls;
                for (const QUrl& u : mime->urls()) urls << QFileInfo(u.path()).fileName();
                step(n, QStringLiteral("drop %1 on the %2 (urls %3; text %4, html %5 characters)")
                            .arg(mime->formats().join(QLatin1Char(',')),
                                 target == panel->composer()->viewport() ? QStringLiteral("composer") : QStringLiteral("transcript"),
                                 urls.join(QLatin1Char(' ')))
                            .arg(mime->hasText() && mime->hasFormat(QStringLiteral("text/plain")) ? mime->text().size() : -1)
                            .arg(mime->hasHtml() ? mime->html().size() : -1));
                QDragEnterEvent enter(QPoint(4, 4), Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(target, &enter);
                QDropEvent drop(QPointF(4, 4), Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(target, &drop);
            } else if (action < 83) {
                QList<QToolButton*> buttons = shownButtons(panel->permissionCard()) + shownButtons(panel->askCard());
                if (buttons.isEmpty()) continue;
                for (QLineEdit* e : panel->askCard()->findChildren<QLineEdit*>()) e->setText(word(d));
                for (QCheckBox* b : panel->askCard()->findChildren<QCheckBox*>()) b->setChecked(d.chance(0.5));
                QToolButton* b = buttons.at(d.below(int(buttons.size())));
                step(n, QStringLiteral("press %1 (%2)").arg(b->objectName(), b->text()));
                b->click();
            } else if (action < 85) {
                const QString pin = d.pickText(documents);
                step(n, QStringLiteral("pin %1").arg(pin));
                panel->pinDocument(pin);
            } else if (action < 87) {
                // What is in front and open, and what the pen drew.
                front = d.pickText(documents);
                if (d.chance(0.5)) schematics = d.chance(0.8) ? documents.mid(d.below(3), 2) : QStringList();
                if (d.chance(0.5)) others = documents.mid(d.below(5), d.below(3));
                if (d.chance(0.6)) {
                    ++drawingRevision;
                    drawing = d.chance(0.2) ? QString() : QStringLiteral("a ring at (%1, %2); ").arg(d.below(99)).arg(d.below(99)) + reply(d, false, nullptr);
                }
                step(n, QStringLiteral("front %1, drawing %2").arg(front).arg(drawingRevision));
                tabs->refreshDocument();
            } else if (action < 89) {
                if (d.chance(0.5)) {
                    step(n, QStringLiteral("keep the conversations"));
                    tabs->saveConversations();
                } else {
                    step(n, QStringLiteral("bring the conversations back"));
                    tabs->restoreConversations();
                    pendingToken.clear();
                }
            } else if (action < 90) {
                const QString query = d.chance(0.5) ? QString() : (d.chance(0.5) ? word(d) : panel->conversationId());
                step(n, QStringLiteral("resume %1").arg(query));
                tabs->showResume(panel, query);
                pendingToken.clear();
            } else if (action < 92) {
                step(n, QStringLiteral("stop / background"));
                if (d.chance(0.5)) panel->stopTurn();
                else panel->sendToBackground();
            } else if (action < 93) {
                const qsizetype i = qsizetype(d.below(12)) - 2;
                step(n, QStringLiteral("edit %1").arg(i));
                if (panel->editPrompt(i)) pendingToken.clear();
                if (d.chance(0.5)) panel->cancelEdit();
            } else if (action < 94) {
                const QStringList anchors = anchorsOf(panel->transcript()->document());
                if (anchors.isEmpty()) continue;
                const QString href = d.pickText(anchors);
                step(n, QStringLiteral("click %1").arg(href));
                if (href.startsWith(QLatin1String("edit:"))) pendingToken.clear();
                emit panel->transcript()->anchorClicked(QUrl(href));
            } else if (action < 95) {
                step(n, QStringLiteral("scroll"));
                QScrollBar* bar = panel->transcript()->verticalScrollBar();
                bar->setValue(d.below(bar->maximum() + 1));
            } else if (action < 96) {
                step(n, QStringLiteral("a theme and a font"));
                QPalette p = QApplication::palette();
                p.setColor(QPalette::Window, d.chance(0.5) ? QColor(30, 30, 30) : QColor(240, 240, 240));
                p.setColor(QPalette::Base, p.color(QPalette::Window));
                p.setColor(QPalette::Text, d.chance(0.5) ? Qt::white : Qt::black);
                QApplication::setPalette(p);
                QFont f = QApplication::font();
                f.setPointSize(d.below(14) + 8);
                QApplication::setFont(f);
            } else if (action < 97) {
                const QString from = d.chance(0.6) ? panel->pinnedDocument() : d.pickText(documents);
                const QString to = d.pickText(documents);
                step(n, QStringLiteral("renamed %1 to %2").arg(from, to));
                tabs->documentRenamed(from, to);
            } else if (action < 98) {
                step(n, QStringLiteral("hidden and shown"));
                tabs->hide();
                QTest::qWait(d.below(5));
                tabs->show();
            } else {
                step(n, QStringLiteral("attach"));
                QStringList some;
                for (int i = 0, k = d.below(3) + 1; i < k; ++i) some << d.pickText(files);
                panel->attachImageFiles(some);
            }
            QCoreApplication::processEvents();
            // (Kept to a size that renders in moderate time.)
            if (n % 300 == 299) {
                step(n, QStringLiteral("new conversation (size)"));
                if (ClaudeCodePanel* p = tabs->current()) p->newConversation();
                pendingToken.clear();
            }
            while (tabs->count() > 8) tabs->closeConversation(tabs->panels().constFirst(), false);
            // The tabs as they should be.
            if (tabs->tabWidget()->count() != int(tabs->panels().size()))
                found(n, QStringLiteral("%1 tabs for %2 conversations").arg(tabs->tabWidget()->count()).arg(tabs->panels().size()));
            for (int i = 0; i < tabs->tabWidget()->count(); ++i) {
                const QString title = tabs->tabWidget()->tabText(i);
                if (title.contains(QLatin1Char('\n')) || title.size() > 120)
                    found(n, QStringLiteral("tab %1's title: %2 characters, %3").arg(i).arg(title.size()).arg(title.left(60)));
            }
            const int least = tabs->minimumSizeHint().width();
            // (In proportion to the font: a step changes it.)
            const qreal grown = std::max<qreal>(1, QFontMetricsF(QApplication::font()).averageCharWidth() / charWidth);
            if (least > grown * std::max(2 * leastWidth, 480))
                found(n, QStringLiteral("the dock's least width %1 (%2 at first), by %3").arg(least).arg(leastWidth).arg(widest(tabs.get(), int(grown * 480))));
        }
        watch.stop();
        // Everything closed, then the calls still waiting answered.
        step(steps, QStringLiteral("closing"));
        tabs.reset();
        host.answer(true);
        QCoreApplication::processEvents();

        // What the dock wrote: JSON lines, each request answered once at most.
        QHash<QString, int> answers;
        int writes = 0;
        for (const QString& name : QDir(dir.path()).entryList({QStringLiteral("written-*.jsonl")}, QDir::Files)) {
            QFile out(dir.filePath(name));
            if (!out.open(QIODevice::ReadOnly)) continue;
            for (const QByteArray& line : out.readAll().split('\n')) {
                if (line.isEmpty()) continue;
                ++writes;
                QJsonParseError error{};
                const QJsonObject m = QJsonDocument::fromJson(line, &error).object();
                if (error.error != QJsonParseError::NoError) {
                    found(steps, QStringLiteral("the dock wrote a line that is not JSON: %1").arg(QString::fromUtf8(line.left(200))));
                    continue;
                }
                if (m.value(QLatin1String("type")).toString() != QLatin1String("control_response")) continue;
                const QString id = m.value(QLatin1String("response")).toObject().value(QLatin1String("request_id")).toString();
                if (id.startsWith(QLatin1String("fz"))) ++answers[id];
            }
        }
        for (auto it = answers.cbegin(); it != answers.cend(); ++it)
            if (it.value() > 1) found(steps, QStringLiteral("request %1 answered %2 times").arg(it.key()).arg(it.value()));
        qInfo("%d lines fed; %d tool calls, %d asked, %d cancelled; %d lines written, %lld requests answered; the slowest render %lld ms; "
              "%lld dialogs closed: %s; %lld links opened",
              lines, host.calls, host.asked, host.cancels, writes, qint64(answers.size()), slowest, qint64(dialogs.size()),
              qPrintable(QStringList(dialogs.mid(0, 5)).join(QLatin1String("; "))), qint64(opened.urls.size()));
        for (const QString& f : findings) qWarning("%s", qPrintable(f));
        for (const char* scheme : {"http", "https", "mailto", "file"}) QDesktopServices::unsetUrlHandler(QString::fromLatin1(scheme));
        QVERIFY2(findings.isEmpty(), qPrintable(QStringLiteral("%1 findings, the first: %2").arg(findings.size()).arg(findings.value(0))));
    }
};

QTEST_MAIN(TestClaudeFuzz)
#include "test_claude_fuzz.moc"

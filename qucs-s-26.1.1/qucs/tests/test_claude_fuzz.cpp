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
 * After each step: no dialog left open (one is closed and said), a render
 * in bounded time, the newest plain reply in the transcript, no formula's
 * mark left unplaced, the view at the conversation's end while it follows
 * it. A crash or a sanitizer's report ends the run; the trace file (each
 * step, flushed) and the seed replay it.
 *
 * QUCS_FUZZ_SEED (1), QUCS_FUZZ_STEPS (400 - CI's short pass) or
 * QUCS_FUZZ_SECONDS (a long run: steps without limit until then),
 * QUCS_FUZZ_TRACE (a file for the steps; else in the test's folder).
 */

#include <QtTest>
#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPlainTextEdit>
#include <QRandomGenerator>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextList>
#include <QTimer>
#include <QToolButton>

#include "claudecode.h"
#include "claudecodepanel.h"
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
                                                                    QStringLiteral("task_started"), QStringLiteral("task_updated"),
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

    // The transcript as drawn: each block - its frame, list, formats,
    // fragments - but the document's own numbers for its lists and frames.
    static QString drawn(QTextDocument* doc)
    {
        const auto props = [](const QTextFormat& f) {
            QMap<int, QVariant> p = f.properties();
            p.remove(QTextFormat::ObjectIndex);
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
                // Drawn in part, drawn whole: the same transcript.
                if (d.chance(0.3)) {
                    const QString part = drawn(panel->transcript()->document());
                    panel->renderNow(true);
                    const QString whole = drawn(panel->transcript()->document());
                    if (part != whole) {
                        if (qEnvironmentVariableIsSet("QUCS_FUZZ_DUMPS")) {
                            QFile fa(qEnvironmentVariable("QUCS_FUZZ_DUMPS") + QStringLiteral("/part%1.txt").arg(n));
                            QFile fb(qEnvironmentVariable("QUCS_FUZZ_DUMPS") + QStringLiteral("/whole%1.txt").arg(n));
                            if (fa.open(QIODevice::WriteOnly)) fa.write(part.toUtf8());
                            if (fb.open(QIODevice::WriteOnly)) fb.write(whole.toUtf8());
                        }
                        const QStringList a = part.split(QLatin1Char('\n')), b = whole.split(QLatin1Char('\n'));
                        qsizetype i = 0;
                        while (i < a.size() && i < b.size() && a.at(i) == b.at(i)) ++i;
                        const QString x = a.value(i), y = b.value(i);
                        qsizetype k = 0;
                        while (k < x.size() && k < y.size() && x.at(k) == y.at(k)) ++k;
                        const qsizetype at = std::max<qsizetype>(0, k - 120);
                        found(n, QStringLiteral("drawn in part (from part %1) not as drawn whole, line %2 (%3):\n  %4\n  %5")
                                     .arg(panel->renderedFrom()).arg(i).arg(a.value(i - 1).left(160), x.mid(at, 300), y.mid(at, 300)));
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
};

QTEST_MAIN(TestClaudeFuzz)
#include "test_claude_fuzz.moc"

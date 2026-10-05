/*
 * claudecodepanel.cpp - the Claude Code dock: a conversation with Claude
 *                       Code, working in the workspace folder
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "claudecodepanel.h"
#include "claudegitbar.h"
#include "claudehistory.h"

#include "apptheme.h"
#include "ink.h"
#include "misc.h"
#include "settings.h"

#include <QAbstractTextDocumentLayout>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QJsonArray>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QScreen>
#include <QScrollBar>
#include <QStyle>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocumentFragment>
#include <QTextFormat>
#include <QTextFrame>
#include <QTextList>
#include <QTextTable>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <iterator>

using qucs_s::claude::ModelChoice;
using qucs_s::claude::State;
using qucs_s::claude::modelName;

namespace {

// The settings the dock keeps.
const QString kProgram = QStringLiteral("ClaudeCode/program");
const QString kMode = QStringLiteral("ClaudeCode/permissionMode");
const QString kModel = QStringLiteral("ClaudeCode/model");
const QString kModels = QStringLiteral("ClaudeCode/models");          // what the program offers
const QString kOtherModels = QStringLiteral("ClaudeCode/otherModels"); // chosen by name, the latest first
const QString kAttach = QStringLiteral("ClaudeCode/attachDocument");
const QString kExportDir = QStringLiteral("ClaudeCode/exportFolder");
const QString kExportDetails = QStringLiteral("ClaudeCode/exportToolDetails");
const QString kCommands = QStringLiteral("ClaudeCode/slashCommands");   // Claude Code's, as it last said
// What each turn's line tells of what it took.
const QString kShowPromptTokens = QStringLiteral("ClaudeCode/showPromptTokens");
const QString kShowConversationTokens = QStringLiteral("ClaudeCode/showConversationTokens");
const QString kShowPromptCost = QStringLiteral("ClaudeCode/showPromptCost");
const QString kShowConversationCost = QStringLiteral("ClaudeCode/showConversationCost");

struct Colours {
    QColor base, text, muted, faint, border, accent, onAccent, bubble, code, ok, warn, error;
};

Colours colours(const QPalette& pal)
{
    using qucs_s::apptheme::mix;
    Colours c;
    c.base = pal.color(QPalette::Base);
    c.text = pal.color(QPalette::Text);
    const bool dark = qucs_s::ink::isDark(c.base);
    // Claude's clay.
    c.accent = dark ? QColor(0xe0, 0x8a, 0x6b) : QColor(0xc4, 0x5f, 0x3e);
    c.onAccent = dark ? QColor(0x1c, 0x12, 0x0e) : QColor(Qt::white);
    c.muted = mix(c.base, c.text, 0.64);
    c.faint = mix(c.base, c.text, 0.42);
    c.border = mix(c.base, c.text, dark ? 0.22 : 0.16);
    c.bubble = mix(c.base, c.accent, dark ? 0.16 : 0.09);
    c.code = mix(c.base, c.text, dark ? 0.10 : 0.05);
    c.ok = dark ? QColor(0x3f, 0xb9, 0x50) : QColor(0x1a, 0x7f, 0x37);
    c.warn = dark ? QColor(0xd2, 0x99, 0x22) : QColor(0x9a, 0x67, 0x00);
    c.error = dark ? QColor(0xf8, 0x51, 0x49) : QColor(0xcf, 0x22, 0x2e);
    return c;
}

// A round mark: filled, or a ring for something under way.
QPixmap dot(const QColor& colour, bool ring, qreal ratio)
{
    QPixmap pixmap(QSize(10, 10) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    if (ring) {
        p.setPen(QPen(colour, 1.8));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QRectF(1.4, 1.4, 7.2, 7.2));
    } else {
        p.setPen(Qt::NoPen);
        p.setBrush(colour);
        p.drawEllipse(QRectF(1.0, 1.0, 8.0, 8.0));
    }
    return pixmap;
}

// "mcp__qucs__add_component": Qucs-S · add component.
QString toolName(const QString& tool)
{
    if (!tool.startsWith(QLatin1String("mcp__"))) return tool;
    const QStringList parts = tool.split(QStringLiteral("__"));
    if (parts.size() < 3) return tool;
    const QString server = parts.at(1) == QLatin1String("qucs") ? QStringLiteral("Qucs-S") : parts.at(1);
    return server + QStringLiteral(" \u00b7 ") + QString(parts.mid(2).join(QStringLiteral("__"))).replace(QLatin1Char('_'), QLatin1Char(' '));
}

// The permission mode in a word, for the header; empty for asking.
QString modeTag(const QString& mode)
{
    if (mode == QLatin1String("acceptEdits")) return ClaudeCodePanel::tr("edits");
    if (mode == QLatin1String("auto")) return ClaudeCodePanel::tr("auto");
    if (mode == QLatin1String("plan")) return ClaudeCodePanel::tr("plan");
    if (mode == QLatin1String("bypassPermissions")) return ClaudeCodePanel::tr("bypass");
    return qucs_s::claude::isAskMode(mode) ? QString() : mode;
}

QString seconds(qint64 ms)
{
    const QLocale locale;
    const double s = ms / 1000.0;
    if (s < 10.0) return ClaudeCodePanel::tr("%1 s").arg(locale.toString(s, 'f', 1));
    if (s < 60.0) return ClaudeCodePanel::tr("%1 s").arg(qRound(s));
    const qint64 whole = (ms + 500) / 1000;
    return ClaudeCodePanel::tr("%1 min %2 s").arg(whole / 60).arg(whole % 60);
}

QString cost(double usd)
{
    return QStringLiteral("$") + QLocale::c().toString(usd, 'f', usd < 1.0 ? 3 : 2);
}

// A number of tokens, short: 812, 12.3k, 1.24M.
QString tokenCount(qint64 n)
{
    const QLocale c = QLocale::c();
    if (n < 1000) return QString::number(n);
    if (n < 99950) return c.toString(n / 1000.0, 'f', 1) + QLatin1Char('k');
    if (n < 999500) return QString::number(qRound64(n / 1000.0)) + QLatin1Char('k');
    return c.toString(n / 1e6, 'f', n < 99995000 ? 2 : 1) + QLatin1Char('M');
}

// Each kind of them, in full.
QString tokenDetail(const qucs_s::claude::TokenUsage& t)
{
    const QLocale locale;
    return ClaudeCodePanel::tr("%1 tokens: %2 input, %3 output, %4 read from the cache, %5 written to it")
        .arg(locale.toString(t.total()), locale.toString(t.input), locale.toString(t.output),
             locale.toString(t.cacheRead), locale.toString(t.cacheWrite));
}

QJsonObject tokensJson(const qucs_s::claude::TokenUsage& t)
{
    return {{QStringLiteral("input"), double(t.input)}, {QStringLiteral("output"), double(t.output)},
            {QStringLiteral("cacheRead"), double(t.cacheRead)}, {QStringLiteral("cacheWrite"), double(t.cacheWrite)}};
}

qucs_s::claude::TokenUsage tokensFromJson(const QJsonObject& o)
{
    const auto count = [&o](const char* name) { return std::max<qint64>(0, qint64(o.value(QLatin1String(name)).toDouble())); };
    qucs_s::claude::TokenUsage t;
    t.input = count("input");
    t.output = count("output");
    t.cacheRead = count("cacheRead");
    t.cacheWrite = count("cacheWrite");
    return t;
}

// The dock's conversations: each shows the usage chosen at once.
QList<ClaudeCodePanel*>& openPanels()
{
    static QList<ClaudeCodePanel*> panels;
    return panels;
}

// A folder as the user knows it: ~ for the home directory.
QString shownPath(const QString& path)
{
    const QString native = QDir::toNativeSeparators(path);
    const QString home = QDir::toNativeSeparators(QDir::homePath());
    if (native.startsWith(home + QDir::separator())) return QStringLiteral("~") + native.mid(home.size());
    return native;
}

// The programs asked in this run which models they offer: once each, for
// every panel (the answer is kept in the settings, where they all read it).
QSet<QString>& askedPrograms()
{
    static QSet<QString> asked;
    return asked;
}

// Math in a reply is kept apart while the Markdown is read: a character
// of the private use area stands for each formula.
constexpr char16_t kMathMark = 0xE000;

// A new block with \a format - the empty one the cursor is in, if it is.
void startBlock(QTextCursor& c, const QTextBlockFormat& format)
{
    if (c.atBlockStart() && c.block().length() <= 1) c.setBlockFormat(format);
    else c.insertBlock(format);
}

void leaveFrame(QTextCursor& c)
{
    c = c.document()->rootFrame()->lastCursorPosition();
}

struct Suggestion {
    const char* label;
    const char* prompt;
    bool attach;   // about the open document
};

const Suggestion kSuggestions[] = {
    {QT_TRANSLATE_NOOP("ClaudeCodePanel", "Explain what the open schematic does"),
     QT_TRANSLATE_NOOP("ClaudeCodePanel", "Explain what the circuit in the open schematic does, stage by stage."), true},
    {QT_TRANSLATE_NOOP("ClaudeCodePanel", "Look for mistakes in the open schematic"),
     QT_TRANSLATE_NOOP("ClaudeCodePanel",
                       "Check the open schematic for mistakes: unconnected pins, a missing ground, suspicious "
                       "values, simulation settings that do not fit the circuit."), true},
    {QT_TRANSLATE_NOOP("ClaudeCodePanel", "Give an overview of the projects here"),
     QT_TRANSLATE_NOOP("ClaudeCodePanel", "Give me an overview of the Qucs-S projects in this folder."), false},
    {QT_TRANSLATE_NOOP("ClaudeCodePanel", "Write an ngspice model for a part"),
     QT_TRANSLATE_NOOP("ClaudeCodePanel", "Write an ngspice .model or .subckt for "), false},
};

// ----------------------------------------------------------------------
// Exports.

// Whether \a a and \a b are the same file.
bool sameFile(const QString& a, const QString& b)
{
    if (a.isEmpty() || b.isEmpty()) return false;
    const QString ca = QFileInfo(a).canonicalFilePath();
    const QString cb = QFileInfo(b).canonicalFilePath();
    if (!ca.isEmpty() && !cb.isEmpty()) return ca == cb;
    return QDir::cleanPath(QFileInfo(a).absoluteFilePath()) == QDir::cleanPath(QFileInfo(b).absoluteFilePath());
}

// A file's name on a button or in a menu: cut as the settings say (the
// tooltip gives it whole), an & of it shown as one.
QString shownName(const QString& file)
{
    return misc::shownFileName(file).replace(QLatin1Char('&'), QLatin1String("&&"));
}

// A push pin, in \a ink.
QIcon pinIcon(const QColor& ink)
{
    QPixmap pixmap(QSize(32, 32));
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(16, 16);
    p.rotate(40);
    p.setPen(Qt::NoPen);
    p.setBrush(ink);
    p.drawRoundedRect(QRectF(-6.5, -15, 13, 5), 2, 2);    // the head
    p.drawRect(QRectF(-3.5, -11, 7, 9));                  // its body
    p.drawRoundedRect(QRectF(-8.5, -3, 17, 4), 2, 2);     // the collar
    QPen needle(ink, 2.6);
    needle.setCapStyle(Qt::RoundCap);
    p.setPen(needle);
    p.drawLine(QPointF(0, 1), QPointF(0, 14));
    p.end();
    return QIcon(pixmap);
}

// The mark of a tool's outcome, as in the dock.
QString outcomeMark(int state)
{
    switch (state) {
    case 1: return QStringLiteral("✓");
    case 2: return QStringLiteral("✕");
    case 3: return QStringLiteral("⊘");
    default: return QStringLiteral("○");
    }
}

// The longest run of \a c in \a text.
int longestRun(const QString& text, QChar c)
{
    int longest = 0, run = 0;
    for (QChar x : text) {
        run = x == c ? run + 1 : 0;
        longest = std::max(longest, run);
    }
    return longest;
}

// \a text as Markdown code in a line: between more backticks than it has
// in a row.
QString inlineCode(const QString& text)
{
    const QString ticks(longestRun(text, QLatin1Char('`')) + 1, QLatin1Char('`'));
    const bool pad = text.startsWith(QLatin1Char('`')) || text.endsWith(QLatin1Char('`'));
    return ticks + (pad ? QStringLiteral(" ") : QString()) + text + (pad ? QStringLiteral(" ") : QString()) + ticks;
}

// \a text as a fenced Markdown code block, each line after \a indent (in
// a list item).
QString codeBlock(const QString& text, const QString& indent)
{
    const QString fence(std::max(3, longestRun(text, QLatin1Char('`')) + 1), QLatin1Char('`'));
    QString out = indent + fence + QLatin1Char('\n');
    for (const QString& line : text.split(QLatin1Char('\n'))) out += (line.isEmpty() ? QString() : indent + line) + QLatin1Char('\n');
    return out + indent + fence + QLatin1Char('\n');
}

// A reply's Markdown read as plain text: paragraphs apart, list items with
// their marks, quotes after "> ", code indented, a table a row to a line
// with " | " between its cells, the math as TeX between its $ signs.
QString plainTextOf(const QString& markdown)
{
    const QList<qucs_s::math::Span> spans = qucs_s::math::findMath(markdown);
    QString md = markdown;
    for (qsizetype k = spans.size(); k-- > 0;)
        md.replace(spans.at(k).start, spans.at(k).length, QString(QChar(char16_t(kMathMark + k))));
    QTextDocument doc;
    qucs_s::markdown::setMarkdown(&doc, md);

    QStringList out;
    QSet<QTextTable*> tables;
    enum { Paragraph, Item, Code } last = Paragraph;
    const auto apart = [&out](bool yes) {
        if (yes && !out.isEmpty() && !out.constLast().isEmpty()) out << QString();
    };
    for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
        if (QTextTable* table = QTextCursor(b).currentTable()) {
            if (tables.contains(table)) continue;
            tables.insert(table);
            apart(true);
            for (int r = 0; r < table->rows(); ++r) {
                QStringList cells;
                for (int k = 0; k < table->columns(); ++k) {
                    const QTextTableCell cell = table->cellAt(r, k);
                    QTextCursor cc = cell.firstCursorPosition();
                    cc.setPosition(cell.lastCursorPosition().position(), QTextCursor::KeepAnchor);
                    // (Its paragraphs and lines - a <br> - on the row's line.)
                    cells << cc.selectedText().replace(QChar::ParagraphSeparator, QLatin1Char(' ')).replace(QChar::LineSeparator, QLatin1Char(' ')).trimmed();
                }
                out << cells.join(QStringLiteral(" | "));
            }
            last = Paragraph;
            continue;
        }
        const QTextBlockFormat bf = b.blockFormat();
        QString text = b.text().replace(QChar::LineSeparator, QLatin1Char('\n')).remove(QChar::ObjectReplacementCharacter);
        const int quote = bf.intProperty(QTextFormat::BlockQuoteLevel);
        const QString quoted = quote > 0 ? QString(quote, QLatin1Char('>')) + QLatin1Char(' ') : QString();
        if (bf.hasProperty(QTextFormat::BlockTrailingHorizontalRulerWidth)) {
            apart(true);
            out << QStringLiteral("----");
            last = Paragraph;
        } else if (QTextList* list = b.textList()) {
            apart(last != Item);
            const QString lead(2 * std::max(0, list->format().indent() - 1), QLatin1Char(' '));
            // (A bullet's item text is its suffix alone.)
            QString mark = list->itemText(b).trimmed();
            if (!mark.contains(QRegularExpression(QStringLiteral("[\\p{L}\\p{N}]"))))
                mark = list->format().style() == QTextListFormat::ListCircle ? QStringLiteral("◦") : QStringLiteral("•");
            out << quoted + lead + mark + QLatin1Char(' ') + text.replace(QLatin1Char('\n'), QLatin1Char('\n') + lead + QStringLiteral("  "));
            last = Item;
        } else if (bf.nonBreakableLines() || bf.hasProperty(QTextFormat::BlockCodeFence)) {
            apart(last != Code);
            out << quoted + QStringLiteral("    ") + text;
            last = Code;
        } else if (!text.trimmed().isEmpty()) {
            apart(true);
            out << quoted + text.replace(QLatin1Char('\n'), QLatin1Char('\n') + quoted);
            last = Paragraph;
        }
    }
    QString result = out.join(QLatin1Char('\n'));
    for (qsizetype k = 0; k < spans.size(); ++k) {
        const QString dollars = spans.at(k).display ? QStringLiteral("$$") : QStringLiteral("$");
        result.replace(QChar(char16_t(kMathMark + k)), dollars + spans.at(k).tex + dollars);
    }
    return result;
}

// A file's name for a conversation about \a title: what a file name may not
// hold made a dash.
QString fileNameFor(const QString& title)
{
    QString name = title;
    name.remove(QChar(0x2026));
    name.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|\\x00-\\x1f]+")), QStringLiteral("-"));
    name = name.simplified();
    while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char('-'))) name.chop(1);
    return name.isEmpty() ? QStringLiteral("Conversation") : name.left(60).trimmed();
}

} // namespace

// ----------------------------------------------------------------------
ClaudeCodePanel::ClaudeCodePanel(QWidget* parent)
    : QWidget(parent),
      a_session(new qucs_s::claude::Session(this)),
      a_modelQuery(new qucs_s::claude::ModelQuery(this)),
      a_renderTimer(new QTimer(this)),
      a_clock(new QTimer(this))
{
    setObjectName(QStringLiteral("claudeCodePanel"));
    openPanels().append(this);
    a_renderTimer->setSingleShot(true);
    a_renderTimer->setInterval(40);
    connect(a_renderTimer, &QTimer::timeout, this, &ClaudeCodePanel::render);
    a_clock->setInterval(1000);
    connect(a_clock, &QTimer::timeout, this, &ClaudeCodePanel::updateState);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    buildHeader();
    layout->addWidget(a_header);

    // The folder Claude works in.
    a_dirRow = new QWidget(this);
    a_dirRow->setObjectName(QStringLiteral("claudeDirRow"));
    auto* dirLayout = new QHBoxLayout(a_dirRow);
    dirLayout->setContentsMargins(10, 4, 6, 4);
    dirLayout->setSpacing(6);
    a_dirIcon = new QLabel(a_dirRow);
    a_dirIcon->setPixmap(style()->standardIcon(QStyle::SP_DirIcon).pixmap(14, 14));
    a_dirLabel = new QLabel(a_dirRow);
    a_dirLabel->setObjectName(QStringLiteral("claudeDir"));
    a_dirLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    a_dirLabel->setTextFormat(Qt::PlainText);
    a_dirLabel->installEventFilter(this);   // elided again as it resizes
    a_dirButton = new QToolButton(a_dirRow);
    a_dirButton->setObjectName(QStringLiteral("claudeLink"));
    a_dirButton->setText(tr("Change…"));
    a_dirButton->setAutoRaise(true);
    a_dirButton->setToolTip(tr("Choose the folder Claude works in"));
    a_dirReset = new QToolButton(a_dirRow);
    a_dirReset->setObjectName(QStringLiteral("claudeLink"));
    a_dirReset->setText(tr("Workspace"));
    a_dirReset->setAutoRaise(true);
    a_dirReset->setToolTip(tr("Work in the workspace folder again"));
    dirLayout->addWidget(a_dirIcon);
    dirLayout->addWidget(a_dirLabel, 1);
    dirLayout->addWidget(a_dirReset);
    dirLayout->addWidget(a_dirButton);
    layout->addWidget(a_dirRow);
    connect(a_dirButton, &QToolButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("The Folder Claude Works In"), workingDirectory());
        if (dir.isEmpty() || QDir::cleanPath(dir) == workingDirectory()) return;
        if (!a_entries.isEmpty()
            && QMessageBox::question(this, tr("Claude Code"),
                                     tr("Claude starts a new conversation in the other folder. Go on?"))
                   != QMessageBox::Yes)
            return;
        setWorkingDirectory(dir);
    });
    connect(a_dirReset, &QToolButton::clicked, this, [this] {
        if (!a_entries.isEmpty()
            && QMessageBox::question(this, tr("Claude Code"),
                                     tr("Claude starts a new conversation in the workspace folder. Go on?"))
                   != QMessageBox::Yes)
            return;
        setWorkingDirectory(QString());
    });

    // The conversation.
    a_view = new QTextBrowser(this);
    a_view->setObjectName(QStringLiteral("claudeTranscript"));
    a_view->setFrameShape(QFrame::NoFrame);
    a_view->setOpenLinks(false);
    a_view->setOpenExternalLinks(false);
    a_view->setTextInteractionFlags(Qt::TextBrowserInteraction);
    connect(a_view, &QTextBrowser::anchorClicked, this, &ClaudeCodePanel::handleLink);
    qucs_s::math::MathObject::install(a_view->document());
    layout->addWidget(a_view, 1);

    buildPermissionCard();
    buildAskCard();
    auto* cardHolder = new QWidget(this);
    auto* cardLayout = new QVBoxLayout(cardHolder);
    cardLayout->setContentsMargins(10, 4, 10, 0);
    cardLayout->addWidget(a_card);
    cardLayout->addWidget(a_askCard);
    layout->addWidget(cardHolder);

    buildComposer();
    // The commands matching a "/" typed, above the composer.
    a_commandList = new QListWidget(this);
    a_commandList->setObjectName(QStringLiteral("claudeCommands"));
    a_commandList->setFocusPolicy(Qt::NoFocus);
    a_commandList->setUniformItemSizes(true);
    a_commandList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    a_commandList->hide();
    connect(a_commandList, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        a_commandList->setCurrentItem(item);
        completeCommand(false);
        focusComposer();
    });
    connect(a_input, &QPlainTextEdit::textChanged, this, &ClaudeCodePanel::updateCommandList);
    auto* composerHolder = new QWidget(this);
    auto* composerLayout = new QVBoxLayout(composerHolder);
    composerLayout->setContentsMargins(10, 6, 10, 10);
    composerLayout->setSpacing(6);
    // The git repository the project is in, right above the prompt.
    a_gitBar = new ClaudeGitBar(composerHolder);
    connect(a_gitBar, &ClaudeGitBar::promptRequested, this, [this](const QString& prompt) { sendPrompt(prompt); });
    connect(a_gitBar, &ClaudeGitBar::openFileRequested, this, &ClaudeCodePanel::openFileRequested);
    composerLayout->addWidget(a_gitBar);
    // Editing a prompt sent before: said above the composer, with Cancel.
    a_editBar = new QFrame(composerHolder);
    a_editBar->setObjectName(QStringLiteral("claudeEditBar"));
    auto* editLayout = new QHBoxLayout(a_editBar);
    editLayout->setContentsMargins(10, 4, 4, 4);
    editLayout->setSpacing(6);
    a_editText = new QLabel(a_editBar);
    a_editText->setObjectName(QStringLiteral("claudeMuted"));
    a_editText->setWordWrap(true);
    a_editText->setText(tr("Editing a prompt: when sent, it replaces the original and everything after it. "
                           "Files Claude changed since then stay as they are."));
    a_editCancel = new QToolButton(a_editBar);
    a_editCancel->setObjectName(QStringLiteral("claudeLink"));
    a_editCancel->setText(tr("Cancel"));
    a_editCancel->setAutoRaise(true);
    a_editCancel->setToolTip(tr("Leave the conversation as it was (Esc)"));
    editLayout->addWidget(a_editText, 1);
    editLayout->addWidget(a_editCancel, 0, Qt::AlignTop);
    a_editBar->hide();
    connect(a_editCancel, &QToolButton::clicked, this, &ClaudeCodePanel::cancelEdit);
    composerLayout->addWidget(a_editBar);
    composerLayout->addWidget(a_composer);
    layout->addWidget(composerHolder);

    buildMenu();

    connect(a_session, &qucs_s::claude::Session::stateChanged, this, [this] { updateState(); });
    connect(a_session, &qucs_s::claude::Session::sessionStarted, this, [this] { updateState(); });
    connect(a_session, &qucs_s::claude::Session::modeInUseChanged, this, [this] { updateState(); });
    connect(a_session, &qucs_s::claude::Session::replyStreamed, this, &ClaudeCodePanel::onReplyStreamed);
    connect(a_session, &qucs_s::claude::Session::replyFinished, this, &ClaudeCodePanel::onReplyFinished);
    connect(a_session, &qucs_s::claude::Session::toolStarted, this, &ClaudeCodePanel::onToolStarted);
    connect(a_session, &qucs_s::claude::Session::toolFinished, this, &ClaudeCodePanel::onToolFinished);
    connect(a_session, &qucs_s::claude::Session::permissionRequested, this, &ClaudeCodePanel::onPermissionRequested);
    connect(a_session, &qucs_s::claude::Session::elicitationRequested, this,
            [this](const QString& id, const QString&, const QString& message, const QJsonObject& schema) {
                a_asks.append(Ask{id, message, schema});
                if (a_asks.size() == 1) showNextAsk();
            });
    connect(a_session, &qucs_s::claude::Session::permissionWithdrawn, this, &ClaudeCodePanel::onPermissionWithdrawn);
    // Allow All Edits on a card: this conversation's alone, as the note
    // says. The saved default - what every new conversation starts with,
    // after a restart too - is the Permissions menu's to change.
    connect(a_session, &qucs_s::claude::Session::permissionModeChanged, this, [this](const QString&) {
        addNote(tr("Claude may change files without asking for the rest of this conversation."));
        updateState();
    });
    connect(a_session, &qucs_s::claude::Session::notice, this, [this](const QString& text) {
        append({Entry::Note, text, {}, {}});
    });
    connect(a_session, &qucs_s::claude::Session::failed, this, [this](const QString& message) {
        append({Entry::Problem, message, {}, {}});
    });
    connect(a_session, &qucs_s::claude::Session::turnFinished, this, &ClaudeCodePanel::onTurnFinished);
    // Kept again as it changes (ClaudeCodeTabs keeps it).
    connect(this, &ClaudeCodePanel::titleChanged, this, &ClaudeCodePanel::conversationChanged);
    connect(this, &ClaudeCodePanel::pinChanged, this, &ClaudeCodePanel::conversationChanged);
    connect(a_session, &qucs_s::claude::Session::turnFinished, this, &ClaudeCodePanel::conversationChanged);
    connect(a_session, &qucs_s::claude::Session::sessionStarted, this, [this] {
        // Its commands, for the list before its next start too.
        const QStringList commands = a_session->slashCommands();
        if (!commands.isEmpty()) QucsSettingsFile().setValue(kCommands, commands);
        // Taken back to before an edited prompt: what was after it goes.
        if (a_rewinding) {
            a_rewinding = false;
            a_rewindIndex = -1;
            a_setAside.clear();
            scheduleRender();
        }
        emit conversationChanged();
    });
    connect(a_session, &qucs_s::claude::Session::rewindFailed, this, [this](const QString& why) {
        if (!a_rewinding) {
            append({Entry::Problem, why, {}, {}});
            return;
        }
        // The conversation as it was, and the edited prompt back in the
        // composer.
        QString sent;
        for (qsizetype i = a_rewindIndex; i < a_entries.size(); ++i)
            if (a_entries.at(i).kind == Entry::You) {
                sent = a_entries.at(i).text;
                break;
            }
        a_entries.resize(a_rewindIndex);
        if (a_editStopped) endStoppedTurn(a_setAside);
        a_entries += a_setAside;
        a_setAside.clear();
        a_rewinding = false;
        a_rewindIndex = -1;
        a_editStopped = false;
        if (a_input->toPlainText().trimmed().isEmpty()) a_input->setPlainText(sent);
        append({Entry::Problem,
                tr("Claude Code could not go back to before that prompt (%1): the conversation is as it was, and the edited "
                   "prompt is in the box again.")
                    .arg(why),
                {}, {}});
        emit conversationChanged();
        updateState();
    });
    connect(a_modelQuery, &qucs_s::claude::ModelQuery::finished, this, [this](const QJsonArray& models) {
        if (!models.isEmpty() && models != a_listedModels) {
            a_listedModels = models;
            QucsSettingsFile().setValue(kModels, QString::fromUtf8(QJsonDocument(models).toJson(QJsonDocument::Compact)));
            rebuildModelMenu();
        }
        listModels();   // the program changed while it was asked
    });
    // Rebuilt in its own aboutToShow, not under a triggered action of it.

    const QucsSettingsFile settings;
    a_session->setPermissionMode(settings.value(kMode).toString());
    a_session->setModel(settings.value(kModel).toString());
    a_listedModels = QJsonDocument::fromJson(settings.value(kModels).toString().toUtf8()).array();
    rebuildModelMenu();
    a_attach->setChecked(settings.value(kAttach, true).toBool());
    findProgram();

    restyle();
    updateDirectory();
    updateComposer();
    updateState();
    render();
}

ClaudeCodePanel::~ClaudeCodePanel()
{
    openPanels().removeAll(this);
}

// ----------------------------------------------------------------------
void ClaudeCodePanel::buildHeader()
{
    a_header = new QWidget(this);
    a_header->setObjectName(QStringLiteral("claudeHeader"));
    auto* layout = new QHBoxLayout(a_header);
    layout->setContentsMargins(10, 6, 4, 6);
    layout->setSpacing(6);
    a_title = new QLabel(tr("Claude Code"), a_header);
    a_title->setObjectName(QStringLiteral("claudeTitle"));
    a_stateDot = new QLabel(a_header);
    a_stateDot->setFixedSize(10, 10);
    a_stateText = new QLabel(a_header);
    a_stateText->setObjectName(QStringLiteral("claudeState"));
    a_modelLabel = new QLabel(a_header);
    a_modelLabel->setObjectName(QStringLiteral("claudeModel"));
    a_newButton = new QToolButton(a_header);
    a_newButton->setObjectName(QStringLiteral("claudeHeaderButton"));
    a_newButton->setText(tr("New"));
    a_newButton->setToolTip(tr("Start a new conversation"));
    a_newButton->setAutoRaise(true);
    a_menuButton = new QToolButton(a_header);
    a_menuButton->setObjectName(QStringLiteral("claudeHeaderButton"));
    a_menuButton->setText(QStringLiteral("⋯"));
    a_menuButton->setToolTip(tr("Permissions, model, folder, program, export"));
    a_menuButton->setAutoRaise(true);
    a_menuButton->setPopupMode(QToolButton::InstantPopup);
    // (The dock's title bar names it: the state comes first.)
    a_title->hide();
    layout->addWidget(a_stateDot);
    layout->addWidget(a_stateText);
    layout->addStretch(1);
    layout->addWidget(a_modelLabel);
    layout->addWidget(a_newButton);
    layout->addWidget(a_menuButton);
    connect(a_newButton, &QToolButton::clicked, this, [this] {
        if (a_newInTab) {
            emit newConversationRequested();
            return;
        }
        if (a_session->isBusy()
            && QMessageBox::question(this, tr("Claude Code"), tr("Stop Claude and start a new conversation?"))
                   != QMessageBox::Yes)
            return;
        newConversation();
    });
}

void ClaudeCodePanel::buildPermissionCard()
{
    a_card = new QFrame(this);
    a_card->setObjectName(QStringLiteral("claudePermission"));
    auto* layout = new QVBoxLayout(a_card);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(6);
    auto* top = new QHBoxLayout;
    a_cardTitle = new QLabel(a_card);
    a_cardTitle->setObjectName(QStringLiteral("claudeCardTitle"));
    a_cardTitle->setWordWrap(true);
    a_cardCount = new QLabel(a_card);
    a_cardCount->setObjectName(QStringLiteral("claudeMuted"));
    top->addWidget(a_cardTitle, 1);
    top->addWidget(a_cardCount);
    layout->addLayout(top);
    a_cardSubject = new QLabel(a_card);
    a_cardSubject->setObjectName(QStringLiteral("claudeCardSubject"));
    a_cardSubject->setWordWrap(true);
    a_cardSubject->setTextFormat(Qt::PlainText);
    a_cardSubject->setTextInteractionFlags(Qt::TextSelectableByMouse);
    a_cardSubject->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    layout->addWidget(a_cardSubject);
    a_cardDetail = new QPlainTextEdit(a_card);
    a_cardDetail->setObjectName(QStringLiteral("claudeCardDetail"));
    a_cardDetail->setReadOnly(true);
    a_cardDetail->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    a_cardDetail->setLineWrapMode(QPlainTextEdit::NoWrap);
    layout->addWidget(a_cardDetail);
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(6);
    a_deny = new QToolButton(a_card);
    a_deny->setObjectName(QStringLiteral("claudeDeny"));
    a_deny->setText(tr("Deny"));
    a_deny->setToolTip(tr("Claude is told no and goes on without it"));
    a_allowEdits = new QToolButton(a_card);
    a_allowEdits->setObjectName(QStringLiteral("claudeAllowEdits"));
    a_allowEdits->setText(tr("Allow All Edits"));
    a_allowEdits->setToolTip(tr("Allow this, and file changes without asking for the rest of the conversation"));
    a_allowTools = new QToolButton(a_card);
    a_allowTools->setObjectName(QStringLiteral("claudeAllowTools"));
    a_allowTools->setText(tr("Allow Qucs-S Control"));
    a_allowTools->setToolTip(tr("Allow this, and Claude's use of the Qucs-S window without asking for the rest of the "
                                "conversation (each change can be undone)"));
    a_allow = new QToolButton(a_card);
    a_allow->setObjectName(QStringLiteral("claudeAllow"));
    a_allow->setText(tr("Allow"));
    buttons->addStretch(1);
    buttons->addWidget(a_deny);
    buttons->addWidget(a_allowEdits);
    buttons->addWidget(a_allowTools);
    buttons->addWidget(a_allow);
    layout->addLayout(buttons);
    connect(a_allow, &QToolButton::clicked, this, [this] { answer(true, false); });
    connect(a_allowEdits, &QToolButton::clicked, this, [this] { answer(true, true); });
    connect(a_allowTools, &QToolButton::clicked, this, [this] { answer(true, false, true); });
    connect(a_deny, &QToolButton::clicked, this, [this] { answer(false, false); });
    a_card->hide();
}

// ----------------------------------------------------------------------
// A question an MCP server asks the user (elicitation) - Qucs-S's own
// before a file is written over or unsaved changes lost: its message, and
// a button for each answer (yes and no, each choice) or a small form.

void ClaudeCodePanel::buildAskCard()
{
    a_askCard = new QFrame(this);
    a_askCard->setObjectName(QStringLiteral("claudePermission"));   // (the permission card's look)
    auto* layout = new QVBoxLayout(a_askCard);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(6);
    a_askCard->hide();
}

void ClaudeCodePanel::showNextAsk()
{
    auto* layout = static_cast<QVBoxLayout*>(a_askCard->layout());
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* w = item->widget()) w->deleteLater();
        if (QLayout* l = item->layout()) {
            while (QLayoutItem* inner = l->takeAt(0)) {
                if (inner->widget() != nullptr) inner->widget()->deleteLater();
                delete inner;
            }
        }
        delete item;
    }
    if (a_asks.isEmpty()) {
        a_askCard->hide();
        return;
    }
    const Ask ask = a_asks.first();
    auto* title = new QLabel(ask.message, a_askCard);
    title->setObjectName(QStringLiteral("claudeCardTitle"));
    title->setWordWrap(true);
    layout->addWidget(title);
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(6);
    buttons->addStretch(1);
    const auto button = [&](const QString& text, const QString& name, std::function<void()> act) {
        auto* b = new QToolButton(a_askCard);
        b->setText(text);
        b->setObjectName(name);
        connect(b, &QToolButton::clicked, this, act);
        buttons->addWidget(b);
        return b;
    };
    button(tr("Cancel"), QStringLiteral("claudeAskCancel"), [this] { answerAsk(QStringLiteral("cancel")); });
    const QJsonObject properties = ask.schema.value(QLatin1String("properties")).toObject();
    const QString only = properties.size() == 1 ? properties.begin().key() : QString();
    const QJsonObject field = properties.value(only).toObject();
    if (!only.isEmpty() && field.value(QLatin1String("type")).toString() == QLatin1String("boolean")) {
        // Yes or no.
        button(tr("No"), QStringLiteral("claudeAskNo"), [this, only] { answerAsk(QStringLiteral("accept"), {{only, false}}); });
        button(tr("Yes"), QStringLiteral("claudeAskYes"), [this, only] { answerAsk(QStringLiteral("accept"), {{only, true}}); });
    } else if (!only.isEmpty() && field.contains(QLatin1String("enum"))) {
        // One of these.
        for (const QJsonValue& v : field.value(QLatin1String("enum")).toArray()) {
            const QString option = v.toString();
            QToolButton* b = button(option, QStringLiteral("claudeAskOption"),
                                    [this, only, option] { answerAsk(QStringLiteral("accept"), {{only, option}}); });
            b->setProperty("option", option);
        }
    } else {
        // A form: a field for each property.
        auto* form = new QFormLayout;
        QHash<QString, QWidget*> editors;
        for (auto it = properties.begin(); it != properties.end(); ++it) {
            const QJsonObject p = it.value().toObject();
            const QString label = p.value(QLatin1String("title")).toString(it.key());
            QWidget* editor = nullptr;
            if (p.value(QLatin1String("type")).toString() == QLatin1String("boolean")) {
                auto* box = new QCheckBox(a_askCard);
                box->setChecked(p.value(QLatin1String("default")).toBool());
                editor = box;
            } else if (p.contains(QLatin1String("enum"))) {
                auto* combo = new QComboBox(a_askCard);
                for (const QJsonValue& v : p.value(QLatin1String("enum")).toArray()) combo->addItem(v.toString());
                editor = combo;
            } else {
                auto* edit = new QLineEdit(a_askCard);
                edit->setText(p.value(QLatin1String("default")).toVariant().toString());
                editor = edit;
            }
            editor->setObjectName(QStringLiteral("claudeAskField_") + it.key());
            editor->setToolTip(p.value(QLatin1String("description")).toString());
            form->addRow(label, editor);
            editors.insert(it.key(), editor);
        }
        layout->addLayout(form);
        button(tr("Submit"), QStringLiteral("claudeAskSubmit"), [this, editors, properties] {
            QJsonObject content;
            for (auto it = editors.cbegin(); it != editors.cend(); ++it) {
                const QString type = properties.value(it.key()).toObject().value(QLatin1String("type")).toString();
                if (auto* box = qobject_cast<QCheckBox*>(it.value())) content.insert(it.key(), box->isChecked());
                else if (auto* combo = qobject_cast<QComboBox*>(it.value())) content.insert(it.key(), combo->currentText());
                else if (auto* edit = qobject_cast<QLineEdit*>(it.value())) {
                    if (type == QLatin1String("number") || type == QLatin1String("integer")) content.insert(it.key(), edit->text().toDouble());
                    else content.insert(it.key(), edit->text());
                }
            }
            answerAsk(QStringLiteral("accept"), content);
        });
    }
    layout->addLayout(buttons);
    a_askCard->show();
}

void ClaudeCodePanel::answerAsk(const QString& action, const QJsonObject& content)
{
    if (a_asks.isEmpty()) return;
    const Ask ask = a_asks.takeFirst();
    a_session->answerElicitation(ask.id, action, content);
    QTimer::singleShot(0, this, &ClaudeCodePanel::showNextAsk);   // (not under the button pressed)
}

void ClaudeCodePanel::buildComposer()
{
    a_composer = new QFrame(this);
    a_composer->setObjectName(QStringLiteral("claudeComposer"));
    auto* layout = new QVBoxLayout(a_composer);
    layout->setContentsMargins(8, 6, 6, 6);
    layout->setSpacing(4);
    a_input = new QPlainTextEdit(a_composer);
    a_input->setObjectName(QStringLiteral("claudeInput"));
    a_input->setFrameShape(QFrame::NoFrame);
    a_input->setPlaceholderText(tr("Ask Claude about your circuits, simulations or files…"));
    a_input->setTabChangesFocus(true);
    a_input->installEventFilter(this);
    layout->addWidget(a_input);
    auto* row = new QHBoxLayout;
    row->setSpacing(6);
    a_attach = new QToolButton(a_composer);
    a_attach->setObjectName(QStringLiteral("claudeAttach"));
    a_attach->setCheckable(true);
    a_attach->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    a_attach->setIcon(style()->standardIcon(QStyle::SP_FileIcon));
    a_attach->setIconSize(QSize(12, 12));
    a_attach->setToolTip(tr("Tell Claude which document is open in Qucs-S"));
    a_pin = new QToolButton(a_composer);
    a_pin->setObjectName(QStringLiteral("claudePin"));
    a_pin->setCheckable(true);
    a_pin->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    a_pin->setIconSize(QSize(12, 12));
    a_hint = new QLabel(tr("⏎ send  ·  ⇧⏎ new line"), a_composer);
    a_hint->setObjectName(QStringLiteral("claudeMuted"));
    a_send = new QToolButton(a_composer);
    a_send->setObjectName(QStringLiteral("claudeSend"));
    a_send->setText(tr("Send"));
    row->addWidget(a_attach);
    row->addWidget(a_pin);
    row->addStretch(1);
    row->addWidget(a_hint);
    row->addWidget(a_send);
    layout->addLayout(row);
    connect(a_input, &QPlainTextEdit::textChanged, this, &ClaudeCodePanel::updateComposer);
    connect(a_attach, &QToolButton::toggled, this, [](bool on) { QucsSettingsFile().setValue(kAttach, on); });
    connect(a_pin, &QToolButton::clicked, this, [this] { pinDocument(a_pinned.isEmpty() ? pinnableDocument() : QString()); });
    connect(a_send, &QToolButton::clicked, this, [this] {
        if (a_session->isBusy()) stopTurn();
        else sendComposer();
    });
}

void ClaudeCodePanel::buildMenu()
{
    a_menu = new QMenu(this);
    QMenu* modes = a_menu->addMenu(tr("Permissions"));
    a_modes = new QActionGroup(this);
    const struct {
        const char* label;
        const char* mode;
        const char* tip;
    } modeList[] = {
        {QT_TR_NOOP("Ask Before Acting"), "", QT_TR_NOOP("Claude asks before it runs a command or changes a file")},
        {QT_TR_NOOP("Accept Edits"), "acceptEdits", QT_TR_NOOP("Claude changes files without asking; commands still need permission")},
        {QT_TR_NOOP("Auto"), "auto", QT_TR_NOOP("Claude acts without asking; a safety check reviews each action first and blocks risky ones")},
        {QT_TR_NOOP("Plan Only"), "plan", QT_TR_NOOP("Claude reads and plans, and changes nothing")},
        {QT_TR_NOOP("Bypass Permissions"), "bypassPermissions", QT_TR_NOOP("Claude does anything without asking")},
    };
    for (const auto& m : modeList) {
        QAction* a = modes->addAction(tr(m.label));
        a->setCheckable(true);
        a->setData(QString::fromLatin1(m.mode));
        a->setStatusTip(tr(m.tip));
        a->setToolTip(tr(m.tip));
        a_modes->addAction(a);
    }
    modes->setToolTipsVisible(true);
    connect(a_modes, &QActionGroup::triggered, this, [this](QAction* a) {
        const QString mode = a->data().toString();
        if (mode == QLatin1String("bypassPermissions")
            && QMessageBox::warning(this, tr("Claude Code"),
                                    tr("Claude will run any command and change any file in %1 without asking. Go on?")
                                        .arg(QDir::toNativeSeparators(workingDirectory())),
                                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
                   != QMessageBox::Yes) {
            updateState();   // the check back where it was
            return;
        }
        setPermissionMode(mode);
    });

    a_modelMenu = a_menu->addMenu(tr("Model"));
    a_modelMenu->setToolTipsVisible(true);
    a_models = new QActionGroup(this);
    connect(a_models, &QActionGroup::triggered, this, [this](QAction* a) {
        QString model = a->data().toString();
        if (model == QLatin1String("*")) {
            bool ok = false;
            model = QInputDialog::getText(this, tr("Claude Code"),
                                          tr("The model's full name (claude-opus-5-5, say) or an alias (opus):"),
                                          QLineEdit::Normal, a_session->model(), &ok)
                        .trimmed();
            if (!ok) {
                updateState();
                return;
            }
            if (!model.isEmpty() && choiceFor(model) == nullptr) {
                // Kept in the menu, the latest first.
                QucsSettingsFile settings;
                QStringList others = settings.value(kOtherModels).toStringList();
                others.removeAll(model);
                others.prepend(model);
                settings.setValue(kOtherModels, others.mid(0, 5));
                QTimer::singleShot(0, this, &ClaudeCodePanel::rebuildModelMenu);   // not under the action
            }
        }
        setModel(model);
    });
    // Asked again when the menu opens: a program installed meanwhile.
    connect(a_modelMenu, &QMenu::aboutToShow, this, &ClaudeCodePanel::listModels);

    a_menu->addSeparator();
    a_menu->addAction(tr("Choose Folder…"), a_dirButton, &QToolButton::click);
    QAction* workspace = a_menu->addAction(tr("Use the Workspace Folder"), a_dirReset, &QToolButton::click);
    QAction* show = a_menu->addAction(tr("Show the Folder"), this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(workingDirectory()));
    });
    show->setObjectName(QStringLiteral("claudeShowFolder"));   // (the GUI monkey leaves it alone)
    QAction* git = a_menu->addAction(tr("Show Git Status"));
    git->setObjectName(QStringLiteral("claudeGitStatus"));
    git->setCheckable(true);
    git->setToolTip(tr("When the project (or the folder Claude works in) is in a git repository, a bar above the "
                       "prompt shows its branch and the lines changed, and offers to create a pull request - in "
                       "every conversation"));
    connect(git, &QAction::triggered, this, [](bool on) { ClaudeGitBar::setOn(on); });
    // What the line that ends each turn tells of what it took.
    QMenu* usage = a_menu->addMenu(tr("Show Usage"));
    usage->setObjectName(QStringLiteral("claudeUsageMenu"));
    usage->setToolTipsVisible(true);
    const struct {
        Usage flag;
        const char* name;
        const char* label;
        const char* tip;
    } usages[] = {
        {PromptTokens, "claudeShowPromptTokens", QT_TR_NOOP("Tokens of Each Prompt"),
         QT_TR_NOOP("The tokens the models read and wrote for the prompt: its input, the context read from the "
                    "cache and written to it, and Claude's output (a tooltip tells each)")},
        {ConversationTokens, "claudeShowConversationTokens", QT_TR_NOOP("Tokens of the Conversation"),
         QT_TR_NOOP("The tokens of all the conversation's prompts so far")},
        {PromptCost, "claudeShowPromptCost", QT_TR_NOOP("Cost of Each Prompt"),
         QT_TR_NOOP("What the prompt cost in US dollars, as Claude Code estimates it at the models' prices")},
        {ConversationCost, "claudeShowConversationCost", QT_TR_NOOP("Cost of the Conversation"),
         QT_TR_NOOP("What all the conversation's prompts have cost so far, as Claude Code estimates it")},
    };
    QList<QAction*> usageActions;
    for (const auto& u : usages) {
        QAction* a = usage->addAction(tr(u.label));
        a->setObjectName(QLatin1String(u.name));
        a->setCheckable(true);
        a->setData(int(u.flag));
        a->setChecked(usageShown() & u.flag);
        a->setToolTip(tr(u.tip));
        connect(a, &QAction::triggered, this, [flag = int(u.flag)](bool on) {
            setUsageShown(on ? usageShown() | flag : usageShown() & ~flag);
        });
        usageActions << a;
    }
    connect(usage, &QMenu::aboutToShow, this, [usageActions] {
        const int shown = usageShown();   // (another conversation may have changed it)
        for (QAction* a : usageActions) a->setChecked(shown & a->data().toInt());
    });
    a_menu->addSeparator();
    a_pinMenu = a_menu->addMenu(tr("Pin to a Schematic"));
    a_pinMenu->setObjectName(QStringLiteral("claudePinMenu"));
    a_pinMenu->setToolTipsVisible(true);
    connect(a_pinMenu, &QMenu::aboutToShow, this, &ClaudeCodePanel::fillPinMenu);
    a_menu->addSeparator();
    QAction* resume = a_menu->addAction(tr("Resume a Conversation…"), this, [this] { emit resumeRequested(QString()); });
    resume->setObjectName(QStringLiteral("claudeResume"));
    QAction* reopen = a_menu->addAction(tr("Reopen Conversations at Start"));
    reopen->setObjectName(QStringLiteral("claudeReopen"));
    reopen->setCheckable(true);
    reopen->setToolTip(tr("The conversations open when Qucs-S closes come back when it opens again, each going on "
                          "where it was"));
    connect(reopen, &QAction::triggered, this, [](bool on) { qucs_s::claude::history::setReopenAtStart(on); });
    connect(a_menu, &QMenu::aboutToShow, this, [reopen] { reopen->setChecked(qucs_s::claude::history::reopenAtStart()); });
    a_menu->addSeparator();
    a_menu->addAction(tr("Claude Program…"), this, [this] {
        const QString program = QFileDialog::getOpenFileName(this, tr("The Claude Code Program"),
                                                             QFileInfo(a_session->program()).absolutePath());
        if (program.isEmpty()) return;
        QucsSettingsFile().setValue(kProgram, program);
        findProgram();
        if (a_session->program().isEmpty())
            append({Entry::Problem, tr("%1 cannot be run.").arg(QDir::toNativeSeparators(program)), {}, {}});
    });
    QAction* again = a_menu->addAction(tr("Look for the Program Again"), this, [this] {
        QucsSettingsFile().remove(kProgram);
        askedPrograms().remove(a_session->program());   // (it may have been updated)
        findProgram();
        addNote(a_session->program().isEmpty() ? tr("Claude Code was not found.")
                                                : tr("Using %1.").arg(QDir::toNativeSeparators(a_session->program())));
    });
    a_menu->addSeparator();
    QMenu* exports = a_menu->addMenu(tr("Export Conversation"));
    exports->setObjectName(QStringLiteral("claudeExport"));
    exports->addAction(tr("PDF…"), this, [this] { exportConversationAs(ExportFormat::Pdf); });
    exports->addAction(tr("Markdown…"), this, [this] { exportConversationAs(ExportFormat::Markdown); });
    exports->addAction(tr("Plain Text…"), this, [this] { exportConversationAs(ExportFormat::Text); });
    exports->addSeparator();
    QAction* details = exports->addAction(tr("Include Tool Details"));
    details->setObjectName(QStringLiteral("claudeExportToolDetails"));
    details->setCheckable(true);
    details->setChecked(exportsToolDetails());
    details->setToolTip(tr("Each tool's input and what it gave, as the boxes that fold hold them. Off, a row "
                           "of tools is the one line that sums it up, and the export is the chat alone."));
    exports->setToolTipsVisible(true);
    connect(details, &QAction::triggered, this, [](bool on) { setExportsToolDetails(on); });
    QAction* copyId = a_menu->addAction(tr("Copy Session ID"), this, [this] {
        QApplication::clipboard()->setText(a_session->sessionId());
    });
    connect(a_menu, &QMenu::aboutToShow, this, [this, workspace, show, git, copyId, again, exports, details] {
        exports->menuAction()->setEnabled(!a_entries.isEmpty());
        git->setChecked(ClaudeGitBar::isOn());
        fillPinMenu();
        details->setChecked(exportsToolDetails());   // (another conversation may have changed it)
        workspace->setEnabled(!a_chosenDir.isEmpty());
        show->setEnabled(QFileInfo(workingDirectory()).isDir());
        copyId->setEnabled(!a_session->sessionId().isEmpty());
        again->setEnabled(!QucsSettingsFile().value(kProgram).toString().isEmpty() || a_session->program().isEmpty());
    });
    a_menuButton->setMenu(a_menu);
}

// ----------------------------------------------------------------------
void ClaudeCodePanel::restyle()
{
    using qucs_s::apptheme::mix;
    const Colours c = colours(palette());
    const QColor window = palette().color(QPalette::Window);
    const QColor windowText = palette().color(QPalette::WindowText);
    const QColor hover = mix(window, windowText, 0.10);
    const QString sheet = QStringLiteral(
        "QWidget#claudeHeader { background: %1; border-bottom: 1px solid %2; }"
        "QLabel#claudeTitle { font-weight: 600; }"
        "QLabel#claudeModel, QLabel#claudeMuted, QLabel#claudeDir { color: %3; }"
        "QToolButton#claudeHeaderButton, QToolButton#claudeLink { border: 1px solid transparent; border-radius: 5px;"
        " padding: 2px 7px; background: transparent; }"
        "QToolButton#claudeHeaderButton:hover, QToolButton#claudeLink:hover { background: %4; }"
        "QToolButton#claudeHeaderButton::menu-indicator { image: none; width: 0; }"
        "QToolButton#claudeLink { color: %5; }"
        "QWidget#claudeDirRow { background: %1; border-bottom: 1px solid %2; }"
        "QTextBrowser#claudeTranscript { background: %6; }"
        "QFrame#claudeComposer { background: %6; border: 1px solid %2; border-radius: 10px; }"
        "QFrame#claudeComposer[focused=\"true\"] { border-color: %5; }"
        "QPlainTextEdit#claudeInput { background: transparent; border: none; color: %7; }"
        "QToolButton#claudeSend { background: %5; color: %8; border: none; border-radius: 7px;"
        " padding: 4px 14px; font-weight: 600; }"
        "QToolButton#claudeSend:hover { background: %9; }"
        "QToolButton#claudeSend:disabled { background: %2; color: %3; }"
        "QToolButton#claudeSend[stop=\"true\"] { background: %10; color: %7; }"
        "QToolButton#claudeAttach { border: 1px solid %2; border-radius: 9px; padding: 1px 8px; color: %3;"
        " background: transparent; }"
        "QToolButton#claudeAttach:checked { background: %11; border-color: %5; color: %7; }"
        "QToolButton#claudeAttach:disabled { color: %12; }"
        "QListWidget#claudeCommands { background: %6; border: 1px solid %2; border-radius: 8px; color: %7; padding: 2px; }"
        "QListWidget#claudeCommands::item { padding: 3px 6px; border-radius: 5px; }"
        "QListWidget#claudeCommands::item:selected { background: %11; color: %7; }"
        "QToolButton#claudePin { border: 1px solid transparent; border-radius: 9px; padding: 1px 2px; background: transparent; }"
        "QToolButton#claudePin:hover { border-color: %2; }"
        "QToolButton#claudePin[pinned=\"true\"] { border-color: %5; background: %5; color: %8; padding: 1px 8px;"
        " font-weight: 600; }"
        "QToolButton#claudePin[pinned=\"true\"]:hover { background: %9; }"
        "QFrame#claudePermission { background: %11; border: 1px solid %5; border-radius: 10px; }"
        "QFrame#claudeEditBar { background: %11; border: 1px solid %2; border-radius: 8px; }"
        "QLabel#claudeCardTitle { font-weight: 600; }"
        "QPlainTextEdit#claudeCardDetail { background: %13; border: 1px solid %2; border-radius: 6px; color: %7; }"
        "QToolButton#claudeAllow { background: %5; color: %8; border: none; border-radius: 7px; padding: 4px 14px;"
        " font-weight: 600; }"
        "QToolButton#claudeAllow:hover { background: %9; }"
        "QToolButton#claudeDeny, QToolButton#claudeAllowEdits, QToolButton#claudeAllowTools { background: %6; color: %7;"
        " border: 1px solid %2; border-radius: 7px; padding: 4px 12px; }"
        "QToolButton#claudeDeny:hover, QToolButton#claudeAllowEdits:hover, QToolButton#claudeAllowTools:hover"
        " { background: %4; }")
                      .arg(window.name(), c.border.name(), c.muted.name(), hover.name(), c.accent.name(),
                           c.base.name(), c.text.name(), c.onAccent.name(), c.accent.darker(112).name())
                      .arg(mix(c.base, c.text, 0.14).name(), c.bubble.name(), c.faint.name(), c.code.name());
    // Only when it changes: a style sheet set polishes every child again.
    if (sheet != styleSheet()) setStyleSheet(sheet);
    scheduleRender();
    updateState();
}

void ClaudeCodePanel::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange) {
        // Not from our own style sheet, which does not change the palette.
        QTimer::singleShot(0, this, &ClaudeCodePanel::restyle);
    }
}

void ClaudeCodePanel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    refreshDocument();
    if (a_session->program().isEmpty()) findProgram();
    listModels();
}

bool ClaudeCodePanel::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == a_dirLabel && event->type() == QEvent::Resize) {
        updateDirectory();
        return false;
    }
    if (watched == a_input) {
        if (event->type() == QEvent::KeyPress && a_commandList->isVisible()) {
            // The commands offered: chosen with the arrows, put in with Tab,
            // run with Enter; Esc puts them away.
            auto* key = static_cast<QKeyEvent*>(event);
            const int rows = a_commandList->count();
            switch (key->key()) {
            case Qt::Key_Down:
            case Qt::Key_Up: {
                const int step = key->key() == Qt::Key_Down ? 1 : -1;
                a_commandList->setCurrentRow((a_commandList->currentRow() + step + rows) % rows);
                return true;
            }
            case Qt::Key_Tab:
                completeCommand(false);
                return true;
            case Qt::Key_Return:
            case Qt::Key_Enter:
                if (key->modifiers() & (Qt::ShiftModifier | Qt::AltModifier)) break;
                completeCommand(true);
                return true;
            case Qt::Key_Escape:
                a_commandList->hide();
                return true;
            default:
                break;
            }
        }
        if (event->type() == QEvent::KeyPress) {
            auto* key = static_cast<QKeyEvent*>(event);
            if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
                && !(key->modifiers() & (Qt::ShiftModifier | Qt::AltModifier))) {
                if (!a_session->isBusy()) sendComposer();
                return true;
            }
            if (key->key() == Qt::Key_Escape && a_session->isBusy()) {
                stopTurn();
                return true;
            }
            if (key->key() == Qt::Key_Escape && a_editing >= 0) {
                cancelEdit();
                return true;
            }
        } else if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut) {
            a_composer->setProperty("focused", event->type() == QEvent::FocusIn);
            a_composer->style()->unpolish(a_composer);
            a_composer->style()->polish(a_composer);
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ClaudeCodePanel::reloadSettings()
{
    const QucsSettingsFile settings;
    a_attach->setChecked(settings.value(kAttach, true).toBool());
    if (!hasConversation() && !a_session->isRunning()) {
        a_session->setPermissionMode(settings.value(kMode).toString());
        a_session->setModel(settings.value(kModel).toString());
    }
    rebuildModelMenu();
    findProgram();
}

// ----------------------------------------------------------------------
QString ClaudeCodePanel::programSetting() const
{
    return QucsSettingsFile().value(kProgram).toString();
}

void ClaudeCodePanel::findProgram()
{
    // QUCS_CLAUDE names the program over the settings: the tests point it
    // nowhere, so that nothing they do reaches a claude installed here.
    const QString forced = qEnvironmentVariable("QUCS_CLAUDE");
    a_session->setProgram(qucs_s::claude::findProgram(forced.isEmpty() ? programSetting() : forced));
    updateState();
    if (isVisible()) listModels();
}

void ClaudeCodePanel::listModels()
{
    // What another panel's program said meanwhile.
    const QJsonArray kept = QJsonDocument::fromJson(QucsSettingsFile().value(kModels).toString().toUtf8()).array();
    if (!kept.isEmpty() && kept != a_listedModels) {
        a_listedModels = kept;
        rebuildModelMenu();
    }
    const QString program = a_session->program();
    if (program.isEmpty() || askedPrograms().contains(program) || a_modelQuery->isRunning()) return;
    if (!QFileInfo(program).isExecutable()) return;   // a program named, not found
    askedPrograms().insert(program);
    a_modelQuery->start(program, workingDirectory());
}

const ModelChoice* ClaudeCodePanel::choiceFor(const QString& model) const
{
    for (const ModelChoice& c : a_choices)
        if (c.value == model) return &c;
    return nullptr;
}

// What the program offers (as it last said), the default first; then the
// newest models it does not offer, and those chosen by name; then Other.
void ClaudeCodePanel::rebuildModelMenu()
{
    a_choices = qucs_s::claude::modelChoices(a_listedModels);
    a_modelMenu->clear();   // and out of the group, as they go
    const auto add = [this](const QString& text, const QString& value, const QString& tip) {
        QAction* a = a_modelMenu->addAction(text);
        a->setCheckable(true);
        a->setData(value);
        a->setToolTip(tip);
        a->setStatusTip(QString(tip).replace(QLatin1Char('\n'), QStringLiteral(" \u2014 ")));
        a_models->addAction(a);
    };
    const auto tipOf = [](const ModelChoice& c) {
        return c.resolved.isEmpty() || c.resolved == c.description ? c.description
                                                                   : c.description + QLatin1Char('\n') + c.resolved;
    };
    bool apart = false;
    for (const ModelChoice& c : a_choices) {
        if (!c.listed && !c.value.isEmpty() && !apart) {
            a_modelMenu->addSeparator();
            apart = true;
        }
        add(c.name, c.value, tipOf(c));
    }
    QStringList others = QucsSettingsFile().value(kOtherModels).toStringList();
    if (!a_session->model().isEmpty()) others.prepend(a_session->model());
    others.removeDuplicates();
    for (const QString& model : std::as_const(others)) {
        if (choiceFor(model) != nullptr) continue;
        if (!apart) {
            a_modelMenu->addSeparator();
            apart = true;
        }
        add(modelName(model), model, model);
    }
    a_modelMenu->addSeparator();
    QAction* other = a_modelMenu->addAction(tr("Other…"));
    other->setCheckable(true);
    other->setData(QStringLiteral("*"));
    other->setToolTip(tr("A model by its name"));
    a_models->addAction(other);
    updateState();
}

QList<QAction*> ClaudeCodePanel::modelActions() const
{
    return a_models->actions();
}

QList<QAction*> ClaudeCodePanel::permissionActions() const
{
    return a_modes->actions();
}

void ClaudeCodePanel::setPermissionMode(const QString& mode)
{
    QucsSettingsFile().setValue(kMode, mode);
    const bool told = a_session->isRunning() && !a_entries.isEmpty() && mode != a_session->permissionMode();
    a_session->setPermissionMode(mode);
    if (told) {
        QString name = a_modes->checkedAction() != nullptr ? a_modes->checkedAction()->text() : mode;
        addNote(tr("Permissions: %1, from the next prompt on.").arg(name.remove(QLatin1Char('&'))));
    }
    if (a_entries.isEmpty()) scheduleRender();   // the welcome says what Claude may do
    updateState();
}

void ClaudeCodePanel::setModel(const QString& model)
{
    QucsSettingsFile().setValue(kModel, model);
    const bool told = a_session->isRunning() && !a_entries.isEmpty() && model != a_session->model();
    a_session->setModel(model);
    if (told)
        addNote(model.isEmpty() ? tr("The default model from the next prompt on.")
                                : tr("%1 from the next prompt on.").arg(modelName(model)));
    updateState();
}

void ClaudeCodePanel::setDefaultDirectory(const QString& dir)
{
    const QString clean = QDir::cleanPath(dir);
    if (clean == a_defaultDir) return;
    a_defaultDir = clean;
    if (!a_chosenDir.isEmpty()) {
        updateDirectory();
        return;
    }
    const bool talking = !a_entries.isEmpty();
    a_session->setWorkingDirectory(workingDirectory());
    forgetEdit();
    a_entries.clear();
    a_requests.clear();
    showNextRequest();
    if (talking) addNote(tr("The workspace is now %1: a new conversation.").arg(QDir::toNativeSeparators(clean)));
    updateDirectory();
    scheduleRender();
}

QString ClaudeCodePanel::workingDirectory() const
{
    return a_chosenDir.isEmpty() ? a_defaultDir : a_chosenDir;
}

void ClaudeCodePanel::setWorkingDirectory(const QString& dir)
{
    const QString clean = dir.isEmpty() ? QString() : QDir::cleanPath(dir);
    a_chosenDir = clean == a_defaultDir ? QString() : clean;
    if (a_session->workingDirectory() == workingDirectory()) {
        updateDirectory();
        return;
    }
    a_session->setWorkingDirectory(workingDirectory());
    forgetEdit();
    a_entries.clear();
    a_requests.clear();
    showNextRequest();
    updateDirectory();
    updateState();
    scheduleRender();
}

void ClaudeCodePanel::updateDirectory()
{
    const QString dir = QDir::toNativeSeparators(workingDirectory());
    a_dirLabel->setText(a_dirLabel->fontMetrics().elidedText(shownPath(workingDirectory()), Qt::ElideMiddle,
                                                             std::max(80, a_dirLabel->width())));
    a_dirLabel->setToolTip(a_chosenDir.isEmpty() ? tr("Claude works in the workspace folder, %1").arg(dir)
                                                 : tr("Claude works in %1").arg(dir));
    a_dirReset->setVisible(!a_chosenDir.isEmpty());
    if (a_gitBar != nullptr) a_gitBar->setDirectory(gitDirectory());
}

void ClaudeCodePanel::setProjectDirectory(const QString& dir)
{
    const QString clean = dir.isEmpty() ? QString() : QDir::cleanPath(dir);
    if (clean == a_projectDir) return;
    a_projectDir = clean;
    updateDirectory();
}

QString ClaudeCodePanel::gitDirectory() const
{
    if (!a_chosenDir.isEmpty()) return a_chosenDir;
    return a_projectDir.isEmpty() ? a_defaultDir : a_projectDir;
}

void ClaudeCodePanel::setDocumentProvider(std::function<QString()> provider)
{
    a_document = std::move(provider);
    refreshDocument();
}

void ClaudeCodePanel::refreshDocument()
{
    const Colours col = colours(palette());
    // Pinned: the pin, named, says so (the document in front is not told).
    const bool pinned = !a_pinned.isEmpty();
    a_attach->setVisible(!pinned);
    if (a_pin->property("pinned").toBool() != pinned) {
        a_pin->setProperty("pinned", pinned);
        a_pin->style()->unpolish(a_pin);
        a_pin->style()->polish(a_pin);
    }
    a_pin->setChecked(pinned);
    if (pinned) {
        const QStringList open = a_schematics ? a_schematics() : QStringList();
        const bool isOpen = std::any_of(open.cbegin(), open.cend(), [this](const QString& f) { return sameFile(f, a_pinned); });
        a_pin->setText(shownName(a_pinned));
        a_pin->setIcon(pinIcon(col.onAccent));
        a_pin->setEnabled(true);
        a_pin->setToolTip(tr("This conversation is pinned to %1%2: its prompts name it, and Qucs-S's tools act on it "
                             "when Claude names no document, whichever document is in front. Click to unpin it.")
                              .arg(QDir::toNativeSeparators(a_pinned), isOpen ? QString() : tr(" (not open now; Claude can open it)")));
        return;
    }
    const QString doc = a_document ? a_document() : QString();
    if (doc.isEmpty()) {
        a_attach->setText(tr("No document"));
        a_attach->setEnabled(false);
        a_attach->setToolTip(tr("No document is open (or it has no file yet)"));
    } else {
        a_attach->setText(shownName(doc));
        a_attach->setEnabled(true);
        a_attach->setToolTip(tr("Tell Claude that %1 is open in Qucs-S").arg(QDir::toNativeSeparators(doc)));
    }
    const QString pinnable = pinnableDocument();
    a_pin->setText(QString());
    a_pin->setIcon(pinIcon(pinnable.isEmpty() ? col.border : col.faint));
    a_pin->setEnabled(!pinnable.isEmpty());
    a_pin->setToolTip(pinnable.isEmpty()
                          ? tr("Pin a schematic to this conversation: a saved one in front, or any open one from the "
                               "menu (⋯ > Pin to a Schematic)")
                          : tr("Pin %1 to this conversation: its prompts name it, and Qucs-S's tools act on it when "
                               "Claude names no document, whichever document is in front")
                                .arg(QFileInfo(pinnable).fileName()));
}

void ClaudeCodePanel::setSchematicsProvider(std::function<QStringList()> provider)
{
    a_schematics = std::move(provider);
    refreshDocument();
}

QString ClaudeCodePanel::pinnableDocument() const
{
    const QString front = a_document ? a_document() : QString();
    if (front.isEmpty() || !a_schematics) return {};
    for (const QString& file : a_schematics())
        if (sameFile(file, front)) return file;
    return {};
}

void ClaudeCodePanel::pinDocument(const QString& path)
{
    const QString file = path.isEmpty() ? QString() : QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    if (file == a_pinned) return;
    a_pinned = file;
    a_session->setDocument(file);
    refreshDocument();
    emit pinChanged();
}

bool ClaudeCodePanel::isPinnedTo(const QString& path) const
{
    return sameFile(a_pinned, path);
}

void ClaudeCodePanel::fillPinMenu()
{
    a_pinMenu->clear();
    const QStringList open = a_schematics ? a_schematics() : QStringList();
    bool listed = false;
    for (const QString& file : open) {
        const QString name = QFileInfo(file).fileName();
        const bool twice = std::count_if(open.cbegin(), open.cend(), [&name](const QString& f) {
                               return QFileInfo(f).fileName() == name;
                           }) > 1;
        const QString shown = shownName(file);
        QAction* a = a_pinMenu->addAction(
            twice ? shown + QStringLiteral("  —  ") + QDir::toNativeSeparators(QFileInfo(file).absolutePath()).replace(QLatin1Char('&'), QLatin1String("&&"))
                  : shown,
            this, [this, file] { pinDocument(file); });
        a->setCheckable(true);
        a->setChecked(sameFile(file, a_pinned));
        a->setToolTip(QDir::toNativeSeparators(file));
        listed = listed || a->isChecked();
    }
    if (!a_pinned.isEmpty() && !listed) {
        QAction* a = a_pinMenu->addAction(tr("%1 (not open)").arg(shownName(a_pinned)));
        a->setCheckable(true);
        a->setChecked(true);
        a->setToolTip(QDir::toNativeSeparators(a_pinned));
    }
    if (open.isEmpty() && a_pinned.isEmpty()) a_pinMenu->addAction(tr("No saved schematic is open"))->setEnabled(false);
    a_pinMenu->addSeparator();
    QAction* unpin = a_pinMenu->addAction(tr("Unpin"), this, [this] { pinDocument(QString()); });
    unpin->setObjectName(QStringLiteral("claudeUnpin"));
    unpin->setEnabled(!a_pinned.isEmpty());
}

// ----------------------------------------------------------------------
void ClaudeCodePanel::updateState()
{
    const Colours c = colours(palette());
    const State s = a_session->state();
    const bool busy = a_session->isBusy();
    if (a_gitBar != nullptr) a_gitBar->setBusy(busy);
    QString text = qucs_s::claude::stateText(s);
    QColor colour = c.faint;
    switch (s) {
    case State::Starting:
    case State::Thinking: colour = c.accent; break;
    case State::Working:
        colour = c.accent;
        if (!a_session->detail().isEmpty()) text = tr("running %1").arg(a_session->detail());
        break;
    case State::Waiting:
        colour = c.warn;
        text = tr("needs your permission");
        break;
    case State::Ready: colour = c.ok; break;
    case State::Failed: colour = c.error; break;
    case State::Off:
        text = a_session->program().isEmpty() ? qucs_s::claude::stateText(State::NotFound) : tr("ready to start");
        break;
    case State::NotFound: colour = c.error; break;
    }
    if (busy && a_session->turnElapsed() >= 1000)
        text += QStringLiteral("  ·  ") + seconds(a_session->turnElapsed());
    a_stateDot->setPixmap(dot(colour, busy && s != State::Waiting, devicePixelRatioF()));
    a_stateText->setText(text);
    a_stateText->setStyleSheet(QStringLiteral("color: %1;").arg(s == State::Off ? c.muted.name() : colour.name()));
    a_stateText->setToolTip(s == State::Failed ? a_session->detail() : QString());
    if (busy) {
        if (!a_clock->isActive()) a_clock->start();
    } else {
        a_clock->stop();
    }

    // The model: the one in use, else the one asked for; and the mode, when
    // Claude does not ask before it acts - the one the program works in,
    // which is not the one asked for when the model has not got it.
    const ModelChoice* choice = choiceFor(a_session->model());
    QString model = modelName(a_session->modelInUse());
    if (model.isEmpty() && choice != nullptr) model = modelName(choice->resolved);
    if (model.isEmpty()) model = modelName(a_session->model());
    const QString inUse = a_session->permissionModeInUse();
    QString mode = a_session->isRunning() && !inUse.isEmpty() ? inUse : a_session->permissionMode();
    if (mode == QLatin1String("auto") && choice != nullptr && !choice->autoMode) mode.clear();   // it will ask
    const QString tag = modeTag(mode);
    a_modelLabel->setText(tag.isEmpty() ? model : model.isEmpty() ? tag : model + QStringLiteral(" \u00b7 ") + tag);
    QStringList about;
    if (!a_session->version().isEmpty()) about << tr("Claude Code %1").arg(a_session->version());
    for (QAction* a : a_modes->actions())
        if (a->data().toString() == mode || (a->data().toString().isEmpty() && qucs_s::claude::isAskMode(mode)))
            about << tr("Permissions: %1").arg(a->text().remove(QLatin1Char('&')));
    a_modelLabel->setToolTip(about.join(QLatin1Char('\n')));

    // Auto mode is not in every model.
    const QString modelShown = choice != nullptr ? choice->name : modelName(a_session->model());
    for (QAction* a : a_modes->actions()) {
        a->setChecked(a->data().toString() == a_session->permissionMode());
        if (a->data().toString() != QLatin1String("auto")) continue;
        const bool can = choice == nullptr || choice->autoMode;
        a->setEnabled(can);
        a->setToolTip(can ? tr("Claude acts without asking; a safety check reviews each action first and blocks risky ones")
                          : tr("Not available with %1").arg(modelShown));
    }
    bool known = false;
    for (QAction* a : a_models->actions()) {
        const bool match = a->data().toString() == a_session->model();
        a->setChecked(match);
        known = known || match;
    }
    if (!known)
        for (QAction* a : a_models->actions())
            if (a->data().toString() == QLatin1String("*")) a->setChecked(true);
    updateComposer();
}

void ClaudeCodePanel::updateComposer()
{
    const bool busy = a_session->isBusy();
    a_send->setText(busy ? tr("Stop") : tr("Send"));
    a_send->setToolTip(busy ? tr("Stop Claude (Esc)") : tr("Send (Enter)"));
    a_send->setProperty("stop", busy);
    a_send->setEnabled(busy || !a_input->toPlainText().trimmed().isEmpty());
    a_send->style()->unpolish(a_send);
    a_send->style()->polish(a_send);
    // Two lines to eight, as the text needs.
    const int lines = std::clamp(int(a_input->document()->size().height()), 2, 8);
    const int height = lines * a_input->fontMetrics().lineSpacing() + 2 * int(a_input->document()->documentMargin())
                       + a_input->contentsMargins().top() + a_input->contentsMargins().bottom() + 2;
    if (a_input->height() != height) a_input->setFixedHeight(height);
}

// ----------------------------------------------------------------------
void ClaudeCodePanel::focusComposer()
{
    a_input->setFocus(Qt::OtherFocusReason);
}

void ClaudeCodePanel::sendComposer()
{
    if (sendText(a_input->toPlainText().trimmed(), true)) a_input->clear();
}

bool ClaudeCodePanel::sendPrompt(const QString& text)
{
    if (a_editing >= 0) cancelEdit();   // (the git bar's: not the prompt edited)
    return sendText(text.trimmed(), false);
}

ClaudeCodePanel::Entry ClaudeCodePanel::promptEntry(const QString& text, const QString& extra)
{
    Entry e{Entry::You, text, extra, {}};
    if (a_editing >= 0) {
        // Sent in the place of one edited: the conversation goes back to
        // where that one stood. The entries set aside stay until the program
        // starts on it (or put back, when it cannot).
        const Entry& edited = a_setAside.constFirst();
        a_session->rewind(edited.session, edited.at);
        e.session = edited.session;
        e.at = edited.at;
        e.rewindable = true;
        a_rewinding = true;
        a_rewindIndex = a_editing;
        a_editing = -1;
        a_draft.clear();
        a_editBar->hide();
        return e;
    }
    // Where the conversation stands before it: nothing said yet, or its
    // last message (unknown in a session continued whose file was not found).
    e.session = a_session->sessionId();
    e.at = a_session->lastMessageId();
    e.rewindable = e.session.isEmpty() || !e.at.isEmpty();
    return e;
}

bool ClaudeCodePanel::canEdit(qsizetype index) const
{
    return index >= 0 && index < a_entries.size() && a_entries.at(index).kind == Entry::You && a_entries.at(index).rewindable
           && !a_rewinding;
}

bool ClaudeCodePanel::editPrompt(qsizetype index)
{
    if (!canEdit(index)) return false;
    if (a_editing >= 0) cancelEdit();   // (another one: back first - the entries before it stay where they are)
    // A turn under way: stopped - it is among what is set aside.
    const bool stopped = a_session->isBusy();
    if (stopped) a_session->stop();
    a_editing = index;
    a_editStopped = stopped;
    a_setAside = a_entries.mid(index);
    a_entries.resize(index);
    a_draft = a_input->toPlainText();
    a_input->setPlainText(a_setAside.constFirst().text);
    a_input->moveCursor(QTextCursor::End);
    a_editBar->show();
    a_requests.clear();
    showNextRequest();
    updateState();
    render();
    focusComposer();
    emit titleChanged();
    return true;
}

void ClaudeCodePanel::cancelEdit()
{
    if (a_editing < 0) return;
    if (a_editStopped) endStoppedTurn(a_setAside);
    a_entries += a_setAside;
    a_setAside.clear();
    a_editing = -1;
    a_editStopped = false;
    a_input->setPlainText(a_draft);
    a_input->moveCursor(QTextCursor::End);
    a_draft.clear();
    a_editBar->hide();
    updateState();
    render();
    emit titleChanged();
    emit conversationChanged();
}

void ClaudeCodePanel::endStoppedTurn(QList<Entry>& list) const
{
    for (Entry& e : list) {
        e.streaming = false;
        if (e.kind == Entry::Tool && e.tool == Entry::Running) {
            e.tool = Entry::Failed;
            if (e.output.isEmpty()) e.output = tr("Stopped.");
        }
    }
    if (!list.isEmpty() && list.constLast().kind != Entry::Summary) list.append({Entry::Summary, tr("Stopped"), {}, {}});
}

void ClaudeCodePanel::forgetEdit()
{
    a_editing = -1;
    a_setAside.clear();
    a_draft.clear();
    a_editStopped = false;
    a_rewinding = false;
    a_rewindIndex = -1;
    if (a_editBar != nullptr) a_editBar->hide();
}

void ClaudeCodePanel::placePrompts(const QString& sessionId)
{
    const QString file = qucs_s::claude::history::claudeSessionFile(sessionId);
    if (file.isEmpty()) return;
    const qucs_s::claude::history::SessionPoints points = qucs_s::claude::history::sessionPoints(file);
    a_session->setLastMessageId(points.last);
    // Each prompt not placed, by its text, in order (one the dock sent may
    // have the document's note after it in the file).
    qsizetype next = 0;
    for (Entry& e : a_entries) {
        if (e.kind != Entry::You) continue;
        for (qsizetype k = next; k < points.prompts.size(); ++k) {
            const QString& text = points.prompts.at(k).first;
            if (text != e.text && !text.startsWith(e.text + QLatin1Char('\n'))) continue;
            if (!e.rewindable) {
                e.session = sessionId;
                e.at = points.prompts.at(k).second;
                e.rewindable = true;
            }
            next = k + 1;
            break;
        }
    }
}

bool ClaudeCodePanel::sendText(const QString& text, bool withDocument)
{
    if (text.isEmpty()) return false;
    // The dock's own commands: run here, whatever Claude is doing and with
    // no program needed.
    if (text.startsWith(QLatin1Char('/')) && runCommand(text)) return true;
    if (a_session->isBusy()) return false;
    if (a_session->program().isEmpty()) findProgram();
    if (a_session->program().isEmpty()) {
        append({Entry::Problem,
                tr("Claude Code was not found. Install it (claude.com/claude-code), sign in once with "
                   "\"claude\" in a terminal, and send again - or choose the program under ⋯."),
                {}, {}});
        return false;
    }
    if (a_session->workingDirectory() != workingDirectory()) a_session->setWorkingDirectory(workingDirectory());

    // Claude Code's commands sent as typed - with no note of the document,
    // which would be taken for their arguments.
    if (text.startsWith(QLatin1Char('/'))) {
        static const QRegularExpression command(QStringLiteral("^/[A-Za-z0-9][A-Za-z0-9:_.-]*(\\s|$)"));
        if (command.match(text).hasMatch()) {
            append(promptEntry(text, QString()));
            const bool sent = a_session->send(text);
            updateState();
            return sent;
        }
    }

    QString prompt = text;
    QString attached;
    refreshDocument();
    if (!withDocument) {
        // (a prompt of the dock's own, the git bar's: about no document)
    } else if (!a_pinned.isEmpty()) {
        // Pinned: always said, as the tools act on it.
        attached = a_pinned;
        prompt += QStringLiteral("\n\n") + tr("(This conversation is pinned to %1 in Qucs-S: its tools act on it when "
                                              "given no path, whichever document is in front.)")
                                               .arg(QDir::toNativeSeparators(a_pinned));
    } else if (a_attach->isEnabled() && a_attach->isChecked() && a_document) {
        attached = a_document();
        if (!attached.isEmpty())
            prompt += QStringLiteral("\n\n") + tr("(The document open in Qucs-S: %1)").arg(QDir::toNativeSeparators(attached));
    }
    append(promptEntry(text, attached.isEmpty() ? QString() : QFileInfo(attached).fileName()));
    const bool sent = a_session->send(prompt);
    updateState();
    return sent;
}

void ClaudeCodePanel::stopTurn()
{
    a_session->interrupt();
}

void ClaudeCodePanel::newConversation()
{
    emit conversationEnding();   // (kept as it is: /resume brings it back)
    a_conversationId.clear();
    a_session->reset();
    // With the saved permissions, not what the last one was allowed.
    a_session->setPermissionMode(QucsSettingsFile().value(kMode).toString());
    a_name.clear();
    pinDocument(QString());   // (a new one is pinned to nothing, as at first)
    forgetEdit();
    a_entries.clear();
    a_expanded.clear();
    a_requests.clear();
    emit titleChanged();
    showNextRequest();
    updateState();
    render();
    focusComposer();
}

void ClaudeCodePanel::addNote(const QString& text)
{
    append({Entry::Note, text, {}, {}});
}

// ----------------------------------------------------------------------
void ClaudeCodePanel::append(const Entry& e)
{
    // A reply being written ends where anything else begins.
    if (!a_entries.isEmpty() && a_entries.last().streaming && e.kind != Entry::Claude)
        a_entries.last().streaming = false;
    const bool firstPrompt = e.kind == Entry::You
                             && std::none_of(a_entries.cbegin(), a_entries.cend(), [](const Entry& x) { return x.kind == Entry::You; });
    a_entries.append(e);
    scheduleRender();
    if (firstPrompt) emit titleChanged();
    emit conversationChanged();
}

void ClaudeCodePanel::onReplyStreamed(const QString& text)
{
    if (!a_entries.isEmpty() && a_entries.last().kind == Entry::Claude && a_entries.last().streaming) {
        a_entries.last().text = text;
        scheduleRender();
        return;
    }
    Entry e{Entry::Claude, text, {}, {}};
    e.streaming = true;
    append(e);
}

void ClaudeCodePanel::onReplyFinished(const QString& text)
{
    if (!a_entries.isEmpty() && a_entries.last().kind == Entry::Claude && a_entries.last().streaming) {
        a_entries.last().text = text;
        a_entries.last().streaming = false;
        scheduleRender();
        emit conversationChanged();
        return;
    }
    append({Entry::Claude, text, {}, {}});
}

void ClaudeCodePanel::onToolStarted(const QString& id, const QString& tool, const QString& subject, const QString& detail)
{
    Entry e{Entry::Tool, tool, subject, id};
    e.detail = detail;
    append(e);
}

void ClaudeCodePanel::onToolFinished(const QString& id, bool failed, const QString& output)
{
    for (int i = a_entries.size() - 1; i >= 0; --i) {
        Entry& e = a_entries[i];
        if (e.kind != Entry::Tool || e.id != id) continue;
        // What it gave, the first lines of it, for when it is opened.
        QStringList lines = output.split(QLatin1Char('\n'));
        while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) lines.removeLast();
        const int kept = 30;
        e.result = lines.mid(0, kept).join(QLatin1Char('\n'));
        if (e.result.size() > 6000) e.result = e.result.left(6000) + QChar(0x2026);
        if (lines.size() > kept) e.result += QLatin1Char('\n') + tr("… %1 more lines").arg(lines.size() - kept);
        if (!failed) {
            e.tool = Entry::Succeeded;
        } else {
            const bool denied = output.contains(QLatin1String("did not allow"))
                                || output.contains(QLatin1String("haven't granted"))
                                || output.contains(QLatin1String("permission"), Qt::CaseInsensitive);
            e.tool = denied ? Entry::Denied : Entry::Failed;
            QString first = output.trimmed().section(QLatin1Char('\n'), 0, 0);
            if (first.size() > 200) first = first.left(199) + QChar(0x2026);
            e.output = first;
        }
        break;
    }
    scheduleRender();
    emit conversationChanged();
}

void ClaudeCodePanel::onPermissionRequested(const qucs_s::claude::PermissionRequest& request)
{
    // (Whoever holds the panel brings it forward: ClaudeCodeTabs.)
    a_requests.append(request);
    showNextRequest();
}

void ClaudeCodePanel::onPermissionWithdrawn(const QString& id)
{
    for (int i = 0; i < a_requests.size(); ++i)
        if (a_requests.at(i).id == id) {
            a_requests.removeAt(i);
            break;
        }
    showNextRequest();
}

void ClaudeCodePanel::showNextRequest()
{
    if (a_requests.isEmpty()) {
        a_card->hide();
        return;
    }
    const auto& r = a_requests.constFirst();
    a_cardTitle->setText(tr("Claude wants to %1").arg(r.action));
    a_cardSubject->setText(r.subject);
    a_cardSubject->setVisible(!r.subject.isEmpty());
    const bool detail = !r.detail.trimmed().isEmpty() && r.detail.trimmed() != r.subject.trimmed();
    a_cardDetail->setPlainText(r.detail);
    a_cardDetail->setVisible(detail);
    if (detail) {
        const int lines = std::clamp(int(r.detail.count(QLatin1Char('\n'))) + 1, 2, 10);
        a_cardDetail->setFixedHeight(lines * a_cardDetail->fontMetrics().lineSpacing() + 14);
    }
    a_allowEdits->setVisible(r.canAllowEdits);
    a_allowTools->setVisible(r.canAllowTools);
    a_cardCount->setText(a_requests.size() > 1 ? tr("1 of %1").arg(a_requests.size()) : QString());
    a_card->show();
}

void ClaudeCodePanel::answer(bool allow, bool allowEdits, bool allowTools)
{
    if (a_requests.isEmpty()) return;
    const qucs_s::claude::PermissionRequest r = a_requests.takeFirst();
    a_session->answer(r.id, allow, allowEdits, allowTools);
    if (allow && allowTools && r.canAllowTools)
        addNote(tr("Claude may use the Qucs-S window without asking for the rest of this conversation."));
    showNextRequest();
    updateState();
}

void ClaudeCodePanel::onTurnFinished(const qucs_s::claude::TurnResult& r)
{
    for (Entry& e : a_entries) e.streaming = false;
    // How it ended, what it took (shown as usageShown() says), the rest.
    Entry summary{Entry::Summary, QString(), {}, {}};
    QStringList rest;
    if (r.stopped) {
        summary.text = tr("Stopped");
    } else if (r.ok) {
        summary.text = tr("Done in %1").arg(seconds(r.durationMs));
    } else {
        append({Entry::Problem, a_session->detail().isEmpty() ? tr("The turn failed.") : a_session->detail(), {}, {}});
    }
    if (r.ok || r.stopped) {
        summary.tokens = r.tokens;
        summary.allTokens = r.conversationTokens;
        summary.cost = r.costUsd;
        summary.allCost = r.conversationCostUsd;
        if (r.turns > 1) rest << tr("%1 steps").arg(r.turns);
        if (r.denials > 0) rest << (r.denials == 1 ? tr("1 action not allowed") : tr("%1 actions not allowed").arg(r.denials));
    }
    if (!r.changedFiles.isEmpty()) {
        QStringList names;
        for (const QString& f : r.changedFiles) names << QFileInfo(f).fileName();
        rest << tr("changed %1").arg(names.join(QStringLiteral(", ")));
    }
    summary.result = rest.join(QStringLiteral("  ·  "));
    if (!summary.text.isEmpty() || !summary.result.isEmpty() || !summary.allTokens.isEmpty() || summary.allCost > 0.0)
        append(summary);
    a_requests.clear();
    showNextRequest();
    updateState();
    if (a_gitBar != nullptr) a_gitBar->refresh(true);   // committed, pushed, a pull request opened...
    if (!r.changedFiles.isEmpty()) emit filesChanged(r.changedFiles);
}

// ----------------------------------------------------------------------
void ClaudeCodePanel::handleLink(const QUrl& url)
{
    if (url.scheme() == QLatin1String("toggle")) {
        toggle(url.path());
        return;
    }
    if (url.scheme() == QLatin1String("edit")) {
        editPrompt(url.path().toLongLong());
        return;
    }
    if (url.scheme() == QLatin1String("prompt")) {
        const int n = url.path().toInt();
        if (n < 0 || n >= int(std::size(kSuggestions))) return;
        const Suggestion& s = kSuggestions[n];
        a_input->setPlainText(tr(s.prompt));
        if (s.attach && a_attach->isEnabled()) a_attach->setChecked(true);
        a_input->moveCursor(QTextCursor::End);
        focusComposer();
    } else if (url.isLocalFile()) {
        emit openFileRequested(url.toLocalFile());
    } else if (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https")
               || url.scheme() == QLatin1String("mailto")) {
        QDesktopServices::openUrl(url);
    }
}

QString ClaudeCodePanel::transcriptText() const
{
    return a_view->toPlainText();
}

void ClaudeCodePanel::scheduleRender()
{
    if (!a_renderTimer->isActive()) a_renderTimer->start();
}

void ClaudeCodePanel::renderNow()
{
    render();
}

void ClaudeCodePanel::render()
{
    a_renderTimer->stop();
    QScrollBar* bar = a_view->verticalScrollBar();
    const bool follow = bar->value() >= bar->maximum() - 16;
    const int keep = bar->value();

    QTextDocument* doc = a_view->document();
    doc->clear();
    doc->setDocumentMargin(12);
    QTextCursor c(doc);
    if (a_entries.isEmpty()) renderWelcome(c);
    else renderConversation(c);

    if (follow) {
        QTimer::singleShot(0, this, [this] {
            a_view->verticalScrollBar()->setValue(a_view->verticalScrollBar()->maximum());
        });
    } else {
        bar->setValue(keep);
    }
}

void ClaudeCodePanel::renderConversation(QTextCursor& c)
{
    bool captioned = false;
    for (qsizetype i = 0; i < a_entries.size();) {
        if (a_entries.at(i).kind == Entry::Tool) {
            qsizetype j = i + 1;
            while (j < a_entries.size() && a_entries.at(j).kind == Entry::Tool) ++j;
            renderTools(c, i, j, captioned);
            i = j;
            continue;
        }
        renderEntry(c, a_entries.at(i), captioned, i);
        ++i;
    }
}

QPalette ClaudeCodePanel::drawingPalette() const
{
    if (!a_exporting) return palette();
    // Paper: dark on white, whatever the dock's theme.
    QPalette paper = palette();
    const QColor ink(0x1f, 0x1f, 0x1f);
    for (auto role : {QPalette::Base, QPalette::Window}) paper.setColor(role, Qt::white);
    for (auto role : {QPalette::Text, QPalette::WindowText, QPalette::ButtonText}) paper.setColor(role, ink);
    paper.setColor(QPalette::Link, QColor(0x1a, 0x5f, 0xb4));
    return paper;
}

bool ClaudeCodePanel::isOpen(const QString& key) const
{
    if (a_exporting) return a_exportDetails;
    return a_expanded.contains(key);
}

void ClaudeCodePanel::renderWelcome(QTextCursor& c)
{
    const Colours col = colours(drawingPalette());
    const QString dir = shownPath(workingDirectory()).toHtmlEscaped();
    QString html = QStringLiteral("<div style='margin-top:10px'><span style='font-size:large; font-weight:600;'>%1</span></div>")
                       .arg(tr("Claude Code").toHtmlEscaped());
    if (a_session->program().isEmpty()) {
        html += QStringLiteral("<p style='color:%1'>%2</p>")
                    .arg(col.muted.name(),
                         tr("Claude Code was not found on this computer. Install it from "
                            "<a href='https://claude.com/claude-code'>claude.com/claude-code</a>, sign in once "
                            "with <code>claude</code> in a terminal, then write here. Or choose the program under "
                            "⋯ › Claude Program."));
    } else {
        const QString mode = a_session->permissionMode();
        const QString asks = mode == QLatin1String("acceptEdits")  ? tr("It changes files without asking and asks before it runs a command.")
                             : mode == QLatin1String("auto")       ? tr("It decides on its own what is safe to do; a safety check blocks risky actions.")
                             : mode == QLatin1String("plan")       ? tr("It reads and plans, and changes nothing.")
                             : mode == QLatin1String("bypassPermissions") ? tr("It does anything without asking.")
                                                                          : tr("It asks before it runs a command or changes a file.");
        html += QStringLiteral("<p style='color:%1'>%2</p>")
                    .arg(col.muted.name(),
                         tr("Ask about your circuits, simulations and files. Claude works in <b>%1</b>. %2")
                             .arg(dir, asks.toHtmlEscaped()));
        html += QStringLiteral("<p style='color:%1; margin-top:12px'>%2</p>").arg(col.faint.name(), tr("Try").toHtmlEscaped());
        for (int i = 0; i < int(std::size(kSuggestions)); ++i)
            html += QStringLiteral("<p style='margin-top:2px; margin-bottom:2px'>→&nbsp; <a href='prompt:%1' "
                                   "style='color:%2; text-decoration:none'>%3</a></p>")
                        .arg(i)
                        .arg(col.accent.name(), tr(kSuggestions[i].label).toHtmlEscaped());
    }
    c.insertHtml(html);
}

void ClaudeCodePanel::renderEntry(QTextCursor& c, const Entry& e, bool& captioned, qsizetype index)
{
    const Colours col = colours(drawingPalette());
    const QFont base = a_view->font();
    QFont small = base;
    small.setPointSizeF(std::max(7.0, base.pointSizeF() * 0.88));
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSizeF(small.pointSizeF());
    const bool first = c.document()->isEmpty();

    QTextCharFormat plain;
    plain.setFont(base);
    plain.setForeground(col.text);
    QTextCharFormat caption = plain;
    caption.setFont(small);
    caption.setFontWeight(QFont::DemiBold);
    QTextCharFormat muted = plain;
    muted.setFont(small);
    muted.setForeground(col.muted);

    const auto claudeCaption = [&] { renderCaption(c, captioned); };

    switch (e.kind) {
    case Entry::You: {
        captioned = false;
        QTextBlockFormat f;
        f.setTopMargin(first ? 2 : 18);
        f.setBottomMargin(4);
        startBlock(c, f);
        QTextCharFormat you = caption;
        you.setForeground(col.muted);
        c.insertText(tr("You"), you);
        // Edit: back to before it, to send it again as changed.
        if (!a_exporting && canEdit(index)) {
            QTextCharFormat edit = muted;
            edit.setFontWeight(QFont::Normal);
            edit.setForeground(col.faint);
            edit.setAnchor(true);
            edit.setAnchorHref(QStringLiteral("edit:%1").arg(index));
            edit.setToolTip(tr("Edit this prompt and send it again: the conversation goes back to before it"));
            c.insertText(QStringLiteral("   "), muted);
            c.insertText(tr("Edit"), edit);
        }
        QTextFrameFormat frame;
        frame.setBackground(col.bubble);
        frame.setPadding(8);
        frame.setBorder(0);
        frame.setMargin(0);
        c.insertFrame(frame);
        const QStringList lines = e.text.split(QLatin1Char('\n'));
        for (int i = 0; i < lines.size(); ++i) {
            if (i > 0) c.insertBlock();
            c.insertText(lines.at(i), plain);
        }
        if (!e.extra.isEmpty()) {
            QTextBlockFormat bf;
            bf.setTopMargin(4);
            c.insertBlock(bf);
            c.insertText(QStringLiteral("↳ ") + e.extra, muted);
        }
        leaveFrame(c);
        break;
    }
    case Entry::Claude: {
        claudeCaption();
        QTextBlockFormat f;
        f.setTopMargin(2);
        startBlock(c, f);
        renderMarkdown(c, e.text);
        if (e.streaming && !a_exporting) {
            QTextCharFormat cursor = plain;
            cursor.setForeground(col.accent);
            c.insertText(QStringLiteral(" ▍"), cursor);
        }
        break;
    }
    case Entry::Tool:
        break;   // (renderTools())
    case Entry::Note: {
        QTextBlockFormat f;
        f.setTopMargin(first ? 2 : 8);
        startBlock(c, f);
        QTextCharFormat note = muted;
        note.setFontItalic(true);
        c.insertText(e.text, note);
        break;
    }
    case Entry::Problem: {
        QTextBlockFormat f;
        f.setTopMargin(first ? 2 : 10);
        startBlock(c, f);
        QTextCharFormat problem = plain;
        problem.setForeground(col.error);
        const QStringList lines = e.text.split(QLatin1Char('\n'));
        c.insertText(QStringLiteral("⚠  ") + lines.constFirst(), problem);
        problem.setFont(small);
        for (int i = 1; i < lines.size(); ++i) {
            c.insertBlock();
            c.insertText(lines.at(i), problem);
        }
        break;
    }
    case Entry::Summary: {
        // (Nothing when all it would tell is hidden.)
        const QList<QPair<QString, QString>> parts = summaryParts(e);
        if (parts.isEmpty()) break;
        QTextBlockFormat f;
        f.setTopMargin(8);
        startBlock(c, f);
        for (qsizetype i = 0; i < parts.size(); ++i) {
            if (i > 0) c.insertText(QStringLiteral("  ·  "), muted);
            QTextCharFormat part = muted;
            part.setToolTip(parts.at(i).second);
            c.insertText(parts.at(i).first, part);
        }
        break;
    }
    }
}

void ClaudeCodePanel::renderCaption(QTextCursor& c, bool& captioned)
{
    if (captioned) return;
    captioned = true;
    const Colours col = colours(drawingPalette());
    QFont small = a_view->font();
    small.setPointSizeF(std::max(7.0, small.pointSizeF() * 0.88));
    QTextCharFormat caption;
    caption.setFont(small);
    caption.setFontWeight(QFont::DemiBold);
    caption.setForeground(col.text);
    QTextBlockFormat f;
    f.setTopMargin(c.document()->isEmpty() ? 2 : 16);
    f.setBottomMargin(4);
    startBlock(c, f);
    QTextCharFormat mark = caption;
    mark.setForeground(col.accent);
    c.insertText(QStringLiteral("● "), mark);
    c.insertText(tr("Claude"), caption);
}

// ----------------------------------------------------------------------
// Tools: a row of them is a line that opens; each opens on its input and
// what it gave.

QString ClaudeCodePanel::toolSummary(qsizetype from, qsizetype to) const
{
    struct Part {
        QString key;
        int count = 0;
        QStringList files;
        QString name;
    };
    QList<Part> parts;
    const auto add = [&parts](const QString& key, const QString& file, const QString& name = QString()) {
        for (Part& p : parts)
            if (p.key == key) {
                ++p.count;
                if (!file.isEmpty() && !p.files.contains(file)) p.files << file;
                return;
            }
        parts.append({key, 1, file.isEmpty() ? QStringList() : QStringList{file}, name});
    };
    for (qsizetype i = from; i < to; ++i) {
        const Entry& e = a_entries.at(i);
        const QString& tool = e.text;
        const QString file = QFileInfo(e.extra).fileName();
        if (tool == QLatin1String("Bash") || tool == QLatin1String("PowerShell")) add(QStringLiteral("run"), {});
        else if (tool == QLatin1String("Read")) add(QStringLiteral("read"), file);
        else if (tool == QLatin1String("Write")) add(QStringLiteral("write"), file);
        else if (tool == QLatin1String("Edit") || tool == QLatin1String("MultiEdit") || tool == QLatin1String("NotebookEdit"))
            add(QStringLiteral("edit"), file);
        else if (tool == QLatin1String("Glob") || tool == QLatin1String("Grep") || tool == QLatin1String("LS"))
            add(QStringLiteral("search"), {});
        else if (tool == QLatin1String("WebFetch")) add(QStringLiteral("fetch"), {});
        else if (tool == QLatin1String("WebSearch")) add(QStringLiteral("web"), {});
        else if (tool == QLatin1String("TodoWrite")) add(QStringLiteral("todo"), {});
        else if (tool == QLatin1String("Task") || tool == QLatin1String("Agent")) add(QStringLiteral("agent"), {});
        else if (tool.startsWith(QLatin1String("mcp__"))) add(QStringLiteral("mcp:") + toolName(tool).section(QStringLiteral(" \u00b7 "), 0, 0), {}, toolName(tool).section(QStringLiteral(" \u00b7 "), 0, 0));
        else if (tool == QLatin1String("ToolSearch")) add(QStringLiteral("search tools"), {});
        else add(QStringLiteral("tool:") + tool, {}, tool);
    }
    QStringList words;
    for (const Part& p : std::as_const(parts)) {
        const int n = p.count;
        const int files = int(p.files.size());
        const QString one = files == 1 ? p.files.constFirst() : QString();
        if (p.key == QLatin1String("run")) words << (n == 1 ? tr("ran a command") : tr("ran %1 commands").arg(n));
        else if (p.key == QLatin1String("read")) words << (files <= 1 && !one.isEmpty() ? tr("read %1").arg(one) : tr("read %1 files").arg(std::max(files, n)));
        else if (p.key == QLatin1String("write")) words << (files <= 1 && !one.isEmpty() ? tr("wrote %1").arg(one) : tr("wrote %1 files").arg(std::max(files, 1)));
        else if (p.key == QLatin1String("edit")) words << (files <= 1 && !one.isEmpty() ? tr("edited %1").arg(one) : tr("edited %1 files").arg(std::max(files, 1)));
        else if (p.key == QLatin1String("search")) words << (n == 1 ? tr("searched") : tr("searched %1 times").arg(n));
        else if (p.key == QLatin1String("fetch")) words << (n == 1 ? tr("fetched a page") : tr("fetched %1 pages").arg(n));
        else if (p.key == QLatin1String("web")) words << tr("searched the web");
        else if (p.key == QLatin1String("todo")) words << tr("updated the to-do list");
        else if (p.key == QLatin1String("agent")) words << (n == 1 ? tr("ran an agent") : tr("ran %1 agents").arg(n));
        else if (p.key.startsWith(QLatin1String("mcp:"))) words << (n == 1 ? tr("used %1").arg(p.name) : tr("used %1 %2 times").arg(p.name).arg(n));
        else if (p.key == QLatin1String("search tools")) words << tr("found its tools");
        else words << (n == 1 ? tr("used %1").arg(p.name) : tr("used %1 %2 times").arg(p.name).arg(n));
    }
    QString text = words.join(QStringLiteral(", "));
    if (!text.isEmpty()) text[0] = text.at(0).toUpper();
    return text;
}

int ClaudeCodePanel::rowOutcome(qsizetype from, qsizetype to) const
{
    int outcome = Entry::Succeeded;
    for (qsizetype i = from; i < to; ++i) {
        const int tool = a_entries.at(i).tool;
        if (tool == Entry::Running) return Entry::Running;
        if (tool == Entry::Failed) outcome = Entry::Failed;
        else if (tool == Entry::Denied && outcome != Entry::Failed) outcome = Entry::Denied;
    }
    return outcome;
}

QString ClaudeCodePanel::rowTrouble(qsizetype from, qsizetype to) const
{
    int failed = 0, denied = 0;
    for (qsizetype i = from; i < to; ++i) {
        if (a_entries.at(i).tool == Entry::Failed) ++failed;
        if (a_entries.at(i).tool == Entry::Denied) ++denied;
    }
    QStringList notes;
    if (failed > 0) notes << (failed == 1 ? tr("1 failed") : tr("%1 failed").arg(failed));
    if (denied > 0) notes << (denied == 1 ? tr("1 not allowed") : tr("%1 not allowed").arg(denied));
    return notes.join(QStringLiteral(", "));
}

void ClaudeCodePanel::renderTools(QTextCursor& c, qsizetype from, qsizetype to, bool& captioned)
{
    renderCaption(c, captioned);
    // A little room before what follows.
    const auto roomAfter = [&c] {
        QTextBlockFormat last = c.blockFormat();
        last.setBottomMargin(std::max(last.bottomMargin(), 5.0));
        c.setBlockFormat(last);
    };
    if (to - from == 1) {
        renderTool(c, a_entries.at(from), 0);
        roomAfter();
        return;
    }
    const Colours col = colours(drawingPalette());
    QFont small = a_view->font();
    small.setPointSizeF(std::max(7.0, small.pointSizeF() * 0.88));
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSizeF(small.pointSizeF());

    const QString key = QStringLiteral("group:") + a_entries.at(from).id;
    const bool open = isOpen(key);
    const Entry* now = nullptr;   // the one running
    for (qsizetype i = from; i < to; ++i)
        if (a_entries.at(i).tool == Entry::Running) now = &a_entries.at(i);
    QTextBlockFormat f;
    f.setTopMargin(3);
    f.setLeftMargin(2);
    startBlock(c, f);
    QTextCharFormat link;
    if (!a_exporting) {
        link.setAnchor(true);
        link.setAnchorHref(QStringLiteral("toggle:") + key);
        link.setToolTip(open ? tr("Fold the tools away") : tr("Show each tool Claude used"));
    }
    QTextCharFormat twisty = link;
    twisty.setFont(small);
    twisty.setForeground(col.faint);
    if (!a_exporting) c.insertText(open ? QStringLiteral("▾ ") : QStringLiteral("▸ "), twisty);
    QTextCharFormat mark = twisty;
    mark.setFontWeight(QFont::DemiBold);
    const int outcome = rowOutcome(from, to);
    mark.setForeground(outcome == Entry::Running  ? col.accent
                       : outcome == Entry::Failed ? col.error
                       : outcome == Entry::Denied ? col.warn
                                                  : col.ok);
    c.insertText(outcomeMark(outcome) + QStringLiteral("  "), mark);
    QTextCharFormat name = twisty;
    name.setFontWeight(QFont::DemiBold);
    name.setForeground(col.text);
    c.insertText(toolSummary(from, to), name);
    QTextCharFormat quiet = twisty;
    quiet.setForeground(col.muted);
    if (now != nullptr && !now->extra.isEmpty()) {
        QTextCharFormat subject = quiet;
        subject.setFont(mono);
        c.insertText(QStringLiteral("   ") + now->extra, subject);
    } else if (const QString trouble = rowTrouble(from, to); !trouble.isEmpty()) {
        c.insertText(QStringLiteral("  ·  ") + trouble, quiet);
    }
    if (open)
        for (qsizetype i = from; i < to; ++i) renderTool(c, a_entries.at(i), 16);
    roomAfter();
}

void ClaudeCodePanel::renderTool(QTextCursor& c, const Entry& e, qreal indent)
{
    const Colours col = colours(drawingPalette());
    QFont small = a_view->font();
    small.setPointSizeF(std::max(7.0, small.pointSizeF() * 0.88));
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSizeF(small.pointSizeF());

    const QString key = QStringLiteral("tool:") + e.id;
    const bool more = !e.detail.trimmed().isEmpty() || !e.result.trimmed().isEmpty();
    const bool open = more && isOpen(key);
    QTextBlockFormat f;
    f.setTopMargin(3);
    f.setLeftMargin(2 + indent);
    startBlock(c, f);
    QTextCharFormat link;
    if (more && !a_exporting) {
        link.setAnchor(true);
        link.setAnchorHref(QStringLiteral("toggle:") + key);
        link.setToolTip(open ? tr("Fold it away") : tr("Show the whole of it, and what it gave"));
    }
    QTextCharFormat twisty = link;
    twisty.setFont(small);
    twisty.setForeground(more ? col.faint : col.base);
    if (!a_exporting) c.insertText(open ? QStringLiteral("▾ ") : QStringLiteral("▸ "), twisty);
    QTextCharFormat mark = twisty;
    mark.setFontWeight(QFont::DemiBold);
    QString glyph;
    switch (e.tool) {
    case Entry::Running: glyph = QStringLiteral("○"); mark.setForeground(col.accent); break;
    case Entry::Succeeded: glyph = QStringLiteral("✓"); mark.setForeground(col.ok); break;
    case Entry::Failed: glyph = QStringLiteral("✕"); mark.setForeground(col.error); break;
    case Entry::Denied: glyph = QStringLiteral("⊘"); mark.setForeground(col.warn); break;
    }
    c.insertText(glyph + QStringLiteral("  "), mark);
    QTextCharFormat name = twisty;
    name.setFontWeight(QFont::DemiBold);
    name.setForeground(col.text);
    c.insertText(toolName(e.text), name);
    if (!e.extra.isEmpty()) {
        QTextCharFormat subject = twisty;
        subject.setFont(mono);
        subject.setForeground(col.muted);
        c.insertText(QStringLiteral("   ") + e.extra, subject);
    }
    QTextCharFormat quiet;
    quiet.setFont(small);
    quiet.setForeground(col.muted);
    if (!e.output.isEmpty() && e.tool != Entry::Succeeded) {
        QTextBlockFormat bf;
        bf.setLeftMargin(22 + indent);
        c.insertBlock(bf);
        QTextCharFormat why = quiet;
        why.setForeground(e.tool == Entry::Denied ? col.warn : col.error);
        c.insertText(e.output, why);
    }
    if (!open) return;
    // The input, then what came out, on a shade.
    QTextCharFormat code;
    code.setFont(mono);
    code.setForeground(col.text);
    QTextCharFormat out = code;
    out.setForeground(col.muted);
    const auto lines = [&](const QString& text, const QTextCharFormat& format, qreal top) {
        const QStringList all = text.split(QLatin1Char('\n'));
        for (int i = 0; i < all.size(); ++i) {
            QTextBlockFormat bf;
            bf.setLeftMargin(22 + indent);
            bf.setBackground(col.code);
            bf.setTopMargin(i == 0 ? top : 0);
            bf.setLineHeight(100, QTextBlockFormat::ProportionalHeight);
            c.insertBlock(bf);
            QTextCharFormat cf = format;
            if (all.at(i).startsWith(QLatin1String("# "))) cf.setForeground(col.muted);   // a command's description
            c.insertText(all.at(i).isEmpty() ? QStringLiteral(" ") : all.at(i), cf);
        }
    };
    if (!e.detail.trimmed().isEmpty()) lines(e.detail, code, 4);
    if (!e.result.trimmed().isEmpty()) lines(e.result, out, e.detail.trimmed().isEmpty() ? 4 : 2);
    else if (e.tool == Entry::Running) {
        QTextBlockFormat bf;
        bf.setLeftMargin(22 + indent);
        c.insertBlock(bf);
        c.insertText(tr("running…"), quiet);
    }
}

void ClaudeCodePanel::toggle(const QString& key)
{
    if (!a_expanded.remove(key)) a_expanded.insert(key);
    render();
}

// ----------------------------------------------------------------------
// A reply: Markdown, its code on a shade, its math typeset.

qucs_s::math::Typeset ClaudeCodePanel::typesetMath(const QString& tex, const QFont& font, bool display)
{
    const QColor colour = colours(drawingPalette()).text;
    const qreal dpr = a_exporting ? 4.0 : a_view->devicePixelRatioF();   // (on paper: sharp at 300 dpi)
    const QString key = tex + QChar(0) + font.key() + QChar(0) + colour.name() + (display ? QLatin1Char('D') : QLatin1Char('T'))
                        + QString::number(dpr);
    auto found = a_math.constFind(key);
    if (found != a_math.cend()) return *found;
    if (a_math.size() > 400) a_math.clear();
    const qucs_s::math::Typeset t = qucs_s::math::typeset(tex, font, colour, display, dpr);
    a_math.insert(key, t);
    return t;
}

void ClaudeCodePanel::renderMarkdown(QTextCursor& c, const QString& text)
{
    const Colours col = colours(drawingPalette());
    const QFont base = a_view->font();

    // The math out of the way of the Markdown: a mark for each formula.
    // Display math that stands on lines of its own is a paragraph of its
    // own (indented as it was, in the list item it may be in).
    const QList<qucs_s::math::Span> spans = qucs_s::math::findMath(text);
    QString md = text;
    for (qsizetype k = spans.size(); k-- > 0;) {
        const qucs_s::math::Span& sp = spans.at(k);
        const QString mark(QChar(char16_t(kMathMark + k)));
        QString put = mark;
        if (sp.display) {
            const qsizetype lineStart = sp.start == 0 ? 0 : md.lastIndexOf(QLatin1Char('\n'), sp.start - 1) + 1;
            const QString before = md.mid(lineStart, sp.start - lineStart);
            const qsizetype end = sp.start + sp.length;
            const qsizetype eol = md.indexOf(QLatin1Char('\n'), end);
            const QString after = md.mid(end, eol < 0 ? -1 : eol - end);
            if (before.trimmed().isEmpty() && after.trimmed().isEmpty())
                put = QLatin1Char('\n') + before + mark + QLatin1Char('\n');
        }
        md.replace(sp.start, sp.length, put);
    }

    const int from = c.position();
    QTextDocument doc;
    doc.setDefaultFont(base);
    qucs_s::markdown::setMarkdown(&doc, md);
    c.insertFragment(QTextDocumentFragment(&doc));
    QTextDocument* target = c.document();
    // Code on a shade, set off from the text.
    for (QTextBlock b = target->findBlock(from); b.isValid() && b.position() <= c.position(); b = b.next()) {
        const QTextBlockFormat bf = b.blockFormat();
        if (bf.nonBreakableLines() || bf.hasProperty(QTextFormat::BlockCodeFence)
            || bf.hasProperty(QTextFormat::BlockCodeLanguage)) {
            QTextCursor bc(b);
            QTextBlockFormat shaded = bf;
            shaded.setBackground(col.code);
            shaded.setLeftMargin(bf.leftMargin() + 4);
            bc.setBlockFormat(shaded);
        }
        if (b == c.block()) break;
    }
    // The math, typeset where its marks are, in the size of the text
    // around it (a heading's is bigger); its TeX in the tool tip.
    for (qsizetype k = 0; k < spans.size(); ++k) {
        QTextCursor hit = target->find(QString(QChar(char16_t(kMathMark + k))), from);
        if (hit.isNull()) continue;
        const qucs_s::math::Span& sp = spans.at(k);
        const QFont font = hit.charFormat().font().resolve(base);
        const qucs_s::math::Typeset t = typesetMath(sp.tex, font, sp.display);
        hit.insertText(QString(QChar::ObjectReplacementCharacter), qucs_s::math::MathObject::format(t, font, sp.tex));
        if (sp.display && hit.block().text().trimmed() == QString(QChar::ObjectReplacementCharacter)) {
            QTextBlockFormat bf = hit.blockFormat();
            bf.setAlignment(Qt::AlignHCenter);
            bf.setTopMargin(std::max(bf.topMargin(), 4.0));
            bf.setBottomMargin(std::max(bf.bottomMargin(), 4.0));
            hit.setBlockFormat(bf);
        }
    }
}

// ----------------------------------------------------------------------
QString ClaudeCodePanel::title() const
{
    if (!a_name.isEmpty()) return a_name;
    // (A prompt being edited: the conversation as it is until it is sent.)
    for (const Entry& e : a_editing >= 0 ? a_entries + a_setAside : a_entries)
        if (e.kind == Entry::You) {
            QString t = e.text.simplified();
            if (t.size() > 40) t = t.left(39).trimmed() + QChar(0x2026);
            return t;
        }
    return tr("New conversation");
}

void ClaudeCodePanel::setName(const QString& name)
{
    const QString tidy = name.simplified();
    if (tidy == a_name) return;
    a_name = tidy;
    emit titleChanged();
}

QPixmap ClaudeCodePanel::statePixmap() const
{
    return a_stateDot->pixmap();
}

QColor ClaudeCodePanel::accentColour(const QPalette& palette)
{
    return colours(palette).accent;
}

void ClaudeCodePanel::setNewInTab(bool on)
{
    a_newInTab = on;
    a_newButton->setToolTip(on ? tr("A new conversation, in a tab of its own") : tr("Start a new conversation"));
}

// ----------------------------------------------------------------------
// Kept, and brought back.

QJsonObject ClaudeCodePanel::conversationJson() const
{
    QJsonArray entries;
    // A prompt being edited: the conversation as it is until it is sent.
    const QList<Entry> all = a_editing >= 0 ? a_entries + a_setAside : a_entries;
    for (const Entry& e : all) {
        QJsonObject o{{QStringLiteral("kind"), int(e.kind)}, {QStringLiteral("text"), e.text}};
        if (e.kind == Entry::You && e.rewindable) {
            o.insert(QStringLiteral("session"), e.session);
            o.insert(QStringLiteral("at"), e.at);
        }
        if (!e.extra.isEmpty()) o.insert(QStringLiteral("extra"), e.extra);
        if (!e.id.isEmpty()) o.insert(QStringLiteral("id"), e.id);
        if (!e.output.isEmpty()) o.insert(QStringLiteral("output"), e.output);
        if (e.kind == Entry::Tool) o.insert(QStringLiteral("tool"), int(e.tool));
        if (!e.detail.isEmpty()) o.insert(QStringLiteral("detail"), e.detail);
        if (!e.result.isEmpty()) o.insert(QStringLiteral("result"), e.result);
        if (e.kind == Entry::Summary) {
            if (!e.allTokens.isEmpty()) {
                o.insert(QStringLiteral("tokens"), tokensJson(e.tokens));
                o.insert(QStringLiteral("allTokens"), tokensJson(e.allTokens));
            }
            if (e.cost >= 0.0) o.insert(QStringLiteral("cost"), e.cost);
            if (e.allCost >= 0.0) o.insert(QStringLiteral("allCost"), e.allCost);
        }
        entries.append(o);
    }
    return {{QStringLiteral("title"), exportTitle()},
            {QStringLiteral("name"), a_name},
            {QStringLiteral("folder"), workingDirectory()},
            {QStringLiteral("pinned"), a_pinned},
            {QStringLiteral("sessionId"), a_session->sessionId()},
            {QStringLiteral("entries"), entries}};
}

void ClaudeCodePanel::restoreConversation(const QJsonObject& conversation)
{
    emit conversationEnding();
    a_session->reset();
    // In its folder: Claude Code keeps a session with the folder it ran in.
    const QString folder = conversation.value(QLatin1String("folder")).toString();
    if (!folder.isEmpty() && QFileInfo(folder).isDir()) setWorkingDirectory(folder);
    forgetEdit();
    a_entries.clear();
    a_expanded.clear();
    a_requests.clear();
    for (const QJsonValue& v : conversation.value(QLatin1String("entries")).toArray()) {
        const QJsonObject o = v.toObject();
        const int kind = o.value(QLatin1String("kind")).toInt(-1);
        if (kind < Entry::You || kind > Entry::Summary) continue;
        Entry e{static_cast<Entry::Kind>(kind), o.value(QLatin1String("text")).toString(),
                o.value(QLatin1String("extra")).toString(), o.value(QLatin1String("id")).toString()};
        e.output = o.value(QLatin1String("output")).toString();
        e.detail = o.value(QLatin1String("detail")).toString();
        e.result = o.value(QLatin1String("result")).toString();
        if (e.kind == Entry::You && o.contains(QLatin1String("at"))) {
            e.session = o.value(QLatin1String("session")).toString();
            e.at = o.value(QLatin1String("at")).toString();
            e.rewindable = true;
        }
        if (e.kind == Entry::Summary) {
            e.tokens = tokensFromJson(o.value(QLatin1String("tokens")).toObject());
            e.allTokens = tokensFromJson(o.value(QLatin1String("allTokens")).toObject());
            e.cost = o.value(QLatin1String("cost")).toDouble(-1.0);
            e.allCost = o.value(QLatin1String("allCost")).toDouble(-1.0);
            // Kept before the line was made as it is shown: its costs
            // ("$0.012", "$0.05 in all") taken out of its text, to be
            // shown as usageShown() says - the rest before and after them.
            if (!o.contains(QLatin1String("cost")) && e.text.contains(QLatin1Char('$'))) {
                static const QRegularExpression amount(QStringLiteral("^\\$(\\d+(?:\\.\\d+)?)(\\s+\\S.*)?$"));
                QStringList before, after;
                for (const QString& part : e.text.split(QStringLiteral("  ·  "))) {
                    const QRegularExpressionMatch m = amount.match(part);
                    if (!m.hasMatch()) {
                        (e.cost < 0.0 && e.allCost < 0.0 ? before : after) << part;
                    } else if (m.captured(2).isEmpty()) {
                        e.cost = m.captured(1).toDouble();
                    } else {
                        e.allCost = m.captured(1).toDouble();
                    }
                }
                e.text = before.join(QStringLiteral("  ·  "));
                if (!after.isEmpty()) e.result = after.join(QStringLiteral("  ·  "));
            }
        }
        if (e.kind == Entry::Tool) {
            const int tool = o.value(QLatin1String("tool")).toInt(Entry::Failed);
            e.tool = tool >= Entry::Running && tool <= Entry::Denied ? static_cast<Entry::ToolState>(tool) : Entry::Failed;
            if (e.tool == Entry::Running) {   // (Qucs-S closed under it)
                e.tool = Entry::Failed;
                if (e.output.isEmpty()) e.output = tr("It had not finished when Qucs-S closed.");
            }
        }
        a_entries.append(e);
    }
    a_name = conversation.value(QLatin1String("name")).toString().simplified();
    pinDocument(conversation.value(QLatin1String("pinned")).toString());
    a_session->resume(conversation.value(QLatin1String("sessionId")).toString());
    // Where it stands, and its prompts kept before they knew where they
    // stood: from Claude Code's file of the session.
    placePrompts(a_session->sessionId());
    // Its totals go on from its last turn's.
    for (auto it = a_entries.crbegin(); it != a_entries.crend(); ++it) {
        if (it->kind != Entry::Summary || (it->allTokens.isEmpty() && it->allCost < 0.0)) continue;
        a_session->setConversationTotals(it->allTokens, it->allCost);
        break;
    }
    showNextRequest();
    updateState();
    scheduleRender();
    emit titleChanged();
}

bool ClaudeCodePanel::importClaudeSession(const QString& file)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const qucs_s::claude::ToolHost* host = a_session->toolHost();
    const QString hostPrefix = host != nullptr ? QStringLiteral("mcp__") + host->serverName() + QStringLiteral("__") : QString();
    static const QRegularExpression typedCommand(QStringLiteral("<command-name>([^<]*)</command-name>"));
    static const QRegularExpression typedArguments(QStringLiteral("<command-args>([^<]*)</command-args>"),
                                                   QRegularExpression::DotMatchesEverythingOption);
    QString folder, title;
    QList<Entry> entries;
    QHash<QString, qsizetype> tools;   // a tool use's id: its entry
    // What a tool gave: its text, the first lines.
    const auto outputOf = [](const QJsonValue& content) {
        QString text = content.toString();
        for (const QJsonValue& v : content.toArray())
            if (v.toObject().value(QLatin1String("type")).toString() == QLatin1String("text"))
                text += v.toObject().value(QLatin1String("text")).toString();
        QStringList lines = text.split(QLatin1Char('\n'));
        while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) lines.removeLast();
        return lines.mid(0, 30).join(QLatin1Char('\n')).left(6000);
    };
    while (!f.atEnd()) {
        const QJsonObject o = QJsonDocument::fromJson(f.readLine()).object();
        if (o.isEmpty()) continue;
        const QString type = o.value(QLatin1String("type")).toString();
        if (type == QLatin1String("custom-title")) {
            title = o.value(QLatin1String("customTitle")).toString();
            continue;
        }
        if (o.value(QLatin1String("isSidechain")).toBool()) continue;   // (a subagent's)
        if (folder.isEmpty()) folder = o.value(QLatin1String("cwd")).toString();
        const QJsonValue content = o.value(QLatin1String("message")).toObject().value(QLatin1String("content"));
        if (type == QLatin1String("user")) {
            if (o.value(QLatin1String("isMeta")).toBool()) continue;
            QString text = content.toString();
            for (const QJsonValue& v : content.toArray()) {
                const QJsonObject part = v.toObject();
                const QString kind = part.value(QLatin1String("type")).toString();
                if (kind == QLatin1String("text")) {
                    text += part.value(QLatin1String("text")).toString();
                } else if (kind == QLatin1String("tool_result")) {
                    const auto it = tools.constFind(part.value(QLatin1String("tool_use_id")).toString());
                    if (it == tools.cend()) continue;
                    Entry& e = entries[*it];
                    const QString output = outputOf(part.value(QLatin1String("content")));
                    if (part.value(QLatin1String("is_error")).toBool()) {
                        e.tool = Entry::Failed;
                        e.output = output.section(QLatin1Char('\n'), 0, 0).left(200);
                    } else {
                        e.result = output;
                    }
                }
            }
            text = text.trimmed();
            if (const auto m = typedCommand.match(text); m.hasMatch()) {
                // A command typed: as it was typed.
                text = (m.captured(1).trimmed() + QLatin1Char(' ') + typedArguments.match(text).captured(1).trimmed()).trimmed();
            } else if (text.startsWith(QLatin1Char('<')) || text.startsWith(QLatin1String("Caveat:"))) {
                continue;   // (what Claude Code adds itself)
            }
            if (!text.isEmpty()) entries.append({Entry::You, text, {}, {}});
        } else if (type == QLatin1String("assistant")) {
            for (const QJsonValue& v : content.toArray()) {
                const QJsonObject part = v.toObject();
                const QString kind = part.value(QLatin1String("type")).toString();
                if (kind == QLatin1String("text")) {
                    const QString reply = part.value(QLatin1String("text")).toString().trimmed();
                    if (!reply.isEmpty()) entries.append({Entry::Claude, reply, {}, {}});
                } else if (kind == QLatin1String("tool_use")) {
                    const QString name = part.value(QLatin1String("name")).toString();
                    const QJsonObject input = part.value(QLatin1String("input")).toObject();
                    const bool own = !hostPrefix.isEmpty() && name.startsWith(hostPrefix);
                    Entry e{Entry::Tool, name,
                            own ? host->subjectOf(name.mid(hostPrefix.size()), input) : qucs_s::claude::toolSubject(name, input, folder),
                            part.value(QLatin1String("id")).toString()};
                    e.tool = Entry::Succeeded;   // (unless its result says otherwise)
                    tools.insert(e.id, entries.size());
                    entries.append(e);
                }
            }
        }
    }
    if (entries.isEmpty()) return false;

    emit conversationEnding();
    a_session->reset();
    a_conversationId.clear();   // (kept as one of ours from now on)
    if (!folder.isEmpty() && QFileInfo(folder).isDir()) setWorkingDirectory(folder);
    forgetEdit();
    a_entries = entries;
    a_expanded.clear();
    a_requests.clear();
    a_name = title.simplified();
    pinDocument(QString());
    a_session->resume(QFileInfo(file).completeBaseName());
    placePrompts(a_session->sessionId());
    showNextRequest();
    updateState();
    scheduleRender();
    emit titleChanged();
    return true;
}

// ----------------------------------------------------------------------
// Commands.

QList<ClaudeCodePanel::Command> ClaudeCodePanel::commands() const
{
    static const struct {
        const char* name;
        const char* arguments;
        const char* description;
    } local[] = {
        {"clear", "", QT_TR_NOOP("Begin a new conversation here (this one is kept: /resume brings it back)")},
        {"new", "", QT_TR_NOOP("Begin a new conversation here, as /clear")},
        {"resume", "[words or a session id]", QT_TR_NOOP("Go on with a conversation from before - one of the dock's, or one of Claude Code's in this folder")},
        {"quit", "", QT_TR_NOOP("End this conversation and close its tab (it is kept: /resume)")},
        {"exit", "", QT_TR_NOOP("End this conversation and close its tab, as /quit")},
        {"help", "", QT_TR_NOOP("The commands")},
        {"model", "[name]", QT_TR_NOOP("The model: the one in use, or another from the next prompt on")},
        {"permissions", "[ask | edits | auto | plan | bypass]", QT_TR_NOOP("What Claude may do without asking")},
        {"rename", "<name>", QT_TR_NOOP("Name this conversation")},
        {"export", "[pdf | md | txt]", QT_TR_NOOP("Save the whole conversation to a file")},
        {"status", "", QT_TR_NOOP("This conversation: its session, model, permissions, folder, schematic")},
        {"pin", "", QT_TR_NOOP("Pin the schematic in front to this conversation")},
        {"unpin", "", QT_TR_NOOP("Unpin its schematic")},
    };
    static const QHash<QString, const char*> theirs = {
        {QStringLiteral("compact"), QT_TR_NOOP("Compacts the conversation so far (what to keep may follow)")},
        {QStringLiteral("context"), QT_TR_NOOP("How full Claude's context is, and with what")},
        {QStringLiteral("cost"), QT_TR_NOOP("Usage and its limits")},
        {QStringLiteral("usage"), QT_TR_NOOP("Usage and its limits")},
        {QStringLiteral("init"), QT_TR_NOOP("Writes a CLAUDE.md about this folder")},
        {QStringLiteral("review"), QT_TR_NOOP("Reviews a pull request")},
        {QStringLiteral("security-review"), QT_TR_NOOP("Reviews the changes for security")},
        {QStringLiteral("recap"), QT_TR_NOOP("Sums up the conversation")},
        {QStringLiteral("mcp"), QT_TR_NOOP("The MCP servers")},
        {QStringLiteral("agents"), QT_TR_NOOP("The subagents")},
        {QStringLiteral("effort"), QT_TR_NOOP("How hard Claude thinks")},
        {QStringLiteral("fast"), QT_TR_NOOP("Fast mode on or off")},
        {QStringLiteral("insights"), QT_TR_NOOP("A report on your sessions")},
    };
    QList<Command> list;
    QSet<QString> names;
    for (const auto& c : local) {
        list << Command{QString::fromLatin1(c.name), QString::fromLatin1(c.arguments), tr(c.description), true};
        names << QString::fromLatin1(c.name);
    }
    // Claude Code's, as it said at its last start; before it ever has,
    // those it always has here.
    QStringList program = a_session->slashCommands();
    if (program.isEmpty()) program = QucsSettingsFile().value(kCommands).toStringList();
    if (program.isEmpty())
        program = {QStringLiteral("compact"), QStringLiteral("context"), QStringLiteral("cost"), QStringLiteral("usage"),
                   QStringLiteral("init"), QStringLiteral("review"), QStringLiteral("security-review")};
    for (const QString& name : std::as_const(program)) {
        if (name.isEmpty() || names.contains(name)) continue;
        names << name;
        list << Command{name, QString(),
                        theirs.contains(name) ? tr(theirs.value(name)) : tr("Claude Code's: a skill or a command of its"), false};
    }
    return list;
}

bool ClaudeCodePanel::runCommand(const QString& text)
{
    static const QRegularExpression form(QStringLiteral("^/([A-Za-z0-9][A-Za-z0-9:_.-]*)(?:\\s+(.*))?$"),
                                         QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch m = form.match(text.trimmed());
    if (!m.hasMatch()) return false;
    const QString name = m.captured(1).toLower();
    const QString args = m.captured(2).trimmed();
    const auto cleanLabel = [](QString label) { return label.remove(QLatin1Char('&')); };

    if (name == QLatin1String("clear") || name == QLatin1String("new") || name == QLatin1String("reset")) {
        newConversation();
    } else if (name == QLatin1String("resume") || name == QLatin1String("continue")) {
        emit resumeRequested(args);
    } else if (name == QLatin1String("quit") || name == QLatin1String("exit")) {
        emit closeRequested();
    } else if (name == QLatin1String("help")) {
        QStringList lines{tr("Commands (type / for the list):")};
        QStringList program;
        for (const Command& c : commands()) {
            if (c.local) lines << QStringLiteral("/%1%2 - %3").arg(c.name, c.arguments.isEmpty() ? QString() : QLatin1Char(' ') + c.arguments, c.description);
            else program << QLatin1Char('/') + c.name;
        }
        if (!program.isEmpty()) lines << tr("Claude Code's, sent to it as typed: %1").arg(program.join(QStringLiteral(", ")));
        addNote(lines.join(QLatin1Char('\n')));
    } else if (name == QLatin1String("model")) {
        if (args.isEmpty()) {
            QStringList choices;
            for (QAction* a : modelActions())
                if (!a->data().toString().isEmpty()) choices << a->data().toString();
            const QString inUse = modelName(a_session->modelInUse().isEmpty() ? a_session->model() : a_session->modelInUse());
            addNote(tr("The model: %1. Another, from the next prompt on: /model <name> - %2.")
                        .arg(inUse.isEmpty() ? tr("the default") : inUse, choices.join(QStringLiteral(", "))));
        } else {
            QAction* chosen = nullptr;
            for (QAction* a : modelActions())
                if (!a->data().toString().isEmpty()
                    && (a->data().toString().compare(args, Qt::CaseInsensitive) == 0
                        || cleanLabel(a->text()).compare(args, Qt::CaseInsensitive) == 0))
                    chosen = a;
            const int notes = int(a_entries.size());
            if (chosen != nullptr) chosen->trigger();
            else setModel(args);
            const QString model = chosen != nullptr ? chosen->data().toString() : args;
            if (a_entries.size() == notes)   // (not said already)
                addNote(tr("The model: %1, from the next prompt on.").arg(modelName(model).isEmpty() ? model : modelName(model)));
        }
    } else if (name == QLatin1String("permissions") || name == QLatin1String("mode")) {
        static const QHash<QString, QString> modes = {
            {QStringLiteral("ask"), QString()}, {QStringLiteral("default"), QString()},
            {QStringLiteral("edits"), QStringLiteral("acceptEdits")}, {QStringLiteral("acceptedits"), QStringLiteral("acceptEdits")},
            {QStringLiteral("auto"), QStringLiteral("auto")}, {QStringLiteral("plan"), QStringLiteral("plan")},
            {QStringLiteral("bypass"), QStringLiteral("bypassPermissions")},
            {QStringLiteral("bypasspermissions"), QStringLiteral("bypassPermissions")}};
        QAction* current = nullptr;
        for (QAction* a : permissionActions())
            if (a->isChecked()) current = a;
        if (args.isEmpty() || !modes.contains(args.toLower())) {
            addNote(tr("Permissions: %1. Others: /permissions ask, edits, auto, plan or bypass.")
                        .arg(current != nullptr ? cleanLabel(current->text()) : tr("Ask Before Acting")));
        } else {
            for (QAction* a : permissionActions())
                if (a->data().toString() == modes.value(args.toLower()) && !a->isChecked()) a->trigger();
        }
    } else if (name == QLatin1String("rename")) {
        if (args.isEmpty()) addNote(tr("A name for it: /rename <name>."));
        else setName(args);
    } else if (name == QLatin1String("export")) {
        const QString format = args.toLower();
        if (a_entries.isEmpty()) addNote(tr("There is nothing to export yet."));
        else if (format.startsWith(QLatin1String("md")) || format.startsWith(QLatin1String("markdown"))) exportConversationAs(ExportFormat::Markdown);
        else if (format.startsWith(QLatin1String("t"))) exportConversationAs(ExportFormat::Text);
        else exportConversationAs(ExportFormat::Pdf);
    } else if (name == QLatin1String("status")) {
        QAction* mode = nullptr;
        for (QAction* a : permissionActions())
            if (a->isChecked()) mode = a;
        const QString model = modelName(a_session->modelInUse().isEmpty() ? a_session->model() : a_session->modelInUse());
        QStringList lines;
        lines << tr("Session: %1").arg(a_session->sessionId().isEmpty() ? tr("not started yet") : a_session->sessionId());
        lines << tr("Model: %1").arg(model.isEmpty() ? tr("the default") : model);
        lines << tr("Permissions: %1").arg(mode != nullptr ? cleanLabel(mode->text()) : tr("Ask Before Acting"));
        lines << tr("Folder: %1").arg(QDir::toNativeSeparators(workingDirectory()));
        if (!a_pinned.isEmpty()) lines << tr("Pinned to: %1").arg(QDir::toNativeSeparators(a_pinned));
        if (!a_session->version().isEmpty()) lines << tr("Claude Code: %1").arg(a_session->version());
        // What it has taken so far, as the turns' lines show it.
        const int usage = usageShown();
        if ((usage & (PromptTokens | ConversationTokens)) && !a_session->conversationTokens().isEmpty())
            lines << tr("Used: %1").arg(tokenDetail(a_session->conversationTokens()));
        if ((usage & (PromptCost | ConversationCost)) && a_session->conversationCost() > 0.0)
            lines << tr("Cost: %1, as Claude Code estimates it").arg(cost(a_session->conversationCost()));
        addNote(lines.join(QLatin1Char('\n')));
    } else if (name == QLatin1String("pin")) {
        const QString schematic = pinnableDocument();
        if (schematic.isEmpty()) addNote(tr("No saved schematic is in front to pin (⋯ > Pin to a Schematic lists them all)."));
        else pinDocument(schematic);
    } else if (name == QLatin1String("unpin")) {
        pinDocument(QString());
    } else {
        return false;   // Claude Code's
    }
    return true;
}

void ClaudeCodePanel::updateCommandList()
{
    static const QRegularExpression typing(QStringLiteral("^/([A-Za-z0-9:_.-]*)$"));
    const QRegularExpressionMatch m = typing.match(a_input->toPlainText());
    if (!m.hasMatch()) {
        a_commandList->hide();
        return;
    }
    const QString typed = m.captured(1).toLower();
    QList<Command> starting, containing;
    for (const Command& c : commands()) {
        const QString name = c.name.toLower();
        if (name.startsWith(typed)) starting << c;
        else if (!typed.isEmpty() && name.contains(typed)) containing << c;
    }
    const QList<Command> shown = starting + containing;
    a_commandList->clear();
    if (shown.isEmpty()) {
        a_commandList->hide();
        return;
    }
    for (const Command& c : shown) {
        auto* item = new QListWidgetItem(QStringLiteral("/%1%2   %3")
                                             .arg(c.name, c.arguments.isEmpty() ? QString() : QLatin1Char(' ') + c.arguments,
                                                  c.description),
                                         a_commandList);
        item->setData(Qt::UserRole, c.name);
        item->setData(Qt::UserRole + 1, c.arguments);
        item->setToolTip(c.description);
    }
    a_commandList->setCurrentRow(0);
    // Above the composer, as wide as it.
    const int rows = std::min<int>(int(shown.size()), 8);
    const int height = rows * std::max(1, a_commandList->sizeHintForRow(0)) + 2 * a_commandList->frameWidth() + 2;
    const QRect composer(a_composer->mapTo(this, QPoint(0, 0)), a_composer->size());
    a_commandList->setGeometry(composer.left(), std::max(0, composer.top() - height - 3), composer.width(), height);
    a_commandList->show();
    a_commandList->raise();
}

void ClaudeCodePanel::completeCommand(bool send)
{
    QListWidgetItem* item = a_commandList->currentItem();
    a_commandList->hide();
    if (item == nullptr) {
        if (send) sendComposer();
        return;
    }
    const QString name = item->data(Qt::UserRole).toString();
    // One that needs something after it waits for it to be written.
    const bool needsMore = item->data(Qt::UserRole + 1).toString().startsWith(QLatin1Char('<'));
    if (send && !needsMore) {
        a_input->setPlainText(QLatin1Char('/') + name);
        sendComposer();
        return;
    }
    a_input->setPlainText(QLatin1Char('/') + name + QLatin1Char(' '));
    QTextCursor end = a_input->textCursor();
    end.movePosition(QTextCursor::End);
    a_input->setTextCursor(end);
}

// ----------------------------------------------------------------------
// Exports.

QString ClaudeCodePanel::exportTitle() const
{
    if (!a_name.isEmpty()) return a_name;
    for (const Entry& e : a_entries)
        if (e.kind == Entry::You) {
            QString t = e.text.simplified();
            if (t.size() > 120) t = t.left(119).trimmed() + QChar(0x2026);
            return t;
        }
    return tr("Conversation with Claude");
}

QList<QPair<QString, QString>> ClaudeCodePanel::exportFacts() const
{
    // The model named as the header names it.
    const ModelChoice* choice = choiceFor(a_session->model());
    QString model = modelName(a_session->modelInUse());
    if (model.isEmpty() && choice != nullptr) model = modelName(choice->resolved);
    if (model.isEmpty()) model = modelName(a_session->model());
    QList<QPair<QString, QString>> facts;
    facts << qMakePair(tr("Exported"), QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")));
    facts << qMakePair(tr("Folder"), QDir::toNativeSeparators(workingDirectory()));
    facts << qMakePair(tr("Model"), model.isEmpty() ? tr("the default") : model);
    if (!a_session->version().isEmpty()) facts << qMakePair(tr("Claude Code"), a_session->version());
    if (!a_session->sessionId().isEmpty()) facts << qMakePair(tr("Session"), a_session->sessionId());
    const int usage = usageShown();
    if ((usage & (PromptTokens | ConversationTokens)) && !a_session->conversationTokens().isEmpty())
        facts << qMakePair(tr("Tokens"), tokenDetail(a_session->conversationTokens()));
    if ((usage & (PromptCost | ConversationCost)) && a_session->conversationCost() > 0.0)
        facts << qMakePair(tr("Cost"), cost(a_session->conversationCost()));
    return facts;
}

int ClaudeCodePanel::usageShown()
{
    const QucsSettingsFile settings;
    int usage = 0;
    if (settings.value(kShowPromptTokens, false).toBool()) usage |= PromptTokens;
    if (settings.value(kShowConversationTokens, false).toBool()) usage |= ConversationTokens;
    if (settings.value(kShowPromptCost, false).toBool()) usage |= PromptCost;
    if (settings.value(kShowConversationCost, false).toBool()) usage |= ConversationCost;
    return usage;
}

void ClaudeCodePanel::setUsageShown(int usage)
{
    {
        QucsSettingsFile settings;
        settings.setValue(kShowPromptTokens, bool(usage & PromptTokens));
        settings.setValue(kShowConversationTokens, bool(usage & ConversationTokens));
        settings.setValue(kShowPromptCost, bool(usage & PromptCost));
        settings.setValue(kShowConversationCost, bool(usage & ConversationCost));
    }
    for (ClaudeCodePanel* panel : std::as_const(openPanels())) panel->scheduleRender();
}

QList<QPair<QString, QString>> ClaudeCodePanel::summaryParts(const Entry& e)
{
    QList<QPair<QString, QString>> parts;
    if (!e.text.isEmpty()) parts << qMakePair(e.text, QString());
    const int usage = usageShown();
    // The conversation's, unless it is the prompt's shown already (the
    // first turn's).
    if ((usage & PromptTokens) && !e.tokens.isEmpty())
        parts << qMakePair(tr("%1 tokens").arg(tokenCount(e.tokens.total())), tokenDetail(e.tokens));
    if ((usage & ConversationTokens) && !e.allTokens.isEmpty()
        && (!(usage & PromptTokens) || e.allTokens.total() > e.tokens.total()))
        parts << qMakePair(tr("%1 tokens in all").arg(tokenCount(e.allTokens.total())),
                           tr("The conversation so far: %1").arg(tokenDetail(e.allTokens)));
    if ((usage & PromptCost) && e.cost > 0.0)
        parts << qMakePair(cost(e.cost), tr("What the prompt cost, as Claude Code estimates it"));
    if ((usage & ConversationCost) && e.allCost > 0.0 && (!(usage & PromptCost) || e.allCost > e.cost + 0.0005))
        parts << qMakePair(tr("%1 in all").arg(cost(e.allCost)), tr("What the conversation has cost so far, as Claude Code estimates it"));
    if (!e.result.isEmpty()) parts << qMakePair(e.result, QString());
    return parts;
}

QString ClaudeCodePanel::summaryText(const Entry& e)
{
    QStringList texts;
    for (const auto& part : summaryParts(e)) texts << part.first;
    return texts.join(QStringLiteral("  ·  "));
}

bool ClaudeCodePanel::exportsToolDetails()
{
    return QucsSettingsFile().value(kExportDetails, true).toBool();
}

void ClaudeCodePanel::setExportsToolDetails(bool on)
{
    QucsSettingsFile().setValue(kExportDetails, on);
}

QString ClaudeCodePanel::conversationMarkdown() const
{
    const bool details = exportsToolDetails();
    QString md = QStringLiteral("# ") + exportTitle() + QStringLiteral("\n\n");
    for (const auto& [label, value] : exportFacts()) md += QStringLiteral("- **%1:** %2\n").arg(label, value);
    md += QStringLiteral("\n---\n");
    bool captioned = false;
    const auto caption = [&] {
        if (captioned) return;
        captioned = true;
        md += QStringLiteral("\n### ") + tr("Claude") + QLatin1Char('\n');
    };
    for (qsizetype i = 0; i < a_entries.size(); ++i) {
        const Entry& e = a_entries.at(i);
        switch (e.kind) {
        case Entry::You:
            captioned = false;
            md += QStringLiteral("\n### ") + tr("You") + QStringLiteral("\n\n");
            for (const QString& line : e.text.split(QLatin1Char('\n')))
                md += (line.isEmpty() ? QStringLiteral(">") : QStringLiteral("> ") + line) + QLatin1Char('\n');
            if (!e.extra.isEmpty()) md += QStringLiteral(">\n> ↳ ") + inlineCode(e.extra) + QLatin1Char('\n');
            break;
        case Entry::Claude:
            caption();
            md += QLatin1Char('\n') + e.text.trimmed() + QLatin1Char('\n');
            break;
        case Entry::Tool: {
            caption();
            // A row of tools is a list; each with its input and what it gave.
            if (i == 0 || a_entries.at(i - 1).kind != Entry::Tool) {
                md += QLatin1Char('\n');
                qsizetype end = i + 1;
                while (end < a_entries.size() && a_entries.at(end).kind == Entry::Tool) ++end;
                if (!details && end - i > 1) {
                    // Without the details, the row as the dock shows it folded.
                    md += QStringLiteral("- %1 **%2**").arg(outcomeMark(rowOutcome(i, end)), toolSummary(i, end));
                    if (const QString trouble = rowTrouble(i, end); !trouble.isEmpty()) md += QStringLiteral(" · ") + trouble;
                    md += QLatin1Char('\n');
                    i = end - 1;
                    break;
                }
            }
            md += QStringLiteral("- %1 **%2**").arg(outcomeMark(e.tool), toolName(e.text));
            if (!e.extra.isEmpty()) md += QLatin1Char(' ') + inlineCode(e.extra);
            md += QLatin1Char('\n');
            if (!e.output.isEmpty() && e.tool != Entry::Succeeded)
                md += QStringLiteral("\n  ") + (e.tool == Entry::Denied ? tr("Not allowed: %1") : tr("Failed: %1")).arg(e.output) + QLatin1Char('\n');
            if (!details) break;
            if (!e.detail.trimmed().isEmpty()) md += QLatin1Char('\n') + codeBlock(e.detail.trimmed(), QStringLiteral("  "));
            if (!e.result.trimmed().isEmpty()) md += QLatin1Char('\n') + codeBlock(e.result.trimmed(), QStringLiteral("  "));
            break;
        }
        case Entry::Note:
            md += QStringLiteral("\n*") + e.text.trimmed() + QStringLiteral("*\n");
            break;
        case Entry::Problem:
            md += QStringLiteral("\n> **⚠** ") + e.text.trimmed().replace(QLatin1Char('\n'), QStringLiteral("\n> ")) + QLatin1Char('\n');
            break;
        case Entry::Summary:
            if (const QString line = summaryText(e); !line.isEmpty()) md += QStringLiteral("\n*") + line + QStringLiteral("*\n");
            break;
        }
    }
    return md;
}

QString ClaudeCodePanel::conversationText() const
{
    const bool details = exportsToolDetails();
    QStringList out;
    out << exportTitle() << QString(exportTitle().size(), QLatin1Char('=')) << QString();
    int width = 0;
    const QList<QPair<QString, QString>> facts = exportFacts();
    for (const auto& fact : facts) width = std::max(width, int(fact.first.size()));
    for (const auto& [label, value] : facts) out << (label + QLatin1Char(':')).leftJustified(width + 2) + value;
    const auto indented = [](const QString& text, const QString& by) {
        QStringList lines = text.split(QLatin1Char('\n'));
        for (QString& line : lines) line = line.isEmpty() ? QString() : by + line;
        return lines.join(QLatin1Char('\n'));
    };
    bool captioned = false;
    for (qsizetype i = 0; i < a_entries.size(); ++i) {
        const Entry& e = a_entries.at(i);
        const bool afterTool = i > 0 && a_entries.at(i - 1).kind == Entry::Tool;
        if (e.kind != Entry::Tool || !afterTool) out << QString();
        switch (e.kind) {
        case Entry::You:
            captioned = false;
            out << tr("You:") << e.text.trimmed();
            if (!e.extra.isEmpty()) out << QStringLiteral("↳ ") + e.extra;
            break;
        case Entry::Claude:
            if (!captioned) out << tr("Claude:");
            captioned = true;
            out << plainTextOf(e.text.trimmed());
            break;
        case Entry::Tool: {
            if (!captioned) out << tr("Claude:");
            captioned = true;
            if (!details && !afterTool) {
                qsizetype end = i + 1;
                while (end < a_entries.size() && a_entries.at(end).kind == Entry::Tool) ++end;
                if (end - i > 1) {
                    // Without the details, the row as the dock shows it folded.
                    QString row = QStringLiteral("  %1 %2").arg(outcomeMark(rowOutcome(i, end)), toolSummary(i, end));
                    if (const QString trouble = rowTrouble(i, end); !trouble.isEmpty()) row += QStringLiteral("  ·  ") + trouble;
                    out << row;
                    i = end - 1;
                    break;
                }
            }
            QString line = QStringLiteral("  %1 %2").arg(outcomeMark(e.tool), toolName(e.text));
            if (!e.extra.isEmpty()) line += QStringLiteral("   ") + e.extra;
            out << line;
            if (!e.output.isEmpty() && e.tool != Entry::Succeeded)
                out << QStringLiteral("      ") + (e.tool == Entry::Denied ? tr("Not allowed: %1") : tr("Failed: %1")).arg(e.output);
            if (!details) break;
            if (!e.detail.trimmed().isEmpty()) out << indented(e.detail.trimmed(), QStringLiteral("      "));
            if (!e.result.trimmed().isEmpty()) out << indented(e.result.trimmed(), QStringLiteral("      │ "));
            break;
        }
        case Entry::Note:
            out << QStringLiteral("(") + e.text.trimmed() + QStringLiteral(")");
            break;
        case Entry::Summary:
            if (const QString line = summaryText(e); !line.isEmpty()) out << QStringLiteral("(") + line + QStringLiteral(")");
            break;
        case Entry::Problem:
            out << QStringLiteral("⚠ ") + e.text.trimmed();
            break;
        }
    }
    return out.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

bool ClaudeCodePanel::exportConversation(const QString& path, ExportFormat format, QString* error)
{
    const auto fail = [error](const QString& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    if (format != ExportFormat::Pdf) {
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return fail(file.errorString());
        file.write((format == ExportFormat::Markdown ? conversationMarkdown() : conversationText()).toUtf8());
        if (!file.commit()) return fail(file.errorString());
        return true;
    }

    // The conversation drawn as in the dock - on paper, every tool open
    // (or, without the details, every one folded) - then laid out on pages
    // and painted onto them, a footer on each.
    QTextDocument doc;
    doc.setDefaultFont(a_view->font());
    doc.setDocumentMargin(0);
    qucs_s::math::MathObject::install(&doc);
    QPalette paper;
    {
        const QScopedValueRollback<bool> onPaper(a_exporting, true);
        const QScopedValueRollback<bool> open(a_exportDetails, exportsToolDetails());
        paper = drawingPalette();
        const Colours col = colours(paper);
        QTextCursor c(&doc);
        QTextCharFormat head;
        QFont big = a_view->font();
        big.setPointSizeF(big.pointSizeF() * 1.5);
        big.setWeight(QFont::DemiBold);
        head.setFont(big);
        head.setForeground(col.text);
        c.insertText(exportTitle(), head);
        QTextCharFormat fact;
        QFont small = a_view->font();
        small.setPointSizeF(std::max(7.0, small.pointSizeF() * 0.88));
        fact.setFont(small);
        fact.setForeground(col.muted);
        QTextBlockFormat line;
        line.setTopMargin(2);
        for (const auto& [label, value] : exportFacts()) {
            c.insertBlock(line);
            c.insertText(label + QStringLiteral(":  ") + value, fact);
        }
        QTextBlockFormat rule;
        rule.setTopMargin(6);
        rule.setBottomMargin(10);
        rule.setProperty(QTextFormat::BlockTrailingHorizontalRulerWidth, QTextLength(QTextLength::PercentageLength, 100));
        c.insertBlock(rule);
        QTextBlockFormat next;
        c.insertBlock(next);
        renderConversation(c);
    }

    QPdfWriter pdf(path);
    pdf.setTitle(exportTitle());
    pdf.setCreator(QStringLiteral("Qucs-S"));
    pdf.setResolution(300);
    pdf.setPageSize(QPageSize(QLocale().measurementSystem() == QLocale::ImperialUSSystem ? QPageSize::Letter : QPageSize::A4));
    pdf.setPageMargins(QMarginsF(16, 16, 16, 14), QPageLayout::Millimeter);
    QPainter p;
    if (!p.begin(&pdf)) return fail(tr("%1 cannot be written.").arg(QDir::toNativeSeparators(path)));
    // The document is laid out as on the screen; the page is that, larger.
    const QScreen* screen = QGuiApplication::primaryScreen();
    const qreal scale = pdf.resolution() / (screen != nullptr ? screen->logicalDotsPerInchY() : 96.0);
    const QRect area = pdf.pageLayout().paintRectPixels(pdf.resolution());
    // A footer under each: the title, the page's number.
    QFont small = a_view->font();
    small.setPointSizeF(std::max(7.0, small.pointSizeF() * 0.8));
    const QFontMetricsF footing(small, &pdf);
    const qreal footer = footing.height() * 2.2;   // (on the page)
    const QSizeF page(area.width() / scale, (area.height() - footer) / scale);
    doc.setPageSize(page);
    const int pages = doc.pageCount();
    const Colours col = colours(paper);
    QAbstractTextDocumentLayout::PaintContext context;
    context.palette = paper;
    for (int i = 0; i < pages; ++i) {
        if (i > 0) pdf.newPage();
        p.save();
        p.scale(scale, scale);
        p.translate(0, -i * page.height());
        context.clip = QRectF(QPointF(0, i * page.height()), page);
        p.setClipRect(context.clip);
        doc.documentLayout()->draw(&p, context);
        p.restore();
        const QRectF foot(0, area.height() - footer, area.width(), footer);
        p.setFont(small);
        p.setPen(col.muted);
        p.drawText(foot, Qt::AlignLeft | Qt::AlignBottom, footing.elidedText(exportTitle(), Qt::ElideRight, area.width() * 0.75));
        p.drawText(foot, Qt::AlignRight | Qt::AlignBottom, tr("%1 of %2").arg(i + 1).arg(pages));
    }
    if (!p.end()) return fail(tr("%1 cannot be written.").arg(QDir::toNativeSeparators(path)));
    return true;
}

void ClaudeCodePanel::exportConversationAs(ExportFormat format)
{
    if (a_entries.isEmpty()) return;
    const QString suffix = format == ExportFormat::Pdf ? QStringLiteral(".pdf")
                           : format == ExportFormat::Markdown ? QStringLiteral(".md")
                                                              : QStringLiteral(".txt");
    const QString filter = format == ExportFormat::Pdf ? tr("PDF (*.pdf)")
                           : format == ExportFormat::Markdown ? tr("Markdown (*.md *.markdown)")
                                                              : tr("Text (*.txt)");
    QucsSettingsFile settings;
    QString dir = settings.value(kExportDir).toString();
    if (dir.isEmpty() || !QFileInfo(dir).isDir()) dir = workingDirectory();
    QString path = misc::saveFileName(this, tr("Export the Conversation"),
                                                QDir(dir).filePath(fileNameFor(exportTitle()) + suffix), filter);
    if (path.isEmpty()) return;
    if (QFileInfo(path).suffix().isEmpty()) path += suffix;
    settings.setValue(kExportDir, QFileInfo(path).absolutePath());
    QString error;
    if (!exportConversation(path, format, &error)) {
        QMessageBox::warning(this, tr("Claude Code"), tr("The conversation could not be exported: %1").arg(error));
        return;
    }
    // Said for a moment where the state is.
    a_stateText->setText(tr("Exported to %1").arg(QFileInfo(path).fileName()));
    QTimer::singleShot(4000, this, &ClaudeCodePanel::updateState);
}

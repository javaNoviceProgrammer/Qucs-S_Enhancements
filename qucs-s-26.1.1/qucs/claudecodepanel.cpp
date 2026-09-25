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

#include "apptheme.h"
#include "ink.h"
#include "settings.h"

#include <QAbstractTextDocumentLayout>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStyle>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocumentFragment>
#include <QTextFormat>
#include <QTextFrame>
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
    auto* cardHolder = new QWidget(this);
    auto* cardLayout = new QVBoxLayout(cardHolder);
    cardLayout->setContentsMargins(10, 4, 10, 0);
    cardLayout->addWidget(a_card);
    layout->addWidget(cardHolder);

    buildComposer();
    auto* composerHolder = new QWidget(this);
    auto* composerLayout = new QVBoxLayout(composerHolder);
    composerLayout->setContentsMargins(10, 6, 10, 10);
    composerLayout->addWidget(a_composer);
    layout->addWidget(composerHolder);

    buildMenu();

    connect(a_session, &qucs_s::claude::Session::stateChanged, this, [this] { updateState(); });
    connect(a_session, &qucs_s::claude::Session::sessionStarted, this, [this] { updateState(); });
    connect(a_session, &qucs_s::claude::Session::replyStreamed, this, &ClaudeCodePanel::onReplyStreamed);
    connect(a_session, &qucs_s::claude::Session::replyFinished, this, &ClaudeCodePanel::onReplyFinished);
    connect(a_session, &qucs_s::claude::Session::toolStarted, this, &ClaudeCodePanel::onToolStarted);
    connect(a_session, &qucs_s::claude::Session::toolFinished, this, &ClaudeCodePanel::onToolFinished);
    connect(a_session, &qucs_s::claude::Session::permissionRequested, this, &ClaudeCodePanel::onPermissionRequested);
    connect(a_session, &qucs_s::claude::Session::permissionWithdrawn, this, &ClaudeCodePanel::onPermissionWithdrawn);
    connect(a_session, &qucs_s::claude::Session::permissionModeChanged, this, [this](const QString& mode) {
        QucsSettingsFile().setValue(kMode, mode);
        addNote(tr("Claude may change files without asking for the rest of this conversation."));
    });
    connect(a_session, &qucs_s::claude::Session::notice, this, [this](const QString& text) {
        append({Entry::Note, text, {}, {}});
    });
    connect(a_session, &qucs_s::claude::Session::failed, this, [this](const QString& message) {
        append({Entry::Problem, message, {}, {}});
    });
    connect(a_session, &qucs_s::claude::Session::turnFinished, this, &ClaudeCodePanel::onTurnFinished);
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

ClaudeCodePanel::~ClaudeCodePanel() = default;

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
    a_menuButton->setToolTip(tr("Permissions, model, folder, program"));
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
    a_hint = new QLabel(tr("⏎ send  ·  ⇧⏎ new line"), a_composer);
    a_hint->setObjectName(QStringLiteral("claudeMuted"));
    a_send = new QToolButton(a_composer);
    a_send->setObjectName(QStringLiteral("claudeSend"));
    a_send->setText(tr("Send"));
    row->addWidget(a_attach);
    row->addStretch(1);
    row->addWidget(a_hint);
    row->addWidget(a_send);
    layout->addLayout(row);
    connect(a_input, &QPlainTextEdit::textChanged, this, &ClaudeCodePanel::updateComposer);
    connect(a_attach, &QToolButton::toggled, this, [](bool on) { QucsSettingsFile().setValue(kAttach, on); });
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
    QAction* copyId = a_menu->addAction(tr("Copy Session ID"), this, [this] {
        QApplication::clipboard()->setText(a_session->sessionId());
    });
    connect(a_menu, &QMenu::aboutToShow, this, [this, workspace, show, copyId, again] {
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
        "QFrame#claudePermission { background: %11; border: 1px solid %5; border-radius: 10px; }"
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
        } else if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut) {
            a_composer->setProperty("focused", event->type() == QEvent::FocusIn);
            a_composer->style()->unpolish(a_composer);
            a_composer->style()->polish(a_composer);
        }
    }
    return QWidget::eventFilter(watched, event);
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
    const bool running = a_session->isRunning();
    a_session->setPermissionMode(mode);
    if (running && !a_entries.isEmpty()) {
        QString name = a_modes->checkedAction() != nullptr ? a_modes->checkedAction()->text() : mode;
        addNote(tr("Permissions: %1, from the next prompt on.").arg(name.remove(QLatin1Char('&'))));
    }
    if (a_entries.isEmpty()) scheduleRender();   // the welcome says what Claude may do
    updateState();
}

void ClaudeCodePanel::setModel(const QString& model)
{
    QucsSettingsFile().setValue(kModel, model);
    const bool running = a_session->isRunning();
    a_session->setModel(model);
    if (running && !a_entries.isEmpty())
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
}

void ClaudeCodePanel::setDocumentProvider(std::function<QString()> provider)
{
    a_document = std::move(provider);
    refreshDocument();
}

void ClaudeCodePanel::refreshDocument()
{
    const QString doc = a_document ? a_document() : QString();
    if (doc.isEmpty()) {
        a_attach->setText(tr("No document"));
        a_attach->setEnabled(false);
        a_attach->setToolTip(tr("No document is open (or it has no file yet)"));
    } else {
        a_attach->setText(QFileInfo(doc).fileName());
        a_attach->setEnabled(true);
        a_attach->setToolTip(tr("Tell Claude that %1 is open in Qucs-S").arg(QDir::toNativeSeparators(doc)));
    }
}

// ----------------------------------------------------------------------
void ClaudeCodePanel::updateState()
{
    const Colours c = colours(palette());
    const State s = a_session->state();
    const bool busy = a_session->isBusy();
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
    const QString text = a_input->toPlainText().trimmed();
    if (text.isEmpty() || a_session->isBusy()) return;
    if (a_session->program().isEmpty()) findProgram();
    if (a_session->program().isEmpty()) {
        append({Entry::Problem,
                tr("Claude Code was not found. Install it (claude.com/claude-code), sign in once with "
                   "\"claude\" in a terminal, and send again - or choose the program under ⋯."),
                {}, {}});
        return;
    }
    if (a_session->workingDirectory() != workingDirectory()) a_session->setWorkingDirectory(workingDirectory());

    QString prompt = text;
    QString attached;
    refreshDocument();
    if (a_attach->isEnabled() && a_attach->isChecked() && a_document) {
        attached = a_document();
        if (!attached.isEmpty())
            prompt += QStringLiteral("\n\n") + tr("(The document open in Qucs-S: %1)").arg(QDir::toNativeSeparators(attached));
    }
    append({Entry::You, text, attached.isEmpty() ? QString() : QFileInfo(attached).fileName(), {}});
    if (a_session->send(prompt)) a_input->clear();
    updateState();
}

void ClaudeCodePanel::stopTurn()
{
    a_session->interrupt();
}

void ClaudeCodePanel::newConversation()
{
    a_session->reset();
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
    QStringList parts;
    if (r.stopped) {
        parts << tr("Stopped");
    } else if (r.ok) {
        parts << tr("Done in %1").arg(seconds(r.durationMs));
    } else {
        append({Entry::Problem, a_session->detail().isEmpty() ? tr("The turn failed.") : a_session->detail(), {}, {}});
    }
    if (r.ok || r.stopped) {
        if (r.costUsd > 0.0) parts << cost(r.costUsd);
        if (r.conversationCostUsd > r.costUsd + 0.0005) parts << tr("%1 in all").arg(cost(r.conversationCostUsd));
        if (r.turns > 1) parts << tr("%1 steps").arg(r.turns);
        if (r.denials > 0) parts << (r.denials == 1 ? tr("1 action not allowed") : tr("%1 actions not allowed").arg(r.denials));
    }
    if (!r.changedFiles.isEmpty()) {
        QStringList names;
        for (const QString& f : r.changedFiles) names << QFileInfo(f).fileName();
        parts << tr("changed %1").arg(names.join(QStringLiteral(", ")));
    }
    if (!parts.isEmpty()) append({Entry::Summary, parts.join(QStringLiteral("  ·  ")), {}, {}});
    a_requests.clear();
    showNextRequest();
    updateState();
    if (!r.changedFiles.isEmpty()) emit filesChanged(r.changedFiles);
}

// ----------------------------------------------------------------------
void ClaudeCodePanel::handleLink(const QUrl& url)
{
    if (url.scheme() == QLatin1String("toggle")) {
        toggle(url.path());
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
    if (a_entries.isEmpty()) {
        renderWelcome(c);
    } else {
        bool captioned = false;
        for (qsizetype i = 0; i < a_entries.size();) {
            if (a_entries.at(i).kind == Entry::Tool) {
                qsizetype j = i + 1;
                while (j < a_entries.size() && a_entries.at(j).kind == Entry::Tool) ++j;
                renderTools(c, i, j, captioned);
                i = j;
                continue;
            }
            renderEntry(c, a_entries.at(i), captioned);
            ++i;
        }
    }

    if (follow) {
        QTimer::singleShot(0, this, [this] {
            a_view->verticalScrollBar()->setValue(a_view->verticalScrollBar()->maximum());
        });
    } else {
        bar->setValue(keep);
    }
}

void ClaudeCodePanel::renderWelcome(QTextCursor& c)
{
    const Colours col = colours(palette());
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

void ClaudeCodePanel::renderEntry(QTextCursor& c, const Entry& e, bool& captioned)
{
    const Colours col = colours(palette());
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
        if (e.streaming) {
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
        QTextBlockFormat f;
        f.setTopMargin(8);
        startBlock(c, f);
        c.insertText(e.text, muted);
        break;
    }
    }
}

void ClaudeCodePanel::renderCaption(QTextCursor& c, bool& captioned)
{
    if (captioned) return;
    captioned = true;
    const Colours col = colours(palette());
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
    const Colours col = colours(palette());
    QFont small = a_view->font();
    small.setPointSizeF(std::max(7.0, small.pointSizeF() * 0.88));
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSizeF(small.pointSizeF());

    const QString key = QStringLiteral("group:") + a_entries.at(from).id;
    const bool open = a_expanded.contains(key);
    int running = 0, failed = 0, denied = 0;
    const Entry* now = nullptr;
    for (qsizetype i = from; i < to; ++i) {
        const Entry& e = a_entries.at(i);
        if (e.tool == Entry::Running) {
            ++running;
            now = &e;
        }
        if (e.tool == Entry::Failed) ++failed;
        if (e.tool == Entry::Denied) ++denied;
    }
    QTextBlockFormat f;
    f.setTopMargin(3);
    f.setLeftMargin(2);
    startBlock(c, f);
    QTextCharFormat link;
    link.setAnchor(true);
    link.setAnchorHref(QStringLiteral("toggle:") + key);
    link.setToolTip(open ? tr("Fold the tools away") : tr("Show each tool Claude used"));
    QTextCharFormat twisty = link;
    twisty.setFont(small);
    twisty.setForeground(col.faint);
    c.insertText(open ? QStringLiteral("▾ ") : QStringLiteral("▸ "), twisty);
    QTextCharFormat mark = twisty;
    mark.setFontWeight(QFont::DemiBold);
    QString glyph;
    if (running > 0) glyph = QStringLiteral("○"), mark.setForeground(col.accent);
    else if (failed > 0) glyph = QStringLiteral("✕"), mark.setForeground(col.error);
    else if (denied > 0) glyph = QStringLiteral("⊘"), mark.setForeground(col.warn);
    else glyph = QStringLiteral("✓"), mark.setForeground(col.ok);
    c.insertText(glyph + QStringLiteral("  "), mark);
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
    } else if (failed + denied > 0) {
        QStringList notes;
        if (failed > 0) notes << (failed == 1 ? tr("1 failed") : tr("%1 failed").arg(failed));
        if (denied > 0) notes << (denied == 1 ? tr("1 not allowed") : tr("%1 not allowed").arg(denied));
        c.insertText(QStringLiteral("  ·  ") + notes.join(QStringLiteral(", ")), quiet);
    }
    if (open)
        for (qsizetype i = from; i < to; ++i) renderTool(c, a_entries.at(i), 16);
    roomAfter();
}

void ClaudeCodePanel::renderTool(QTextCursor& c, const Entry& e, qreal indent)
{
    const Colours col = colours(palette());
    QFont small = a_view->font();
    small.setPointSizeF(std::max(7.0, small.pointSizeF() * 0.88));
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSizeF(small.pointSizeF());

    const QString key = QStringLiteral("tool:") + e.id;
    const bool more = !e.detail.trimmed().isEmpty() || !e.result.trimmed().isEmpty();
    const bool open = more && a_expanded.contains(key);
    QTextBlockFormat f;
    f.setTopMargin(3);
    f.setLeftMargin(2 + indent);
    startBlock(c, f);
    QTextCharFormat link;
    if (more) {
        link.setAnchor(true);
        link.setAnchorHref(QStringLiteral("toggle:") + key);
        link.setToolTip(open ? tr("Fold it away") : tr("Show the whole of it, and what it gave"));
    }
    QTextCharFormat twisty = link;
    twisty.setFont(small);
    twisty.setForeground(more ? col.faint : col.base);
    c.insertText(open ? QStringLiteral("▾ ") : QStringLiteral("▸ "), twisty);
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
    const QColor colour = colours(palette()).text;
    const qreal dpr = a_view->devicePixelRatioF();
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
    const Colours col = colours(palette());
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
    doc.setMarkdown(md, QTextDocument::MarkdownDialectGitHub);
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
    for (const Entry& e : a_entries)
        if (e.kind == Entry::You) {
            QString t = e.text.simplified();
            if (t.size() > 40) t = t.left(39).trimmed() + QChar(0x2026);
            return t;
        }
    return tr("New conversation");
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

/*
 * claudecodepanel.h - the Claude Code dock: a conversation with Claude
 *                     Code, working in the workspace folder
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_CLAUDECODEPANEL_H
#define QUCS_CLAUDECODEPANEL_H

#include "claudecode.h"
#include "mathtypeset.h"

#include <QHash>
#include <QList>
#include <QPair>
#include <QPixmap>
#include <QSet>
#include <QWidget>

#include <functional>

class QAction;
class QActionGroup;
class QFrame;
class QLabel;
class QListWidget;
class QMenu;
class QPlainTextEdit;
class QTextBrowser;
class QTextCursor;
class QTimer;
class QToolButton;
class QUrl;

/*!
 * The Claude Code dock. At the top, the state of the session, the model,
 * a new conversation and a menu (who decides what Claude may do, the
 * model, the folder, the program, the conversation exported as a PDF,
 * Markdown or plain text); under it the folder Claude works in -
 * the workspace unless another is chosen. Then the conversation: the
 * prompts, the replies as they are written (Markdown), each tool Claude
 * uses and how it went, how each turn ended. A tool that needs permission
 * is asked about in a card above the prompt, which is written at the
 * bottom: Enter sends, Shift+Enter starts a line, and the document in
 * front can go along with it. The tools Claude uses in a row fold into a
 * line ("Ran 3 commands, read 2 files") that opens on a click, and each
 * of them opens on the whole command and what it gave; TeX math in the
 * replies is typeset (mathtypeset.h).
 *
 * One panel is one conversation; ClaudeCodeTabs keeps several.
 *
 * The program is the one the settings name, else claude as installed
 * (qucs_s::claude::findProgram()); QUCS_CLAUDE in the environment names
 * it over both.
 */
class ClaudeCodePanel : public QWidget
{
    Q_OBJECT

public:
    explicit ClaudeCodePanel(QWidget* parent = nullptr);
    ~ClaudeCodePanel() override;

    qucs_s::claude::Session* session() const { return a_session; }

    /// Where Claude works unless the user chose another folder: the
    /// workspace. A panel working there moves along (a new conversation).
    void setDefaultDirectory(const QString& dir);
    QString defaultDirectory() const { return a_defaultDir; }
    /// Where Claude works now.
    QString workingDirectory() const;
    /// Works in \a dir from now on - a new conversation; empty: the
    /// default directory.
    void setWorkingDirectory(const QString& dir);

    /// What names the document in front (its file, empty for none).
    void setDocumentProvider(std::function<QString()> provider);
    /// The document in front changed.
    void refreshDocument();
    /// What names the open schematics a conversation can be pinned to
    /// (their files).
    void setSchematicsProvider(std::function<QStringList()> provider);
    /// The schematic the conversation is pinned to (its file), or empty:
    /// none, it follows the document in front - as at first. Pinned, its
    /// prompts name that schematic, not the document in front, and
    /// Qucs-S's tools act on it when Claude names no document
    /// (Session::setDocument()). Chosen by the user: the pin by the
    /// composer, or ⋯ > Pin to a Schematic.
    QString pinnedDocument() const { return a_pinned; }
    /// Pins the conversation to \a path; empty: unpins it.
    void pinDocument(const QString& path);
    /// Whether it is pinned to the file \a path.
    bool isPinnedTo(const QString& path) const;
    /// The schematic in front, when it has a file (what the pin pins), or
    /// empty.
    QString pinnableDocument() const;

    /// A line in the conversation from the application (a document
    /// reloaded, say).
    void addNote(const QString& text);

    /// What the conversation is about: the name it was given, else its
    /// first prompt, shortened; "New conversation" before one.
    QString title() const;
    /// The name given to the conversation (Rename, in its tab's menu), or
    /// empty: its first prompt names it. A new conversation has none.
    QString name() const { return a_name; }
    /// Names the conversation \a name (its spaces tidied); empty: its
    /// first prompt names it again.
    void setName(const QString& name);
    /// The mark of its state, as in the header.
    QPixmap statePixmap() const;
    /// New starts a conversation in a tab of its own
    /// (newConversationRequested) rather than here.
    void setNewInTab(bool on);
    /// Claude's colour (its clay), for the palette given.
    static QColor accentColour(const QPalette& palette);

    /// The conversation as it is kept (claudehistory.h): its title and
    /// name, its folder, the schematic pinned, Claude Code's session, and
    /// what was said.
    QJsonObject conversationJson() const;
    /// Brings back one kept (conversationJson()): what was said shown as
    /// it was, its session continued from the next prompt (--resume).
    void restoreConversation(const QJsonObject& conversation);
    /// Brings back one of Claude Code's own sessions from its file: what
    /// was said, as far as it reads, and the session continued.
    bool importClaudeSession(const QString& file);
    /// Where it is kept (claudehistory.h): empty until it is, and again
    /// once it begins anew.
    QString conversationId() const { return a_conversationId; }
    void setConversationId(const QString& id) { a_conversationId = id; }

    /// A command typed as "/name arguments": the dock's own - /clear,
    /// /resume, /quit, /help, /model, /permissions, /rename, /export,
    /// /status, /pin... - or Claude Code's (its skills too), which is sent
    /// to it as typed.
    struct Command {
        QString name;          ///< without its slash
        QString arguments;     ///< what it takes, as help shows it
        QString description;
        bool local = false;    ///< run by the dock
    };
    /// Every command, the dock's first, then Claude Code's (those it
    /// said it has, kept from its last start).
    QList<Command> commands() const;
    /// Runs \a text when it is one of the dock's commands; false when it
    /// is not (Claude Code's, or no command at all).
    bool runCommand(const QString& text);

    /// What a conversation is exported as.
    enum class ExportFormat { Pdf, Markdown, Text };
    /// Whether an export holds what the boxes that fold hold - each tool's
    /// input and what it gave - or, off, a row of tools as the one line
    /// that sums it up, as the dock shows it folded: the chat alone
    /// (Export Conversation > Include Tool Details; kept in the settings,
    /// on at first).
    static bool exportsToolDetails();
    static void setExportsToolDetails(bool on);
    /// The whole conversation - a header (what it is about, when, the
    /// folder, the model, the session), then every prompt, reply, tool
    /// (its input and what it gave, as when it is opened, unless
    /// exportsToolDetails() is off), note, problem and how each turn ended
    /// - as Markdown (the replies as Claude wrote them), as plain text
    /// (their Markdown read, the math as TeX), or drawn as in the dock on
    /// paper, a PDF (the math typeset).
    QString conversationMarkdown() const;
    QString conversationText() const;
    bool exportConversation(const QString& path, ExportFormat format, QString* error = nullptr);
    /// Asks where to, then exports (⋯ › Export Conversation).
    void exportConversationAs(ExportFormat format);
    bool hasConversation() const { return !a_entries.isEmpty(); }

    // For the tests.
    QPlainTextEdit* composer() const { return a_input; }
    QTextBrowser* transcript() const { return a_view; }
    QToolButton* sendButton() const { return a_send; }
    QToolButton* attachButton() const { return a_attach; }
    QToolButton* pinButton() const { return a_pin; }
    /// The commands matching what is typed, as the composer offers them.
    QListWidget* commandList() const { return a_commandList; }
    QMenu* pinMenu() const { return a_pinMenu; }
    QFrame* permissionCard() const { return a_card; }
    QToolButton* allowButton() const { return a_allow; }
    QToolButton* allowEditsButton() const { return a_allowEdits; }
    QToolButton* allowToolsButton() const { return a_allowTools; }
    QToolButton* denyButton() const { return a_deny; }
    QToolButton* newButton() const { return a_newButton; }
    QLabel* stateLabel() const { return a_stateText; }
    QLabel* modelLabel() const { return a_modelLabel; }
    QList<QAction*> modelActions() const;
    QList<QAction*> permissionActions() const;
    qucs_s::claude::ModelQuery* modelQuery() const { return a_modelQuery; }
    QString transcriptText() const;
    /// The conversation drawn now rather than a moment later.
    void renderNow();
    /// Opens or folds a line of tools ("group:<first tool's id>") or a
    /// tool ("tool:<id>"), as a click on it does.
    void toggle(const QString& key);
    bool isExpanded(const QString& key) const { return a_expanded.contains(key); }

public slots:
    void focusComposer();
    /// Sends what the composer holds.
    void sendComposer();
    /// Stops the turn under way.
    void stopTurn();
    void newConversation();

signals:
    /// Claude changed these files in a turn.
    void filesChanged(const QStringList& files);
    /// A file named in the conversation was clicked.
    void openFileRequested(const QString& path);
    /// New was pressed, and new conversations open in tabs.
    void newConversationRequested();
    /// The title changed (the first prompt was sent, the conversation was
    /// named, or it began again).
    void titleChanged();
    /// The conversation was pinned to a schematic, or unpinned.
    void pinChanged();
    /// What it holds changed: to be kept again.
    void conversationChanged();
    /// It is about to begin anew, or to take another one's place: kept
    /// now, as it is.
    void conversationEnding();
    /// /quit, /exit: its tab to close.
    void closeRequested();
    /// /resume (\a query: what was typed after it), ⋯ > Resume a
    /// Conversation.
    void resumeRequested(const QString& query);

protected:
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    struct Entry {
        enum Kind { You, Claude, Tool, Note, Problem, Summary } kind;
        QString text;          // the prompt, the reply (Markdown), the note
        QString extra;         // the document sent along; a tool's subject; a failure
        QString id;            // a tool use's
        QString output = QString();   // why a tool failed
        enum ToolState { Running, Succeeded, Failed, Denied } tool = Running;
        bool streaming = false;
        QString detail = QString();   // a tool's input: the command, the edit
        QString result = QString();   // what it gave (the first lines)
    };

    void buildHeader();
    void buildComposer();
    void buildPermissionCard();
    void buildMenu();
    void fillPinMenu();
    /// The commands matching what the composer holds, offered above it.
    void updateCommandList();
    /// The command chosen in the list put in the composer; \a send: and
    /// sent, when it needs no arguments.
    void completeCommand(bool send);
    void restyle();
    void scheduleRender();
    void render();
    /// The entries, from the first prompt on.
    void renderConversation(QTextCursor& c);
    /// What the conversation is, for an export: [label, value] pairs.
    QList<QPair<QString, QString>> exportFacts() const;
    /// Its title at length, for an export (the first prompt, not cut short
    /// as for a tab).
    QString exportTitle() const;
    /// The dock's palette - or paper's, while exporting.
    QPalette drawingPalette() const;
    /// Whether a line of tools or a tool is open (all are, exported).
    bool isOpen(const QString& key) const;
    void renderWelcome(QTextCursor& c);
    void renderEntry(QTextCursor& c, const Entry& e, bool& captioned);
    void renderCaption(QTextCursor& c, bool& captioned);
    /// The tools a_entries[from, to) used in a row: one line that opens.
    void renderTools(QTextCursor& c, qsizetype from, qsizetype to, bool& captioned);
    void renderTool(QTextCursor& c, const Entry& e, qreal indent);
    QString toolSummary(qsizetype from, qsizetype to) const;
    /// How a row of tools a_entries[from, to) stands as one: running while
    /// one runs, else failed, not allowed, or done (an Entry::ToolState).
    int rowOutcome(qsizetype from, qsizetype to) const;
    /// What went wrong in a row of tools ("1 failed, 2 not allowed"), or
    /// empty.
    QString rowTrouble(qsizetype from, qsizetype to) const;
    /// A reply's Markdown, its math typeset.
    void renderMarkdown(QTextCursor& c, const QString& text);
    qucs_s::math::Typeset typesetMath(const QString& tex, const QFont& font, bool display);
    void append(const Entry& e);
    void updateState();
    void updateDirectory();
    void updateComposer();
    void showNextRequest();
    void answer(bool allow, bool allowEdits, bool allowTools = false);
    void handleLink(const QUrl& url);
    QString programSetting() const;
    void findProgram();
    /// Asks the program which models it offers (once for each program).
    void listModels();
    void rebuildModelMenu();
    /// The choice that \a model is, or null.
    const qucs_s::claude::ModelChoice* choiceFor(const QString& model) const;
    void setPermissionMode(const QString& mode);
    void setModel(const QString& model);

    // Session events.
    void onReplyStreamed(const QString& text);
    void onReplyFinished(const QString& text);
    void onToolStarted(const QString& id, const QString& tool, const QString& subject, const QString& detail);
    void onToolFinished(const QString& id, bool failed, const QString& output);
    void onPermissionRequested(const qucs_s::claude::PermissionRequest& request);
    void onPermissionWithdrawn(const QString& id);
    void onTurnFinished(const qucs_s::claude::TurnResult& result);

    qucs_s::claude::Session* a_session;
    QString a_defaultDir;
    QString a_chosenDir;       // empty: the default
    std::function<QString()> a_document;
    std::function<QStringList()> a_schematics;
    QString a_pinned;          // the schematic pinned (its file), or empty
    QString a_conversationId;  // where it is kept

    qucs_s::claude::ModelQuery* a_modelQuery;
    QJsonArray a_listedModels; // what the program offers (kept in the settings)
    QList<qucs_s::claude::ModelChoice> a_choices;

    QList<Entry> a_entries;
    QSet<QString> a_expanded;  // the tool lines opened
    QHash<QString, qucs_s::math::Typeset> a_math;   // typeset math, by TeX, size and colour
    bool a_newInTab = false;
    QString a_name;            // given by the user; empty: the first prompt
    bool a_exporting = false;  // drawing for paper: no links
    bool a_exportDetails = true;   // and, exporting, every tool open
    QList<qucs_s::claude::PermissionRequest> a_requests;
    QTimer* a_renderTimer;
    QTimer* a_clock;           // the seconds of a turn under way

    // The header.
    QWidget* a_header;
    QLabel* a_title;
    QLabel* a_stateDot;
    QLabel* a_stateText;
    QLabel* a_modelLabel;
    QToolButton* a_newButton;
    QToolButton* a_menuButton;
    QMenu* a_menu;
    QActionGroup* a_modes;
    QMenu* a_modelMenu;
    QActionGroup* a_models;
    // The folder.
    QWidget* a_dirRow;
    QLabel* a_dirIcon;
    QLabel* a_dirLabel;
    QToolButton* a_dirButton;
    QToolButton* a_dirReset;
    // The conversation.
    QTextBrowser* a_view;
    // The permission card.
    QFrame* a_card;
    QLabel* a_cardTitle;
    QLabel* a_cardSubject;
    QPlainTextEdit* a_cardDetail;
    QLabel* a_cardCount;
    QToolButton* a_allow;
    QToolButton* a_allowEdits;
    QToolButton* a_allowTools;
    QToolButton* a_deny;
    // The composer.
    QFrame* a_composer;
    QPlainTextEdit* a_input;
    QToolButton* a_attach;
    QToolButton* a_pin;        // pins the schematic in front; pinned, names it
    QMenu* a_pinMenu;          // ⋯ > Pin to a Schematic
    QListWidget* a_commandList = nullptr;   // the commands matching what is typed
    QLabel* a_hint;
    QToolButton* a_send;
};

#endif // QUCS_CLAUDECODEPANEL_H

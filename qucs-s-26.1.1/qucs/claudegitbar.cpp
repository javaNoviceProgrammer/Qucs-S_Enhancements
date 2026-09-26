/*
 * claudegitbar.cpp - the Claude Code dock's bar of the git repository
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "claudegitbar.h"

#include "apptheme.h"
#include "ink.h"
#include "settings.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPromise>
#include <QPushButton>
#include <QSplitter>
#include <QSyntaxHighlighter>
#include <QThreadPool>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>

#include <memory>

using qucs_s::git::PullRequest;
using qucs_s::git::Status;

namespace {

const QString kOn = QStringLiteral("ClaudeCode/gitStatus");
constexpr int kLookEveryMs = 10000;
// The diff of all the changes shown up to this many characters.
constexpr qsizetype kDiffShown = 2 * 1024 * 1024;

int& onCache()
{
    static int on = -1;   // not read yet
    return on;
}

QList<ClaudeGitBar*>& bars()
{
    static QList<ClaudeGitBar*> all;
    return all;
}

// \a work run on the thread pool; its result in the future.
template <typename T, typename F>
QFuture<T> runAside(F work)
{
    auto promise = std::make_shared<QPromise<T>>();
    QFuture<T> future = promise->future();
    promise->start();
    QThreadPool::globalInstance()->start([promise, work] {
        promise->addResult(work());
        promise->finish();
    });
    return future;
}

struct DiffColours {
    QColor added, removed, hunk, header;
};

DiffColours diffColours(const QPalette& pal)
{
    const bool dark = qucs_s::ink::isDark(pal.color(QPalette::Base));
    DiffColours c;
    c.added = dark ? QColor(0x3f, 0xb9, 0x50) : QColor(0x1a, 0x7f, 0x37);
    c.removed = dark ? QColor(0xf8, 0x51, 0x49) : QColor(0xcf, 0x22, 0x2e);
    c.hunk = dark ? QColor(0xd2, 0xa8, 0xff) : QColor(0x82, 0x50, 0xdf);
    c.header = qucs_s::apptheme::mix(pal.color(QPalette::Base), pal.color(QPalette::Text), 0.64);
    return c;
}

// A unified diff coloured: lines added green, removed red, the hunks'
// heads violet, the files' heads bold.
class DiffHighlighter : public QSyntaxHighlighter
{
public:
    DiffHighlighter(QTextDocument* doc, const QPalette& palette) : QSyntaxHighlighter(doc) { setPalette(palette); }

    void setPalette(const QPalette& palette)
    {
        a_colours = diffColours(palette);
        rehighlight();
    }

protected:
    void highlightBlock(const QString& text) override
    {
        QTextCharFormat f;
        if (text.startsWith(QLatin1String("diff --git")) || text.startsWith(QLatin1String("+++ "))
            || text.startsWith(QLatin1String("--- ")) || text.startsWith(QLatin1String("index "))
            || text.startsWith(QLatin1String("new file")) || text.startsWith(QLatin1String("deleted file"))
            || text.startsWith(QLatin1String("Binary file"))) {
            f.setForeground(a_colours.header);
            f.setFontWeight(QFont::Bold);
        } else if (text.startsWith(QLatin1String("@@"))) {
            f.setForeground(a_colours.hunk);
        } else if (text.startsWith(QLatin1Char('+'))) {
            f.setForeground(a_colours.added);
        } else if (text.startsWith(QLatin1Char('-'))) {
            f.setForeground(a_colours.removed);
        } else {
            return;
        }
        setFormat(0, int(text.size()), f);
    }

private:
    DiffColours a_colours;
};

QString native(const QString& path)
{
    return QDir::toNativeSeparators(path);
}

QString tr(const char* text)
{
    return QCoreApplication::translate("ClaudeGitBar", text);
}

// "1 file", "3 files"; "1 commit", "2 commits".
QString fileCount(qsizetype n)
{
    return n == 1 ? tr("1 file") : tr("%1 files").arg(QLocale().toString(qlonglong(n)));
}

QString commitCount(int n)
{
    return n == 1 ? tr("1 commit") : tr("%1 commits").arg(QLocale().toString(n));
}

// What the changes are counted from, in words.
QString baseName(const Status& s)
{
    return s.base == QLatin1String("HEAD") ? QCoreApplication::translate("ClaudeGitBar", "the last commit") : s.base;
}

} // namespace

// ----------------------------------------------------------------------
ClaudeGitBar::ClaudeGitBar(QWidget* parent)
    : QFrame(parent),
      a_timer(new QTimer(this))
{
    setObjectName(QStringLiteral("claudeGitBar"));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 3, 4, 3);
    layout->setSpacing(10);

    a_folder = new QLabel(this);
    a_folder->setObjectName(QStringLiteral("claudeGitFolder"));
    a_folder->setTextFormat(Qt::PlainText);
    a_branch = new QLabel(this);
    a_branch->setObjectName(QStringLiteral("claudeGitBranch"));
    a_branch->setTextFormat(Qt::PlainText);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSizeF(font().pointSizeF());
    a_branch->setFont(mono);
    a_stat = new QLabel(this);
    a_stat->setObjectName(QStringLiteral("claudeGitStat"));
    a_stat->setTextFormat(Qt::RichText);
    a_stat->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    a_stat->setCursor(Qt::PointingHandCursor);
    a_stat->setAttribute(Qt::WA_Hover);
    connect(a_stat, &QLabel::linkActivated, this, [this] { showChanges(); });

    a_prButton = new QToolButton(this);
    a_prButton->setObjectName(QStringLiteral("claudeGitPr"));
    a_prButton->setPopupMode(QToolButton::MenuButtonPopup);
    a_menu = new QMenu(a_prButton);
    a_menu->setObjectName(QStringLiteral("claudeGitMenu"));
    a_menu->setToolTipsVisible(true);
    a_prButton->setMenu(a_menu);
    connect(a_menu, &QMenu::aboutToShow, this, &ClaudeGitBar::fillMenu);
    connect(a_prButton, &QToolButton::clicked, this, [this] {
        if (a_pr.number > 0) {
            QDesktopServices::openUrl(QUrl(a_pr.url));
        } else if (a_status.changes.isEmpty()) {
            QToolTip::showText(a_prButton->mapToGlobal(QPoint(0, a_prButton->height())),
                               tr("Nothing to propose: no changes since %1.").arg(baseName(a_status)), a_prButton);
        } else {
            ask(createPrPrompt(false));
        }
    });

    a_close = new QToolButton(this);
    a_close->setObjectName(QStringLiteral("claudeGitClose"));
    a_close->setText(QStringLiteral("✕"));
    a_close->setAutoRaise(true);
    a_close->setToolTip(tr("Hide until the repository or the branch changes (⋯ > Show Git Status turns it off)"));
    connect(a_close, &QToolButton::clicked, this, [this] {
        a_dismissed = dismissKey();
        updateVisibility();
    });

    layout->addWidget(a_folder);
    layout->addWidget(a_branch);
    layout->addStretch(1);
    layout->addWidget(a_stat);
    layout->addWidget(a_prButton);
    layout->addWidget(a_close);

    // Looked at while it is in sight, and when Qucs-S comes to the front
    // (a commit made in a terminal, a branch switched).
    a_timer->setInterval(kLookEveryMs);
    connect(a_timer, &QTimer::timeout, this, [this] {
        if (parentWidget() != nullptr && parentWidget()->isVisible()) refresh();
    });
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive) refresh();
    });

    bars().append(this);
    QFrame::hide();
    restyle();
}

ClaudeGitBar::~ClaudeGitBar()
{
    bars().removeAll(this);
}

bool ClaudeGitBar::isOn()
{
    int& on = onCache();
    if (on < 0) on = QucsSettingsFile().value(kOn, true).toBool() ? 1 : 0;
    return on == 1;
}

void ClaudeGitBar::setOn(bool on)
{
    QucsSettingsFile().setValue(kOn, on);
    onCache() = on ? 1 : 0;
    for (ClaudeGitBar* bar : bars()) {
        bar->a_dismissed.clear();
        bar->updateVisibility();
        if (on) bar->refresh(true);
        else bar->a_timer->stop();
    }
}

void ClaudeGitBar::setDirectory(const QString& dir)
{
    const QString clean = dir.isEmpty() ? QString() : QDir::cleanPath(dir);
    if (clean == a_dir) return;
    a_dir = clean;
    ++a_generation;
    refresh(true);
}

void ClaudeGitBar::refresh(bool askGitHubToo)
{
    if (!isOn()) return;
    a_askAgain = a_askAgain || askGitHubToo;
    if (a_looking) {
        a_again = true;
        return;
    }
    look();
}

void ClaudeGitBar::look()
{
    a_looking = true;
    a_again = false;
    const bool github = a_askAgain;
    a_askAgain = false;
    const QString dir = a_dir;
    const int generation = a_generation;
    auto* watcher = new QFutureWatcher<Status>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation, github] {
        watcher->deleteLater();
        a_looking = false;
        if (generation == a_generation) {
            const Status s = watcher->result();
            display(s);
            // The branch's pull request: asked for when the branch is
            // another, or after a turn - not every few seconds.
            const QString key = s.root + QLatin1Char('\n') + s.branch;
            if (key != a_prFor || github) askGitHub();
            else emit updated();
        }
        if (a_again || generation != a_generation) look();
    });
    watcher->setFuture(runAside<Status>([dir] { return qucs_s::git::status(dir); }));
    if (!a_timer->isActive()) a_timer->start();
}

void ClaudeGitBar::askGitHub()
{
    const Status s = a_status;
    a_prFor = s.root + QLatin1Char('\n') + s.branch;
    // None to ask for: no repository, no branch, the default one, no
    // remote, or no gh.
    if (!s.repository || s.branch.isEmpty() || s.onDefaultBranch() || !s.hasRemote || qucs_s::git::ghProgram().isEmpty()) {
        if (a_pr.number != 0) {
            a_pr = PullRequest();
            updateBar();
        }
        emit updated();
        return;
    }
    a_askingGitHub = true;
    const int generation = a_generation;
    const QString key = a_prFor;
    auto* watcher = new QFutureWatcher<PullRequest>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation, key] {
        watcher->deleteLater();
        a_askingGitHub = false;
        if (generation != a_generation || key != a_status.root + QLatin1Char('\n') + a_status.branch) return;
        const PullRequest pr = watcher->result();
        // A merged or closed one: a new one can be made.
        a_pr = pr.state == QLatin1String("OPEN") ? pr : PullRequest();
        updateBar();
        emit updated();
    });
    const QString root = s.root, branch = s.branch;
    watcher->setFuture(runAside<PullRequest>([root, branch] { return qucs_s::git::pullRequest(root, branch); }));
}

void ClaudeGitBar::display(const Status& status)
{
    const bool branchChanged = status.root != a_status.root || status.branch != a_status.branch;
    if (status == a_status && !branchChanged) return;
    if (branchChanged) a_pr = PullRequest();
    a_status = status;
    updateBar();
    if (a_changes) static_cast<GitChangesDialog*>(a_changes.data())->setStatus(a_status);
}

QString ClaudeGitBar::dismissKey() const
{
    return a_status.root + QLatin1Char('\n') + a_status.branch;
}

bool ClaudeGitBar::isDismissed() const
{
    return !a_dismissed.isEmpty() && a_dismissed == dismissKey();
}

void ClaudeGitBar::updateVisibility()
{
    setVisible(isOn() && a_status.repository && !isDismissed());
}

void ClaudeGitBar::updateBar()
{
    const Status& s = a_status;
    if (s.repository) {
        const QString name = QFileInfo(s.root).fileName();
        a_folder->setText(a_folder->fontMetrics().elidedText(name, Qt::ElideRight, 200));
        a_folder->setToolTip(tr("The git repository %1").arg(native(s.root)));

        QString branch = s.branch.isEmpty()
                             ? (s.head.isEmpty() ? tr("no commits yet") : tr("detached at %1").arg(s.head))
                             : s.branch;
        if (s.ahead > 0) branch += QStringLiteral(" ↑%1").arg(s.ahead);
        if (s.behind > 0) branch += QStringLiteral(" ↓%1").arg(s.behind);
        a_branch->setText(a_branch->fontMetrics().elidedText(branch, Qt::ElideMiddle, 220));
        QStringList tip;
        if (s.branch.isEmpty()) {
            tip << (s.head.isEmpty() ? tr("The repository has no commit yet.") : tr("HEAD is detached, at %1.").arg(s.head));
        } else if (!s.upstream.isEmpty()) {
            tip << tr("%1 tracks %2.").arg(s.branch, s.upstream);
            if (s.ahead > 0) tip << tr("%1 to push.").arg(commitCount(s.ahead));
            if (s.behind > 0) tip << tr("%1 to pull.").arg(commitCount(s.behind));
        } else {
            tip << (s.hasRemote ? tr("%1 is not pushed yet.").arg(s.branch)
                                : tr("%1 - the repository has no remote.").arg(s.branch));
        }
        a_branch->setToolTip(tip.join(QLatin1Char('\n')));

        if (s.changes.isEmpty()) {
            a_stat->hide();
        } else {
            const DiffColours c = diffColours(themePalette());
            const QLocale locale;
            a_stat->setText(QStringLiteral("<a href=\"changes\" style=\"text-decoration: none;\">"
                                           "<span style=\"color: %1;\">+%2</span>&nbsp;"
                                           "<span style=\"color: %3;\">−%4</span></a>")
                                .arg(c.added.name(), locale.toString(s.added()), c.removed.name(),
                                     locale.toString(s.removed())));
            a_stat->setToolTip(tr("%1 changed since %2 (%3): click to see the changes")
                                   .arg(fileCount(s.changes.size()), baseName(s), s.baseCommit.left(7)));
            a_stat->show();
        }
    }
    if (a_pr.number > 0) {
        a_prButton->setText(tr("PR #%1").arg(a_pr.number));
        a_prButton->setToolTip(tr("%1%2\nOpen it on GitHub").arg(a_pr.title, a_pr.draft ? tr(" (draft)") : QString()));
    } else {
        a_prButton->setText(tr("Create PR"));
        a_prButton->setToolTip(tr("Ask Claude to commit the changes, push the branch and open a pull request"));
    }
    updateVisibility();
}

void ClaudeGitBar::setBusy(bool busy)
{
    a_busy = busy;
}

void ClaudeGitBar::ask(const QString& prompt)
{
    if (a_busy) {
        QToolTip::showText(a_prButton->mapToGlobal(QPoint(0, a_prButton->height())),
                           tr("Claude is busy: ask again when the turn ends."), a_prButton);
        return;
    }
    emit promptRequested(prompt);
}

void ClaudeGitBar::fillMenu()
{
    a_menu->clear();
    const Status& s = a_status;
    if (a_pr.number > 0) {
        a_menu->addAction(tr("Open Pull Request #%1").arg(a_pr.number), this,
                          [this] { QDesktopServices::openUrl(QUrl(a_pr.url)); });
        QAction* update = a_menu->addAction(tr("Update Pull Request"), this, [this] { ask(updatePrPrompt()); });
        update->setObjectName(QStringLiteral("claudeGitUpdatePr"));
        update->setEnabled(!a_busy && (s.dirty || s.ahead > 0));
        update->setToolTip(tr("Ask Claude to commit the changes and push them to the pull request"));
        a_menu->addAction(tr("Copy Pull Request Link"), this, [this] { QApplication::clipboard()->setText(a_pr.url); });
    } else {
        const bool something = !a_busy && !s.changes.isEmpty();
        QAction* create = a_menu->addAction(tr("Create Pull Request"), this, [this] { ask(createPrPrompt(false)); });
        create->setObjectName(QStringLiteral("claudeGitCreatePr"));
        create->setEnabled(something);
        QAction* draft = a_menu->addAction(tr("Create Draft Pull Request"), this, [this] { ask(createPrPrompt(true)); });
        draft->setObjectName(QStringLiteral("claudeGitCreateDraft"));
        draft->setEnabled(something);
    }
    a_menu->addSeparator();
    QAction* commit = a_menu->addAction(tr("Commit Changes"), this, [this] { ask(commitPrompt()); });
    commit->setObjectName(QStringLiteral("claudeGitCommit"));
    commit->setEnabled(!a_busy && s.dirty);
    commit->setToolTip(tr("Ask Claude to commit what is not committed, with a message of its own, and not push"));
    QAction* push = a_menu->addAction(tr("Push Branch"), this, [this] { ask(pushPrompt()); });
    push->setObjectName(QStringLiteral("claudeGitPush"));
    push->setEnabled(!a_busy && !s.branch.isEmpty() && s.hasRemote && (s.upstream.isEmpty() || s.ahead > 0));
    a_menu->addSeparator();
    QAction* changes = a_menu->addAction(tr("Show Changes…"), this, &ClaudeGitBar::showChanges);
    changes->setEnabled(!s.changes.isEmpty());
    a_menu->addAction(tr("Refresh"), this, [this] { refresh(true); });
    QAction* copy = a_menu->addAction(tr("Copy Branch Name"), this, [this] { QApplication::clipboard()->setText(a_status.branch); });
    copy->setEnabled(!s.branch.isEmpty());
}

QString ClaudeGitBar::where() const
{
    return tr("the git repository at %1").arg(native(a_status.root));
}

QString ClaudeGitBar::createPrPrompt(bool draft) const
{
    const Status& s = a_status;
    QStringList facts;
    facts << tr("%1 changed, %2 since %3").arg(fileCount(s.changes.size()), qucs_s::git::statText(s.added(), s.removed()), baseName(s));
    if (s.ahead > 0) facts << tr("%1 not pushed").arg(commitCount(s.ahead));
    QString prompt = (draft ? tr("Create a draft pull request for the work in %1") : tr("Create a pull request for the work in %1"))
                         .arg(where());
    prompt += s.branch.isEmpty() ? tr(", where HEAD is detached at %1").arg(s.head) : tr(", on the branch %1").arg(s.branch);
    prompt += QStringLiteral(" (") + facts.join(QStringLiteral("; ")) + QStringLiteral(").\n");
    if (s.branch.isEmpty())
        prompt += tr("- Create a branch for the work first, named after it.\n");
    else if (s.onDefaultBranch())
        prompt += tr("- %1 is the default branch: create a branch for the work first, named after it.\n").arg(s.branch);
    if (s.dirty) prompt += tr("- Commit the uncommitted changes with a clear message.\n");
    if (!s.hasRemote)
        prompt += tr("- The repository has no remote: tell me so and ask where it should go, rather than making one up.\n");
    prompt += draft ? tr("- Push the branch and open the pull request as a draft with gh (GitHub CLI), with a short title "
                         "and a description of what changed and why.\n")
                    : tr("- Push the branch and open the pull request with gh (GitHub CLI), with a short title and a "
                         "description of what changed and why.\n");
    prompt += tr("- Tell me the pull request's link.");
    return prompt;
}

QString ClaudeGitBar::updatePrPrompt() const
{
    return tr("Update pull request #%1 (%2) of the branch %3 in %4: commit the uncommitted changes with a clear message, "
              "and push the branch.")
        .arg(a_pr.number)
        .arg(a_pr.url, a_status.branch, where());
}

QString ClaudeGitBar::commitPrompt() const
{
    return tr("Commit the uncommitted changes in %1 (branch %2) with a clear, concise message. Do not push.")
        .arg(where(), a_status.branch.isEmpty() ? tr("none: HEAD is detached") : a_status.branch);
}

QString ClaudeGitBar::pushPrompt() const
{
    return a_status.upstream.isEmpty()
               ? tr("Push the branch %1 of %2 to its remote, and set it as the branch's upstream.").arg(a_status.branch, where())
               : tr("Push the branch %1 of %2 to %3.").arg(a_status.branch, where(), a_status.upstream);
}

void ClaudeGitBar::showChanges()
{
    if (a_changes) {
        static_cast<GitChangesDialog*>(a_changes.data())->setStatus(a_status);
    } else {
        auto* dialog = new GitChangesDialog(a_status, this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(dialog, &GitChangesDialog::openFileRequested, this, &ClaudeGitBar::openFileRequested);
        a_changes = dialog;
    }
    a_changes->show();
    a_changes->raise();
    a_changes->activateWindow();
}

void ClaudeGitBar::changeEvent(QEvent* event)
{
    QFrame::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)
        QTimer::singleShot(0, this, &ClaudeGitBar::restyle);
}

QPalette ClaudeGitBar::themePalette() const
{
    // Not its own: its style sheet's background becomes its palette's, and
    // the next restyle would mix from that.
    return parentWidget() != nullptr ? parentWidget()->palette() : QApplication::palette();
}

void ClaudeGitBar::restyle()
{
    using qucs_s::apptheme::mix;
    const QPalette pal = themePalette();
    const QColor base = pal.color(QPalette::Base), text = pal.color(QPalette::Text);
    const bool dark = qucs_s::ink::isDark(base);
    const QColor bar = mix(base, text, dark ? 0.07 : 0.04);
    const QColor border = mix(base, text, dark ? 0.22 : 0.16);
    const QColor chip = mix(base, text, dark ? 0.14 : 0.09);
    const QColor hover = mix(base, text, dark ? 0.2 : 0.14);
    const QColor muted = mix(base, text, 0.64);
    const QString sheet =
        QStringLiteral(
            "QFrame#claudeGitBar { background: %1; border: 1px solid %2; border-radius: 10px; }"
            "QLabel#claudeGitFolder, QLabel#claudeGitBranch { color: %5; background: transparent; }"
            "QLabel#claudeGitStat { background: %3; border-radius: 6px; padding: 2px 8px; }"
            "QLabel#claudeGitStat:hover { background: %4; }"
            "QToolButton#claudeGitPr { background: %3; color: %6; border: none; border-radius: 6px; padding: 2px 8px;"
            " padding-right: 24px; }"
            "QToolButton#claudeGitPr:hover { background: %4; }"
            "QToolButton#claudeGitPr::menu-button { border: none; border-left: 1px solid %2; width: 18px;"
            " border-top-right-radius: 6px; border-bottom-right-radius: 6px; }"
            "QToolButton#claudeGitPr::menu-button:hover { background: %4; }"
            "QToolButton#claudeGitClose { color: %5; border: none; border-radius: 5px; padding: 2px 5px; background: transparent; }"
            "QToolButton#claudeGitClose:hover { background: %4; }")
            .arg(bar.name(), border.name(), chip.name(), hover.name(), muted.name(), text.name());
    if (sheet != styleSheet()) setStyleSheet(sheet);
    updateBar();   // the stat's colours
}

// ----------------------------------------------------------------------
GitChangesDialog::GitChangesDialog(const Status& status, QWidget* parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("gitChanges"));
    auto* layout = new QVBoxLayout(this);
    a_summary = new QLabel(this);
    a_summary->setObjectName(QStringLiteral("gitChangesSummary"));
    a_summary->setWordWrap(true);
    layout->addWidget(a_summary);

    auto* split = new QSplitter(Qt::Horizontal, this);
    a_files = new QListWidget(split);
    a_files->setObjectName(QStringLiteral("gitChangesFiles"));
    a_diff = new QPlainTextEdit(split);
    a_diff->setObjectName(QStringLiteral("gitChangesDiff"));
    a_diff->setReadOnly(true);
    a_diff->setLineWrapMode(QPlainTextEdit::NoWrap);
    a_diff->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    new DiffHighlighter(a_diff->document(), a_diff->palette());
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 3);
    layout->addWidget(split, 1);

    auto* buttons = new QHBoxLayout;
    auto* open = new QPushButton(tr("Open File"), this);
    open->setObjectName(QStringLiteral("gitChangesOpen"));
    auto* close = new QPushButton(tr("Close"), this);
    buttons->addStretch(1);
    buttons->addWidget(open);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    const auto openItem = [this](QListWidgetItem* item) {
        if (item == nullptr) return;
        const QString path = item->data(Qt::UserRole).toString();
        const QString file = QDir(a_status.root).filePath(path);
        if (!path.isEmpty() && QFileInfo(file).isFile()) emit openFileRequested(file);
    };
    connect(a_files, &QListWidget::currentRowChanged, this, [this, open] {
        QListWidgetItem* item = a_files->currentItem();
        const QString path = item != nullptr ? item->data(Qt::UserRole).toString() : QString();
        open->setEnabled(!path.isEmpty() && QFileInfo(QDir(a_status.root).filePath(path)).isFile());
        showDiff();
    });
    connect(a_files, &QListWidget::itemDoubleClicked, this, openItem);
    connect(open, &QPushButton::clicked, this, [this, openItem] { openItem(a_files->currentItem()); });
    connect(close, &QPushButton::clicked, this, &QDialog::close);

    setStatus(status);
    resize(920, 580);
}

void GitChangesDialog::setStatus(const Status& status)
{
    const QString current = a_files->currentItem() != nullptr ? a_files->currentItem()->data(Qt::UserRole).toString() : QString();
    a_status = status;
    setWindowTitle(tr("Changes in %1").arg(QFileInfo(status.root).fileName()));
    a_summary->setText(tr("%1 changed, %2, since %3 (%4), on %5.")
                           .arg(fileCount(status.changes.size()), qucs_s::git::statText(status.added(), status.removed()), baseName(status),
                                status.baseCommit.left(7),
                                status.branch.isEmpty() ? tr("a detached HEAD") : tr("the branch %1").arg(status.branch)));
    const QSignalBlocker quiet(a_files);
    a_files->clear();
    auto* all = new QListWidgetItem(tr("All changes"), a_files);
    all->setData(Qt::UserRole, QString());
    QFont bold = all->font();
    bold.setBold(true);
    all->setFont(bold);
    int row = 0;
    for (const auto& f : status.changes) {
        const QString counts = f.binary ? tr("binary") : QStringLiteral("+%1 −%2").arg(f.added).arg(f.removed);
        auto* item = new QListWidgetItem(QStringLiteral("%1   %2%3").arg(f.path, counts, f.untracked ? tr("  new") : QString()),
                                         a_files);
        item->setData(Qt::UserRole, f.path);
        item->setToolTip(QDir::toNativeSeparators(QDir(status.root).filePath(f.path)));
        if (f.path == current) row = a_files->count() - 1;
    }
    a_files->setCurrentRow(row);
    showDiff();
}

void GitChangesDialog::showDiff()
{
    QListWidgetItem* item = a_files->currentItem();
    if (item == nullptr) {
        a_diff->clear();
        return;
    }
    QString text = qucs_s::git::diff(a_status, item->data(Qt::UserRole).toString());
    if (text.size() > kDiffShown) {
        text.truncate(kDiffShown);
        text += QLatin1Char('\n') + tr("(The rest is not shown: choose a file.)");
    }
    a_diff->setPlainText(text);
}

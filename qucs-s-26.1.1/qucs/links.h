/*
 * What a click on a link in a document does - a Markdown preview's, a PDF's.
 *
 * A document may come with a project from anywhere (downloaded, cloned),
 * and a link's text need not say where it leads. So, as the Claude Code
 * dock does: http, https and mailto go to the system's browser or mail
 * program; a file Qucs-S opens itself (a schematic, a text, a spreadsheet,
 * a PDF, ...) opens in a tab. Nothing else is ever handed to the system to
 * open - a program (.command, .app, .exe, .bat, .lnk, ...) would run: any
 * other file is only shown in the file manager, once the user says so, and
 * a URL of another scheme goes to the system only once the user agrees,
 * the URL named in the question.
 */
#ifndef QUCS_LINKS_H
#define QUCS_LINKS_H

#include <QString>
#include <QUrl>

#include <functional>

class QWidget;

namespace qucs_s::links {

/// What following a link does (follow()).
enum class Action {
    Nothing,   ///< a file that is not there, a URL that is none
    Outside,   ///< http, https, mailto: to the system's browser or mail program
    Open,      ///< a document Qucs-S opens: in a tab
    Reveal,    ///< another file: shown in the file manager, once asked
    Ask,       ///< a URL of another scheme: to the system, once asked
};

struct Target {
    Action action = Action::Nothing;
    QUrl url;       ///< (Outside, Ask)
    QString path;   ///< the file, absolute (Open, Reveal)
};

/// Where \a url leads and what following it does; a relative one is
/// relative to \a baseDir (the document's folder).
Target resolve(const QUrl& url, const QString& baseDir);

/// Whether Qucs-S opens a file named so in a tab of its own.
bool opensItself(const QString& path);

/// Follows \a url from a document in \a baseDir: \a open opens a file in
/// Qucs-S; the questions are \a parent's.
void follow(QWidget* parent, const QUrl& url, const QString& baseDir, const std::function<void(const QString&)>& open);

/// The system's file manager, with \a path selected where it can.
void reveal(const QString& path);

} // namespace qucs_s::links

#endif

/*
 * qucscontrol_help.cpp - read_help: the help this build of Qucs-S has, to
 *                        answer from - each menu action's own help (its
 *                        What's This, its status tip, its shortcut), the
 *                        component types, the example schematics and any
 *                        papers it ships; where the online manual is
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "qucscontrol.h"
#include "qucscontrol_p.h"
#include "components/component.h"
#include "main.h"
#include "module.h"
#include "misc.h"
#include "qucs.h"

#include <QAction>
#include <QSet>
#include <QProcess>
#include <QJsonDocument>
#include <QFile>
#include <QDateTime>
#include <QDirIterator>
#include <QJsonArray>
#include <QMenu>
#include <QMenuBar>
#include <QRegularExpression>

using namespace qucs_s::control;

namespace {

const char* const kOnlineHelp = "https://qucs-s-help.readthedocs.io/";   // Help > Help Index
const char* const kGettingStarted = "https://ra3xdh.github.io/pdf/qucs_s_tutorial.pdf";   // Help > Getting Started

QString plainText(QString text)
{
    text.remove(QLatin1Char('&'));
    text.replace(QChar(0x2026), QStringLiteral("..."));
    return text.simplified();
}

// Every word of \a words in \a text.
bool matches(const QString& text, const QStringList& words)
{
    for (const QString& w : words)
        if (!text.contains(w, Qt::CaseInsensitive)) return false;
    return true;
}

// ---- The online manual, its text cached: Sphinx's sources of each page
// (_sources/<page>.rst.txt), from the index down its tables of contents.

// Where the manual is read from: QUCS_MANUAL_URL (a file:// copy, for the
// tests), else the online manual - ending in a slash.
QString manualBase()
{
    QString base = qEnvironmentVariable("QUCS_MANUAL_URL", QLatin1String(kOnlineHelp) + QStringLiteral("en/latest/"));
    if (!base.endsWith(QLatin1Char('/'))) base += QLatin1Char('/');
    return base;
}

QString manualDir()
{
    return QDir(misc::cacheDir()).filePath(QStringLiteral("manual"));
}

// The manual's pages fetched into \a dir with curl: Sphinx's search index
// names every page's source (index.rst, overview/x.md), and each is read
// as _sources/<file>.txt - in one run of curl. How many came; 0 and why in
// \a error when none did, and in \a missing how many it lacks.
int fetchManual(const QString& dir, QString* error, int* missing)
{
    QDir().mkpath(dir);
    const QString base = manualBase();
    const auto curl = [](const QStringList& more, QString* why) {
        QProcess p;
        // (Only pages that are there: after a few that are not, curl waited
        // out its time on the next.)
        p.start(QStringLiteral("curl"), QStringList{QStringLiteral("--silent"), QStringLiteral("--show-error"), QStringLiteral("--fail"),
                                                    QStringLiteral("--location"), QStringLiteral("--create-dirs"),
                                                    QStringLiteral("--connect-timeout"), QStringLiteral("15"), QStringLiteral("--max-time"),
                                                    QStringLiteral("60")} + more);
        if (!p.waitForStarted(5000)) {
            *why = QObject::tr("curl, which fetches it, could not be started (%1)").arg(p.errorString());
            return false;
        }
        if (!p.waitForFinished(300000)) {
            p.kill();
            p.waitForFinished(2000);
            *why = QObject::tr("it took more than 5 minutes");
            return false;
        }
        if (p.exitCode() != 0) *why = QString::fromUtf8(p.readAllStandardError()).trimmed().section(QLatin1Char('\n'), 0, 0);
        return p.exitCode() == 0;
    };
    const QString index = QDir(dir).filePath(QStringLiteral("searchindex.js"));
    QString why;
    curl({QStringLiteral("-o"), index, base + QStringLiteral("searchindex.js")}, &why);
    QFile f(index);
    QByteArray text;
    if (f.open(QIODevice::ReadOnly)) text = f.readAll();
    const qsizetype open = text.indexOf('('), close = text.lastIndexOf(')');
    const QJsonArray files = open >= 0 && close > open ? QJsonDocument::fromJson(text.mid(open + 1, close - open - 1)).object().value(QStringLiteral("filenames")).toArray()
                                                       : QJsonArray();
    if (files.isEmpty()) {
        *error = QObject::tr("%1searchindex.js, which lists its pages, did not come (%2)").arg(base, why.isEmpty() ? QObject::tr("not one") : why);
        return 0;
    }
    QStringList args;
    for (const QJsonValue& v : files)
        args << QStringLiteral("-o") << QDir(dir).filePath(v.toString() + QStringLiteral(".txt")) << base + QStringLiteral("_sources/") + v.toString() + QStringLiteral(".txt");
    curl(args, &why);
    int fetched = 0;
    for (const QJsonValue& v : files)
        if (QFileInfo(QDir(dir).filePath(v.toString() + QStringLiteral(".txt"))).size() > 0) ++fetched;
    *missing = int(files.size()) - fetched;
    if (fetched == 0) {
        *error = QObject::tr("no page came from %1 (%2)").arg(base, why);
        return 0;
    }
    QFile stamp(QDir(dir).filePath(QStringLiteral(".fetched")));
    if (stamp.open(QIODevice::WriteOnly | QIODevice::Truncate))
        stamp.write(QStringLiteral("%1\n%2\n").arg(base, QDateTime::currentDateTime().toString(Qt::ISODate)).toUtf8());
    return fetched;
}

// A page's sections: its headings (a line underlined with = - ~ ^ and the
// like), each with its text.
struct Section {
    QString page, heading, text;
};

QList<Section> sectionsOf(const QString& page, const QString& text)
{
    static const QRegularExpression underline(QStringLiteral("^([=\\-~^\"'`#*+<>_:.])\\1{2,}\\s*$"));
    static const QRegularExpression markdown(QStringLiteral("^#{1,6}\\s+(.+?)\\s*#*\\s*$"));
    QList<Section> out;
    const QStringList lines = text.split(QLatin1Char('\n'));
    Section current{page, page, QString()};
    bool fenced = false;   // (in a ``` block: a # there is a comment)
    for (int i = 0; i < lines.size(); ++i) {
        const QString l = lines.at(i);
        if (l.trimmed().startsWith(QLatin1String("```"))) fenced = !fenced;
        // A Markdown page's (MyST): # Heading.
        if (const QRegularExpressionMatch m = markdown.match(l); !fenced && m.hasMatch()) {
            if (!current.text.trimmed().isEmpty()) out << current;
            current = Section{page, m.captured(1), QString()};
            continue;
        }
        const bool heading = i + 1 < lines.size() && !l.trimmed().isEmpty() && underline.match(lines.at(i + 1)).hasMatch()
                             && lines.at(i + 1).trimmed().size() >= l.trimmed().size() - 2;
        if (heading) {
            if (!current.text.trimmed().isEmpty()) out << current;
            current = Section{page, l.trimmed(), QString()};
            ++i;   // (its underline)
            continue;
        }
        if (underline.match(l).hasMatch()) continue;   // an overline
        current.text += l + QLatin1Char('\n');
    }
    if (!current.text.trimmed().isEmpty()) out << current;
    return out;
}

// Every page under \a dir (x.rst.txt, x.md.txt), by its name (overview/x).
QMap<QString, QString> manualPages(const QString& dir)
{
    QMap<QString, QString> pages;
    for (QDirIterator it(dir, {QStringLiteral("*.rst.txt"), QStringLiteral("*.md.txt")}, QDir::Files, QDirIterator::Subdirectories); it.hasNext();) {
        const QString path = it.next();
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) continue;
        QString name = QDir(dir).relativeFilePath(path);
        name.chop(name.endsWith(QLatin1String(".rst.txt")) ? 8 : 7);
        pages.insert(name, QString::fromUtf8(f.readAll()));
    }
    return pages;
}

} // namespace

QJsonObject QucsControl::readManual(const QJsonObject& args)
{
    const QString dir = manualDir();
    const bool cached = QFileInfo::exists(QDir(dir).filePath(QStringLiteral(".fetched")));
    QJsonObject result;
    if (!cached || args.value(QLatin1String("refresh")).toBool()) {
        QDir(dir).removeRecursively();
        QString error;
        int missing = 0;
        const int n = fetchManual(dir, &error, &missing);
        if (n == 0)
            return errorResult(tr("The manual could not be fetched: %1. Help > Help Index opens it in the browser (%2).")
                                   .arg(error, QLatin1String(kOnlineHelp)));
        result.insert(QStringLiteral("fetched"), tr("%1 pages of the manual from %2, kept in %3: read from there from now on "
                                                   "('refresh' fetches them again)").arg(n).arg(manualBase(), QDir::toNativeSeparators(dir)));
        if (missing > 0) result.insert(QStringLiteral("not fetched"), tr("%1 of its pages did not come: 'refresh' tries again").arg(missing));
    }
    const QMap<QString, QString> pages = manualPages(dir);
    const QString base = manualBase();
    // One page whole.
    if (const QString page = args.value(QLatin1String("page")).toString().trimmed().remove(QRegularExpression(QStringLiteral("^/|\\.(rst|html)$")));
        !page.isEmpty()) {
        if (!pages.contains(page)) {
            QStringList close;
            for (const QString& p : pages.keys())
                if (p.contains(page.section(QLatin1Char('/'), -1), Qt::CaseInsensitive) && close.size() < 10) close << p;
            return errorResult(tr("The manual has no page %1%2.").arg(page, close.isEmpty() ? QString() : tr(" - it has %1").arg(close.join(QStringLiteral(", ")))));
        }
        QString text = pages.value(page);
        const bool cut = text.size() > 30000;
        if (cut) text = text.left(30000);
        result.insert(QStringLiteral("page"), page);
        result.insert(QStringLiteral("url"), base + page + QStringLiteral(".html"));
        result.insert(QStringLiteral("text"), text);
        if (cut) result.insert(QStringLiteral("note"), tr("the first 30000 characters"));
        return jsonResult(result);
    }
    const QStringList words = args.value(QLatin1String("topic")).toString().simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (words.isEmpty()) {
        result.insert(QStringLiteral("pages"), QJsonArray::fromStringList(pages.keys()));
        result.insert(QStringLiteral("note"), tr("'topic' finds the sections with all its words; 'page' gives one page whole."));
        return jsonResult(result);
    }
    struct Hit {
        Section s;
        int score;
    };
    QList<Hit> hits;
    for (auto it = pages.cbegin(); it != pages.cend(); ++it)
        for (const Section& sec : sectionsOf(it.key(), it.value())) {
            const QString all = sec.heading + QLatin1Char(' ') + sec.text;
            bool every = true;
            int score = 0;
            for (const QString& w : words) {
                const int n = int(all.count(w, Qt::CaseInsensitive));
                if (n == 0) every = false;
                score += n + (sec.heading.contains(w, Qt::CaseInsensitive) ? 10 : 0);
            }
            if (every) hits << Hit{sec, score};
        }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
    QJsonArray found;
    for (const Hit& h : std::as_const(hits)) {
        if (found.size() >= 8) break;
        const qsizetype at = h.s.text.indexOf(words.first(), 0, Qt::CaseInsensitive);
        const QString excerpt = h.s.text.mid(std::max<qsizetype>(0, at - 200), 700).simplified();
        found.append(QJsonObject{{QStringLiteral("page"), h.s.page},
                                 {QStringLiteral("section"), h.s.heading},
                                 {QStringLiteral("url"), base + h.s.page + QStringLiteral(".html")},
                                 {QStringLiteral("excerpt"), excerpt}});
    }
    result.insert(QStringLiteral("manual"), found);
    if (found.isEmpty()) result.insert(QStringLiteral("found"), tr("No section of the manual has all of \"%1\".").arg(words.join(QLatin1Char(' '))));
    else if (hits.size() > found.size()) result.insert(QStringLiteral("more"), tr("%1 sections have them; the best 8 are given.").arg(hits.size()));
    result.insert(QStringLiteral("note"), tr("'page' gives a page whole. The manual is the online one, of the latest Qucs-S, fetched %1.")
                                              .arg([&dir] {
                                                  QFile f(QDir(dir).filePath(QStringLiteral(".fetched")));
                                                  return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).section(QLatin1Char('\n'), 1, 1)
                                                                                     : QObject::tr("earlier");
                                              }()));
    return jsonResult(result);
}

QJsonObject QucsControl::readHelp(const QJsonObject& args)
{
    // The manual: fetched once, then searched.
    if (args.value(QLatin1String("manual")).toBool() || args.contains(QLatin1String("page")) || args.value(QLatin1String("refresh")).toBool())
        return readManual(args);
    const QString topic = args.value(QLatin1String("topic")).toString().simplified();
    const QStringList words = topic.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    constexpr int kMost = 25;

    // The menu actions, each with its own help.
    struct Entry {
        QString path;
        QAction* action;
    };
    QList<Entry> actions;
    std::function<void(QMenu*, const QString&)> walk = [&](QMenu* menu, const QString& path) {
        for (QAction* a : menu->actions()) {
            if (a->isSeparator() || plainText(a->text()).isEmpty()) continue;
            const QString here = path + QStringLiteral(" > ") + plainText(a->text());
            if (a->menu() != nullptr) walk(a->menu(), here);
            else actions.append({here, a});
        }
    };
    for (QAction* top : a_app->menuBar()->actions())
        if (top->menu() != nullptr) walk(top->menu(), plainText(top->text()));
    // The examples, as Qucs-S ships them (File > Open Examples).
    QStringList examples;
    const QDir exampleDir(QucsSettings.ExamplesDir);
    if (!QucsSettings.ExamplesDir.trimmed().isEmpty() && exampleDir.exists())
        for (QDirIterator it(exampleDir.absolutePath(), {QStringLiteral("*.sch")}, QDir::Files, QDirIterator::Subdirectories); it.hasNext();)
            examples << exampleDir.relativeFilePath(it.next());
    examples.sort(Qt::CaseInsensitive);
    // Papers, reports and tutorials in its docs folder (none in most builds).
    QStringList papers;
    const QDir docDir(QucsSettings.DocDir);
    for (const char* sub : QucsSettings.DocDir.trimmed().isEmpty() ? std::initializer_list<const char*>{} : std::initializer_list<const char*>{"technical", "report", "tutorial"})
        for (const QFileInfo& f : QDir(docDir.filePath(QLatin1String(sub))).entryInfoList(QDir::Files, QDir::Name)) papers << f.absoluteFilePath();
    int types = 0;
    for (Category* category : Category::Categories) types += int(category->Content.size());
    // The manual: searched here when it has been fetched.
    const bool manualCached = QFileInfo::exists(QDir(manualDir()).filePath(QStringLiteral(".fetched")));
    const QJsonObject online{{QStringLiteral("manual"), QLatin1String(kOnlineHelp)},
                             {QStringLiteral("getting started"), QLatin1String(kGettingStarted)},
                             {QStringLiteral("note"), manualCached ? tr("The manual's text is kept in Qucs-S's cache: read_help with 'manual' searches it, 'page' "
                                                                       "gives a page whole (the topic's sections are below).")
                                                                    : tr("Online, not in this build (Help > Help Index and Help > Getting Started open them in the "
                                                                         "browser): read_help with 'manual' fetches the manual's text once (a few hundred KB, "
                                                                         "with curl) and searches it, from then on offline.")}};

    if (words.isEmpty())
        return jsonResult(QJsonObject{
            {QStringLiteral("help in this build"),
             QJsonObject{{QStringLiteral("actions"), tr("%1 menu actions, each with its own help (What's This, status tip) and shortcut").arg(actions.size())},
                         {QStringLiteral("components"), tr("%1 component types (describe_component_type gives one's properties and their meaning)").arg(types)},
                         {QStringLiteral("examples"), examples.isEmpty() ? tr("none in this build")
                                                                         : tr("%1 example schematics in %2 (open_document opens one)")
                                                                               .arg(examples.size()).arg(QDir::toNativeSeparators(exampleDir.absolutePath()))},
                         {QStringLiteral("papers"), papers.isEmpty() ? tr("none in this build") : tr("%1 PDFs (read_pdf reads one)").arg(papers.size())}}},
            {QStringLiteral("online"), online},
            {QStringLiteral("note"), tr("read_help with a 'topic' (words: \"tuner\", \"s-parameter\", \"monte carlo\") finds them in all "
                                        "of these. ngspice_commands tells ngspice's commands; describe_tool a tool of Claude's.")}});

    QJsonArray foundActions;
    for (const Entry& e : std::as_const(actions)) {
        const QString help = plainText(e.action->whatsThis());
        const QString tip = plainText(e.action->statusTip());
        if (!matches(e.path + QLatin1Char(' ') + help + QLatin1Char(' ') + tip + QLatin1Char(' ') + e.action->toolTip(), words)) continue;
        if (foundActions.size() >= kMost) break;
        QJsonObject o{{QStringLiteral("action"), e.path}};
        // (Its What's This, which names it first: the rest of it.)
        QString said = help;
        const QString title = plainText(e.action->text()).remove(QStringLiteral("..."));
        if (said.startsWith(title, Qt::CaseInsensitive)) said = said.mid(title.size()).trimmed();
        if (!said.isEmpty()) o.insert(QStringLiteral("help"), said);
        if (!tip.isEmpty() && tip.compare(said, Qt::CaseInsensitive) != 0) o.insert(QStringLiteral("tip"), tip);
        if (!e.action->shortcut().isEmpty()) o.insert(QStringLiteral("shortcut"), e.action->shortcut().toString(QKeySequence::NativeText));
        if (!e.action->isEnabled()) o.insert(QStringLiteral("enabled"), false);
        foundActions.append(o);
    }
    QJsonArray foundTypes;
    for (Category* category : Category::Categories)
        for (Module* m : category->Content) {
            const QString type = typeOf(m);
            if (type.isEmpty()) continue;
            QString name;
            if (m->info != nullptr) {
                char* file = nullptr;
                m->info(name, file, false);
            }
            if (!matches(type + QLatin1Char(' ') + name + QLatin1Char(' ') + category->Name, words) || foundTypes.size() >= kMost) continue;
            foundTypes.append(QJsonObject{{QStringLiteral("type"), type}, {QStringLiteral("name"), name}, {QStringLiteral("category"), category->Name}});
        }
    QJsonArray foundExamples;
    for (const QString& e : std::as_const(examples)) {
        // (Words as the names write them: S-parameter_active_analysis.)
        QString spaced = e;
        spaced.replace(QRegularExpression(QStringLiteral("[_/.]")), QStringLiteral(" "));
        if (!matches(e + QLatin1Char(' ') + spaced, words)) continue;
        if (foundExamples.size() >= kMost) break;
        foundExamples.append(QDir::toNativeSeparators(exampleDir.filePath(e)));
    }
    QJsonArray foundPapers;
    for (const QString& p : std::as_const(papers))
        if (matches(QFileInfo(p).fileName(), words)) foundPapers.append(QDir::toNativeSeparators(p));

    QJsonObject result{{QStringLiteral("topic"), topic}};
    if (!foundActions.isEmpty()) result.insert(QStringLiteral("actions"), foundActions);
    if (!foundTypes.isEmpty())
        result.insert(QStringLiteral("components"),
                      QJsonObject{{QStringLiteral("types"), foundTypes},
                                  {QStringLiteral("note"), tr("describe_component_type gives a type's properties and what each means.")}});
    if (!foundExamples.isEmpty())
        result.insert(QStringLiteral("examples"), QJsonObject{{QStringLiteral("files"), foundExamples},
                                                              {QStringLiteral("note"), tr("open_document opens one; get_schematic reads it.")}});
    if (!foundPapers.isEmpty()) result.insert(QStringLiteral("papers"), foundPapers);
    // The manual's sections too, when it is here already.
    bool manualFound = false;
    if (manualCached) {
        const QJsonObject m = readManual(QJsonObject{{QStringLiteral("topic"), topic}});
        const QJsonArray sections = QJsonDocument::fromJson(textOf(m).toUtf8()).object().value(QStringLiteral("manual")).toArray();
        if (!sections.isEmpty()) {
            result.insert(QStringLiteral("manual"), sections);
            manualFound = true;
        }
    }
    result.insert(QStringLiteral("online"), online);
    if (foundActions.isEmpty() && foundTypes.isEmpty() && foundExamples.isEmpty() && foundPapers.isEmpty() && !manualFound)
        result.insert(QStringLiteral("found"), tr("Nothing in this build's help has all of \"%1\": fewer or other words, or the online manual.").arg(topic));
    return jsonResult(result);
}

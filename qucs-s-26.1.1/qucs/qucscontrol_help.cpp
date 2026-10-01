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
#include "qucs.h"

#include <QAction>
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

} // namespace

QJsonObject QucsControl::readHelp(const QJsonObject& args)
{
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
    const QJsonObject online{{QStringLiteral("manual"), QLatin1String(kOnlineHelp)},
                             {QStringLiteral("getting started"), QLatin1String(kGettingStarted)},
                             {QStringLiteral("note"), tr("Online, not in this build (Help > Help Index and Help > Getting Started open "
                                                         "them in the browser): WebFetch reads them, where it may.")}};

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
    result.insert(QStringLiteral("online"), online);
    if (foundActions.isEmpty() && foundTypes.isEmpty() && foundExamples.isEmpty() && foundPapers.isEmpty())
        result.insert(QStringLiteral("found"), tr("Nothing in this build's help has all of \"%1\": fewer or other words, or the online manual.").arg(topic));
    return jsonResult(result);
}

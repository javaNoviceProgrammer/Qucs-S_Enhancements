/*
 * qucscontrol_pptx.cpp - read_pptx and show_pptx: a PowerPoint
 *                        presentation's slides read, shown, played
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

#include "misc.h"
#include "pptx.h"
#include "pptxdoc.h"
#include "qucs.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

using namespace qucs_s::control;

namespace {

/// The slides a 'slides' argument names: 7, [3, 4], "2-5" (from 1), each
/// once, in order; \a fallback when it names none.
QList<int> slidesNamed(const QJsonValue& v, int count, const QList<int>& fallback)
{
    QList<int> out;
    const auto add = [&out, count](int n) {
        if (n >= 1 && n <= count && !out.contains(n) && out.size() < 500) out << n;
    };
    if (v.isDouble()) add(v.toInt());
    else if (v.isArray())
        for (const QJsonValue& x : v.toArray()) add(x.toInt());
    else if (v.isString())
        for (const QString& part : v.toString().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const QStringList ends = part.split(QLatin1Char('-'));
            const int a = ends.value(0).trimmed().toInt(), b = ends.size() > 1 ? ends.at(1).trimmed().toInt() : a;
            for (int k = a; k <= b && k - a < 500; ++k) add(k);
        }
    else return fallback;
    return out;
}

} // namespace

PptxDoc* QucsControl::presentationDocument(const QJsonObject& args, bool open, QString* error)
{
    QString file = args.value(QLatin1String("path")).toString().trimmed();
    if (file.isEmpty()) {
        auto* deck = dynamic_cast<PptxDoc*>(a_app->DocumentTab->count() > 0 ? a_app->getDoc() : nullptr);
        if (deck == nullptr) *error = tr("Which presentation? 'path' (the document in front is none).");
        return deck;
    }
    // Open: by its file, or its tab's title.
    QString none;
    if (auto* deck = dynamic_cast<PptxDoc*>(document(QJsonObject{{QStringLiteral("path"), file}}, &none))) return deck;
    file = absolute(file);
    for (QucsDoc* d : a_app->allDocuments())
        if (auto* deck = dynamic_cast<PptxDoc*>(d); deck != nullptr && sameFile(deck->getDocName(), file)) return deck;
    if (!QFileInfo(file).isFile()) {
        *error = tr("There is no file %1.").arg(QDir::toNativeSeparators(file));
        return nullptr;
    }
    if (!qucs_s::pptx::isPresentationFile(file)) {
        *error = tr("%1 is no presentation (.pptx, .pptm, .ppsx, .potx): a PowerPoint 97 .ppt is not read.").arg(QFileInfo(file).fileName());
        return nullptr;
    }
    if (!open) return nullptr;
    misc::ErrorCapture said;
    if (!a_app->gotoPage(file, false, false)) {
        *error = tr("%1 could not be opened: %2").arg(QDir::toNativeSeparators(file), said.errors().join(QLatin1Char(' ')));
        return nullptr;
    }
    auto* deck = dynamic_cast<PptxDoc*>(a_app->getDoc());
    if (deck == nullptr) *error = tr("%1 did not open as a presentation.").arg(QFileInfo(file).fileName());
    return deck;
}

QJsonObject QucsControl::readPptx(const QJsonObject& args)
{
    // An open one as its tab shows it, else its file read here.
    QString error;
    std::shared_ptr<const qucs_s::pptx::Presentation> deck;
    QString file;
    if (PptxDoc* open = presentationDocument(args, false, &error)) {
        file = open->getDocName();
        deck = open->presentation();
        if (deck == nullptr) return errorResult(open->problem().isEmpty() ? tr("%1 shows nothing.").arg(titleOf(open)) : open->problem());
    } else {
        if (!error.isEmpty()) return errorResult(error);
        file = absolute(args.value(QLatin1String("path")).toString().trimmed());
        QFile f(file);
        if (!f.open(QIODevice::ReadOnly)) return errorResult(tr("%1 cannot be read: %2").arg(QDir::toNativeSeparators(file), f.errorString()));
        QString why;
        deck = qucs_s::pptx::Presentation::read(f.readAll(), &why);
        if (deck == nullptr) return errorResult(tr("%1 cannot be read as a presentation: %2").arg(QFileInfo(file).fileName(), why));
    }
    const int count = deck->slideCount();
    const QString search = args.value(QLatin1String("search")).toString().trimmed();
    QList<int> all;
    for (int i = 1; i <= count; ++i) all << i;
    const QList<int> slides = slidesNamed(args.value(QLatin1String("slides")), count, all);
    if (slides.isEmpty()) return errorResult(count == 0 ? tr("%1 has no slides.").arg(QFileInfo(file).fileName())
                                                        : tr("%1 has slides 1 to %2.").arg(QFileInfo(file).fileName()).arg(count));
    const QSizeF size = deck->slideSize();
    QJsonObject result{{QStringLiteral("file"), QDir::toNativeSeparators(file)},
                       {QStringLiteral("slides in all"), count},
                       {QStringLiteral("slide size"), tr("%1 x %2 points").arg(qRound(size.width())).arg(qRound(size.height()))}};
    if (!deck->title().isEmpty()) result.insert(QStringLiteral("title"), deck->title());
    if (!search.isEmpty()) {
        // The paragraphs and notes that have it, on the slides asked.
        QJsonArray found;
        for (int n : slides) {
            const qucs_s::pptx::SlideInfo& s = deck->slide(n - 1);
            for (const QString& p : s.paragraphs)
                if (p.contains(search, Qt::CaseInsensitive) && found.size() < 80)
                    found.append(QJsonObject{{QStringLiteral("slide"), n}, {QStringLiteral("text"), p}});
            for (const QString& p : s.notes.split(QLatin1Char('\n')))
                if (p.contains(search, Qt::CaseInsensitive) && found.size() < 80)
                    found.append(QJsonObject{{QStringLiteral("slide"), n}, {QStringLiteral("notes"), p}});
        }
        result.insert(QStringLiteral("search"), search);
        result.insert(QStringLiteral("found"), found);
        if (found.isEmpty()) result.insert(QStringLiteral("note"), tr("Not found in the slides' text or notes (text in a picture is not read)."));
    } else {
        // Each slide: its title, text, notes, and what is not drawn - as
        // much as 40,000 characters, the rest named.
        constexpr int kMost = 40000;
        int used = 0;
        QJsonArray read;
        QStringList left;
        for (int n : slides) {
            const qucs_s::pptx::SlideInfo& s = deck->slide(n - 1);
            QJsonObject o{{QStringLiteral("slide"), n}};
            if (!s.title.isEmpty()) o.insert(QStringLiteral("title"), s.title);
            const QString text = s.paragraphs.join(QLatin1Char('\n'));
            if (used + text.size() + s.notes.size() > kMost && !read.isEmpty()) {
                left << QString::number(n);
                continue;
            }
            used += int(text.size() + s.notes.size());
            o.insert(QStringLiteral("text"), text);
            if (!s.notes.isEmpty()) o.insert(QStringLiteral("notes"), s.notes);
            if (s.hidden) o.insert(QStringLiteral("hidden"), true);
            if (s.pictures > 0) o.insert(QStringLiteral("pictures"), s.pictures);
            if (s.tables > 0) o.insert(QStringLiteral("tables"), s.tables);
            if (s.charts > 0) o.insert(QStringLiteral("charts"), s.charts);
            if (!s.notShown.isEmpty()) o.insert(QStringLiteral("not drawn"), QJsonArray::fromStringList(s.notShown));
            read.append(o);
        }
        result.insert(QStringLiteral("slides"), read);
        if (!left.isEmpty()) result.insert(QStringLiteral("left out"), tr("slides %1: too much text at once - ask for them").arg(left.join(QStringLiteral(", "))));
    }
    // The slides drawn too (four at most).
    if (args.value(QLatin1String("image")).toBool()) {
        QJsonArray content{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                       {QStringLiteral("text"), QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact))}}};
        QStringList drawn;
        for (int n : slides) {
            if (drawn.size() >= 4) break;
            const QImage image = deck->render(n - 1, std::clamp(1600.0 / std::max(size.width(), size.height()), 0.25, 4.0));
            if (image.isNull()) continue;
            content.append(pngContent(fitted(image)));
            drawn << QString::number(n);
        }
        content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                   {QStringLiteral("text"), drawn.isEmpty() ? tr("No slide could be drawn.")
                                                                            : tr("Slides %1 drawn%2.").arg(drawn.join(QStringLiteral(", ")),
                                                                                                          slides.size() > drawn.size() ? tr(" (four at most: ask for the others)") : QString())}});
        return {{QStringLiteral("content"), content}, {QStringLiteral("isError"), false}};
    }
    return jsonResult(result);
}

QJsonObject QucsControl::showPptx(const QJsonObject& args)
{
    QString error;
    PptxDoc* deck = presentationDocument(args, true, &error);
    if (deck == nullptr) return errorResult(error);
    a_app->showDocument(QucsApp::documentWidget(deck));
    if (deck->presentation() == nullptr) return errorResult(deck->problem().isEmpty() ? tr("%1 shows nothing.").arg(titleOf(deck)) : deck->problem());
    const auto presentation = deck->presentation();
    const int count = presentation->slideCount();
    QJsonObject o{{QStringLiteral("file"), QDir::toNativeSeparators(deck->getDocName())}, {QStringLiteral("slides in all"), count}};
    // 'slide': a number, or the start of a slide's title.
    const QJsonValue slide = args.value(QLatin1String("slide"));
    if (slide.isDouble()) {
        const int n = slide.toInt();
        if (n < 1 || n > count) return errorResult(tr("%1 has slides 1 to %2.").arg(titleOf(deck)).arg(count));
        deck->goToSlide(n - 1);
    } else if (slide.isString() && !slide.toString().trimmed().isEmpty()) {
        const QString wanted = slide.toString().trimmed();
        bool ok = false;
        const int n = wanted.toInt(&ok);
        int found = -1;
        if (ok && n >= 1 && n <= count) found = n - 1;
        for (int i = 0; i < count && found < 0; ++i)
            if (presentation->slide(i).title.startsWith(wanted, Qt::CaseInsensitive)) found = i;
        for (int i = 0; i < count && found < 0; ++i)
            if (presentation->slide(i).title.contains(wanted, Qt::CaseInsensitive)) found = i;
        if (found < 0) {
            QStringList titles;
            for (int i = 0; i < count && titles.size() < 40; ++i)
                if (!presentation->slide(i).title.isEmpty()) titles << QStringLiteral("%1 %2").arg(i + 1).arg(presentation->slide(i).title);
            return errorResult(tr("No slide is titled %1. The slides: %2").arg(wanted, titles.join(QStringLiteral("; "))));
        }
        deck->goToSlide(found);
    }
    // 'find': the slides with it, the first shown.
    if (const QString find = args.value(QLatin1String("find")).toString().trimmed(); !find.isEmpty()) {
        QJsonArray hits;
        for (const auto& [i, n] : presentation->find(find)) hits.append(QJsonObject{{QStringLiteral("slide"), i + 1}, {QStringLiteral("times"), n}});
        o.insert(QStringLiteral("hits"), hits);
        if (hits.isEmpty()) o.insert(QStringLiteral("note"), tr("%1 is not in the slides' text or notes.").arg(find));
        else deck->goToSlide(hits.first().toObject().value(QStringLiteral("slide")).toInt() - 1);
    }
    // 'zoom': a percentage, or a fit.
    const QJsonValue zoom = args.value(QLatin1String("zoom"));
    if (zoom.isDouble()) {
        if (!(zoom.toDouble() > 0)) return errorResult(tr("'zoom' is a percentage (150) or \"fit width\" / \"fit slide\"."));
        deck->view()->setZoom(zoom.toDouble() / 100);
    } else if (zoom.isString() && !zoom.toString().trimmed().isEmpty()) {
        const QString z = zoom.toString().trimmed().toLower();
        if (z.contains(QLatin1String("width"))) deck->view()->setFit(qucs_s::pptx::SlideView::Fit::Width);
        else if (z.contains(QLatin1String("slide")) || z.contains(QLatin1String("page")) || z.contains(QLatin1String("all")))
            deck->view()->setFit(qucs_s::pptx::SlideView::Fit::Slide);
        else {
            bool ok = false;
            const double v = QString(z).remove(QLatin1Char('%')).toDouble(&ok);
            if (!ok || v <= 0) return errorResult(tr("'zoom' is a percentage (150) or \"fit width\" / \"fit slide\"."));
            deck->view()->setZoom(v / 100);
        }
    }
    if (args.contains(QLatin1String("notes"))) deck->setNotesShown(args.value(QLatin1String("notes")).toBool());
    // 'play': the slides full screen from the one shown; false ends it.
    if (args.contains(QLatin1String("play"))) {
        if (args.value(QLatin1String("play")).toBool()) deck->startShow(-1);
        else if (deck->slideShow() != nullptr) deck->slideShow()->close();
    }
    const int current = deck->currentSlide();
    const qucs_s::pptx::SlideInfo& info = presentation->slide(current);
    o.insert(QStringLiteral("slide shown"), current + 1);
    if (!info.title.isEmpty()) o.insert(QStringLiteral("its title"), info.title);
    o.insert(QStringLiteral("zoom"), deck->view()->fit() == qucs_s::pptx::SlideView::Fit::Width ? QStringLiteral("fit width")
                                     : deck->view()->fit() == qucs_s::pptx::SlideView::Fit::Slide
                                         ? QStringLiteral("fit slide")
                                         : QStringLiteral("%1%").arg(qRound(deck->view()->zoom() * 100)));
    o.insert(QStringLiteral("notes shown"), deck->notesShown());
    if (deck->slideShow() != nullptr) o.insert(QStringLiteral("playing"), tr("slide %1, full screen (Escape ends it; show_pptx with 'play': false too)").arg(deck->slideShow()->current() + 1));
    o.insert(QStringLiteral("note"), tr("read_pptx reads the slides' text and notes, and draws them with 'image'; screenshot takes a picture of the tab"));
    return jsonResult(o);
}

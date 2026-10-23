/*
 * qucscontrol_pptx.cpp - read_pptx and show_pptx: a PowerPoint
 *                        presentation's slides read, shown, played; its
 *                        tab driven as the user drives it; its slides
 *                        drawn for screenshot and written by export_image
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
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QSaveFile>

#include <algorithm>
#include <cmath>

using namespace qucs_s::control;
using qucs_s::pptx::Presentation;

namespace {

/// The most slides one call reads or writes.
constexpr int kMostSlides = 500;

/// The slides a 'slides' argument names: 7, [3, 4], "2-5" (from 1), each
/// once, in order; "all" all of them; \a fallback when it names none. No
/// more than kMostSlides of them: \a left takes how many were named
/// beyond that, which the answer says (bug hunt 2026-10-23 B4 - a deck of
/// 3,000 slides gave 500 and said nothing).
QList<int> slidesNamed(const QJsonValue& v, int count, const QList<int>& fallback, int* left = nullptr)
{
    QList<int> out;
    int over = 0;
    const auto add = [&out, &over, count](int n) {
        if (n < 1 || n > count || out.contains(n)) return;
        if (out.size() < kMostSlides) out << n;
        else ++over;
    };
    if (v.isDouble()) add(v.toInt());
    else if (v.isArray())
        for (const QJsonValue& x : v.toArray()) add(x.toInt());
    else if (v.isString() && v.toString().trimmed().compare(QLatin1String("all"), Qt::CaseInsensitive) == 0)
        for (int n = 1; n <= count; ++n) add(n);
    else if (v.isString())
        for (const QString& part : v.toString().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const QStringList ends = part.split(QLatin1Char('-'));
            const int a = ends.value(0).trimmed().toInt(), b = ends.size() > 1 ? ends.at(1).trimmed().toInt() : a;
            for (int k = a; k <= b; ++k) add(k);
        }
    else return fallback;
    if (left != nullptr) *left = over;
    return out;
}

/// A slide's 'region': [x1, y1, x2, y2] in points from its top left, or
/// fractions of it (all four between 0 and 1); within the slide.
bool slideRegion(const QJsonValue& v, QSizeF size, QRectF* out, QString* error)
{
    const QJsonArray a = v.toArray();
    bool numbers = a.size() == 4;
    for (const QJsonValue& x : a) numbers = numbers && x.isDouble();
    const QString what = tr("'region' is [x1, y1, x2, y2] of the slide: points from its top left (it is %1 x %2), or fractions of it "
                            "([0.5, 0, 1, 0.5] its top right quarter).")
                             .arg(qRound(size.width())).arg(qRound(size.height()));
    if (!numbers) {
        *error = what;
        return false;
    }
    double c[4];
    bool fractions = true;
    for (int i = 0; i < 4; ++i) {
        c[i] = a.at(i).toDouble();
        fractions = fractions && c[i] >= 0 && c[i] <= 1;
    }
    QRectF r(QPointF(std::min(c[0], c[2]), std::min(c[1], c[3])), QPointF(std::max(c[0], c[2]), std::max(c[1], c[3])));
    if (fractions) r = QRectF(r.left() * size.width(), r.top() * size.height(), r.width() * size.width(), r.height() * size.height());
    r = r.intersected(QRectF(QPointF(0, 0), size));
    if (r.width() < 1 || r.height() < 1) {
        *error = what;
        return false;
    }
    *out = r;
    return true;
}

/// Slide \a index, or \a region of it (points; empty: all of it), drawn
/// sharp enough to read: a whole slide's longer side 2000 pixels at most,
/// a region's some 1600 (1 to 8 a point); \a scale what it was drawn at.
QImage drawnToRead(const Presentation& deck, int index, const QRectF& region, qreal* scale)
{
    const QSizeF size = deck.slideSize();
    if (region.isEmpty()) {
        *scale = std::clamp(2000.0 / std::max(size.width(), size.height()), 0.25, 4.0);
        return deck.render(index, *scale);
    }
    // (The whole slide is drawn, the region cut from it: kept to 40
    // million pixels.)
    *scale = std::clamp(1600.0 / std::max(region.width(), region.height()), 1.0, 8.0);
    *scale = std::min(*scale, std::sqrt(40e6 / std::max(1.0, size.width() * size.height())));
    const QImage whole = deck.render(index, *scale);
    if (whole.isNull()) return whole;
    const QRect part = QRectF(region.left() * *scale, region.top() * *scale, region.width() * *scale, region.height() * *scale).toAlignedRect();
    return whole.copy(part.intersected(whole.rect()));
}

QString regionText(const QRectF& r)
{
    return tr("the region %1, %2 - %3, %4 points")
        .arg(r.left(), 0, 'f', 0).arg(r.top(), 0, 'f', 0).arg(r.right(), 0, 'f', 0).arg(r.bottom(), 0, 'f', 0);
}

/// What an export of slides writes: the formats, by name and suffix.
struct SlideFormat {
    const char* name;
    const char* suffix;
    const char* writer;   // QImageWriter's (empty: a PDF)
};
const SlideFormat kSlideFormats[] = {{"png", "png", "png"},  {"jpeg", "jpg", "jpeg"}, {"jpg", "jpg", "jpeg"}, {"bmp", "bmp", "bmp"},
                                     {"tiff", "tiff", "tiff"}, {"tif", "tif", "tiff"}, {"webp", "webp", "webp"}, {"pdf", "pdf", ""}};

/// What an export of slides is to write: its format, slides and files.
struct ExportPlan {
    const SlideFormat* format = nullptr;
    QList<int> slides;   // from 1
    QStringList files;
    qreal scale = 2;
    int left = 0;   ///< slides named beyond kMostSlides, said in the answer
};

/// The plan of export_image's \a args for \a deck (its file \a file, the
/// slide in sight \a current from 0); false, and why, when it cannot be.
bool planExport(const Presentation& deck, int current, const QJsonObject& args, ExportPlan* plan, QString* error)
{
    QString file = args.value(QLatin1String("save_as")).toString().trimmed();
    if (file.isEmpty()) {
        *error = tr("'save_as' names the file to write.");
        return false;
    }
    if (const QString bad = badFileName(QFileInfo(file).fileName()); !bad.isEmpty()) {
        *error = tr("'save_as': %1.").arg(bad);
        return false;
    }
    file = absolute(file);
    for (const char* key : {"diagram", "selection"})
        if (args.contains(QLatin1String(key))) {
            *error = tr("A presentation has no diagrams or selection: 'slides' names the slides written.");
            return false;
        }
    // The format: as given, else the file's suffix.
    const QString name = args.value(QLatin1String("format")).toString().trimmed().toLower();
    const QString suffix = QFileInfo(file).suffix().toLower();
    for (const SlideFormat& f : kSlideFormats)
        if (name.isEmpty() ? suffix == QLatin1String(f.name) || suffix == QLatin1String(f.suffix) : name == QLatin1String(f.name)) {
            plan->format = &f;
            break;
        }
    if (plan->format == nullptr) {
        *error = name.isEmpty() || name == QLatin1String("svg") || name == QLatin1String("eps") || name == QLatin1String("pdf_tex")
                     ? tr("A presentation's slides are written as png, jpeg, bmp, tiff or webp pictures, or as one pdf (a slide a page): "
                          "give 'format', or a name ending so.")
                     : tr("'format' is png, jpeg, bmp, tiff, webp or pdf for a presentation's slides.");
        return false;
    }
    const bool pdf = QLatin1String(plan->format->writer).isEmpty();
    if (!pdf && !QImageWriter::supportedImageFormats().contains(QByteArray(plan->format->writer))) {
        *error = tr("This build of Qucs-S does not write %1 pictures.").arg(QString::fromLatin1(plan->format->name));
        return false;
    }
    if (suffix != QLatin1String(plan->format->suffix) && suffix != QLatin1String(plan->format->name)) file += QLatin1Char('.') + QLatin1String(plan->format->suffix);
    if (!QFileInfo(QFileInfo(file).absolutePath()).isDir()) {
        *error = tr("There is no folder %1.").arg(QDir::toNativeSeparators(QFileInfo(file).absolutePath()));
        return false;
    }
    const QString colours = args.value(QLatin1String("colours")).toString(QStringLiteral("colour")).trimmed().toLower();
    if (colours != QLatin1String("colour") && colours != QLatin1String("color") && (pdf || (colours != QLatin1String("grayscale") && colours != QLatin1String("greyscale")))) {
        *error = pdf ? tr("A presentation's PDF is written in colour.") : tr("'colours' is colour or grayscale for a presentation's slides.");
        return false;
    }
    // Pixels a point: 2 (a 960-point slide 1920 pixels wide), a PDF's 4.
    const QJsonValue scale = args.value(QLatin1String("scale"));
    if (!scale.isUndefined() && (!scale.isDouble() || scale.toDouble() < 0.1 || scale.toDouble() > 12)) {
        *error = tr("'scale' is a slide's pixels a point, 0.1 to 12 (2: a 960-point slide 1920 pixels wide).");
        return false;
    }
    plan->scale = scale.isDouble() ? scale.toDouble() : pdf ? 4.0 : 2.0;
    const QSizeF size = deck.slideSize();
    if (size.width() * plan->scale * size.height() * plan->scale > 120e6) {
        *error = tr("At 'scale' %1 a slide would be %2 x %3 pixels: 120 million at most.").arg(plan->scale)
                     .arg(qRound(size.width() * plan->scale)).arg(qRound(size.height() * plan->scale));
        return false;
    }
    // The slides: as asked; a PDF all of them, a picture the slide in sight.
    const int count = deck.slideCount();
    QList<int> all;
    for (int n = 1; n <= count; ++n) all << n;
    plan->slides = slidesNamed(args.value(QLatin1String("slides")), count,
                               pdf ? all : QList<int>{std::clamp(current + 1, 1, std::max(1, count))}, &plan->left);
    if (count == 0 || plan->slides.isEmpty()) {
        *error = count == 0 ? tr("The presentation has no slides.") : tr("'slides': the presentation has slides 1 to %1.").arg(count);
        return false;
    }
    // One file; pictures of several slides numbered after its name
    // ("talk.png": talk-03.png, talk-10.png).
    plan->files.clear();
    if (pdf || plan->slides.size() == 1) {
        plan->files << file;
    } else {
        const QFileInfo at(file);
        const int digits = int(QString::number(count).size());
        for (int n : plan->slides)
            plan->files << at.absoluteDir().filePath(QStringLiteral("%1-%2.%3").arg(at.completeBaseName()).arg(n, digits, 10, QLatin1Char('0')).arg(at.suffix()));
    }
    return true;
}

} // namespace

PptxDoc* QucsControl::presentationDocument(const QJsonObject& args, bool open, QString* error)
{
    if (PptxDoc* deck = presentationFound(args, error)) return deck;
    if (!error->isEmpty() || !open) return nullptr;
    const QString file = absolute(args.value(QLatin1String("path")).toString().trimmed());
    misc::ErrorCapture said;
    if (!a_app->gotoPage(file, false, false)) {
        *error = tr("%1 could not be opened: %2").arg(QDir::toNativeSeparators(file), said.errors().join(QLatin1Char(' ')));
        return nullptr;
    }
    auto* deck = dynamic_cast<PptxDoc*>(a_app->getDoc());
    if (deck == nullptr) *error = tr("%1 did not open as a presentation.").arg(QFileInfo(file).fileName());
    return deck;
}

PptxDoc* QucsControl::presentationFound(const QJsonObject& args, QString* error) const
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
    return nullptr;   // (a presentation's file, not open)
}

std::shared_ptr<const Presentation> QucsControl::presentationOf(const QJsonObject& args, QString* file, int* current, QString* error) const
{
    // An open one as its tab shows it, else its file read here.
    *current = 0;
    if (PptxDoc* open = presentationFound(args, error)) {
        *file = open->getDocName();
        *current = open->currentSlide();
        if (open->presentation() == nullptr) *error = open->problem().isEmpty() ? tr("%1 shows nothing.").arg(titleOf(open)) : open->problem();
        return open->presentation();
    }
    if (!error->isEmpty()) return nullptr;
    *file = absolute(args.value(QLatin1String("path")).toString().trimmed());
    QFile f(*file);
    if (!f.open(QIODevice::ReadOnly)) {
        *error = tr("%1 cannot be read: %2").arg(QDir::toNativeSeparators(*file), f.errorString());
        return nullptr;
    }
    QString why;
    auto deck = Presentation::read(f.readAll(), &why);
    if (deck == nullptr) *error = tr("%1 cannot be read as a presentation: %2").arg(QFileInfo(*file).fileName(), why);
    return deck;
}

QJsonObject QucsControl::readPptx(const QJsonObject& args)
{
    QString error, file;
    int current = 0;
    const std::shared_ptr<const Presentation> deck = presentationOf(args, &file, &current, &error);
    if (deck == nullptr) return errorResult(error);
    PptxDoc* open = presentationFound(args, &error);
    const int count = deck->slideCount();
    const QString search = args.value(QLatin1String("search")).toString().trimmed();
    QList<int> all;
    for (int i = 1; i <= count; ++i) all << i;
    int leftOut = 0;
    const QList<int> slides = slidesNamed(args.value(QLatin1String("slides")), count, all, &leftOut);
    if (slides.isEmpty()) return errorResult(count == 0 ? tr("%1 has no slides.").arg(QFileInfo(file).fileName())
                                                        : tr("%1 has slides 1 to %2.").arg(QFileInfo(file).fileName()).arg(count));
    const QSizeF size = deck->slideSize();
    QRectF region;
    if (args.contains(QLatin1String("region")) && !slideRegion(args.value(QLatin1String("region")), size, &region, &error)) return errorResult(error);
    QJsonObject result{{QStringLiteral("file"), QDir::toNativeSeparators(file)},
                       {QStringLiteral("slides in all"), count},
                       {QStringLiteral("slide size"), tr("%1 x %2 points").arg(qRound(size.width())).arg(qRound(size.height()))}};
    if (!deck->title().isEmpty()) result.insert(QStringLiteral("title"), deck->title());
    // Open: what its tab shows.
    if (open != nullptr) result.insert(QStringLiteral("its tab"), pptxView(open));
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
        QStringList over;
        if (!left.isEmpty()) over << tr("slides %1: too much text at once").arg(left.join(QStringLiteral(", ")));
        if (leftOut > 0) over << tr("%1 slides more, %2 at a time at most").arg(leftOut).arg(kMostSlides);
        if (!over.isEmpty())
            result.insert(QStringLiteral("left out"), over.join(QStringLiteral("; ")) + tr(" - ask for them by their numbers"));
    }
    // The slides drawn too (four at most), or 'region' of each.
    if (args.value(QLatin1String("image")).toBool()) {
        QJsonArray content{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                       {QStringLiteral("text"), QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact))}}};
        QStringList drawn;
        for (int n : slides) {
            if (drawn.size() >= 4) break;
            qreal scale = 1;
            const QImage image = drawnToRead(*deck, n - 1, region, &scale);
            if (image.isNull()) continue;
            content.append(pngContent(fitted(image)));
            drawn << QString::number(n);
        }
        QString said = drawn.isEmpty() ? tr("No slide could be drawn.") : tr("Slides %1 drawn").arg(drawn.join(QStringLiteral(", ")));
        if (!drawn.isEmpty() && !region.isEmpty()) said += QStringLiteral(", ") + regionText(region);
        if (!drawn.isEmpty()) said += slides.size() > drawn.size() ? tr(" (four at most: ask for the others).") : QStringLiteral(".");
        content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), said}});
        return {{QStringLiteral("content"), content}, {QStringLiteral("isError"), false}};
    }
    if (!region.isEmpty()) result.insert(QStringLiteral("note"), tr("'region' is of the pictures 'image' draws."));
    return jsonResult(result);
}

QJsonObject QucsControl::pptxView(PptxDoc* deck) const
{
    QJsonObject o;
    const auto presentation = deck->presentation();
    if (presentation == nullptr) {
        o.insert(QStringLiteral("problem"), deck->problem().isEmpty() ? tr("nothing is shown") : deck->problem());
        return o;
    }
    const int count = presentation->slideCount();
    o.insert(QStringLiteral("slides in all"), count);
    if (count == 0) return o;
    const int current = deck->currentSlide();
    const qucs_s::pptx::SlideInfo& info = presentation->slide(current);
    o.insert(QStringLiteral("slide shown"), current + 1);
    if (!info.title.isEmpty()) o.insert(QStringLiteral("its title"), info.title);
    if (info.hidden) o.insert(QStringLiteral("it is hidden"), tr("left out of a slide show"));
    using Fit = qucs_s::pptx::SlideView::Fit;
    o.insert(QStringLiteral("zoom"), deck->view()->fit() == Fit::Width   ? QStringLiteral("fit width")
                                     : deck->view()->fit() == Fit::Slide ? QStringLiteral("fit slide")
                                                                         : QStringLiteral("%1%").arg(qRound(deck->view()->zoom() * 100)));
    o.insert(QStringLiteral("notes shown"), deck->notesShown());
    o.insert(QStringLiteral("thumbnails shown"), deck->sidebarShown());
    if (deck->findShown()) {
        QJsonObject f{{QStringLiteral("text"), deck->findText()}};
        QJsonArray hits;
        for (int h : deck->hits()) hits.append(h + 1);
        f.insert(QStringLiteral("slides with it"), hits);
        if (deck->hitShown() >= 0 && deck->hitShown() < deck->hits().size()) {
            const int at = deck->hits().at(deck->hitShown());
            // "shown" only where the tab is on that slide: after a reload
            // the hits are counted again without the view moving, and the
            // one it would go to next is not the one shown (bug hunt
            // 2026-10-23 N4).
            f.insert(at == deck->currentSlide() ? QStringLiteral("shown") : QStringLiteral("next"),
                     tr("slide %1, the %2 of %3").arg(at + 1).arg(deck->hitShown() + 1).arg(deck->hits().size()));
        }
        o.insert(QStringLiteral("find bar"), f);
    }
    if (const qucs_s::pptx::SlideShow* show = deck->slideShow()) {
        QJsonObject playing;
        if (show->atEnd()) {
            playing.insert(QStringLiteral("at the end"), tr("black, past the last slide: 'slide' \"next\" ends the show, \"previous\" goes back"));
        } else {
            playing.insert(QStringLiteral("slide"), show->current() + 1);
            if (!presentation->slide(show->current()).title.isEmpty()) playing.insert(QStringLiteral("its title"), presentation->slide(show->current()).title);
        }
        playing.insert(QStringLiteral("note"), tr("full screen: 'slide' moves it, 'play': false ends it (the user's Escape too)"));
        o.insert(QStringLiteral("playing"), playing);
    }
    return o;
}

bool QucsControl::viewPptx(PptxDoc* deck, const QJsonObject& args, QJsonObject* out, QString* error)
{
    // What is asked looked at before anything is done: none of it, or all.
    const QJsonValue slide = args.value(QLatin1String("slide"));
    const QString step = slide.isString() ? slide.toString().trimmed().toLower() : QString();
    const bool stepped = step == QLatin1String("next") || step == QLatin1String("previous") || step == QLatin1String("first") || step == QLatin1String("last");
    if (!slide.isUndefined() && !slide.isNull() && !slide.isDouble() && !slide.isString()) {
        *error = tr("'slide' is a number (1 the first), a slide's title (or its start), or next, previous, first or last.");
        return false;
    }
    const QString findNext = args.value(QLatin1String("find_next")).toString().trimmed().toLower();
    if (args.contains(QLatin1String("find_next")) && findNext != QLatin1String("next") && findNext != QLatin1String("previous")) {
        *error = tr("'find_next' is next or previous: the next slide with the find bar's text, or the one before.");
        return false;
    }
    const QJsonValue play = args.value(QLatin1String("play"));
    if (args.contains(QLatin1String("play")) && !play.isBool() && play.toString().trimmed().compare(QLatin1String("first"), Qt::CaseInsensitive) != 0) {
        *error = tr("'play' is true (from the slide shown), \"first\" (from the first slide) or false (the show ended).");
        return false;
    }
    const QString copy = args.value(QLatin1String("copy")).toString().trimmed().toLower();
    if (args.contains(QLatin1String("copy")) && copy != QLatin1String("picture") && copy != QLatin1String("text")) {
        *error = tr("'copy' is picture (the slide shown, 1920 pixels wide) or text (its text and notes): to the clipboard.");
        return false;
    }
    const QJsonValue zoom = args.value(QLatin1String("zoom"));
    std::optional<qucs_s::pptx::SlideView::Fit> fit;
    double scale = 0;
    if (zoom.isDouble()) {
        scale = zoom.toDouble() / 100;
    } else if (zoom.isString() && !zoom.toString().trimmed().isEmpty()) {
        const QString z = zoom.toString().trimmed().toLower();
        if (z.contains(QLatin1String("width"))) fit = qucs_s::pptx::SlideView::Fit::Width;
        else if (z.contains(QLatin1String("slide")) || z.contains(QLatin1String("page")) || z.contains(QLatin1String("all"))) fit = qucs_s::pptx::SlideView::Fit::Slide;
        else scale = QString(z).remove(QLatin1Char('%')).toDouble() / 100;
    }
    if (args.contains(QLatin1String("zoom")) && !fit && !(scale > 0)) {
        *error = tr("'zoom' is a percentage (150) or \"fit width\" / \"fit slide\".");
        return false;
    }

    // Read again first: what follows is of what is read then.
    if (args.value(QLatin1String("reload")).toBool() && !deck->reload()) {
        *error = deck->problem();
        return false;
    }
    const auto presentation = deck->presentation();
    if (presentation == nullptr) {
        *error = deck->problem().isEmpty() ? tr("%1 shows nothing.").arg(titleOf(deck)) : deck->problem();
        return false;
    }
    const int count = presentation->slideCount();
    QRectF region;
    if (args.contains(QLatin1String("region")) && !slideRegion(args.value(QLatin1String("region")), presentation->slideSize(), &region, error)) return false;
    QStringList done;
    QPointer<qucs_s::pptx::SlideShow> show = deck->slideShow();   // (playing: shown)

    // 'slide': a number, a title's start, or a step - the show's while it
    // plays, the tab's else.
    if (!slide.isUndefined() && !slide.isNull() && count > 0) {
        int target = -1;
        if (slide.isDouble()) {
            const int n = slide.toInt();
            if (n < 1 || n > count) {
                *error = tr("%1 has slides 1 to %2.").arg(titleOf(deck)).arg(count);
                return false;
            }
            target = n - 1;
        } else if (!stepped && !slide.toString().trimmed().isEmpty()) {
            const QString wanted = slide.toString().trimmed();
            bool ok = false;
            const int n = wanted.toInt(&ok);
            if (ok && n >= 1 && n <= count) target = n - 1;
            for (int i = 0; i < count && target < 0; ++i)
                if (presentation->slide(i).title.startsWith(wanted, Qt::CaseInsensitive)) target = i;
            for (int i = 0; i < count && target < 0; ++i)
                if (presentation->slide(i).title.contains(wanted, Qt::CaseInsensitive)) target = i;
            if (target < 0) {
                QStringList titles;
                for (int i = 0; i < count && titles.size() < 40; ++i)
                    if (!presentation->slide(i).title.isEmpty()) titles << QStringLiteral("%1 %2").arg(i + 1).arg(presentation->slide(i).title);
                *error = tr("No slide is titled %1. The slides: %2").arg(wanted, titles.join(QStringLiteral("; ")));
                return false;
            }
        }
        if (show) {
            if (step == QLatin1String("next")) show->next();
            else if (step == QLatin1String("previous")) show->previous();
            else if (step == QLatin1String("first")) show->first();
            else if (step == QLatin1String("last")) show->last();
            else if (target >= 0) show->goTo(target);
            if (deck->slideShow() == nullptr) done << tr("The show ended (past its end).");
            else if (show->atEnd()) done << tr("The show is at its end (black).");
        } else {
            const int was = deck->currentSlide();
            if (step == QLatin1String("next")) deck->nextSlide();
            else if (step == QLatin1String("previous")) deck->previousSlide();
            else if (step == QLatin1String("first")) deck->firstSlide();
            else if (step == QLatin1String("last")) deck->lastSlide();
            else if (target >= 0) deck->goToSlide(target);
            // A step that moved nothing is said, so a step at an end is not
            // read as one that moved (bug hunt 2026-10-23 N1).
            if (!step.isEmpty() && deck->currentSlide() == was)
                done << (step == QLatin1String("next") || step == QLatin1String("last")
                             ? tr("Already the last slide (%1).").arg(was + 1)
                             : tr("Already the first slide."));
        }
    }
    // 'find': the find bar with it, the first slide with it from the one
    // shown on (as typed there); "" closes the bar. 'find_next' goes on.
    if (args.contains(QLatin1String("find"))) {
        const QString text = args.value(QLatin1String("find")).toString();
        if (text.trimmed().isEmpty()) {
            deck->hideSearch();
        } else {
            deck->setFindText(text);
            QJsonArray hits;
            for (const auto& [i, n] : presentation->find(text)) hits.append(QJsonObject{{QStringLiteral("slide"), i + 1}, {QStringLiteral("times"), n}});
            out->insert(QStringLiteral("hits"), hits);
            if (hits.isEmpty()) done << tr("%1 is not in the slides' text or notes (text in a picture is not read).").arg(text.trimmed());
        }
    }
    if (args.contains(QLatin1String("find_next"))) {
        if (!deck->findShown() || deck->hits().isEmpty()) {
            *error = tr("Nothing found to go on from: 'find' a text first.");
            return false;
        }
        deck->findNext(findNext == QLatin1String("previous"));
    }
    // 'zoom', then 'region' of the slide shown. (A zoom keeps the slide it
    // was on in sight: SlideView::setZoom, which no longer opens on the
    // end of the slide before - bug hunt 2026-10-23 B2.)
    if (fit) deck->view()->setFit(*fit);
    else if (scale > 0) deck->view()->setZoom(scale);
    if (!region.isEmpty()) {
        deck->showRegion(deck->currentSlide(), region);
        done << tr("Slide %1, %2, as large as the tab takes it.").arg(deck->currentSlide() + 1).arg(regionText(region));
    }
    if (args.contains(QLatin1String("notes"))) deck->setNotesShown(args.value(QLatin1String("notes")).toBool());
    if (args.contains(QLatin1String("sidebar"))) deck->setSidebarShown(args.value(QLatin1String("sidebar")).toBool());
    // 'play': the slides full screen - from the slide shown, or the first;
    // false ends it. (Already playing: it goes on where it is.)
    if (args.contains(QLatin1String("play"))) {
        if (play.isBool() && !play.toBool()) {
            if (show) show->close();
        } else if (play.isString()) {
            deck->startShow(0);
        } else if (!show) {
            deck->startShow(-1);
        }
    }
    // 'copy': the slide shown to the clipboard.
    if (copy == QLatin1String("picture")) {
        deck->copySlide();
        done << tr("Slide %1 copied as a picture, 1920 pixels wide.").arg(deck->currentSlide() + 1);
    } else if (copy == QLatin1String("text")) {
        deck->copySlideText();
        done << tr("Slide %1's text copied, its notes after it.").arg(deck->currentSlide() + 1);
    }
    if (args.value(QLatin1String("reload")).toBool()) done.prepend(tr("Read again from its file."));
    const QJsonObject view = pptxView(deck);
    for (auto it = view.begin(); it != view.end(); ++it) out->insert(it.key(), it.value());
    if (!done.isEmpty()) out->insert(QStringLiteral("done"), done.join(QLatin1Char(' ')));
    return true;
}

QJsonObject QucsControl::showPptx(const QJsonObject& args)
{
    QString error;
    PptxDoc* deck = presentationDocument(args, true, &error);
    if (deck == nullptr) return errorResult(error);
    a_app->showDocument(QucsApp::documentWidget(deck));
    QJsonObject o{{QStringLiteral("file"), QDir::toNativeSeparators(deck->getDocName())}};
    if (!viewPptx(deck, args, &o, &error)) return errorResult(error);
    o.insert(QStringLiteral("note"), tr("read_pptx reads the slides' text and notes (and draws them with 'image'); screenshot takes a picture of "
                                        "the tab, or of a slide with 'slide'; export_image writes slides as pictures or a PDF; get_ui and "
                                        "set_ui with area \"document\" use the tab's bar, context_menu {\"slide\": n} its right-click menu"));
    return jsonResult(o);
}

QJsonObject QucsControl::slidePicture(PptxDoc* deck, const QJsonObject& args)
{
    const auto presentation = deck->presentation();
    if (presentation == nullptr) return errorResult(deck->problem().isEmpty() ? tr("%1 shows nothing.").arg(titleOf(deck)) : deck->problem());
    const int count = presentation->slideCount();
    const int slide = args.contains(QLatin1String("slide")) ? args.value(QLatin1String("slide")).toInt() : deck->currentSlide() + 1;
    if (slide < 1 || slide > count) return errorResult(tr("%1 has slides 1 to %2.").arg(titleOf(deck)).arg(count));
    QRectF region;
    QString error;
    if (args.contains(QLatin1String("region")) && !slideRegion(args.value(QLatin1String("region")), presentation->slideSize(), &region, &error))
        return errorResult(error);
    qreal scale = 1;
    const QImage image = drawnToRead(*presentation, slide - 1, region, &scale);
    if (image.isNull()) return errorResult(tr("Slide %1 of %2 could not be drawn.").arg(slide).arg(titleOf(deck)));
    const QString what = region.isEmpty() ? tr("slide %1 of %2").arg(slide).arg(count) : tr("slide %1 of %2, %3").arg(slide).arg(count).arg(regionText(region));
    return {{QStringLiteral("content"),
             QJsonArray{pngContent(fitted(image)),
                        QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                    {QStringLiteral("text"), tr("%1, %2 - drawn at %3 pixels a point, %4 x %5 pixels; its tab left as it is.")
                                                                 .arg(titleOf(deck), what).arg(scale, 0, 'g', 3).arg(image.width()).arg(image.height())}}}},
            {QStringLiteral("isError"), false}};
}

bool QucsControl::isSlideExport(const QJsonObject& args) const
{
    const QString path = args.value(QLatin1String("path")).toString().trimmed();
    if (path.isEmpty()) return dynamic_cast<PptxDoc*>(a_app->DocumentTab->count() > 0 ? a_app->getDoc() : nullptr) != nullptr;
    QString error;
    if (presentationFound(args, &error) != nullptr) return true;
    return error.isEmpty() || qucs_s::pptx::isPresentationFile(path);
}

QStringList QucsControl::slideExportFiles(const QJsonObject& args) const
{
    QString error, file;
    int current = 0;
    const auto deck = presentationOf(args, &file, &current, &error);
    ExportPlan plan;
    if (deck == nullptr || !planExport(*deck, current, args, &plan, &error)) return {};
    return plan.files;
}

QJsonObject QucsControl::exportSlides(const QJsonObject& args)
{
    QString error, file;
    int current = 0;
    const auto deck = presentationOf(args, &file, &current, &error);
    if (deck == nullptr) return errorResult(error);
    ExportPlan plan;
    if (!planExport(*deck, current, args, &plan, &error)) return errorResult(error);
    for (const QString& f : std::as_const(plan.files))
        if (const QString no = aboutToWrite(f); !no.isEmpty()) return errorResult(no);
    const QSizeF size = deck->slideSize();
    const bool pdf = QLatin1String(plan.format->writer).isEmpty();
    const bool grey = args.value(QLatin1String("colours")).toString().trimmed().toLower().startsWith(QLatin1String("gr"));
    if (pdf) {
        // A page a slide, as large as the slide, its picture at 'scale'.
        QPdfWriter writer(plan.files.first());
        writer.setCreator(QStringLiteral("Qucs-S"));
        writer.setTitle(deck->title().isEmpty() ? QFileInfo(file).completeBaseName() : deck->title());
        writer.setPageSize(QPageSize(size, QPageSize::Point));
        writer.setPageMargins(QMarginsF(0, 0, 0, 0));
        writer.setResolution(std::max(1, qRound(72 * plan.scale)));
        QPainter painter;
        if (!painter.begin(&writer)) return errorResult(tr("%1 could not be written.").arg(QDir::toNativeSeparators(plan.files.first())));
        for (int i = 0; i < plan.slides.size(); ++i) {
            if (i > 0) writer.newPage();
            deck->render(plan.slides.at(i) - 1, &painter, QRectF(painter.viewport()));
        }
        painter.end();
    } else {
        for (int i = 0; i < plan.slides.size(); ++i) {
            QImage image = deck->render(plan.slides.at(i) - 1, plan.scale);
            if (image.isNull()) return errorResult(tr("Slide %1 could not be drawn.").arg(plan.slides.at(i)));
            if (grey) image = image.convertToFormat(QImage::Format_Grayscale8);
            QSaveFile out(plan.files.at(i));
            QImageWriter writer(&out, QByteArray(plan.format->writer));
            if (!out.open(QIODevice::WriteOnly) || !writer.write(image) || !out.commit())
                return errorResult(tr("%1 was not written: %2").arg(QDir::toNativeSeparators(plan.files.at(i)), writer.errorString()));
        }
    }
    QJsonArray written, slides;
    qint64 bytes = 0;
    for (const QString& f : std::as_const(plan.files)) {
        written.append(QDir::toNativeSeparators(f));
        bytes += QFileInfo(f).size();
    }
    QStringList hidden;
    for (int n : std::as_const(plan.slides)) {
        slides.append(n);
        if (deck->slide(n - 1).hidden) hidden << QString::number(n);
    }
    QJsonObject result{{QStringLiteral("written"), written},
                       {QStringLiteral("format"), QString::fromLatin1(plan.format->name)},
                       {QStringLiteral("slides"), slides},
                       {QStringLiteral("bytes"), bytes}};
    const QJsonArray pixels{qRound(size.width() * plan.scale), qRound(size.height() * plan.scale)};
    if (pdf) {
        result.insert(QStringLiteral("pages in points"), QJsonArray{qRound(size.width()), qRound(size.height())});
        result.insert(QStringLiteral("note"), tr("Each page a picture of its slide, %1 x %2 pixels: its text is not selectable.").arg(pixels.at(0).toInt()).arg(pixels.at(1).toInt()));
    } else {
        result.insert(QStringLiteral("pixels"), pixels);
    }
    if (!hidden.isEmpty()) result.insert(QStringLiteral("hidden slides among them"), hidden.join(QStringLiteral(", ")));
    if (plan.left > 0)
        result.insert(QStringLiteral("left out"),
                      tr("%1 slides more were asked for: %2 at a time at most - ask for the rest by their numbers")
                          .arg(plan.left).arg(kMostSlides));
    if (args.value(QLatin1String("transparent")).toBool()) result.insert(QStringLiteral("transparent"), tr("no: a slide keeps its background"));
    return jsonResult(result);
}

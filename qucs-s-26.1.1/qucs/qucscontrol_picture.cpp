/*
 * qucscontrol_picture.cpp - show_picture: a picture's tab (the image
 *                           viewer) as it shows it, and the user's pen on
 *                           it
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
#include "imagedoc.h"
#include "markup.h"
#include "misc.h"
#include "qucs.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <cmath>

using namespace qucs_s::control;
using qucs_s::image::ImageView;

ImageDoc* QucsControl::pictureDocument(const QJsonObject& args, bool open, QString* error)
{
    const QString path = args.value(QLatin1String("path")).toString().trimmed();
    if (path.isEmpty()) {
        auto* picture = dynamic_cast<ImageDoc*>(a_app->DocumentTab->count() > 0 ? a_app->getDoc() : nullptr);
        if (picture == nullptr) *error = tr("Which picture? 'path' (the document in front is none).");
        return picture;
    }
    // Open: by its file, or its tab's title.
    QString none;
    if (QucsDoc* doc = document(QJsonObject{{QStringLiteral("path"), path}}, &none)) {
        if (auto* picture = dynamic_cast<ImageDoc*>(doc)) return picture;
        *error = tr("%1 is open, and not as a picture: the image viewer shows %2.").arg(titleOf(doc), qucs_s::image::suffixes().join(QStringLiteral(", ")));
        return nullptr;
    }
    const QString file = absolute(path);
    if (!QFileInfo(file).isFile()) {
        *error = tr("There is no file %1.").arg(QDir::toNativeSeparators(file));
        return nullptr;
    }
    if (!QucsApp::isImageFile(file)) {
        *error = tr("%1 is no picture the image viewer opens (%2).").arg(QFileInfo(file).fileName(), qucs_s::image::suffixes().join(QStringLiteral(", ")));
        return nullptr;
    }
    if (!open) return nullptr;
    misc::ErrorCapture said;
    if (!a_app->gotoPage(file, false, false)) {
        *error = tr("%1 could not be opened: %2").arg(QDir::toNativeSeparators(file), said.errors().join(QLatin1Char(' ')));
        return nullptr;
    }
    auto* picture = dynamic_cast<ImageDoc*>(a_app->getDoc());
    if (picture == nullptr) *error = tr("%1 did not open as a picture.").arg(QFileInfo(file).fileName());
    return picture;
}

QJsonObject QucsControl::showPicture(const QJsonObject& args)
{
    // Every argument checked before anything is done.
    const QJsonValue pen = args.value(QLatin1String("pen"));
    if (args.contains(QLatin1String("pen")) && !pen.isBool())
        return errorResult(tr("'pen' is true (the user's pen taken up: a drag over the picture draws on it) or false (put down)."));
    const QString drawings = args.value(QLatin1String("drawings")).toString().trimmed().toLower();
    if (args.contains(QLatin1String("drawings")) && drawings != QLatin1String("clear"))
        return errorResult(tr("'drawings' is clear: what the pen drew on the picture taken away (the pen's Undo gives it back)."));
    const QJsonValue addressed = args.value(QLatin1String("addressed"));
    const bool addressedNone = addressed.isString() && addressed.toString().trimmed().compare(QLatin1String("none"), Qt::CaseInsensitive) == 0;
    QList<int> addressedIds;
    if (args.contains(QLatin1String("addressed")) && !addressedNone) {
        bool good = addressed.isArray() && !addressed.toArray().isEmpty();
        for (const QJsonValue& v : addressed.toArray()) {
            good = good && v.isDouble() && v.toDouble() == std::floor(v.toDouble()) && v.toDouble() >= 1 && v.toDouble() <= 1e9;
            if (good) addressedIds << v.toInt();
        }
        if (!good)
            return errorResult(tr("'addressed' is a list of the pen's stroke numbers on the frame shown, as read_drawings numbers them "
                                  "(marked dealt with: faded, and left out of what a prompt tells), or none."));
    }
    QString error;
    ImageDoc* picture = pictureDocument(args, true, &error);
    if (picture == nullptr) return errorResult(error);
    a_app->showDocument(QucsApp::documentWidget(picture));
    ImageView* view = picture->view();
    // (A stroke it does not have refused before anything is done, as on a
    //  layout's tab.)
    if (!addressedIds.isEmpty()) {
        QStringList missing, there;
        for (const int id : std::as_const(addressedIds))
            if (view->drawing().find(view->part(), id) == nullptr) missing << QString::number(id);
        if (!missing.isEmpty()) {
            for (const qucs_s::markup::Stroke& s : view->drawing().on(view->part())) there << QString::number(s.id);
            return errorResult(tr("%1 has no stroke %2 (its strokes: %3).")
                                   .arg(picture->drawnByFrame() ? tr("The frame shown") : tr("The picture"), missing.join(QStringLiteral(", ")),
                                        there.isEmpty() ? tr("none") : there.join(QStringLiteral(", "))));
        }
    }

    QJsonObject o{{QStringLiteral("document"), titleOf(picture)}, {QStringLiteral("file"), QDir::toNativeSeparators(picture->getDocName())}};
    QStringList done;
    if (drawings == QLatin1String("clear"))
        done << (view->clearDrawing() ? tr("what the pen drew taken away (the pen's Undo gives it back)") : tr("nothing was drawn to take away"));
    if (!addressedIds.isEmpty()) {
        done << tr("%n stroke(s) marked addressed: drawn faded, and left out of what a prompt tells", "", view->setAddressed(addressedIds, true));
    } else if (addressedNone) {
        QList<int> all;
        for (const qucs_s::markup::Stroke& s : view->drawing().on(view->part())) all << s.id;
        done << tr("%n stroke(s) no longer marked addressed", "", view->setAddressed(all, false));
    }
    if (pen.isBool()) {
        picture->penButton()->setChecked(pen.toBool());
        done << (pen.toBool() ? tr("the pen taken up") : tr("the pen put down"));
    }
    if (!done.isEmpty()) o.insert(QStringLiteral("done"), done.join(QStringLiteral("; ")));

    // What the tab shows.
    if (!picture->problem().isEmpty()) o.insert(QStringLiteral("problem"), picture->problem());
    o.insert(QStringLiteral("format"), picture->format());
    o.insert(QStringLiteral("size"), tr("%1 × %2 pixels").arg(picture->imageSize().width()).arg(picture->imageSize().height()));
    if (picture->frameCount() > 1)
        o.insert(QStringLiteral("frame"), tr("%1 of %2%3")
                                              .arg(picture->currentFrame() + 1)
                                              .arg(picture->frameCount())
                                              .arg(picture->isAnimated() ? (picture->isPlaying() ? tr(", playing") : tr(", an animation")) : QString()));
    using Fit = ImageView::Fit;
    const Fit fit = view->fit();
    o.insert(QStringLiteral("zoom"), QStringLiteral("%1%%2")
                                         .arg(std::round(view->zoom() * 1000) / 10)
                                         .arg(fit == Fit::Auto ? tr(" (best fit)") : fit == Fit::Window ? tr(" (fit window)") : fit == Fit::Width ? tr(" (fit width)") : QString()));
    if (view->turns() != 0) o.insert(QStringLiteral("turned"), tr("%1° clockwise (the view; the strokes' pixels are the file's)").arg(90 * view->turns()));
    if (!view->selection().isEmpty()) {
        const QRect r = view->selection();
        o.insert(QStringLiteral("selection"), QJsonArray{r.left(), r.top(), r.right() + 1, r.bottom() + 1});
    }
    o.insert(QStringLiteral("pen"), view->penActive() ? tr("taken up: a drag over the picture draws on it (Escape puts it down)")
                                                      : tr("put down: its toolbar's pen takes it up"));
    // What the user drew, read stroke by stroke - its gesture and box in
    // the picture's pixels - the latest 40.
    if (!view->drawing().isEmpty()) {
        insertDrawings(o, picture->readDrawing(false));
        o.insert(QStringLiteral("measures"), tr("the picture's pixels from its top left, as in the file (not turned); what is under a stroke is not "
                                                "known: screenshot shows it"));
    } else {
        o.insert(QStringLiteral("drawings"), tr("nothing is drawn on the picture"));
    }
    return jsonResult(o);
}

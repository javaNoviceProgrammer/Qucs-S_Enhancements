/*
 * test_rf25_stack_drawing.cpp - an RF 2.5D model's stack drawn as a PDF of
 *                               its cross-section, as EMX draws a process
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * What is drawn (the layers and what is said of each, the boxes and their
 * columns, the key), where it goes on the page, the PDF, and the pixels of
 * a page painted. With QUCS_STACK_DRAWING_OF naming a model (.qrf25) and
 * QUCS_STACK_DRAWING_TO a folder, that model's drawing is written there as
 * a PDF and a PNG of each page, to be looked at.
 */
#include <QtTest>

#include "rf25/rf25_model.h"
#include "rf25/rf25_stackfile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPageSize>
#include <QPainter>
#include <QTemporaryDir>

#include <cmath>

using namespace qucs_s::rf25;

namespace {

Model modelOf(const QString& stack)
{
    const QString json = QStringLiteral(R"({"format": "qucs-s rf 2.5d 1", "tree": {"type": "rf_model", "children": [
        {"type": "rf_stack", "tag": "stack", "children": [%1]},
        {"type": "rf_layout", "tag": "layout", "children": [{"type": "rf_layer_map", "tag": "layer_map"}]},
        {"type": "rf_ports", "tag": "ports"},
        {"type": "rf_study", "tag": "study", "properties": {"kind": "full_wave", "from": "1[GHz]", "to": "10[GHz]", "points": "4"}}]}})")
                             .arg(stack);
    QString why;
    const std::optional<Model> m = Model::fromJson(json.toUtf8(), &why);
    if (!m) qFatal("the test's model does not read: %s", qPrintable(why));
    return *m;
}

/// A plane, lossy silicon, an oxide, M1, an oxide a via goes through up to
/// M2, M2 and a passivation; \a more goes on top.
QString twoMetals(const QString& more = {}, const QString& ground = QStringLiteral("perfect"))
{
    return QStringLiteral(R"(
        {"type": "rf_ground", "tag": "g", "label": "GND", "properties": {"kind": "%2", "sigma": "4.1e7"}},
        {"type": "rf_substrate", "tag": "s", "label": "Si", "properties": {"thickness": "300[um]", "epsr": "11.9", "tand": "0.001", "sigma": "10"}},
        {"type": "rf_dielectric", "tag": "d1", "label": "Ox1", "properties": {"thickness": "2[um]", "epsr": "4.1", "tand": "0.002"}},
        {"type": "rf_metal", "tag": "m1", "label": "M1", "properties": {"thickness": "0.5[um]", "sigma": "3.3e7"}},
        {"type": "rf_dielectric", "tag": "d2", "label": "Ox2", "properties": {"thickness": "1[um]", "epsr": "4.2", "tand": "0"}},
        {"type": "rf_via", "tag": "v1", "label": "V1", "properties": {"from": "M1", "to": "M2", "sigma": "2e7"}},
        {"type": "rf_metal", "tag": "m2", "label": "M2", "properties": {"thickness": "2[um]", "sigma": "4e7", "model": "faces", "roughness": "0.3[um]"}},
        {"type": "rf_dielectric", "tag": "d3", "label": "Pass", "properties": {"thickness": "1.5[um]", "epsr": "7", "tand": "0.01"}}%1)")
        .arg(more.isEmpty() ? QString() : QStringLiteral(",\n        ") + more, ground);
}

const StackDrawing::Box* boxNamed(const StackDrawing& d, const QString& name, int column = -1)
{
    for (const StackDrawing::Box& b : d.boxes)
        if (b.name == name && (column < 0 || b.column == column)) return &b;
    return nullptr;
}

const QSizeF kA4 = QPageSize(QPageSize::A4).size(QPageSize::Point);
const QSizeF kLetter = QPageSize(QPageSize::Letter).size(QPageSize::Point);

bool near(double a, double b, double tolerance = 1e-9)
{
    return std::abs(a - b) <= tolerance;
}

/// Everything a sheet puts on its pages, checked: the levels in order, each
/// layer at least a line of small print, each box where its z is, nothing
/// off the page, and nothing said beside a layer outside its rows.
void checkSheet(const StackDrawing& d, const StackSheet& s)
{
    QCOMPARE(s.levels.size(), d.bands.size() + 1);
    QCOMPARE(s.bandRects.size(), d.bands.size());
    for (int i = 1; i < s.levels.size(); ++i) {
        QVERIFY2(s.levels.at(i) > s.levels.at(i - 1), qPrintable(QString::number(i)));
        QVERIFY2(s.levelY.at(i) < s.levelY.at(i - 1), qPrintable(QString::number(i)));
        QVERIFY(near(s.bandRects.at(i - 1).height(), s.levelY.at(i - 1) - s.levelY.at(i), 1e-6));
        QVERIFY2(s.bandRects.at(i - 1).height() >= 3.4, qPrintable(d.bands.at(i - 1).name));
    }
    const QRectF page(QPointF(0, 0), s.size);
    for (int p = 0; p < s.pages.size(); ++p) {
        for (const StackSheet::Text& t : s.pages.at(p).texts) {
            QVERIFY2(page.contains(t.rect), qPrintable(t.text));
            QVERIFY2(t.points >= 3, qPrintable(t.text));
        }
        for (const StackSheet::Shape& sh : s.pages.at(p).shapes) QVERIFY2(page.contains(sh.rect), qPrintable(sh.name));
    }
    // Each box spans its z, and a via stands between the metals it joins.
    int boxes = 0;
    for (const StackSheet::Shape& sh : s.pages.first().shapes) {
        if (sh.kind != StackSheet::Shape::Metal && sh.kind != StackSheet::Shape::Via) continue;
        const StackDrawing::Box& b = d.boxes.at(boxes++);
        QCOMPARE(sh.name, b.name);
        QVERIFY(near(sh.rect.top(), s.yOf(b.z1), 1e-6));
        QVERIFY(near(sh.rect.bottom(), s.yOf(b.z0), 1e-6));
    }
    QCOMPARE(boxes, d.boxes.size());
    // What each layer is, said within its rows (one line of words there).
    for (int i = 0; i < d.bands.size(); ++i) {
        const QString one = d.bands.at(i).words.join(QStringLiteral(", ")), lines = d.bands.at(i).words.join(QLatin1Char('\n'));
        bool found = false;
        for (const StackSheet::Text& t : s.pages.first().texts)
            found = found
                    || ((t.text == one || t.text == lines) && near(t.rect.top(), s.bandRects.at(i).top(), 1e-6)
                        && near(t.rect.height(), s.bandRects.at(i).height(), 1e-6));
        QVERIFY2(found, qPrintable(one));
    }
}

} // namespace

class TestRf25StackDrawing : public QObject
{
    Q_OBJECT

private slots:
    void whatIsDrawn();
    void groundsAndCovers();
    void viasStandInColumns();
    void itIsLaidOutOnThePage();
    void aTallStackPutsItsKeyApart();
    void thePdfIsWrittenAndNotRead();
    void aPageIsPainted();
    void aModelDrawnToLookAt();
};

void TestRf25StackDrawing::whatIsDrawn()
{
    const StackDrawing d = stackDrawingOf(modelOf(twoMetals()), QStringLiteral("two"));
    QVERIFY2(d.ok(), qPrintable(d.problems.join(QLatin1Char(' '))));
    QCOMPARE(d.title, QStringLiteral("The stack of two"));
    // The layers from the bottom up; a metal's room of the dielectric above.
    QStringList bands;
    for (const StackDrawing::Band& b : d.bands) bands << b.name + QStringLiteral(": ") + b.words.join(QStringLiteral(", "));
    QCOMPARE(bands, (QStringList{QStringLiteral("Si: h=300 µm, ε=11.9, tan δ=0.001, σ=10 S/m, ρ=10 Ω-cm"),
                                 QStringLiteral("Ox1: h=2 µm, ε=4.1, tan δ=0.002"),
                                 QStringLiteral("M1: h=0.5 µm, ε=4.2 (Ox2)"),
                                 QStringLiteral("Ox2: h=1 µm, ε=4.2"),
                                 QStringLiteral("M2: h=2 µm, ε=7 (Pass), tan δ=0.01"),
                                 QStringLiteral("Pass: h=1.5 µm, ε=7, tan δ=0.01")}));
    QCOMPARE(d.bands.at(2).kind, StackDrawing::Band::Room);
    for (int i = 1; i < d.bands.size(); ++i) QVERIFY(near(d.bands.at(i).z0, d.bands.at(i - 1).z1));
    // M1, V1 and M2 in one column, the via from M1's top to M2's bottom.
    QCOMPARE(d.columns, 1);
    QCOMPARE(d.boxes.size(), 3);
    const StackDrawing::Box* v1 = boxNamed(d, QStringLiteral("V1"));
    QVERIFY(v1 != nullptr && v1->via);
    QVERIFY(near(v1->z0, 302.5e-6, 1e-12) && near(v1->z1, 303.5e-6, 1e-12));
    QVERIFY(near(boxNamed(d, QStringLiteral("M2"))->z1, 305.5e-6, 1e-12));
    QCOMPARE(d.ground, QStringLiteral("GND: a perfect plane"));
    QCOMPARE(d.top, QStringLiteral("h=∞, ε=1"));
    QVERIFY(!d.lid);
    // The key, from the top: thickness, conductivity, sheet resistance and
    // what else the model says of each.
    QCOMPARE(d.metals, (QStringList{QStringLiteral("M2: t=2 µm, σ=4e+07 S/m, 12.5 mΩ/sq, solved as its two faces, roughness 0.3 µm rms"),
                                    QStringLiteral("M1: t=0.5 µm, σ=3.3e+07 S/m, 60.6 mΩ/sq")}));
    QCOMPARE(d.vias, (QStringList{QStringLiteral("V1: M1 to M2, h=1 µm, σ=2e+07 S/m")}));
}

void TestRf25StackDrawing::groundsAndCovers()
{
    const QString lid = QStringLiteral(R"({"type": "rf_cover", "tag": "c", "label": "Lid", "properties": {"kind": "perfect", "height": "500[um]"}})");
    StackDrawing d = stackDrawingOf(modelOf(twoMetals(lid, QStringLiteral("metal"))));
    QVERIFY(d.ok());
    QCOMPARE(d.ground, QStringLiteral("GND: a metal plane, σ=4.1e+07 S/m"));
    QVERIFY(d.lid);
    QCOMPARE(d.top, QStringLiteral("Lid: a perfect metal lid"));
    QCOMPARE(d.bands.last().kind, StackDrawing::Band::Gap);
    QCOMPARE(d.bands.last().words, (QStringList{QStringLiteral("h=500 µm"), QStringLiteral("ε=1")}));
    StackSheet s = stackSheetOf(d, kA4);
    checkSheet(d, s);
    QVERIFY(std::any_of(s.pages.first().shapes.cbegin(), s.pages.first().shapes.cend(),
                        [](const StackSheet::Shape& sh) { return sh.kind == StackSheet::Shape::Lid; }));
    // Open below: no plane, a dashed level, and said; a via to it left out.
    const QString tsv = QStringLiteral(R"({"type": "rf_via", "tag": "v0", "label": "TSV", "properties": {"from": "GND", "to": "M1", "sigma": "5.8e7"}})");
    d = stackDrawingOf(modelOf(twoMetals(tsv, QStringLiteral("open"))));
    QVERIFY2(d.ok(), qPrintable(d.problems.join(QLatin1Char(' '))));
    QVERIFY(d.ground.isEmpty());
    QVERIFY(boxNamed(d, QStringLiteral("TSV")) == nullptr);
    s = stackSheetOf(d, kA4);
    QVERIFY(s.pages.first().lines.first().dashed);
    QVERIFY(std::none_of(s.pages.first().shapes.cbegin(), s.pages.first().shapes.cend(),
                         [](const StackSheet::Shape& sh) { return sh.kind == StackSheet::Shape::Ground; }));
    QVERIFY(std::any_of(s.pages.first().texts.cbegin(), s.pages.first().texts.cend(),
                        [](const StackSheet::Text& t) { return t.text.startsWith(QLatin1String("No ground plane")); }));
    // An impedance plane, said by its ohms a square.
    const QString impedance = twoMetals({}, QStringLiteral("impedance")).replace(QLatin1String("\"sigma\": \"4.1e7\"}}"),
                                                                                QLatin1String("\"impedance\": \"0.02\"}}"));
    QCOMPARE(stackDrawingOf(modelOf(impedance)).ground, QStringLiteral("GND: a plane of 20 mΩ/sq"));
}

void TestRf25StackDrawing::viasStandInColumns()
{
    // A via from the plane to M1, the two of M1 to M2 overlap: the second
    // goes in a column of its own, with M1 and M2 drawn again there.
    const QString more = QStringLiteral(R"(
        {"type": "rf_via", "tag": "v0", "label": "TSV", "properties": {"from": "GND", "to": "M1", "sigma": "5.8e7"}},
        {"type": "rf_via", "tag": "v2", "label": "V1b", "properties": {"from": "M2", "to": "M1", "sigma": "1e7", "fill": "0.6"}})");
    const StackDrawing d = stackDrawingOf(modelOf(twoMetals(more)));
    QVERIFY2(d.ok(), qPrintable(d.problems.join(QLatin1Char(' '))));
    QCOMPARE(d.columns, 2);
    const StackDrawing::Box* tsv = boxNamed(d, QStringLiteral("TSV"));
    QVERIFY(tsv != nullptr && tsv->column == 0);
    QVERIFY(near(tsv->z0, 0) && near(tsv->z1, 302e-6, 1e-12));
    QCOMPARE(boxNamed(d, QStringLiteral("V1"))->column, 0);
    const StackDrawing::Box* v1b = boxNamed(d, QStringLiteral("V1b"));
    QVERIFY(v1b != nullptr && v1b->column == 1);
    // (Named from M2 to M1: drawn from the lower up all the same.)
    QVERIFY(near(v1b->z0, 302.5e-6, 1e-12) && near(v1b->z1, 303.5e-6, 1e-12));
    for (int c = 0; c < 2; ++c) {
        QVERIFY(boxNamed(d, QStringLiteral("M1"), c) != nullptr);
        QVERIFY(boxNamed(d, QStringLiteral("M2"), c) != nullptr);
    }
    // No two boxes of a column overlap.
    for (const StackDrawing::Box& a : d.boxes)
        for (const StackDrawing::Box& b : d.boxes)
            if (&a != &b && a.column == b.column) QVERIFY2(a.z1 <= b.z0 + 1e-12 || b.z1 <= a.z0 + 1e-12, qPrintable(a.name + b.name));
    QCOMPARE(d.vias, (QStringList{QStringLiteral("V1b: M1 to M2, h=1 µm, σ=1e+07 S/m, 60% filled"),
                                  QStringLiteral("V1: M1 to M2, h=1 µm, σ=2e+07 S/m"),
                                  QStringLiteral("TSV: GND to M1, h=302 µm, σ=5.8e+07 S/m")}));
    // A metal no via joins goes in the first column with room for it.
    const QString lone = QStringLiteral(R"(
        {"type": "rf_metal", "tag": "m3", "label": "M3", "properties": {"thickness": "1[um]", "sigma": "3e7"}},
        {"type": "rf_dielectric", "tag": "d4", "label": "Top", "properties": {"thickness": "1[um]", "epsr": "3"}})");
    const StackDrawing three = stackDrawingOf(modelOf(twoMetals(lone)));
    QVERIFY(three.ok());
    QCOMPARE(boxNamed(three, QStringLiteral("M3"))->column, 0);
    checkSheet(three, stackSheetOf(three, kLetter));
}

void TestRf25StackDrawing::itIsLaidOutOnThePage()
{
    // A layer of 1 nm beside one of 300 µm: still a line of small print.
    const QString thin = QStringLiteral(R"(
        {"type": "rf_dielectric", "tag": "d4", "label": "Cap", "properties": {"thickness": "1[nm]", "epsr": "9"}})");
    const StackDrawing d = stackDrawingOf(modelOf(twoMetals(thin)));
    QVERIFY(d.ok());
    for (const QSizeF& size : {kA4, kLetter}) {
        const StackSheet s = stackSheetOf(d, size, QStringLiteral("Drawn by a test."));
        QCOMPARE(s.pages.size(), 1);
        checkSheet(d, s);
        // Thicker draws taller, the thinnest no thinner than the least.
        QVERIFY(s.bandRects.first().height() > s.bandRects.at(1).height());
        QVERIFY(s.bandRects.at(1).height() > s.bandRects.at(2).height());
        QVERIFY(s.bandRects.last().height() >= 6.99);
        // ...and by the cube root: 300 µm some eight times 0.5 µm, not 600.
        const double ratio = s.bandRects.first().height() / s.bandRects.at(2).height();
        QVERIFY2(ratio > 6 && ratio < 10, qPrintable(QString::number(ratio)));
        // The key under the ground, inside the page; the footer at its foot.
        double keyBottom = 0;
        for (const StackSheet::Text& t : s.pages.first().texts) {
            if (t.text.startsWith(QLatin1String("M1: t="))) {
                QVERIFY(t.rect.top() > s.levelY.first());
                keyBottom = t.rect.bottom();
            }
        }
        QVERIFY(keyBottom > 0);
        QVERIFY(std::any_of(s.pages.first().texts.cbegin(), s.pages.first().texts.cend(),
                            [&](const StackSheet::Text& t) { return t.text == QLatin1String("Drawn by a test.") && t.rect.top() > keyBottom; }));
        // The levels' z where there is room: the plane's always.
        QVERIFY(std::any_of(s.pages.first().texts.cbegin(), s.pages.first().texts.cend(),
                            [&](const StackSheet::Text& t) { return t.text == QLatin1String("0.000"); }));
        QVERIFY(std::any_of(s.pages.first().texts.cbegin(), s.pages.first().texts.cend(),
                            [&](const StackSheet::Text& t) { return t.text == QLatin1String("300.000"); }));
    }
}

void TestRf25StackDrawing::aTallStackPutsItsKeyApart()
{
    // Sixty metals: the key on a second page.
    QStringList layers{QStringLiteral(R"({"type": "rf_ground", "tag": "g", "label": "GND", "properties": {"kind": "perfect"}})"),
                       QStringLiteral(R"({"type": "rf_substrate", "tag": "s", "label": "Si", "properties": {"thickness": "100[um]", "epsr": "11.9"}})")};
    for (int i = 1; i <= 60; ++i) {
        layers << QStringLiteral(R"({"type": "rf_dielectric", "tag": "d%1", "label": "D%1", "properties": {"thickness": "0.3[um]", "epsr": "4"}})").arg(i);
        layers << QStringLiteral(R"({"type": "rf_metal", "tag": "m%1", "label": "M%1", "properties": {"thickness": "0.2[um]", "sigma": "3e7"}})").arg(i);
        if (i > 1) layers << QStringLiteral(R"({"type": "rf_via", "tag": "v%1", "label": "V%1", "properties": {"from": "M%2", "to": "M%1", "sigma": "2e7"}})").arg(i).arg(i - 1);
    }
    layers << QStringLiteral(R"({"type": "rf_dielectric", "tag": "top", "label": "Top", "properties": {"thickness": "1[um]", "epsr": "3"}})");
    const StackDrawing d = stackDrawingOf(modelOf(layers.join(QStringLiteral(",\n"))));
    QVERIFY2(d.ok(), qPrintable(d.problems.join(QLatin1Char(' '))));
    QCOMPARE(d.columns, 1);
    const StackSheet s = stackSheetOf(d, kA4, QStringLiteral("Foot."));
    QCOMPARE(s.pages.size(), 2);
    checkSheet(d, s);
    QVERIFY(std::any_of(s.pages.at(1).texts.cbegin(), s.pages.at(1).texts.cend(),
                        [](const StackSheet::Text& t) { return t.text.startsWith(QLatin1String("M60: t=")); }));
    QVERIFY(std::any_of(s.pages.at(1).texts.cbegin(), s.pages.at(1).texts.cend(),
                        [](const StackSheet::Text& t) { return t.text == QLatin1String("Foot. Page 2 of 2."); }));
    QVERIFY(std::none_of(s.pages.first().texts.cbegin(), s.pages.first().texts.cend(),
                         [](const StackSheet::Text& t) { return t.text.startsWith(QLatin1String("M60: t=")); }));
}

void TestRf25StackDrawing::thePdfIsWrittenAndNotRead()
{
    QCOMPARE(stackFormatOf(QStringLiteral("a/b.PDF")), std::optional<StackFormat>(StackFormat::Pdf));
    const QString lid = QStringLiteral(R"({"type": "rf_cover", "tag": "c", "label": "Lid", "properties": {"kind": "perfect", "height": "500[um]"}})");
    const StackWritten w = stackPdfOf(modelOf(twoMetals(lid)), QStringLiteral("two"), QStringLiteral("a test"));
    QVERIFY2(w.ok(), qPrintable(w.problems.join(QLatin1Char(' '))));
    QVERIFY(w.data.startsWith("%PDF-"));
    QVERIFY(w.data.trimmed().endsWith("%%EOF"));
    QCOMPARE(w.bytes(), w.data);
    QCOMPARE(w.pages, 1);
    QCOMPARE(w.layers, 7);
    QCOMPARE(w.vias, 1);
    QVERIFY(w.data.count("/Type /Page\n") + w.data.count("/Type /Page ") + w.data.count("/Type /Page\r") + w.data.count("/Type /Page/") >= 1
            || w.data.contains("/Type /Page"));
    QVERIFY2(w.notes.join(QLatin1Char(' ')).contains(QLatin1String("does not solve a metal lid")), qPrintable(w.notes.join(QLatin1Char(' '))));
    // A stack with problems is not drawn: why is said.
    const StackWritten none = stackPdfOf(modelOf(QStringLiteral(R"({"type": "rf_substrate", "tag": "s", "label": "Si", "properties": {"thickness": "100[um]", "epsr": "11.9"}})")));
    QVERIFY(!none.ok());
    QVERIFY(none.data.isEmpty());
    QVERIFY(none.problems.join(QLatin1Char(' ')).contains(QLatin1String("no metal")));
    // Never read back, by its suffix or by what it begins with.
    QTemporaryDir dir;
    for (const QString& name : {QStringLiteral("drawn.pdf"), QStringLiteral("drawn")}) {
        QFile f(dir.filePath(name));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(w.data);
        f.close();
        QString why;
        QVERIFY(!stackFileRead(f.fileName(), &why));
        QVERIFY2(why.contains(QLatin1String("drawing of a stack")), qPrintable(why));
    }
}

void TestRf25StackDrawing::aPageIsPainted()
{
    const StackDrawing d = stackDrawingOf(modelOf(twoMetals()));
    const StackSheet s = stackSheetOf(d, kA4);
    QImage image(int(kA4.width() * 2), int(kA4.height() * 2), QImage::Format_RGB32);
    image.fill(Qt::white);
    image.setDotsPerMeterX(2835);   // (72 an inch, the PDF's: a point a pixel before the scale)
    image.setDotsPerMeterY(2835);
    {
        QPainter p(&image);
        p.scale(2, 2);
        paintStackPage(p, s.pages.first());
    }
    // The plane dark under the substrate, M2 grey at its middle, the paper
    // white beside the layers where nothing is drawn.
    const auto at = [&](double x, double y) { return QColor(image.pixel(int(x * 2), int(y * 2))); };
    QVERIFY(at(kA4.width() / 2, s.levelY.first() + 2).lightness() < 100);
    for (const StackSheet::Shape& sh : s.pages.first().shapes) {
        if (sh.name != QLatin1String("M2")) continue;
        const QColor c = at(sh.rect.left() + 2, sh.rect.center().y());
        QVERIFY2(c.lightness() > 200 && c.lightness() < 240, qPrintable(c.name()));
    }
    const QRectF ox1 = s.bandRects.at(1);
    QCOMPARE(at(ox1.left() + 3, ox1.center().y()), QColor(Qt::white));
}

void TestRf25StackDrawing::aModelDrawnToLookAt()
{
    const QString of = qEnvironmentVariable("QUCS_STACK_DRAWING_OF"), to = qEnvironmentVariable("QUCS_STACK_DRAWING_TO");
    if (of.isEmpty() || to.isEmpty()) QSKIP("QUCS_STACK_DRAWING_OF and QUCS_STACK_DRAWING_TO name no model and folder");
    QFile f(of);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QString why;
    const std::optional<Model> m = Model::fromJson(f.readAll(), &why);
    QVERIFY2(m, qPrintable(why));
    const QString base = QDir(to).filePath(QFileInfo(of).completeBaseName());
    const StackWritten w = stackPdfOf(*m, QFileInfo(of).completeBaseName(), QStringLiteral("test_rf25_stack_drawing"));
    QVERIFY2(w.ok(), qPrintable(w.problems.join(QLatin1Char(' '))));
    QFile pdf(base + QStringLiteral(".pdf"));
    QVERIFY(pdf.open(QIODevice::WriteOnly));
    pdf.write(w.data);
    const StackDrawing d = stackDrawingOf(*m, QFileInfo(of).completeBaseName());
    const StackSheet s = stackSheetOf(d, kA4, QStringLiteral("Drawn by test_rf25_stack_drawing."));
    checkSheet(d, s);
    for (int i = 0; i < s.pages.size(); ++i) {
        QImage image(int(kA4.width() * 2), int(kA4.height() * 2), QImage::Format_RGB32);
        image.fill(Qt::white);
        image.setDotsPerMeterX(2835);   // (72 an inch, the PDF's: a point a pixel before the scale)
        image.setDotsPerMeterY(2835);
        QPainter p(&image);
        p.scale(2, 2);
        paintStackPage(p, s.pages.at(i));
        p.end();
        QVERIFY(image.save(QStringLiteral("%1_%2.png").arg(base).arg(i + 1)));
    }
}

QTEST_MAIN(TestRf25StackDrawing)
#include "test_rf25_stack_drawing.moc"

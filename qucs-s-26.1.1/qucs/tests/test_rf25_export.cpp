/***************************************************************************
                            test_rf25_export.cpp
                           ----------------------
 *  An RF 2.5D model's shapes and stack written out (rf25_export.h): the
 *  GDSII file read back by the layout reader, the DXF drawing by the
 *  drawing importer, the 3D model built by Multiphysics 3D's geometry with
 *  every material on its solids.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 ***************************************************************************/

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "multiphysics/fem3_geometry.h"
#include "multiphysics/fem_import.h"
#include "multiphysics/fem_model.h"
#include "rf25/rf25_export.h"
#include "rf25/rf25_layout.h"
#include "rf25/rf25_model.h"

namespace rf = qucs_s::rf25;
namespace fem = qucs_s::fem;

namespace {

QJsonArray rowsOf(const QList<QStringList>& rows)
{
    QJsonArray out;
    for (const QStringList& r : rows) out.append(QJsonArray::fromStringList(r));
    return out;
}

/// The blank model's stack (a ground, a substrate, ILD1, M1) with ILD2, a
/// via V1 from M1 to M2, M2 and a cover of air; the layer map sending 31/0
/// and 7/2 to M1 and nothing to the others.
rf::Model stackedModel()
{
    rf::Model m = rf::Model::blank();
    rf::Node ild2 = m.make(QStringLiteral("rf_dielectric"));
    ild2.label = QStringLiteral("ILD2");
    ild2.props.insert(QStringLiteral("thickness"), QStringLiteral("2[um]"));
    ild2.props.insert(QStringLiteral("epsr"), QStringLiteral("4.1"));
    ild2.props.insert(QStringLiteral("tand"), QStringLiteral("0.002"));
    rf::Node via = m.make(QStringLiteral("rf_via"));
    via.label = QStringLiteral("V1");
    via.props.insert(QStringLiteral("from"), QStringLiteral("M1"));
    via.props.insert(QStringLiteral("to"), QStringLiteral("M2"));
    rf::Node m2 = m.make(QStringLiteral("rf_metal"));
    m2.label = QStringLiteral("M2");
    m2.props.insert(QStringLiteral("thickness"), QStringLiteral("0.9[um]"));
    rf::Node top = m.make(QStringLiteral("rf_dielectric"));
    top.label = QStringLiteral("Passivation");
    top.props.insert(QStringLiteral("thickness"), QStringLiteral("1[um]"));
    rf::Node cover = m.make(QStringLiteral("rf_cover"));
    cover.label = QStringLiteral("Air");
    cover.props.insert(QStringLiteral("height"), QStringLiteral("50[um]"));
    for (const rf::Node& n : {ild2, via, m2, top, cover}) m.add(QStringLiteral("stack"), n);
    m.layout().child(QStringLiteral("rf_layer_map"))
        ->props.insert(QStringLiteral("rows"), rowsOf({{QStringLiteral("31"), QStringLiteral("0"), QStringLiteral("M1")},
                                                       {QStringLiteral("7"), QStringLiteral("2"), QStringLiteral("M1")}}));
    return m;
}

QPolygonF box(double x0, double y0, double x1, double y1)
{
    return QPolygonF(QRectF(QPointF(x0, y0), QPointF(x1, y1)));
}

/// Shapes as the processing leaves them: two on M1, one each on M2 and V1.
rf::Processed processedShapes()
{
    rf::Processed p;
    p.conductors << rf::Conductor{QStringLiteral("M1"), box(0, 0, 200, 20), false}
                 << rf::Conductor{QStringLiteral("M1"), box(0, 40, 200, 60), false}
                 << rf::Conductor{QStringLiteral("V1"), box(95, 5, 100, 10), false}
                 << rf::Conductor{QStringLiteral("M2"), box(50, 0, 150, 20), false};
    return p;
}

} // namespace

class TestRf25Export : public QObject
{
    Q_OBJECT

private slots:
    // Each layer of the stack on the lowest of the file's layers the map
    // sends there; one it sends none the next number free, said.
    void theShapesTakeTheMapsNumbers()
    {
        const rf::Model m = stackedModel();
        QVERIFY2(m.stack().ok(), qPrintable(m.stack().problems.join(' ')));
        const rf::LayoutExport e = rf::layoutExportOf(m, processedShapes());
        QVERIFY2(e.ok(), qPrintable(e.problems.join(' ')));
        QCOMPARE(e.layers, (QStringList{QStringLiteral("M1"), QStringLiteral("V1"), QStringLiteral("M2")}));
        QCOMPARE(e.numbers.value(QStringLiteral("M1")), std::make_pair(7, 2));
        QCOMPARE(e.numbers.value(QStringLiteral("V1")), std::make_pair(32, 0));
        QCOMPARE(e.numbers.value(QStringLiteral("M2")), std::make_pair(33, 0));
        QCOMPARE(int(e.shapes.size()), 4);
        QCOMPARE(e.bounds(), QRectF(0, 0, 200, 60));
        QVERIFY2(e.notes.join(' ').contains(QStringLiteral("V1 on 32/0")), qPrintable(e.notes.join(' ')));
        // Nothing on the stack: nothing to write, and said.
        const rf::LayoutExport none = rf::layoutExportOf(m, rf::Processed());
        QVERIFY(!none.ok());
    }

    void theGdsIsReadBackOnItsLayers()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const rf::LayoutExport e = rf::layoutExportOf(stackedModel(), processedShapes());
        const QString path = dir.filePath(QStringLiteral("shapes.gds"));
        QString why;
        QVERIFY2(rf::writeLayoutGds(e, path, QStringLiteral("TOP"), &why), qPrintable(why));
        const fem::LayoutShapes read = fem::readLayoutShapes(path);
        QVERIFY2(read.error.isEmpty(), qPrintable(read.error));
        QCOMPARE(read.cell, QStringLiteral("TOP"));
        const auto loops = [&read](int layer, int datatype) {
            const fem::LayoutShapes::Layer* l = read.layer(fem::LayoutLayer{quint32(layer), quint32(datatype)});
            return l != nullptr ? int(l->loops.size()) : 0;
        };
        QCOMPARE(loops(7, 2), 2);
        QCOMPARE(loops(32, 0), 1);
        QCOMPARE(loops(33, 0), 1);
        QCOMPARE(loops(31, 0), 0);
        // In µm, as written: round every loop read.
        double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
        for (const fem::LayoutShapes::Layer& l : read.layers)
            for (const auto& loop : l.loops)
                for (const QPointF& p : loop) {
                    x0 = std::min(x0, p.x());
                    y0 = std::min(y0, p.y());
                    x1 = std::max(x1, p.x());
                    y1 = std::max(y1, p.y());
                }
        QVERIFY2(std::abs(x0) < 1e-6 && std::abs(y0) < 1e-6 && std::abs(x1 - 200) < 1e-6 && std::abs(y1 - 60) < 1e-6,
                 qPrintable(QStringLiteral("%1, %2 to %3, %4").arg(x0).arg(y0).arg(x1).arg(y1)));
    }

    void theDxfIsReadBackLayerByLayerInMicrometres()
    {
        const rf::LayoutExport e = rf::layoutExportOf(stackedModel(), processedShapes());
        const QByteArray dxf = rf::dxfOf(e, {{QStringLiteral("M1"), QColor(255, 0, 0)}}, QStringLiteral("a test"));
        QVERIFY(dxf.startsWith("999\na test\n"));
        QVERIFY(dxf.contains("AC1009"));
        QVERIFY(dxf.trimmed().endsWith("EOF"));
        const fem::ImportedDrawing all = fem::readDxf(dxf);
        QVERIFY2(all.error.isEmpty(), qPrintable(all.error));
        QCOMPARE(all.unit, 1e-6);
        QCOMPARE(int(all.paths.size()), 4);
        for (const fem::ImportedPath& p : all.paths) QVERIFY(p.closed);
        QCOMPARE(int(fem::readDxf(dxf, {QStringLiteral("M1")}).paths.size()), 2);
        QCOMPARE(int(fem::readDxf(dxf, {QStringLiteral("M2")}).paths.size()), 1);
        // A layer coloured as the tab draws it: red, AutoCAD's 1.
        QVERIFY(dxf.contains("2\nM1\n70\n0\n62\n1\n"));
        // A name the DXF cannot take, made one it can.
        QCOMPARE(rf::dxfLayerName(QStringLiteral("Oxide (top)")), QStringLiteral("Oxide__top_"));
    }

    // The 3D model built by Multiphysics 3D's own geometry: every row a
    // solid, every material on solids of its own, a metal's room filled with
    // the dielectric over it, the air framed and a hidden layer hidden.
    void theThreeDModelIsBuiltWithItsMaterials()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        rf::Model m = stackedModel();
        // M2 hidden in the RF 2.5D tab: hidden there too.
        for (rf::Node& n : m.stackNode().children)
            if (rf::nameOf(n) == QLatin1String("M2")) n.visibility = QStringLiteral("hidden");
        const rf::LayoutExport e = rf::layoutExportOf(m, processedShapes());
        QString why;
        QVERIFY2(rf::writeLayoutGds(e, dir.filePath(QStringLiteral("model.gds")), QStringLiteral("TOP"), &why), qPrintable(why));
        const rf::Mp3dExport three = rf::mp3dOf(m, e, QStringLiteral("model.gds"), QStringLiteral("model"), 5e-6);
        QVERIFY2(three.ok(), qPrintable(three.problems.join(' ')));
        const QString path = dir.filePath(QStringLiteral("model.qfem3d"));
        {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(three.json);
        }
        std::optional<fem::Model> model = fem::Model::fromJson(three.json, &why);
        QVERIFY2(model.has_value(), qPrintable(why));
        model->fileName = path;
        const std::shared_ptr<fem::ParameterScope> ps = fem::evaluateParameters(*model);
        const fem::GeometryBuild3 g = fem::buildGeometry3(*model, *ps);
        QVERIFY2(g.ok(), qPrintable(g.errors.values().join(' ') + g.failedAt));
        QVERIFY2(g.warnings.isEmpty(), qPrintable(QStringList(g.warnings.values()).join(' ')));
        const fem::Topology3& t = *g.topology;
        // The window: the shapes and 5 µm round them; the stack from the
        // ground under nought to the air's top.
        QVERIFY2(std::abs(t.bounds.x0 + 5) < 1e-6 && std::abs(t.bounds.x1 - 205) < 1e-6 && std::abs(t.bounds.y1 - 65) < 1e-6,
                 qPrintable(QStringLiteral("%1 %2 %3").arg(t.bounds.x0).arg(t.bounds.x1).arg(t.bounds.y1)));
        const rf::Stack stack = m.stack();
        QVERIFY(t.bounds.z0 < 0);
        QVERIFY2(std::abs(t.bounds.z1 - stack.height() * 1e6) < 1e-6, qPrintable(QString::number(t.bounds.z1)));
        // Each material on solids, none on nothing.
        const fem::Node* materials = nullptr;
        for (const fem::Node& c : model->component().children)
            if (c.type == QLatin1String("materials")) materials = &c;
        QVERIFY(materials != nullptr);
        QStringList labels;
        for (const fem::Node& mat : materials->children) {
            labels << mat.label;
            QString error;
            const QVector<int> on = fem::resolveSelection3(t, *model, fem::Level::Domain, mat.props.value(QStringLiteral("selection")).toObject(), &error);
            QVERIFY2(error.isEmpty(), qPrintable(mat.label + QStringLiteral(": ") + error));
            QVERIFY2(!on.isEmpty(), qPrintable(mat.label));
        }
        // Each domain under one material alone: a metal's is not its room's
        // too, so that what a dielectric's visibility does stops at it.
        {
            QHash<int, QString> taken;
            for (const fem::Node& mat : materials->children)
                for (const int d : fem::resolveSelection3(t, *model, fem::Level::Domain, mat.props.value(QStringLiteral("selection")).toObject())) {
                    QVERIFY2(!taken.contains(d), qPrintable(QStringLiteral("domain %1: %2 and %3").arg(d).arg(taken.value(d), mat.label)));
                    taken.insert(d, mat.label);
                }
            QCOMPARE(int(taken.size()), int(t.domains.size()));
        }
        for (const char* name : {"Ground", "Substrate", "ILD1", "ILD2", "Passivation", "Air", "M1", "V1", "M2"})
            QVERIFY2(labels.contains(QLatin1String(name)), qPrintable(labels.join(", ")));
        // M1's room filled with ILD2 (the dielectric over it), whose
        // permittivity and loss tangent it has.
        for (const fem::Node& mat : materials->children) {
            const QJsonArray rows = mat.props.value(QStringLiteral("selection")).toObject().value(QStringLiteral("only")).toArray();
            if (mat.label == QLatin1String("ILD2")) {
                QVERIFY(rows.contains(QStringLiteral("M1 fill")));
                QCOMPARE(mat.props.value(QStringLiteral("epsilonr")).toString(), QStringLiteral("4.1"));
                QCOMPARE(mat.props.value(QStringLiteral("tand")).toString(), QStringLiteral("0.002"));
            }
            if (mat.label == QLatin1String("Air")) QCOMPARE(mat.visibility, QStringLiteral("frame"));
            if (mat.label == QLatin1String("M2")) QCOMPARE(mat.visibility, QStringLiteral("hidden"));
            if (mat.label == QLatin1String("M1")) {
                QVERIFY(mat.visibility.isEmpty());
                QVERIFY(mat.props.value(QStringLiteral("sigma")).toString().endsWith(QStringLiteral("[S/m]")));
            }
        }
        // The via through the room between M1 and M2 alone: its top M2's
        // bottom, its bottom M1's top.
        const rf::Layer* m1 = stack.named(QStringLiteral("M1"));
        const rf::Layer* m2 = stack.named(QStringLiteral("M2"));
        const QJsonArray rows = model->geometry().children.front().props.value(QStringLiteral("stack")).toArray();
        bool viaSeen = false;
        for (const QJsonValue& r : rows) {
            const QJsonArray row = r.toArray();
            if (row.at(0).toString() != QLatin1String("V1")) continue;
            viaSeen = true;
            QCOMPARE(row.at(1).toString(), QStringLiteral("32/0"));
            QVERIFY(std::abs(row.at(2).toString().toDouble() - m1->z1 * 1e6) < 1e-9);
            QVERIFY(std::abs(row.at(3).toString().toDouble() - (m2->z0 - m1->z1) * 1e6) < 1e-9);
        }
        QVERIFY(viaSeen);
        // A perfect ground written with its stand-in conductivity, said.
        QVERIFY2(three.notes.join(' ').contains(QStringLiteral("Ground perfect")), qPrintable(three.notes.join(' ')));
    }

    // A lid's side walls give the window; a margin of nothing, the shapes'
    // own box; a stack that cannot be read, nothing.
    void theWindowIsTheWallsOrTheShapes()
    {
        rf::Model m = stackedModel();
        const rf::LayoutExport e = rf::layoutExportOf(m, processedShapes());
        const auto windowOf = [](const rf::Mp3dExport& x) {
            const QJsonObject stack = QJsonDocument::fromJson(x.json)
                                          .object()
                                          .value(QStringLiteral("components"))
                                          .toArray()
                                          .first()
                                          .toObject()
                                          .value(QStringLiteral("geometry"))
                                          .toArray()
                                          .first()
                                          .toObject();
            const QJsonArray a = stack.value(QStringLiteral("from")).toArray(), b = stack.value(QStringLiteral("to")).toArray();
            return QRectF(QPointF(a.at(0).toString().toDouble(), a.at(1).toString().toDouble()),
                          QPointF(b.at(0).toString().toDouble(), b.at(1).toString().toDouble()));
        };
        QCOMPARE(windowOf(rf::mp3dOf(m, e, QStringLiteral("x.gds"), QStringLiteral("x"), 0)), QRectF(0, 0, 200, 60));
        // (A tenth of the larger side when none is given.)
        QCOMPARE(windowOf(rf::mp3dOf(m, e, QStringLiteral("x.gds"), QStringLiteral("x"))), QRectF(-20, -20, 240, 100));
        for (rf::Node& n : m.stackNode().children)
            if (n.type == QLatin1String("rf_cover")) {
                n.props.insert(QStringLiteral("kind"), QStringLiteral("perfect"));
                n.props.insert(QStringLiteral("walls"), true);
                n.props.insert(QStringLiteral("box_x0"), QStringLiteral("-100[um]"));
                n.props.insert(QStringLiteral("box_y0"), QStringLiteral("-50[um]"));
                n.props.insert(QStringLiteral("box_x1"), QStringLiteral("300[um]"));
                n.props.insert(QStringLiteral("box_y1"), QStringLiteral("150[um]"));
            }
        const rf::Stack walled = m.stack();
        QVERIFY2(walled.ok(), qPrintable(walled.problems.join(' ')));
        bool hasWalls = false;
        for (const rf::Layer& l : walled.layers) hasWalls = hasWalls || (l.kind == rf::Layer::Cover && l.walls);
        QVERIFY(hasWalls);
        const rf::Mp3dExport x = rf::mp3dOf(m, e, QStringLiteral("x.gds"), QStringLiteral("x"));
        QVERIFY2(x.ok(), qPrintable(x.problems.join(' ')));
        QCOMPARE(windowOf(x), QRectF(QPointF(-100, -50), QPointF(300, 150)));
        QVERIFY(x.json.contains("Air lid"));
        QVERIFY2(x.notes.join(' ').contains(QStringLiteral("side walls")), qPrintable(x.notes.join(' ')));
        rf::Model broken = stackedModel();
        for (rf::Node& n : broken.stackNode().children)
            if (n.type == QLatin1String("rf_substrate")) n.props.insert(QStringLiteral("thickness"), QStringLiteral("0"));
        QVERIFY(!rf::mp3dOf(broken, e, QStringLiteral("x.gds"), QStringLiteral("x")).ok());
    }

    // Hidden is no part of what is solved: the model with nothing hidden is
    // written as before, and one with a layer hidden reads back so.
    void aLayersVisibilityIsKeptInTheFile()
    {
        rf::Model m = stackedModel();
        QVERIFY(!m.toJson().contains("visibility"));
        rf::Node* m1 = m.find(QStringLiteral("m1"));
        QVERIFY(m1 != nullptr && rf::canHide(*m1));
        QVERIFY(!rf::canHide(m.layout()));
        m1->visibility = QStringLiteral("frame");
        const QByteArray json = m.toJson();
        QVERIFY(json.contains("\"visibility\": \"frame\""));
        QString why;
        std::optional<rf::Model> read = rf::Model::fromJson(json, &why);
        QVERIFY2(read.has_value(), qPrintable(why));
        QCOMPARE(read->find(QStringLiteral("m1"))->visibility, QStringLiteral("frame"));
        QCOMPARE(rf::layerVisibility(*read).value(QStringLiteral("M1")), QStringLiteral("frame"));
        QVERIFY2(read->problems().filter(QStringLiteral("visibility")).isEmpty(), qPrintable(read->problems().join(' ')));
        QCOMPARE(rf::withoutVisibility(*read).toObject(), stackedModel().toObject());
    }
};

QTEST_GUILESS_MAIN(TestRf25Export)
#include "test_rf25_export.moc"

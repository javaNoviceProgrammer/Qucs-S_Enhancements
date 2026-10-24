/*
 * An RF 2.5D model (rf25/rf25_model.h), phase A of
 * docs/proposals/2026-10-23-rf-2p5d-moment-method.md: the tree and its
 * kinds, the file it is written to and read from, the stack read out of it
 * in SI, and everything the stack and the model say is wrong with them.
 * There is no solver yet, and nothing here asks for one.
 */
#include <QtTest>

#include "rf25/rf25_model.h"

namespace rf = qucs_s::rf25;

namespace {

/// A node of \a type under \a parent, with a tag and a label.
rf::Node& add(rf::Node& parent, const char* type, const QString& tag, const QString& label = {})
{
    rf::Node n;
    n.type = QLatin1String(type);
    n.tag = tag;
    n.label = label;
    parent.children.push_back(n);
    return parent.children.back();
}

void set(rf::Node& node, const char* key, const QString& value)
{
    node.props.insert(QLatin1String(key), value);
}

/// A stack of a substrate, an oxide and two metals with a via between them -
/// the shape of a real one, and the base for the checks below.
rf::Model twoMetals()
{
    rf::Model m;
    for (const char* type : {"rf_parameters", "rf_stack", "rf_layout", "rf_ports", "rf_mesh", "rf_study"})
        add(m.root, type, QString());
    rf::Node& stack = m.stackNode();
    rf::Node& substrate = add(stack, "rf_substrate", QStringLiteral("sub1"), QStringLiteral("Si"));
    set(substrate, "thickness", QStringLiteral("300[um]"));
    set(substrate, "epsr", QStringLiteral("11.9"));
    set(substrate, "sigma", QStringLiteral("10"));
    rf::Node& ild = add(stack, "rf_dielectric", QStringLiteral("d1"), QStringLiteral("ILD1"));
    set(ild, "thickness", QStringLiteral("1[um]"));
    set(ild, "epsr", QStringLiteral("4.1"));
    rf::Node& m1 = add(stack, "rf_metal", QStringLiteral("m1"), QStringLiteral("M1"));
    set(m1, "thickness", QStringLiteral("0.6[um]"));
    set(m1, "sigma", QStringLiteral("3.3e7"));
    rf::Node& ild2 = add(stack, "rf_dielectric", QStringLiteral("d2"), QStringLiteral("ILD2"));
    set(ild2, "thickness", QStringLiteral("2[um]"));
    set(ild2, "epsr", QStringLiteral("4.1"));
    rf::Node& via = add(stack, "rf_via", QStringLiteral("v1"), QStringLiteral("V1"));
    set(via, "from", QStringLiteral("M1"));
    set(via, "to", QStringLiteral("M2"));
    set(via, "sigma", QStringLiteral("2.1e7"));
    rf::Node& m2 = add(stack, "rf_metal", QStringLiteral("m2"), QStringLiteral("M2"));
    set(m2, "thickness", QStringLiteral("0.9[um]"));
    set(m2, "sigma", QStringLiteral("3.3e7"));
    return m;
}

bool says(const QStringList& problems, const char* words)
{
    for (const QString& p : problems)
        if (p.contains(QLatin1String(words), Qt::CaseInsensitive)) return true;
    return false;
}

} // namespace

class TestRf25Model : public QObject
{
    Q_OBJECT

private slots:
    // Every kind of the tree is in the table, each with its own type, and
    // the multiphysics table has none of them (nor this one any of its).
    void theKindsAreItsOwn()
    {
        const QList<const rf::NodeKind*> kinds = rf::nodeKinds();
        QVERIFY2(kinds.size() >= 18, qPrintable(QString::number(kinds.size())));
        QSet<QString> types;
        for (const rf::NodeKind* k : kinds) {
            QVERIFY2(k->type.startsWith(QLatin1String("rf_")), qPrintable(k->type));
            QVERIFY2(!types.contains(k->type), qPrintable(k->type));
            types.insert(k->type);
            QVERIFY2(!k->title.isEmpty(), qPrintable(k->type));
            // Every child it offers is a kind of this table.
            for (const QString& child : k->children)
                QVERIFY2(rf::nodeKind(child) != nullptr, qPrintable(k->type + QStringLiteral(" -> ") + child));
            // Every property has a key and a label.
            for (const rf::PropertyDef& p : k->properties) {
                QVERIFY2(!p.key.isEmpty(), qPrintable(k->type));
                QVERIFY2(!p.label.isEmpty(), qPrintable(k->type + QStringLiteral(".") + p.key));
            }
        }
        // The two tables do not reach into each other.
        QVERIFY(rf::nodeKind(QStringLiteral("rectangle")) == nullptr);
        QVERIFY(qucs_s::fem::nodeKind(QStringLiteral("rf_metal")) == nullptr);
        QVERIFY(rf::nodeKind(QStringLiteral("rf_metal")) != nullptr);
    }

    // A new model has the parts of the tree and a stack one can look at,
    // and it goes to JSON and back as it was.
    void aBlankModelIsWrittenAndRead()
    {
        const rf::Model blank = rf::Model::blank();
        for (const char* type : {"rf_parameters", "rf_stack", "rf_layout", "rf_ports", "rf_mesh", "rf_study",
                                 "rf_results", "rf_exports"})
            QVERIFY2(blank.root.child(QLatin1String(type)) != nullptr, type);
        QVERIFY(blank.layout().child(QStringLiteral("rf_layer_map")) != nullptr);
        QVERIFY(blank.layout().child(QStringLiteral("rf_processing")) != nullptr);
        QCOMPARE(blank.stackNode().children.size(), std::size_t(3));

        const QByteArray json = blank.toJson();
        QVERIFY2(json.contains("\"qucs-s rf 2.5d 1\""), json.left(120).constData());
        QString why;
        const std::optional<rf::Model> back = rf::Model::fromJson(json, &why);
        QVERIFY2(back.has_value(), qPrintable(why));
        QCOMPARE(back->toJson(), json);

        // What is not an RF 2.5D model is refused, and says why.
        QVERIFY(!rf::Model::fromJson(QByteArray("{"), &why).has_value());
        QVERIFY2(why.contains(QLatin1String("JSON")), qPrintable(why));
        QVERIFY(!rf::Model::fromJson(QByteArray("{\"format\": \"qucs-s multiphysics 1\"}"), &why).has_value());
        QVERIFY2(why.contains(QLatin1String("rf 2.5d")), qPrintable(why));
        QVERIFY(!rf::Model::fromJson(QByteArray("{\"format\": \"qucs-s rf 2.5d 1\", \"tree\": {\"type\": \"model\"}}"), &why)
                     .has_value());
        QVERIFY2(why.contains(QLatin1String("not rooted")), qPrintable(why));
    }

    // Every node can be found by tag: the parts of the tree itself are
    // named by their kind ("stack", "layer_map"), a file written before
    // they were gets them on read, and a tag two nodes share - a file
    // written by hand, or two stacks copied together - is made its own,
    // since find() would otherwise answer with the wrong node.
    void everyNodeHasATagOfItsOwn()
    {
        const rf::Model blank = rf::Model::blank();
        QCOMPARE(blank.root.tag, QStringLiteral("model"));
        QCOMPARE(blank.stackNode().tag, QStringLiteral("stack"));
        QCOMPARE(blank.layout().tag, QStringLiteral("layout"));
        QCOMPARE(blank.layout().child(QStringLiteral("rf_layer_map"))->tag, QStringLiteral("layer_map"));
        QVERIFY(blank.find(QStringLiteral("stack")) != nullptr);

        // Two metals of one tag, as a hand-written file may have them.
        const QByteArray json =
            "{\"format\": \"qucs-s rf 2.5d 1\", \"tree\": {\"type\": \"rf_model\", \"children\": ["
            "{\"type\": \"rf_stack\", \"children\": ["
            "{\"type\": \"rf_metal\", \"tag\": \"m1\", \"label\": \"M1\"},"
            "{\"type\": \"rf_metal\", \"tag\": \"m1\", \"label\": \"M2\"}]}]}}";
        QString why;
        const std::optional<rf::Model> m = rf::Model::fromJson(json, &why);
        QVERIFY2(m.has_value(), qPrintable(why));
        QCOMPARE(m->stackNode().children.size(), std::size_t(2));
        const QString first = m->stackNode().children[0].tag, second = m->stackNode().children[1].tag;
        QCOMPARE(first, QStringLiteral("m1"));
        QVERIFY2(second != first, qPrintable(second));
        QVERIFY(!second.isEmpty());
        QCOMPARE(m->find(first)->label, QStringLiteral("M1"));
        QCOMPARE(m->find(second)->label, QStringLiteral("M2"));
    }

    // The stack read out: SI throughout, the layers placed from the bottom
    // up, a via taking no room of its own, and the parameters usable in
    // any of it.
    void theStackIsReadInSi()
    {
        rf::Model m = twoMetals();
        QJsonArray parameters;   // (one row: by append, never a braced copy)
        parameters.append(QJsonArray{QStringLiteral("t_top"), QStringLiteral("0.9[um]")});
        QCOMPARE(parameters.size(), 1);
        QVERIFY(parameters.first().isArray());
        m.root.child(QStringLiteral("rf_parameters"))->props.insert(QStringLiteral("rows"), parameters);
        m.stackNode().children.back().props.insert(QStringLiteral("thickness"), QStringLiteral("t_top"));

        const rf::Stack s = m.stack();
        QVERIFY2(s.problems.isEmpty(), qPrintable(s.problems.join(QStringLiteral(" | "))));
        QCOMPARE(s.layers.size(), 6);
        QCOMPARE(s.layers.at(0).kind, rf::Layer::Substrate);
        QCOMPARE(s.layers.at(0).thickness, 300e-6);
        QCOMPARE(s.layers.at(0).epsr, 11.9);
        QCOMPARE(s.layers.at(0).sigma, 10.0);
        QCOMPARE(s.layers.at(2).name, QStringLiteral("M1"));
        QCOMPARE(s.layers.at(2).thickness, 0.6e-6);
        // The parameter reached the top metal.
        QCOMPARE(s.layers.at(5).thickness, 0.9e-6);
        // From the bottom up, each on the one below it.
        QCOMPARE(s.layers.at(0).z0, 0.0);
        QCOMPARE(s.layers.at(1).z0, 300e-6);
        QCOMPARE(s.layers.at(2).z0, 301e-6);
        QCOMPARE(s.layers.at(2).z1, 301.6e-6);
        // The via sits where the dielectric it goes through is, and adds
        // nothing to the height.
        QCOMPARE(s.layers.at(4).kind, rf::Layer::Via);
        QCOMPARE(s.layers.at(4).z0, s.layers.at(4).z1);
        QCOMPARE(s.height(), s.layers.at(5).z1);
        QCOMPARE(s.conductors(), (QStringList{QStringLiteral("M1"), QStringLiteral("M2")}));
        QVERIFY(s.named(QStringLiteral("M2")) != nullptr);
        QVERIFY(s.named(QStringLiteral("M3")) == nullptr);
        QVERIFY(s.named(QStringLiteral("m1")) != nullptr);   // (by its tag too)

        // A layer switched off is not in the stack at all.
        m.stackNode().children.at(3).enabled = false;
        QCOMPARE(m.stack().layers.size(), 5);
    }

    // What the stack says cannot be: each check of the proposal, one at a
    // time, on a stack that is otherwise sound.
    void aStackThatCannotBeSaysWhy()
    {
        {   // A thickness of nothing, and one that is no number at all.
            rf::Model m = twoMetals();
            m.stackNode().children.at(2).props.insert(QStringLiteral("thickness"), QStringLiteral("0"));
            QVERIFY2(says(m.stack().problems, "M1 has no thickness"), qPrintable(m.stack().problems.join(" | ")));
            m.stackNode().children.at(2).props.insert(QStringLiteral("thickness"), QStringLiteral("0.6[um"));
            QVERIFY2(says(m.stack().problems, "is not a number"), qPrintable(m.stack().problems.join(" | ")));
        }
        {   // A permittivity below 1 and a negative loss tangent.
            rf::Model m = twoMetals();
            m.stackNode().children.at(1).props.insert(QStringLiteral("epsr"), QStringLiteral("0.5"));
            m.stackNode().children.at(1).props.insert(QStringLiteral("tand"), QStringLiteral("-1"));
            QVERIFY(says(m.stack().problems, "permittivity below 1"));
            QVERIFY(says(m.stack().problems, "loss tangent below zero"));
        }
        {   // A metal and a via without conductivity.
            rf::Model m = twoMetals();
            m.stackNode().children.at(2).props.insert(QStringLiteral("sigma"), QStringLiteral("0"));
            m.stackNode().children.at(4).props.insert(QStringLiteral("sigma"), QStringLiteral("0"));
            QVERIFY(says(m.stack().problems, "M1 has no conductivity"));
            QVERIFY(says(m.stack().problems, "via V1 has no conductivity"));
        }
        {   // A via that names nothing, something that is no metal, and
            // two metals that are not neighbours.
            rf::Model m = twoMetals();
            m.stackNode().children.at(4).props.insert(QStringLiteral("to"), QString());
            QVERIFY(says(m.stack().problems, "does not say which metals"));
            m.stackNode().children.at(4).props.insert(QStringLiteral("to"), QStringLiteral("M7"));
            QVERIFY(says(m.stack().problems, "which is no metal"));

            rf::Model three = twoMetals();
            rf::Node& stack = three.stackNode();
            rf::Node& ild3 = add(stack, "rf_dielectric", QStringLiteral("d3"), QStringLiteral("ILD3"));
            set(ild3, "thickness", QStringLiteral("2[um]"));
            rf::Node& m3 = add(stack, "rf_metal", QStringLiteral("m3"), QStringLiteral("M3"));
            set(m3, "thickness", QStringLiteral("1[um]"));
            set(m3, "sigma", QStringLiteral("3.3e7"));
            QVERIFY(three.stack().problems.isEmpty());
            stack.children.at(4).props.insert(QStringLiteral("to"), QStringLiteral("M3"));
            QVERIFY2(says(three.stack().problems, "not neighbours"), qPrintable(three.stack().problems.join(" | ")));
        }
        {   // Two metals with nothing between them.
            rf::Model m = twoMetals();
            m.stackNode().children.erase(m.stackNode().children.begin() + 3);   // the oxide above M1
            QVERIFY2(says(m.stack().problems, "touch"), qPrintable(m.stack().problems.join(" | ")));
        }
        {   // A substrate that is not the first layer, and two of them.
            rf::Model m = twoMetals();
            rf::Node& second = add(m.stackNode(), "rf_substrate", QStringLiteral("sub2"), QStringLiteral("Si2"));
            set(second, "thickness", QStringLiteral("100[um]"));
            QVERIFY(says(m.stack().problems, "more than one substrate"));
            QVERIFY(says(m.stack().problems, "not the first layer"));
        }
        {   // A cover that is not last.
            rf::Model m = twoMetals();
            rf::Node& cover = add(m.stackNode(), "rf_cover", QStringLiteral("cov1"), QStringLiteral("Air"));
            set(cover, "height", QStringLiteral("1[mm]"));
            QVERIFY(m.stack().problems.isEmpty());
            std::swap(m.stackNode().children.at(5), m.stackNode().children.at(6));
            QVERIFY(says(m.stack().problems, "cover is not the last"));
        }
        {   // A stack with no metal at all.
            rf::Model m;
            add(m.root, "rf_stack", QString());
            rf::Node& only = add(m.stackNode(), "rf_dielectric", QStringLiteral("d1"), QStringLiteral("ILD"));
            set(only, "thickness", QStringLiteral("1[um]"));
            QVERIFY(says(m.stack().problems, "no metal"));
        }
    }

    // The model as a whole: what is missing before anything can be solved.
    void aModelSaysWhatIsMissing()
    {
        rf::Model m = twoMetals();
        add(m.layout(), "rf_layer_map", QString());
        add(m.layout(), "rf_processing", QString());
        QStringList said = m.problems();
        QVERIFY2(says(said, "names no file"), qPrintable(said.join(" | ")));
        QVERIFY2(says(said, "no port"), qPrintable(said.join(" | ")));

        set(m.layout(), "file", QStringLiteral("inductor.gds"));
        m.layout().child(QStringLiteral("rf_layer_map"))
            ->props.insert(QStringLiteral("rows"),
                           QJsonArray{QJsonArray{QStringLiteral("31"), QStringLiteral("0"), QStringLiteral("M1")},
                                      QJsonArray{QJsonArray{}, QStringLiteral("0"), QStringLiteral("M9")}});
        said = m.problems();
        QVERIFY(!says(said, "names no file"));
        QVERIFY2(says(said, "no layer of the stack"), qPrintable(said.join(" | ")));

        // A port that names a metal that is not there, and one that is.
        rf::Node& port = add(m.ports(), "rf_edge_port", QStringLiteral("p1"), QStringLiteral("P1"));
        set(port, "conductor", QStringLiteral("M9"));
        QVERIFY2(says(m.problems(), "port P1 names M9"), qPrintable(m.problems().join(" | ")));
        set(port, "conductor", QStringLiteral("M2"));
        QVERIFY(!says(m.problems(), "port P1"));
        QVERIFY(!says(m.problems(), "no port"));

        // An internal port to the ground is no error, ground being no
        // layer of the stack.
        rf::Node& inner = add(m.ports(), "rf_internal_port", QStringLiteral("p2"), QStringLiteral("P2"));
        set(inner, "from", QStringLiteral("M2"));
        set(inner, "to", QStringLiteral("ground"));
        QVERIFY2(!says(m.problems(), "port P2"), qPrintable(m.problems().join(" | ")));

        // With the one bad row of the map put right, nothing is missing.
        QJsonArray oneRow;
        oneRow.append(QJsonArray{QStringLiteral("31"), QStringLiteral("0"), QStringLiteral("M1")});
        QCOMPARE(oneRow.size(), 1);
        QVERIFY(oneRow.first().isArray());
        m.layout().child(QStringLiteral("rf_layer_map"))->props.insert(QStringLiteral("rows"), oneRow);
        QVERIFY2(m.problems().isEmpty(), qPrintable(m.problems().join(" | ")));
    }
};

QTEST_MAIN(TestRf25Model)
#include "test_rf25_model.moc"

/*
 * An RF 2.5D model (rf25/rf25_model.h), phase A of
 * docs/proposals/2026-10-23-rf-2p5d-moment-method.md: the tree and its
 * kinds, the file it is written to and read from, the stack read out of it
 * in SI, and everything the stack and the model say is wrong with them.
 * There is no solver yet, and nothing here asks for one.
 */
#include <QtTest>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>

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

    // A ground plane is under the stack wherever the tree puts it: at
    // nought, with no place in the order to get wrong. It was put at the
    // running height, so after the passivation it sat on top, and the one
    // order the solver took was the one these checks refused (the phase A
    // check of 24 October).
    void aGroundIsUnderTheStackWhereverItIsPut()
    {
        const auto withGround = [](int at, bool cover) {
            rf::Model m = twoMetals();
            rf::Node& stack = m.stackNode();
            if (cover) {
                rf::Node& c = add(stack, "rf_cover", QStringLiteral("cov1"), QStringLiteral("Air"));
                set(c, "height", QStringLiteral("1[mm]"));
            }
            rf::Node ground;
            ground.type = QStringLiteral("rf_ground");
            ground.tag = QStringLiteral("gnd1");
            ground.label = QStringLiteral("GND");
            const int where = at < 0 ? int(stack.children.size()) : at;
            stack.children.insert(stack.children.begin() + where, ground);
            return m;
        };
        // Where the layers sit with no ground at all, for comparison.
        const rf::Stack bare = twoMetals().stack();
        QVERIFY2(bare.ok(), qPrintable(bare.problems.join(" | ")));
        for (const bool cover : {false, true})
            for (const int at : {0, 1, 3, 6, -1}) {
                const rf::Stack s = withGround(at, cover).stack();
                const QString where = QStringLiteral("ground at %1%2").arg(at).arg(cover ? QStringLiteral(", a cover") : QString());
                QVERIFY2(s.ok(), qPrintable(where + QStringLiteral(": ") + s.problems.join(" | ")));
                const rf::Layer* g = s.named(QStringLiteral("GND"));
                QVERIFY(g != nullptr);
                QVERIFY2(g->z0 == 0 && g->z1 == 0, qPrintable(QStringLiteral("%1: the ground at %2 to %3 m").arg(where).arg(g->z0).arg(g->z1)));
                // Nothing else moved for it.
                for (const rf::Layer& l : bare.layers) {
                    const rf::Layer* same = s.named(l.name);
                    QVERIFY(same != nullptr);
                    QVERIFY2(same->z0 == l.z0 && same->z1 == l.z1, qPrintable(where + QStringLiteral(": ") + l.name + QStringLiteral(" moved")));
                }
            }
        // The order checks still see a real mistake, past the ground.
        {
            rf::Model m = withGround(0, false);
            rf::Node& stack = m.stackNode();
            std::swap(stack.children.at(1), stack.children.at(2));   // the substrate above the oxide
            QVERIFY2(says(m.stack().problems, "substrate is not the first"), qPrintable(m.stack().problems.join(" | ")));
        }
        {
            rf::Model m = withGround(-1, true);
            rf::Node& stack = m.stackNode();
            // The cover before the last metal, the ground still after both.
            std::swap(stack.children.at(5), stack.children.at(6));
            QVERIFY2(says(m.stack().problems, "cover is not the last"), qPrintable(m.stack().problems.join(" | ")));
        }
        // Its own kind, and a cover's, are read for the solver.
        {
            rf::Model m = withGround(0, true);
            set(*m.find(QStringLiteral("gnd1")), "kind", QStringLiteral("open"));
            set(*m.find(QStringLiteral("cov1")), "kind", QStringLiteral("perfect"));
            const rf::Stack s = m.stack();
            QCOMPARE(s.named(QStringLiteral("GND"))->variant, QStringLiteral("open"));
            QCOMPARE(s.named(QStringLiteral("Air"))->variant, QStringLiteral("perfect"));
            QCOMPARE(twoMetals().stack().layers.first().variant, QString());
        }
    }

    // A ground added goes first, a substrate after it, and the rest before
    // the cover - the order of the tree being the order in z. A ground was
    // put at the end, after a cover added before it.
    void aLayerAddedGoesWhereItBelongs()
    {
        rf::Model m = rf::Model::blank();
        const QString stack = m.stackNode().tag;
        QVERIFY(m.add(stack, m.make(QStringLiteral("rf_cover"))) != nullptr);
        QCOMPARE(m.stackNode().children.back().type, QStringLiteral("rf_cover"));
        QVERIFY(m.add(stack, m.make(QStringLiteral("rf_ground"))) != nullptr);
        QCOMPARE(m.stackNode().children.front().type, QStringLiteral("rf_ground"));
        QCOMPARE(m.stackNode().children.back().type, QStringLiteral("rf_cover"));
        QVERIFY(m.add(stack, m.make(QStringLiteral("rf_dielectric"))) != nullptr);
        QCOMPARE(m.stackNode().children.back().type, QStringLiteral("rf_cover"));
        QCOMPARE(m.stackNode().children.at(m.stackNode().children.size() - 2).type, QStringLiteral("rf_dielectric"));
        QVERIFY2(m.stack().ok(), qPrintable(m.stack().problems.join(" | ")));
        // A substrate goes right after a ground, not under it (one at
        // most, so the blank model's is taken out first).
        rf::Model fresh;
        add(fresh.root, "rf_stack", QStringLiteral("st"));
        QVERIFY(fresh.add(QStringLiteral("st"), fresh.make(QStringLiteral("rf_ground"))) != nullptr);
        QVERIFY(fresh.add(QStringLiteral("st"), fresh.make(QStringLiteral("rf_substrate"))) != nullptr);
        QCOMPARE(fresh.stackNode().children.at(0).type, QStringLiteral("rf_ground"));
        QCOMPARE(fresh.stackNode().children.at(1).type, QStringLiteral("rf_substrate"));
        // And the help says what the code does: the ground has no place.
        const rf::NodeKind* ground = rf::nodeKind(QStringLiteral("rf_ground"));
        const rf::NodeKind* stackKind = rf::nodeKind(QStringLiteral("rf_stack"));
        QVERIFY(ground != nullptr && stackKind != nullptr);
        QVERIFY2(!ground->help.contains(QStringLiteral("last in the tree")), qPrintable(ground->help));
        QVERIFY2(ground->help.contains(QStringLiteral("wherever")), qPrintable(ground->help));
        QVERIFY2(stackKind->help.contains(QStringLiteral("ground")), qPrintable(stackKind->help));
    }

    // A value that will not read is said once: not "not a number" and
    // then the "no thickness" of the nought it was left at.
    void aBadValueIsSaidOnce()
    {
        const auto count = [](const QStringList& problems, const QString& of) {
            int n = 0;
            for (const QString& p : problems)
                if (p.contains(of)) ++n;
            return n;
        };
        {
            rf::Model m = twoMetals();
            set(m.stackNode().children.at(2), "thickness", QStringLiteral("nonsense"));
            const QStringList p = m.stack().problems;
            QCOMPARE(count(p, QStringLiteral("M1")), 1);
            QVERIFY2(says(p, "thickness is not a number"), qPrintable(p.join(" | ")));
            QVERIFY2(!says(p, "M1 has no thickness"), qPrintable(p.join(" | ")));
        }
        {   // Each of the values that is checked after it is read.
            rf::Model m = twoMetals();
            set(m.stackNode().children.at(2), "sigma", QStringLiteral("x"));
            set(m.stackNode().children.at(2), "roughness", QStringLiteral("y"));
            set(m.stackNode().children.at(1), "epsr", QStringLiteral("z"));
            set(m.stackNode().children.at(1), "tand", QStringLiteral("w"));
            set(m.stackNode().children.at(4), "sigma", QStringLiteral("v"));
            const QStringList p = m.stack().problems;
            QCOMPARE(count(p, QStringLiteral("M1")), 2);    // sigma and roughness, once each
            QCOMPARE(count(p, QStringLiteral("ILD1")), 2);  // epsr and tand
            QCOMPARE(count(p, QStringLiteral("V1")), 1);
            QVERIFY2(!says(p, "has no conductivity") && !says(p, "below 1") && !says(p, "below zero"),
                     qPrintable(p.join(" | ")));
        }
        {   // A value that reads but is wrong is still said as such.
            rf::Model m = twoMetals();
            set(m.stackNode().children.at(2), "thickness", QStringLiteral("0"));
            QVERIFY2(says(m.stack().problems, "M1 has no thickness"), qPrintable(m.stack().problems.join(" | ")));
        }
        // And the model's own list counts it once too.
        rf::Model whole = rf::Model::blank();
        for (rf::Node& c : whole.stackNode().children)
            if (c.type == QLatin1String("rf_metal")) set(c, "thickness", QStringLiteral("nonsense"));
        QCOMPARE(count(whole.problems(), QStringLiteral("thickness")), 1);
    }

    // What a file has that is not read is said, not passed over: "props"
    // for "properties" loaded every value at its default, and the model
    // said that nothing was missing.
    void whatAFileHasThatIsNotReadIsSaid()
    {
        // A blank model, written and read, reads with nothing to say.
        QString notes = QStringLiteral("unset");
        QVERIFY(rf::Model::fromJson(rf::Model::blank().toJson(), &notes).has_value());
        QVERIFY2(notes.isEmpty(), qPrintable(notes));

        QJsonObject o = QJsonDocument::fromJson(rf::Model::blank().toJson()).object();
        QJsonObject tree = o.value(QStringLiteral("tree")).toObject();
        QJsonArray parts = tree.value(QStringLiteral("children")).toArray();
        for (int i = 0; i < parts.size(); ++i) {
            QJsonObject part = parts.at(i).toObject();
            if (part.value(QStringLiteral("type")).toString() != QLatin1String("rf_stack")) continue;
            QJsonArray layers = part.value(QStringLiteral("children")).toArray();
            for (int j = 0; j < layers.size(); ++j) {
                QJsonObject layer = layers.at(j).toObject();
                if (layer.value(QStringLiteral("type")).toString() != QLatin1String("rf_metal")) continue;
                // "props" for "properties", a key no node has, and a
                // property no metal has.
                QJsonObject values = layer.value(QStringLiteral("properties")).toObject();
                values.insert(QStringLiteral("thickness"), QStringLiteral("5[um]"));
                layer.remove(QStringLiteral("properties"));
                layer.insert(QStringLiteral("props"), values);
                layer.insert(QStringLiteral("colour"), QStringLiteral("red"));
                layers[j] = layer;
            }
            for (int j = 0; j < layers.size(); ++j) {
                QJsonObject layer = layers.at(j).toObject();
                if (layer.value(QStringLiteral("type")).toString() != QLatin1String("rf_substrate")) continue;
                QJsonObject values = layer.value(QStringLiteral("properties")).toObject();
                values.insert(QStringLiteral("thicknes"), QStringLiteral("1[mm]"));
                layer.insert(QStringLiteral("properties"), values);
                layers[j] = layer;
            }
            part.insert(QStringLiteral("children"), layers);
            parts[i] = part;
        }
        tree.insert(QStringLiteral("children"), parts);
        o.insert(QStringLiteral("tree"), tree);
        const std::optional<rf::Model> read = rf::Model::fromJson(QJsonDocument(o).toJson(), &notes);
        QVERIFY2(read.has_value(), "a file with keys it does not read is still read");
        QVERIFY2(notes.contains(QStringLiteral("\"props\"")) && notes.contains(QStringLiteral("\"properties\"")), qPrintable(notes));
        QVERIFY2(notes.contains(QStringLiteral("\"colour\"")), qPrintable(notes));
        QVERIFY2(notes.contains(QStringLiteral("\"thicknes\"")), qPrintable(notes));
        // They are not read: nothing is guessed.
        for (const rf::Layer& l : read->stack().layers)
            if (l.kind == rf::Layer::Metal) QVERIFY2(l.thickness != 5e-6, "the props were read after all");
        // Many are said as many, up to a point.
        QJsonObject noisy = QJsonDocument::fromJson(rf::Model::blank().toJson()).object();
        QJsonObject noisyTree = noisy.value(QStringLiteral("tree")).toObject();
        for (int k = 0; k < 20; ++k) noisyTree.insert(QStringLiteral("extra%1").arg(k), k);
        noisy.insert(QStringLiteral("tree"), noisyTree);
        QVERIFY(rf::Model::fromJson(QJsonDocument(noisy).toJson(), &notes).has_value());
        QVERIFY2(notes.contains(QStringLiteral("and 12 more")), qPrintable(notes));
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

/*
 * componentsearch.cpp - finding components (by name, net label or property
 * value) in a schematic, and replacing property values
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "componentsearch.h"

#include "schematic.h"
#include "node.h"
#include "wire.h"
#include "wirelabel.h"
#include "components/component.h"

#include <QRegularExpression>

namespace qucs_s::search {

namespace {

QRegularExpression::PatternOptions caseOption(const Query& q)
{
    return q.matchCase ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption;
}

QRegularExpression textExpression(const Query& q)
{
    return QRegularExpression(q.wholeValue ? QRegularExpression::anchoredPattern(q.text) : q.text, caseOption(q));
}

QRegularExpression nameExpression(const Query& q)
{
    return QRegularExpression(QRegularExpression::wildcardToRegularExpression(q.namePattern),
                              QRegularExpression::CaseInsensitiveOption);
}

// Whether a name or value is what the query looks for.
class Matcher
{
public:
    explicit Matcher(const Query& q)
        : m_q(q), m_re(q.regex ? textExpression(q) : QRegularExpression())
    {}

    bool operator()(const QString& s) const
    {
        if (m_q.text.isEmpty()) return true;
        if (m_q.regex) return m_re.match(s).hasMatch();
        const Qt::CaseSensitivity cs = m_q.matchCase ? Qt::CaseSensitive : Qt::CaseInsensitive;
        return m_q.wholeValue ? s.compare(m_q.text, cs) == 0 : s.contains(m_q.text, cs);
    }

private:
    const Query& m_q;
    QRegularExpression m_re;
};

// The components the query is about, whatever it says of their text.
bool inScope(const Component* c, const Query& q, const QRegularExpression& names)
{
    if (!q.model.isEmpty() && c->Model != q.model) return false;
    if (!q.namePattern.isEmpty() && q.namePattern != QLatin1String("*") && !names.match(c->Name).hasMatch())
        return false;
    return true;
}

} // namespace

bool Query::isValid(QString* error) const
{
    if (regex && !text.isEmpty()) {
        const QRegularExpression re = textExpression(*this);
        if (!re.isValid()) {
            if (error) *error = re.errorString();
            return false;
        }
    }
    if (!namePattern.isEmpty() && !nameExpression(*this).isValid()) {
        if (error) *error = QStringLiteral("invalid name pattern");
        return false;
    }
    return true;
}

QList<Match> find(const Schematic* doc, const Query& query)
{
    QList<Match> exact, names, labels, values;
    if (doc == nullptr || doc->getSymbolMode() || !query.isValid()) return exact;
    const Matcher matches(query);
    const QRegularExpression wildcard = nameExpression(query);
    const Qt::CaseSensitivity cs = query.matchCase ? Qt::CaseSensitive : Qt::CaseInsensitive;

    for (Component* c : doc->a_DocComps) {
        if (!inScope(c, query, wildcard)) continue;
        const QPoint where(c->cx, c->cy);
        if (query.names && !query.text.isEmpty() && matches(c->Name)) {
            Match m{Match::Name, c->Name, c->Model, QString(), c->Name, where};
            (c->Name.compare(query.text, cs) == 0 ? exact : names) << m;
        }
        if (!query.values) continue;
        for (const Property* p : c->Props) {
            if (!query.property.isEmpty() && p->Name != query.property) continue;
            if (matches(p->Value)) values << Match{Match::Value, c->Name, c->Model, p->Name, p->Value, where};
        }
    }

    // A label only answers to a name search that is not narrowed to some
    // kind of component.
    if (query.names && !query.text.isEmpty() && query.model.isEmpty() && query.property.isEmpty()) {
        auto label = [&](const Conductor* owner) {
            if (!owner->hasLabel()) return;
            const WireLabel* l = owner->label();
            if (matches(l->Name)) labels << Match{Match::Label, l->Name, QString(), QString(), l->Name, l->root()};
        };
        for (const Wire* w : doc->a_DocWires) label(w);
        for (const Node* n : doc->a_DocNodes) label(n);
    }
    return exact + names + labels + values;
}

QString replaced(const QString& value, const Query& query, const QString& with)
{
    if (query.text.isEmpty()) return with;
    if (query.regex) return QString(value).replace(textExpression(query), with);
    const Qt::CaseSensitivity cs = query.matchCase ? Qt::CaseSensitive : Qt::CaseInsensitive;
    if (query.wholeValue) return value.compare(query.text, cs) == 0 ? with : value;
    return QString(value).replace(query.text, with, cs);
}

Component* componentOf(const Schematic* doc, const Match& match)
{
    if (doc == nullptr || match.kind == Match::Label) return nullptr;
    Component* named = nullptr;
    int count = 0;
    for (Component* c : doc->a_DocComps) {
        if (c->Name != match.component) continue;
        if (QPoint(c->cx, c->cy) == match.where) return c;
        named = c;
        ++count;
    }
    return count == 1 ? named : nullptr;
}

int replace(Schematic* doc, const QList<Match>& matches, const Query& query, const QString& with, int* skipped)
{
    int changed = 0, left = 0;
    for (const Match& m : matches) {
        if (m.kind != Match::Value) continue;
        Component* c = componentOf(doc, m);
        Property* p = nullptr;
        if (c != nullptr)
            for (Property* candidate : c->Props)
                if (candidate->Name == m.property && candidate->Value == m.value) {
                    p = candidate;
                    break;
                }
        const QString value = p != nullptr ? replaced(p->Value, query, with) : QString();
        if (p == nullptr || value.isEmpty() || value.contains(QLatin1Char('"'))) {
            ++left;
            continue;
        }
        if (value == p->Value) continue;

        // Text left of or above the symbol keeps its distance to it while
        // its size changes (as the properties dialog does).
        int txDist, tyDist;
        c->textSize(txDist, tyDist);
        int gap = c->tx + txDist - c->x1;
        if (gap > 0 || gap < -6) txDist = 0;
        gap = c->ty + tyDist - c->y1;
        if (gap > 0 || gap < -6) tyDist = 0;

        p->Value = value;

        int dx, dy;
        c->textSize(dx, dy);
        if (txDist != 0) c->tx += txDist - dx;
        if (tyDist != 0) c->ty += tyDist - dy;
        doc->recreateComponent(c);
        ++changed;
    }
    if (skipped != nullptr) *skipped = left;
    return changed;
}

void reveal(Schematic* doc, const Match& match)
{
    if (doc == nullptr) return;
    doc->deselectElements(nullptr);
    if (match.kind == Match::Label) {
        auto pick = [&](Conductor* owner, Wire* wire) {
            if (!owner->hasLabel() || owner->label()->Name != match.component || owner->label()->root() != match.where)
                return;
            owner->label()->isSelected = true;
            if (wire != nullptr) wire->isSelected = true;   // the net lights up
        };
        for (Wire* w : doc->a_DocWires) pick(w, w);
        for (Node* n : doc->a_DocNodes) pick(n, n->wires().empty() ? nullptr : n->wires().front());
    } else if (Component* c = componentOf(doc, match)) {
        c->isSelected = true;
    }
    doc->centerOn(match.where);
    doc->viewport()->update();
}

} // namespace qucs_s::search

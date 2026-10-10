/*
 * xmlmodel.cpp - an XML text read into its nodes, each where it is in the
 *                text; its paths; the text made tidy or compact
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "xmlmodel.h"

#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QXmlStreamReader>

#include <algorithm>
#include <functional>

namespace qucs_s::xml {

namespace {

bool isSpace(QChar c)
{
    return c == QLatin1Char(' ') || c == QLatin1Char('\t') || c == QLatin1Char('\n') || c == QLatin1Char('\r');
}

// The attributes of the start tag text[from, to) (its name after '<'),
// each where it is.
void readAttributes(const QString& text, int from, int to, int nameLength, const QXmlStreamAttributes& values, Parsed& p, int element,
                    const std::function<int(int)>& lineOf)
{
    int i = from + 1 + nameLength;
    while (i < to) {
        while (i < to && isSpace(text.at(i))) ++i;
        if (i >= to || text.at(i) == QLatin1Char('/') || text.at(i) == QLatin1Char('>')) break;
        const int nameStart = i;
        while (i < to && !isSpace(text.at(i)) && text.at(i) != QLatin1Char('=') && text.at(i) != QLatin1Char('>')
               && text.at(i) != QLatin1Char('/'))
            ++i;
        const int nameEnd = i;
        while (i < to && isSpace(text.at(i))) ++i;
        if (i >= to || text.at(i) != QLatin1Char('=')) break;
        ++i;
        while (i < to && isSpace(text.at(i))) ++i;
        if (i >= to || (text.at(i) != QLatin1Char('"') && text.at(i) != QLatin1Char('\''))) break;
        const QChar quote = text.at(i++);
        const int valueStart = i;
        while (i < to && text.at(i) != quote) ++i;
        const int valueEnd = i;
        if (i < to) ++i;
        Node a;
        a.kind = Node::Attribute;
        a.name = text.mid(nameStart, nameEnd - nameStart);
        a.value = values.hasAttribute(a.name) ? values.value(a.name).toString() : text.mid(valueStart, valueEnd - valueStart);
        a.start = nameStart;
        a.end = i;
        a.nameStart = nameStart;
        a.nameEnd = nameEnd;
        a.valueStart = valueStart;
        a.valueEnd = valueEnd;
        a.line = lineOf(nameStart);
        a.endLine = lineOf(std::max(nameStart, i - 1));
        a.parent = element;
        p.nodes[element].children << int(p.nodes.size());
        p.nodes << a;
    }
}

int addNode(Parsed& p, Node node)
{
    const int index = int(p.nodes.size());
    p.nodes[node.parent].children << index;
    p.nodes << std::move(node);
    return index;
}

// Whether element \a e of \a p keeps its content as it is (xml:space).
bool preserves(const Parsed& p, int e)
{
    for (int n = e; n > 0; n = p.nodes.at(n).parent) {
        for (int c : p.nodes.at(n).children) {
            const Node& a = p.nodes.at(c);
            if (a.kind != Node::Attribute) break;
            if (a.name == QLatin1String("xml:space")) return a.value == QLatin1String("preserve");
        }
    }
    return false;
}

// What a tidy or compact text is written of.
struct Writer {
    const QString& text;
    const Parsed& p;
    QString indent;   // empty: compact
    QString out;

    QString slice(int from, int to) const { return text.mid(from, to - from); }
    QString pad(int depth) const
    {
        QString s;
        for (int k = 0; k < depth; ++k) s += indent;
        return s;
    }
    void line(int depth, const QString& what)
    {
        if (indent.isEmpty()) {
            out += what;
            return;
        }
        if (!out.isEmpty()) out += QLatin1Char('\n');
        out += pad(depth) + what;
    }
    void element(int e, int depth)
    {
        const Node& n = p.nodes.at(e);
        QString head = QLatin1Char('<') + n.name;
        QList<int> content;
        for (int c : n.children) {
            const Node& child = p.nodes.at(c);
            if (child.kind == Node::Attribute) head += QLatin1Char(' ') + slice(child.start, child.end);
            else content << c;
        }
        if (n.tailStart < 0 && content.isEmpty()) {
            line(depth, head + QStringLiteral("/>"));
            return;
        }
        const QString tail = slice(n.tailStart, n.end);
        const bool hasText = std::any_of(content.cbegin(), content.cend(), [&](int c) {
            return p.nodes.at(c).kind == Node::Text || p.nodes.at(c).kind == Node::CData;
        });
        // Text in it (alone or mixed), or kept as it is: inside as written.
        if (content.isEmpty() || hasText || preserves(p, e)) {
            line(depth, head + QLatin1Char('>') + slice(n.headEnd, n.tailStart) + tail);
            return;
        }
        line(depth, head + QLatin1Char('>'));
        for (int c : content) node(c, depth + 1);
        line(depth, tail);
    }
    void node(int c, int depth)
    {
        const Node& n = p.nodes.at(c);
        switch (n.kind) {
        case Node::Element: element(c, depth); break;
        case Node::Attribute: break;
        default: line(depth, slice(n.start, n.end).trimmed()); break;
        }
    }
};

QString rewrite(const QString& text, const QString& indent, QString* error)
{
    const Parsed p = parse(text);
    if (!p.wellFormed) {
        if (error != nullptr)
            *error = QStringLiteral("line %1, column %2: %3").arg(p.errorLine).arg(p.errorColumn).arg(p.error);
        return {};
    }
    Writer w{text, p, indent, {}};
    for (int c : p.nodes.at(0).children) w.node(c, 0);
    if (!indent.isEmpty()) w.out += QLatin1Char('\n');
    return w.out;
}

} // namespace

int Parsed::elementCount() const
{
    return int(std::count_if(nodes.cbegin(), nodes.cend(), [](const Node& n) { return n.kind == Node::Element; }));
}

Parsed parse(const QString& text)
{
    Parsed p;
    QList<int> lineStarts{0};
    for (int i = 0; i < text.size(); ++i)
        if (text.at(i) == QLatin1Char('\n')) lineStarts << i + 1;
    const std::function<int(int)> lineOf = [&lineStarts](int offset) {
        return int(std::upper_bound(lineStarts.cbegin(), lineStarts.cend(), offset) - lineStarts.cbegin());
    };
    Node document;
    document.kind = Node::Document;
    document.end = int(text.size());
    document.endLine = int(lineStarts.size());
    p.nodes << document;
    if (text.trimmed().isEmpty()) return p;   // (nothing written yet: nothing wrong)

    // The reader says what each token is, and its value; where it is, the
    // text says: the reader's offsets run ahead of it (a text's takes the
    // '<' after it; the start of a document without a declaration, "<r").
    QXmlStreamReader r(text);
    r.setNamespaceProcessing(false);   // (names as written; xmlns an attribute)
    QList<int> open{0};
    int pos = 0;   // where the last token ended
    const int size = int(text.size());
    const auto found = [&](const QString& what, int from) {
        const qsizetype at = text.indexOf(what, from);
        return at < 0 ? size : int(at);
    };
    while (!r.atEnd()) {
        r.readNext();
        if (r.hasError()) break;
        const int after = std::clamp(int(r.characterOffset()), pos, size);
        Node n;
        n.parent = open.last();
        const auto place = [&](int from, int to) {
            n.start = from;
            n.end = std::max(from, to);
            n.line = lineOf(from);
            n.endLine = lineOf(std::max(from, n.end - 1));
            pos = n.end;
        };
        switch (r.tokenType()) {
        case QXmlStreamReader::StartDocument: {
            // A declaration, if it has one.
            int s = pos;
            while (s < size && isSpace(text.at(s))) ++s;
            if (!text.mid(s, 5).startsWith(QLatin1String("<?xml"))) break;
            place(s, std::min(size, found(QStringLiteral("?>"), s) + 2));
            n.kind = Node::Declaration;
            n.name = QStringLiteral("xml");
            n.value = text.mid(n.start, n.end - n.start);
            p.encoding = r.documentEncoding().toString();
            addNode(p, n);
            break;
        }
        case QXmlStreamReader::DTD:
            place(found(QStringLiteral("<!DOCTYPE"), pos), after);
            n.kind = Node::Doctype;
            n.name = r.dtdName().toString();
            n.value = r.text().toString();
            addNode(p, n);
            break;
        case QXmlStreamReader::StartElement: {
            place(found(QStringLiteral("<"), pos), after);
            n.kind = Node::Element;
            n.name = r.qualifiedName().toString();
            n.nameStart = n.start + 1;
            n.nameEnd = n.nameStart + int(n.name.size());
            n.headEnd = n.end;
            const int e = addNode(p, n);
            readAttributes(text, n.start, n.end, int(n.name.size()), r.attributes(), p, e, lineOf);
            open << e;
            break;
        }
        case QXmlStreamReader::EndElement: {
            const int e = open.takeLast();
            Node& el = p.nodes[e];
            // <empty/>: no end tag (the reader reads nothing for it).
            const bool empty = el.headEnd >= 2 && text.at(el.headEnd - 2) == QLatin1Char('/') && pos == el.headEnd;
            if (empty) {
                el.tailStart = -1;
                el.end = el.headEnd;
            } else {
                el.tailStart = found(QStringLiteral("</"), pos);
                el.end = std::min(size, found(QStringLiteral(">"), el.tailStart) + 1);
                pos = el.end;
            }
            el.endLine = lineOf(std::max(el.start, el.end - 1));
            break;
        }
        case QXmlStreamReader::Characters: {
            if (r.isCDATA()) {
                place(found(QStringLiteral("<![CDATA["), pos), 0);
                place(n.start, std::min(size, found(QStringLiteral("]]>"), n.start) + 3));
                n.kind = Node::CData;
                n.value = r.text().toString();
                n.valueStart = n.start + 9;
                n.valueEnd = n.end - 3;
                addNode(p, n);
                break;
            }
            // Text runs to the next markup.
            const int textEnd = found(QStringLiteral("<"), pos);
            if (r.isWhitespace()) {
                pos = textEnd;
                break;
            }
            int s = pos, e = textEnd;
            pos = textEnd;
            while (s < e && isSpace(text.at(s))) ++s;   // (its words, the whitespace around them left out)
            while (e > s && isSpace(text.at(e - 1))) --e;
            n.kind = Node::Text;
            n.start = n.valueStart = s;
            n.end = n.valueEnd = e;
            n.line = lineOf(s);
            n.endLine = lineOf(std::max(s, e - 1));
            n.value = r.text().toString().trimmed();
            // (A text the reader gives in pieces - around an entity it
            // could not resolve: one node.)
            if (const QList<int>& siblings = p.nodes.at(n.parent).children; !siblings.isEmpty()) {
                Node& last = p.nodes[siblings.last()];
                if (last.kind == Node::Text && last.end >= s - 1) {
                    last.end = last.valueEnd = e;
                    last.endLine = n.endLine;
                    last.value = text.mid(last.valueStart, e - last.valueStart);
                    break;
                }
            }
            addNode(p, n);
            break;
        }
        case QXmlStreamReader::Comment: {
            const int s = found(QStringLiteral("<!--"), pos);
            place(s, std::min(size, found(QStringLiteral("-->"), s + 4) + 3));
            n.kind = Node::Comment;
            n.value = r.text().toString();
            n.valueStart = n.start + 4;
            n.valueEnd = n.end - 3;
            addNode(p, n);
            break;
        }
        case QXmlStreamReader::ProcessingInstruction: {
            const int s = found(QStringLiteral("<?"), pos);
            place(s, std::min(size, found(QStringLiteral("?>"), s) + 2));
            n.kind = Node::Instruction;
            n.name = r.processingInstructionTarget().toString();
            n.value = r.processingInstructionData().toString();
            addNode(p, n);
            break;
        }
        case QXmlStreamReader::EntityReference: {
            const int s = found(QStringLiteral("&"), pos);
            place(s, std::min(size, found(QStringLiteral(";"), s) + 1));
            n.kind = Node::Text;
            n.value = r.text().toString();
            n.valueStart = n.start;
            n.valueEnd = n.end;
            addNode(p, n);
            break;
        }
        default:
            break;
        }
    }
    if (r.hasError()) {
        p.wellFormed = false;
        p.error = r.errorString();
        p.errorLine = int(r.lineNumber());
        p.errorColumn = int(r.columnNumber()) + 1;
        p.errorOffset = std::clamp(int(r.characterOffset()), 0, int(text.size()));
        // What was open, to where the reading stopped.
        for (int e : std::as_const(open)) {
            if (e == 0) continue;
            p.nodes[e].end = std::max(p.nodes[e].end, p.errorOffset);
            p.nodes[e].endLine = lineOf(std::max(p.nodes[e].start, p.nodes[e].end - 1));
        }
    }
    return p;
}

int nodeAt(const Parsed& parsed, int offset)
{
    int at = 0;
    for (;;) {
        const QList<int>& children = parsed.nodes.at(at).children;
        // (In the order of the text: the last that starts at or before it.)
        const auto it = std::upper_bound(children.cbegin(), children.cend(), offset,
                                         [&](int o, int c) { return o < parsed.nodes.at(c).start; });
        if (it == children.cbegin()) return at;
        const Node& n = parsed.nodes.at(*std::prev(it));
        if (offset >= n.end && !(offset == n.end && n.end == n.start)) return at;
        at = *std::prev(it);
    }
}

QList<int> ancestry(const Parsed& parsed, int node)
{
    QList<int> chain;
    for (int n = node; n > 0; n = parsed.nodes.at(n).parent)
        if (parsed.nodes.at(n).kind == Node::Element) chain.prepend(n);
    return chain;
}

QString pathOf(const Parsed& parsed, int node)
{
    if (node <= 0 || node >= parsed.nodes.size()) return QStringLiteral("/");
    const Node& n = parsed.nodes.at(node);
    const QString up = n.parent > 0 ? pathOf(parsed, n.parent) : QString();
    const auto step = [&](const QString& name, const std::function<bool(const Node&)>& same) {
        int count = 0, index = 0;
        for (int c : parsed.nodes.at(n.parent).children) {
            if (!same(parsed.nodes.at(c))) continue;
            ++count;
            if (c == node) index = count;
        }
        return up + QLatin1Char('/') + name + (count > 1 ? QStringLiteral("[%1]").arg(index) : QString());
    };
    switch (n.kind) {
    case Node::Element:
        return step(n.name, [&](const Node& o) { return o.kind == Node::Element && o.name == n.name; });
    case Node::Attribute:
        return up + QStringLiteral("/@") + n.name;
    case Node::Text:
    case Node::CData:
        return step(QStringLiteral("text()"), [](const Node& o) { return o.kind == Node::Text || o.kind == Node::CData; });
    case Node::Comment:
        return step(QStringLiteral("comment()"), [](const Node& o) { return o.kind == Node::Comment; });
    case Node::Instruction:
        return step(QStringLiteral("processing-instruction()"), [](const Node& o) { return o.kind == Node::Instruction; });
    default:
        return up.isEmpty() ? QStringLiteral("/") : up;
    }
}

QStringList pathsOf(const Parsed& parsed)
{
    QStringList paths(parsed.nodes.size());
    if (paths.isEmpty()) return paths;
    paths[0] = QStringLiteral("/");
    // Down the tree, each parent's children numbered by their names.
    QList<int> todo{0};
    while (!todo.isEmpty()) {
        const int at = todo.takeLast();
        const QString up = at == 0 ? QString() : paths.at(at);
        const QList<int>& children = parsed.nodes.at(at).children;
        const auto keyOf = [&](const Node& n) -> QString {
            switch (n.kind) {
            case Node::Element: return n.name;
            case Node::Text:
            case Node::CData: return QStringLiteral("text()");
            case Node::Comment: return QStringLiteral("comment()");
            case Node::Instruction: return QStringLiteral("processing-instruction()");
            default: return {};
            }
        };
        QHash<QString, int> count, seen;
        for (int c : children) ++count[keyOf(parsed.nodes.at(c))];
        for (int c : children) {
            const Node& n = parsed.nodes.at(c);
            if (n.kind == Node::Attribute) {
                paths[c] = up + QStringLiteral("/@") + n.name;
                continue;
            }
            const QString key = keyOf(n);
            if (key.isEmpty()) {
                paths[c] = up.isEmpty() ? QStringLiteral("/") : up;
                continue;
            }
            const int k = ++seen[key];
            paths[c] = up + QLatin1Char('/') + key + (count.value(key) > 1 ? QStringLiteral("[%1]").arg(k) : QString());
            if (!n.children.isEmpty()) todo << c;
        }
    }
    return paths;
}

QString escapeText(const QString& text)
{
    QString out = text;
    out.replace(QLatin1Char('&'), QLatin1String("&amp;"));
    out.replace(QLatin1Char('<'), QLatin1String("&lt;"));
    out.replace(QLatin1String("]]>"), QLatin1String("]]&gt;"));
    return out;
}

QString escapeAttribute(const QString& value)
{
    QString out = escapeText(value);
    out.replace(QLatin1Char('"'), QLatin1String("&quot;"));
    return out;
}

bool isName(const QString& name)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z_:][\\w.\\-:]*$"));
    return re.match(name).hasMatch();
}

QString format(const QString& text, const QString& indent, QString* error)
{
    return rewrite(text, indent.isEmpty() ? QStringLiteral("  ") : indent, error);
}

QString minify(const QString& text, QString* error)
{
    return rewrite(text, QString(), error);
}

bool isXmlFile(const QString& path)
{
    static const QStringList suffixes{QStringLiteral("xml"),  QStringLiteral("xsd"),  QStringLiteral("xsl"),   QStringLiteral("xslt"),
                                      QStringLiteral("xhtml"), QStringLiteral("plist"), QStringLiteral("qrc"),  QStringLiteral("ui"),
                                      QStringLiteral("kml"),  QStringLiteral("gpx"),  QStringLiteral("xmi"),   QStringLiteral("xaml"),
                                      QStringLiteral("wsdl"), QStringLiteral("rss"),  QStringLiteral("atom"),  QStringLiteral("xlf"),
                                      QStringLiteral("xliff"), QStringLiteral("resx"), QStringLiteral("csproj"), QStringLiteral("vcxproj")};
    return suffixes.contains(QFileInfo(path).suffix().toLower());
}

} // namespace qucs_s::xml

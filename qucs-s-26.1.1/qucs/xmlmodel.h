/*
 * xmlmodel.h - an XML text read into its nodes, each where it is in the
 *              text (elements, attributes, text, comments...); whether it
 *              is well formed, and where not; its paths; the text made
 *              tidy or compact
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_XMLMODEL_H
#define QUCS_XMLMODEL_H

#include <QList>
#include <QString>

namespace qucs_s::xml {

/// A node of the text, and where it is (offsets in the text; lines from 1).
struct Node {
    enum Kind { Document, Element, Attribute, Text, CData, Comment, Instruction, Doctype, Declaration };
    Kind kind = Element;
    QString name;        ///< an element's or attribute's name, an instruction's target
    QString value;       ///< an attribute's value, a text's, a comment's, an instruction's data (as read: entities resolved)
    int start = 0;       ///< the whole node: from its first character...
    int end = 0;         ///< ...to after its last (an element: its end tag's '>')
    int nameStart = -1;  ///< an element's name in its start tag, an attribute's
    int nameEnd = -1;
    int valueStart = -1; ///< an attribute's value between its quotes; a text's, a comment's...
    int valueEnd = -1;
    int headEnd = -1;    ///< an element: after its start tag's '>'
    int tailStart = -1;  ///< an element: where its end tag begins (-1: <empty/>)
    int line = 1;
    int endLine = 1;
    int parent = -1;
    QList<int> children; ///< an element's attributes first, then what is in it
};

/// The text read: its nodes (0: the document, all else under it), whether
/// it is well formed - and if not the first error, where - and the
/// encoding its declaration names.
struct Parsed {
    QList<Node> nodes;
    bool wellFormed = true;
    QString error;
    int errorLine = 0;
    int errorColumn = 0;
    int errorOffset = -1;
    QString encoding;
    int elementCount() const;
};

/// \a text read (whitespace between elements is no node). What comes before
/// an error is there.
Parsed parse(const QString& text);

/// The deepest node \a offset is in (an attribute when it is in one's text;
/// 0: none).
int nodeAt(const Parsed& parsed, int offset);
/// Where \a node is: "/catalog/book[2]/title", "/catalog/book[2]/@id",
/// "/catalog/book[2]/text()" - a number where its parent has more of its
/// name.
QString pathOf(const Parsed& parsed, int node);
/// Every node's path at once (as pathOf()), in one pass: [0] the
/// document's ("/").
QStringList pathsOf(const Parsed& parsed);
/// Its elements from the document's down to \a node's (or the one it is
/// in), for a path to show.
QList<int> ancestry(const Parsed& parsed, int node);

/// \a text as XML escapes it: in text (& <) or in an attribute's quotes
/// (& < ").
QString escapeText(const QString& text);
QString escapeAttribute(const QString& value);
/// Whether \a name may name an element or an attribute.
bool isName(const QString& name);

/// \a text tidy: each element on a line of its own, indented by \a indent
/// a level, short ones (text alone) on one line; comments, instructions,
/// CDATA and the declaration kept; the text of mixed content, and of an
/// element with xml:space="preserve", kept as it is. Empty (and why in \a
/// error) when it is not well formed.
QString format(const QString& text, const QString& indent, QString* error = nullptr);
/// \a text on as few characters as it can: the whitespace between
/// elements taken away (not where xml:space="preserve"). Empty, and why,
/// when it is not well formed.
QString minify(const QString& text, QString* error = nullptr);

/// A file's name an XML document has: .xml, .xsd, .xsl, .xslt, .xhtml,
/// .plist, .qrc, .ui, .kml, .gpx, .xmi, .xaml, .wsdl, .rss, .atom, .svgz
/// not (compressed), .svg not (a picture: Edit as Text opens it so).
bool isXmlFile(const QString& path);

} // namespace qucs_s::xml

#endif // QUCS_XMLMODEL_H

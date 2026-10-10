/*
 * qucscontrol_xml.cpp - Claude's XML: a file as the XML editor reads it -
 *                       whether it is well formed, its tree, a node by its
 *                       path - and changed node by node in its tab
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
#include "qucs.h"
#include "xmldoc.h"
#include "xmlmodel.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>

using namespace qucs_s::control;
using XNode = qucs_s::xml::Node;

namespace {

QString kindName(XNode::Kind kind)
{
    switch (kind) {
    case XNode::Element: return QStringLiteral("element");
    case XNode::Attribute: return QStringLiteral("attribute");
    case XNode::Text: return QStringLiteral("text");
    case XNode::CData: return QStringLiteral("cdata");
    case XNode::Comment: return QStringLiteral("comment");
    case XNode::Instruction: return QStringLiteral("instruction");
    case XNode::Doctype: return QStringLiteral("doctype");
    case XNode::Declaration: return QStringLiteral("declaration");
    default: return QStringLiteral("document");
    }
}

} // namespace

QJsonObject QucsControl::xmlDocument(const QJsonObject& args)
{
    const QString given = args.value(QLatin1String("path")).toString().trimmed();
    const QString action = args.value(QLatin1String("action")).toString(QStringLiteral("outline"));
    static const QStringList actions{QStringLiteral("check"),  QStringLiteral("outline"),       QStringLiteral("get"),
                                     QStringLiteral("set"),    QStringLiteral("rename"),        QStringLiteral("add_attribute"),
                                     QStringLiteral("add_element"), QStringLiteral("delete"), QStringLiteral("format"),
                                     QStringLiteral("minify")};
    if (!actions.contains(action)) return errorResult(tr("'action' is %1.").arg(actions.join(QStringLiteral(", "))));
    if (given.isEmpty()) return errorResult(tr("Which XML file? ('path')"));
    // Its tab: the one open, else opened (as XML).
    QString error;
    QucsDoc* doc = document(QJsonObject{{QStringLiteral("path"), given}}, &error);
    if (doc == nullptr) {
        const QString file = absolute(given);
        if (!QFileInfo(file).isFile()) return errorResult(tr("There is no file %1.").arg(QDir::toNativeSeparators(file)));
        if (!qucs_s::xml::isXmlFile(file))
            return errorResult(tr("%1 is not a file the XML editor opens (.xml, .xsd, .xsl, .plist, .ui, .qrc...).").arg(QFileInfo(file).fileName()));
        if (!a_app->gotoPage(file)) return errorResult(tr("%1 could not be opened.").arg(QDir::toNativeSeparators(file)));
        doc = document(QJsonObject{{QStringLiteral("path"), file}}, &error);
    }
    auto* xml = dynamic_cast<XmlDoc*>(doc);
    if (xml == nullptr) return errorResult(tr("%1 is open, but not in the XML editor.").arg(titleOf(doc)));
    xml->parseNow();
    const qucs_s::xml::Parsed& p = xml->parsed();
    const QString file = QDir::toNativeSeparators(xml->getDocName());

    const auto state = [&] {
        QJsonObject o{{QStringLiteral("file"), file},
                      {QStringLiteral("well formed"), p.wellFormed},
                      {QStringLiteral("elements"), p.elementCount()}};
        if (!p.wellFormed)
            o.insert(QStringLiteral("error"), QJsonObject{{QStringLiteral("line"), p.errorLine},
                                                          {QStringLiteral("column"), p.errorColumn},
                                                          {QStringLiteral("message"), p.error}});
        if (!p.encoding.isEmpty()) o.insert(QStringLiteral("encoding"), p.encoding);
        if (doc->getDocChanged()) o.insert(QStringLiteral("unsaved"), true);
        return o;
    };
    if (action == QLatin1String("check")) return jsonResult(state());

    if (action == QLatin1String("format") || action == QLatin1String("minify")) {
        misc::ErrorCapture said;
        const bool done = action == QLatin1String("format") ? xml->formatNow() : xml->minifyNow();
        if (!done) return errorResult(said.errors().join(QLatin1Char(' ')));
        QJsonObject o = state();
        o.insert(action, tr("done (one step of Edit > Undo; save_document saves it)"));
        return jsonResult(o);
    }

    const QStringList paths = qucs_s::xml::pathsOf(p);
    if (action == QLatin1String("outline")) {
        // The elements to 'depth' levels, each with its attributes and its
        // text; comments and instructions too.
        const int depth = std::clamp(args.value(QLatin1String("depth")).toInt(3), 1, 64);
        const int most = std::clamp(args.value(QLatin1String("max")).toInt(200), 1, 5000);
        QJsonArray nodes;
        int left = 0;
        const std::function<void(int, int)> walk = [&](int at, int level) {
            for (int c : p.nodes.at(at).children) {
                const XNode& n = p.nodes.at(c);
                if (n.kind == XNode::Attribute || n.kind == XNode::Text || n.kind == XNode::CData) continue;
                if (nodes.size() >= most) {
                    ++left;
                    continue;
                }
                QJsonObject o{{QStringLiteral("path"), paths.at(c)}, {QStringLiteral("line"), n.line}};
                if (n.kind == XNode::Element) {
                    QJsonObject attributes;
                    QString text;
                    int elements = 0;
                    for (int k : n.children) {
                        const XNode& child = p.nodes.at(k);
                        if (child.kind == XNode::Attribute) attributes.insert(child.name, child.value);
                        else if (child.kind == XNode::Text || child.kind == XNode::CData) text += child.value;
                        else if (child.kind == XNode::Element) ++elements;
                    }
                    if (!attributes.isEmpty()) o.insert(QStringLiteral("attributes"), attributes);
                    if (!text.isEmpty()) o.insert(QStringLiteral("text"), text.size() > 200 ? text.left(200) + QStringLiteral("…") : text);
                    if (elements > 0) o.insert(QStringLiteral("elements in it"), elements);
                } else {
                    o.insert(kindName(n.kind), n.value.left(200));
                }
                nodes.append(o);
                if (n.kind == XNode::Element && level < depth) walk(c, level + 1);
            }
        };
        walk(0, 1);
        QJsonObject o = state();
        o.insert(QStringLiteral("nodes"), nodes);
        if (left > 0) o.insert(QStringLiteral("more"), tr("%1 more nodes: 'max' takes more, or 'get' one by its path").arg(left));
        return jsonResult(o);
    }

    // A node by its path.
    const QString nodePath = args.value(QLatin1String("node")).toString().trimmed();
    if (nodePath.isEmpty() && action != QLatin1String("add_element"))
        return errorResult(tr("Which node? ('node': its path, as outline gives it - /catalog/book[2]/@id)"));
    const int node = nodePath.isEmpty() || nodePath == QLatin1String("/") ? 0 : int(paths.indexOf(nodePath));
    if (node < 0) return errorResult(tr("There is no node %1 (outline gives the paths there are).").arg(nodePath));
    const QString name = args.value(QLatin1String("name")).toString();
    const QString value = args.value(QLatin1String("value")).toString();

    if (action == QLatin1String("get")) {
        const XNode& n = p.nodes.at(node);
        const QString source = xml->toPlainText().mid(n.start, n.end - n.start);
        QJsonObject o{{QStringLiteral("path"), paths.at(node)},
                      {QStringLiteral("kind"), kindName(n.kind)},
                      {QStringLiteral("line"), n.line},
                      {QStringLiteral("text in the file"), source.size() > 20000 ? source.left(20000) + QStringLiteral("…") : source}};
        if (!n.name.isEmpty()) o.insert(QStringLiteral("name"), n.name);
        if (n.kind != XNode::Element) o.insert(QStringLiteral("value"), n.value);
        return jsonResult(o);
    }

    bool done = false;
    if (action == QLatin1String("set")) done = xml->setValue(node, value, &error);
    else if (action == QLatin1String("rename")) done = xml->rename(node, value, &error);
    else if (action == QLatin1String("add_attribute")) done = xml->addAttribute(node, name, value, &error);
    else if (action == QLatin1String("add_element")) done = xml->addElement(node, name, &error);
    else if (action == QLatin1String("delete")) done = xml->removeNode(node, &error);
    if (!done) return errorResult(error.isEmpty() ? tr("It could not be done.") : error);
    QJsonObject o = state();
    o.insert(action, tr("done (one step of Edit > Undo; save_document saves it)"));
    return jsonResult(o);
}

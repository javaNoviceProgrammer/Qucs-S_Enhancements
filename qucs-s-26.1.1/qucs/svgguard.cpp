/***************************************************************************
                               svgguard.cpp
                              --------------
 ***************************************************************************/

#include "svgguard.h"

#include "zipfile.h"

#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QLocale>
#include <QXmlStreamReader>

#include <algorithm>
#include <vector>

namespace qucs_s::svg {
namespace {

// The most an .svgz is inflated to be looked at (the archives elsewhere
// allow 128 MB a file).
constexpr qsizetype kMaxInflated = 64 << 20;

// A container drawn only where something refers to what is in it: its
// elements cost nothing until a <use> names them.
bool referredOnly(QStringView name)
{
    return name == u"defs" || name == u"symbol" || name == u"marker" || name == u"clipPath" || name == u"mask"
           || name == u"pattern" || name == u"linearGradient" || name == u"radialGradient";
}

// gzip (what an .svgz holds): its header taken off, the rest inflated.
QByteArray unzipped(const QByteArray& bytes)
{
    if (bytes.size() < 18 || uchar(bytes.at(0)) != 0x1F || uchar(bytes.at(1)) != 0x8B || uchar(bytes.at(2)) != 8) return {};
    const int flags = uchar(bytes.at(3));
    qsizetype at = 10;
    if ((flags & 0x04) != 0) {   // fields of its own
        if (at + 2 > bytes.size()) return {};
        at += 2 + (uchar(bytes.at(at)) | (uchar(bytes.at(at + 1)) << 8));
    }
    for (const int flag : {0x08, 0x10}) {   // the file's name, a comment
        if ((flags & flag) == 0) continue;
        while (at < bytes.size() && bytes.at(at) != '\0') ++at;
        ++at;
    }
    if ((flags & 0x02) != 0) at += 2;   // a sum over the header
    if (at <= 0 || at >= bytes.size()) return {};
    bool ok = false;
    const QByteArray out = zip::inflate(bytes.mid(at), &ok, kMaxInflated);
    return ok ? out : QByteArray();
}

// What the elements belonging to one id hold (index 0: the document
// itself). An element with an id of its own, and one of the containers
// above, begin another.
struct Scope {
    qint64 own = 0;             ///< the elements counted here
    std::vector<int> inside;    ///< the scopes drawn where they stand in this one
    QStringList refers;         ///< what the <use> elements of this one name
};

qint64 plus(qint64 a, qint64 b)
{
    return a > kMaxNodes - b ? kMaxNodes : a + b;
}

} // namespace

qint64 nodes(const QByteArray& raw)
{
    QByteArray bytes = raw;
    if (bytes.size() > 2 && uchar(bytes.at(0)) == 0x1F && uchar(bytes.at(1)) == 0x8B) bytes = unzipped(bytes);
    // One that names nothing again: its work is its size, which QtSvg
    // reads in step with it. (A prefixed name is a reference too, and
    // QtSvg honours it: <svg:use> expands as <use> does. Nothing else is
    // asked of the bytes - a root <svg> may stand after any amount of
    // comment, and what is no XML at all parses to nothing here.)
    if (bytes.isEmpty() || (!bytes.contains("<use") && !bytes.contains(":use"))) return 0;

    std::vector<Scope> scopes(1);
    QHash<QString, int> byId;
    std::vector<int> enclosing{0};
    std::vector<char> began;   // whether that open element began a scope
    qint64 elements = 0;
    QXmlStreamReader xml(bytes);
    xml.setNamespaceProcessing(true);
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isEndElement()) {
            if (!began.empty()) {
                if (began.back() != 0 && enclosing.size() > 1) enclosing.pop_back();
                began.pop_back();
            }
            continue;
        }
        if (!xml.isStartElement()) continue;
        const QStringView name = xml.name();
        const QXmlStreamAttributes attrs = xml.attributes();
        const QString id = attrs.value(QLatin1String("id")).toString();
        const bool apart = referredOnly(name);
        int scope = enclosing.back();
        if (!id.isEmpty() || apart) {
            scopes.push_back({});
            const int made = int(scopes.size()) - 1;
            if (!apart) scopes[scope].inside.push_back(made);
            if (!id.isEmpty() && !byId.contains(id)) byId.insert(id, made);
            scope = made;
            enclosing.push_back(made);
            began.push_back(1);
        } else {
            began.push_back(0);
        }
        scopes[scope].own += 1;
        if (name == u"use") {
            QString href = attrs.value(QLatin1String("http://www.w3.org/1999/xlink"), QLatin1String("href")).toString();
            if (href.isEmpty()) href = attrs.value(QLatin1String("href")).toString();
            if (href.startsWith(QLatin1Char('#'))) scopes[scope].refers << href.mid(1);
        }
        // A file that holds this many elements itself draws no faster than
        // one that asks for them through its references.
        if (++elements >= kMaxNodes) return kMaxNodes;
    }

    // What each scope draws: its own elements, the scopes drawn in it, and
    // what its references name.
    const auto childOf = [&scopes, &byId](int s, std::size_t k) {
        const Scope& sc = scopes[std::size_t(s)];
        if (k < sc.inside.size()) return sc.inside[k];
        const auto it = byId.constFind(sc.refers.at(qsizetype(k - sc.inside.size())));
        return it == byId.constEnd() ? -1 : *it;
    };
    struct Frame {
        int scope;
        qint64 sum;
        std::size_t next;
    };
    std::vector<qint64> cost(scopes.size(), 0);
    std::vector<char> state(scopes.size(), 0);   // 0 untouched, 1 under way, 2 counted
    std::vector<Frame> stack{{0, scopes[0].own, 0}};
    state[0] = 1;
    qint64 answer = 0;
    while (!stack.empty()) {
        Frame& f = stack.back();
        const Scope& sc = scopes[std::size_t(f.scope)];
        if (f.sum >= kMaxNodes || f.next >= sc.inside.size() + std::size_t(sc.refers.size())) {
            const qint64 value = f.sum;
            cost[std::size_t(f.scope)] = value;
            state[std::size_t(f.scope)] = 2;
            stack.pop_back();
            if (stack.empty()) {
                answer = value;
                break;
            }
            stack.back().sum = plus(stack.back().sum, value);
            continue;
        }
        const int kid = childOf(f.scope, f.next++);
        if (kid < 0) continue;                                      // a name nothing defines
        if (state[std::size_t(kid)] == 2) {                         // counted before
            f.sum = plus(f.sum, cost[std::size_t(kid)]);
            continue;
        }
        if (state[std::size_t(kid)] == 1) continue;                 // round to itself
        state[std::size_t(kid)] = 1;
        stack.push_back({kid, scopes[std::size_t(kid)].own, 0});
    }
    return answer;
}

bool expandsTooFar(const QByteArray& bytes, qint64* found)
{
    const qint64 count = nodes(bytes);
    if (found != nullptr) *found = count;
    return count >= kMaxNodes;
}

bool fileExpandsTooFar(const QString& path, qint64* found)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (found != nullptr) *found = 0;
        return false;
    }
    return expandsTooFar(f.read(kMaxInflated), found);
}

QString tooFarSaid(qint64 found)
{
    Q_UNUSED(found);
    return QCoreApplication::translate("svg", "it asks for more than %1 nodes through its <use> references")
        .arg(QLocale().toString(kMaxNodes));
}

} // namespace qucs_s::svg

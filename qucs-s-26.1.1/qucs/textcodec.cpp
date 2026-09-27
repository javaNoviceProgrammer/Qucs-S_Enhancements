/*
 * How a text file's bytes are characters: textcodec.h.
 */
#include "textcodec.h"

#include <QStringDecoder>
#include <QStringEncoder>

#include <algorithm>

namespace qucs_s::textcodec {

namespace {

// Windows-1252's bytes 0x80-0x9F; the five it leaves undefined are the C1
// control characters of their number, as Windows reads them, so that every
// byte is a character and back.
constexpr char16_t Cp1252High[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

QString fromCp1252(const QByteArray& bytes)
{
    QString text(bytes.size(), Qt::Uninitialized);
    QChar* out = text.data();
    for (const char c : bytes) {
        const auto b = static_cast<unsigned char>(c);
        *out++ = QChar(b >= 0x80 && b < 0xA0 ? Cp1252High[b - 0x80] : char16_t(b));
    }
    return text;
}

// The byte of \a c in Windows-1252, or -1.
int cp1252Byte(char16_t c)
{
    if (c < 0x80 || (c >= 0xA0 && c <= 0xFF)) return c;
    for (int i = 0; i < 32; ++i)
        if (Cp1252High[i] == c) return 0x80 + i;
    return -1;
}

struct Mark {
    const char* bytes;
    int size;
    Encoding::Kind kind;
};
// UTF-32 LE before UTF-16 LE: FF FE starts both.
constexpr Mark Marks[] = {
    {"\xEF\xBB\xBF", 3, Encoding::Kind::Utf8},
    {"\xFF\xFE\x00\x00", 4, Encoding::Kind::Utf32LE},
    {"\x00\x00\xFE\xFF", 4, Encoding::Kind::Utf32BE},
    {"\xFF\xFE", 2, Encoding::Kind::Utf16LE},
    {"\xFE\xFF", 2, Encoding::Kind::Utf16BE},
};

QStringConverter::Encoding converterFor(Encoding::Kind kind)
{
    switch (kind) {
    case Encoding::Kind::Utf16LE: return QStringConverter::Utf16LE;
    case Encoding::Kind::Utf16BE: return QStringConverter::Utf16BE;
    case Encoding::Kind::Utf32LE: return QStringConverter::Utf32LE;
    case Encoding::Kind::Utf32BE: return QStringConverter::Utf32BE;
    default: return QStringConverter::Utf8;
    }
}

// UTF-16 without a mark: text of mostly ASCII has a NUL in every other
// byte - the second of each pair (little endian) or the first.
bool looksUtf16(const QByteArray& bytes, Encoding::Kind* kind)
{
    const qsizetype n = std::min<qsizetype>(bytes.size(), 4096) & ~qsizetype(1);
    if (n < 4) return false;
    qsizetype evenNul = 0, oddNul = 0;
    for (qsizetype i = 0; i < n; i += 2) {
        if (bytes.at(i) == '\0') ++evenNul;
        if (bytes.at(i + 1) == '\0') ++oddNul;
    }
    const qsizetype pairs = n / 2;
    if (oddNul * 10 >= pairs * 4 && evenNul * 20 < pairs) {
        *kind = Encoding::Kind::Utf16LE;
        return true;
    }
    if (evenNul * 10 >= pairs * 4 && oddNul * 20 < pairs) {
        *kind = Encoding::Kind::Utf16BE;
        return true;
    }
    return false;
}

} // namespace

QString Encoding::name() const
{
    switch (kind) {
    case Kind::Utf8: return QStringLiteral("UTF-8");
    case Kind::Utf16LE: return QStringLiteral("UTF-16 LE");
    case Kind::Utf16BE: return QStringLiteral("UTF-16 BE");
    case Kind::Utf32LE: return QStringLiteral("UTF-32 LE");
    case Kind::Utf32BE: return QStringLiteral("UTF-32 BE");
    case Kind::Windows1252: return QStringLiteral("Windows-1252");
    }
    return {};
}

QString decode(const QByteArray& bytes, Encoding* found)
{
    Encoding encoding;
    QByteArray data = bytes;
    bool known = false;
    for (const Mark& mark : Marks)
        if (bytes.startsWith(QByteArray::fromRawData(mark.bytes, mark.size))) {
            encoding = {mark.kind, true};
            data = bytes.mid(mark.size);
            known = true;
            break;
        }
    if (!known && looksUtf16(bytes, &encoding.kind)) known = true;
    // UTF-8 unless known otherwise - and only when the text is written back
    // as these very bytes. Anything else (not UTF-8, a mark followed by what
    // is not of its encoding, a sequence the end cuts short, UTF-32 beyond
    // Unicode: not all of them errors to Qt's decoders) is read as
    // Windows-1252, every byte, the mark too: nothing is lost.
    QStringDecoder decoder(known ? converterFor(encoding.kind) : QStringConverter::Utf8,
                           QStringConverter::Flag::ConvertInitialBom | QStringConverter::Flag::Stateless);
    QString text = decoder.decode(data);
    QByteArray again;
    if (decoder.hasError() || !encode(text, encoding, &again) || again != bytes) {
        encoding = {Encoding::Kind::Windows1252, false};
        text = fromCp1252(bytes);
    }
    if (found != nullptr) *found = encoding;
    return text;
}

bool encode(const QString& text, const Encoding& encoding, QByteArray* bytes, QString* missing)
{
    bytes->clear();
    if (encoding.kind == Encoding::Kind::Windows1252) {
        QByteArray out(text.size(), Qt::Uninitialized);
        for (qsizetype i = 0; i < text.size(); ++i) {
            const int b = cp1252Byte(text.at(i).unicode());
            if (b < 0) {
                if (missing != nullptr) {
                    const bool pair = text.at(i).isHighSurrogate() && i + 1 < text.size();
                    *missing = text.mid(i, pair ? 2 : 1);
                }
                return false;
            }
            out[i] = char(b);
        }
        *bytes = out;
        return true;
    }
    for (const Mark& mark : Marks)
        if (encoding.bom && mark.kind == encoding.kind) {
            bytes->append(mark.bytes, mark.size);
            break;
        }
    QStringEncoder encoder(converterFor(encoding.kind));
    bytes->append(encoder.encode(text));
    return true;
}

} // namespace qucs_s::textcodec

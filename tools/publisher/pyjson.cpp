// SPDX-License-Identifier: GPL-2.0-or-later
#include "pyjson.h"

#include <QStringList>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

#if defined(__APPLE__)
#include <xlocale.h>
#endif

namespace Thoughts::Py
{

namespace
{
const Json &nullJson()
{
    static const Json value;
    return value;
}

QList<Json> &emptyItems()
{
    static QList<Json> items;
    items.clear();
    return items;
}

QList<Json::Member> &emptyMembers()
{
    static QList<Json::Member> members;
    members.clear();
    return members;
}
}

Json::Json() = default;
Json::Json(std::nullptr_t) { }
Json::Json(bool value) : m_type(Type::Bool), m_bool(value) { }
Json::Json(int value) : m_type(Type::Int), m_int(value) { }
Json::Json(qint64 value) : m_type(Type::Int), m_int(value) { }
Json::Json(double value) : m_type(Type::Float), m_double(value) { }
Json::Json(const char *value) : m_type(Type::String), m_string(QString::fromUtf8(value)) { }
Json::Json(const QString &value) : m_type(Type::String), m_string(value) { }

Json Json::array(QList<Json> items)
{
    Json json;
    json.m_type = Type::Array;
    json.m_items = std::make_shared<QList<Json>>(std::move(items));
    return json;
}

Json Json::object(QList<Member> members)
{
    Json json;
    json.m_type = Type::Object;
    json.m_members = std::make_shared<QList<Member>>(std::move(members));
    return json;
}

Json Json::bigInt(const QByteArray &digits)
{
    Json json;
    json.m_type = Type::BigInt;
    json.m_bigInt = digits;
    return json;
}

void Json::detach()
{
    if (m_items && m_items.use_count() > 1) {
        m_items = std::make_shared<QList<Json>>(*m_items);
    }
    if (m_members && m_members.use_count() > 1) {
        m_members = std::make_shared<QList<Member>>(*m_members);
    }
}

bool Json::toBool(bool fallback) const { return m_type == Type::Bool ? m_bool : fallback; }

qint64 Json::toInt(qint64 fallback) const
{
    if (m_type == Type::Int) {
        return m_int;
    }
    return fallback;
}

double Json::toDouble(double fallback) const
{
    if (m_type == Type::Float) {
        return m_double;
    }
    if (m_type == Type::Int) {
        return double(m_int);
    }
    return fallback;
}

QString Json::toString(const QString &fallback) const { return m_type == Type::String ? m_string : fallback; }

const QList<Json> &Json::items() const &
{
    if (m_type == Type::Array && m_items) {
        return *m_items;
    }
    static const QList<Json> empty;
    return empty;
}

QList<Json> &Json::items() &
{
    if (m_type != Type::Array) {
        return emptyItems();
    }
    detach();
    return *m_items;
}

const QList<Json::Member> &Json::members() const &
{
    if (m_type == Type::Object && m_members) {
        return *m_members;
    }
    static const QList<Member> empty;
    return empty;
}

QList<Json::Member> &Json::members() &
{
    if (m_type != Type::Object) {
        return emptyMembers();
    }
    detach();
    return *m_members;
}

bool Json::contains(const QString &key) const
{
    for (const auto &member : members()) {
        if (member.first == key) {
            return true;
        }
    }
    return false;
}

const Json &Json::value(const QString &key) const &
{
    for (const auto &member : members()) {
        if (member.first == key) {
            return member.second;
        }
    }
    return nullJson();
}

void Json::insert(const QString &key, const Json &value)
{
    if (m_type != Type::Object) {
        *this = object();
    }
    detach();
    for (auto &member : *m_members) {
        if (member.first == key) {
            member.second = value;
            return;
        }
    }
    m_members->append({key, value});
}

bool Json::remove(const QString &key)
{
    if (m_type != Type::Object) {
        return false;
    }
    detach();
    for (qsizetype index = 0; index < m_members->size(); ++index) {
        if (m_members->at(index).first == key) {
            m_members->removeAt(index);
            return true;
        }
    }
    return false;
}

QStringList Json::keys() const
{
    QStringList result;
    for (const auto &member : members()) {
        result.append(member.first);
    }
    return result;
}

void Json::append(const Json &value)
{
    if (m_type != Type::Array) {
        *this = array();
    }
    detach();
    m_items->append(value);
}

qsizetype Json::size() const
{
    if (m_type == Type::Array) {
        return items().size();
    }
    if (m_type == Type::Object) {
        return members().size();
    }
    return 0;
}

bool Json::operator==(const Json &other) const
{
    // Python's == on json.loads values: 1 == 1.0 and True == 1 are equal.
    const auto numeric = [](const Json &json) {
        return json.m_type == Type::Int || json.m_type == Type::Float || json.m_type == Type::Bool;
    };
    if (numeric(*this) && numeric(other)) {
        if (m_type == Type::Float || other.m_type == Type::Float) {
            const double left = m_type == Type::Bool ? double(m_bool) : toDouble();
            const double right = other.m_type == Type::Bool ? double(other.m_bool) : other.toDouble();
            return left == right;
        }
        const qint64 left = m_type == Type::Bool ? qint64(m_bool) : m_int;
        const qint64 right = other.m_type == Type::Bool ? qint64(other.m_bool) : other.m_int;
        return left == right;
    }
    if (m_type != other.m_type) {
        return false;
    }
    switch (m_type) {
    case Type::Null:
        return true;
    case Type::BigInt:
        return m_bigInt == other.m_bigInt;
    case Type::String:
        return m_string == other.m_string;
    case Type::Array:
        return items() == other.items();
    case Type::Object: {
        // dict equality ignores order.
        if (members().size() != other.members().size()) {
            return false;
        }
        for (const auto &member : members()) {
            if (!other.contains(member.first) || other.value(member.first) != member.second) {
                return false;
            }
        }
        return true;
    }
    default:
        return false;
    }
}

std::optional<QString> decodeUtf8(const QByteArray &bytes, QString *error, bool surrogatePass)
{
    // Python's strict UTF-8 decoder; reports the first bad byte like
    // "'utf-8' codec can't decode byte 0xff in position 3: invalid start byte".
    QString out;
    out.reserve(bytes.size());
    const auto *data = reinterpret_cast<const unsigned char *>(bytes.constData());
    const qsizetype size = bytes.size();
    // A sequence broken after valid continuation bytes is reported as a
    // range of bytes, a single bad byte by its value.
    const auto failAt = [&](qsizetype position, const char *reason, qsizetype span = 1) {
        if (error) {
            *error = span > 1 ? QStringLiteral("'utf-8' codec can't decode bytes in position %1-%2: %3").arg(position).arg(position + span - 1).arg(QString::fromLatin1(reason))
                              : QStringLiteral("'utf-8' codec can't decode byte 0x%1 in position %2: %3")
                                    .arg(QString::number(data[position], 16).rightJustified(2, QLatin1Char('0')))
                                    .arg(position)
                                    .arg(QString::fromLatin1(reason));
        }
        return std::optional<QString>();
    };
    for (qsizetype index = 0; index < size;) {
        const unsigned char lead = data[index];
        if (lead < 0x80) {
            out.append(QChar(char16_t(lead)));
            ++index;
            continue;
        }
        int length = 0;
        char32_t point = 0;
        unsigned char low = 0x80, high = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
            point = lead & 0x1F;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            point = lead & 0x0F;
            if (lead == 0xE0) low = 0xA0;
            if (lead == 0xED && !surrogatePass) high = 0x9F;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            point = lead & 0x07;
            if (lead == 0xF0) low = 0x90;
            if (lead == 0xF4) high = 0x8F;
        } else {
            return failAt(index, "invalid start byte");
        }
        for (int offset = 1; offset < length; ++offset) {
            if (index + offset >= size) {
                return failAt(index, "unexpected end of data", offset);
            }
            const unsigned char continuation = data[index + offset];
            const unsigned char minimum = offset == 1 ? low : 0x80;
            const unsigned char maximum = offset == 1 ? high : 0xBF;
            if (continuation < minimum || continuation > maximum) {
                return failAt(index, "invalid continuation byte", offset);
            }
            point = (point << 6) | (continuation & 0x3F);
        }
        if (point > 0xFFFF) {
            out.append(QChar::highSurrogate(point));
            out.append(QChar::lowSurrogate(point));
        } else {
            out.append(QChar(char16_t(point)));
        }
        index += length;
    }
    return out;
}

bool codePointLess(const QString &left, const QString &right)
{
    // UTF-16 order differs from code-point order only when a surrogate meets
    // a unit in U+E000..U+FFFF.
    const qsizetype length = std::min(left.size(), right.size());
    for (qsizetype index = 0; index < length; ++index) {
        const char16_t a = left.at(index).unicode();
        const char16_t b = right.at(index).unicode();
        if (a == b) {
            continue;
        }
        const bool aSurrogate = a >= 0xD800 && a <= 0xDFFF;
        const bool bSurrogate = b >= 0xD800 && b <= 0xDFFF;
        if (aSurrogate != bSurrogate && (a >= 0xE000 || b >= 0xE000)) {
            return aSurrogate ? false : true;
        }
        return a < b;
    }
    return left.size() < right.size();
}

QByteArray floatRepr(double value)
{
    if (std::isnan(value)) {
        return QByteArrayLiteral("NaN");
    }
    if (std::isinf(value)) {
        return value > 0 ? QByteArrayLiteral("Infinity") : QByteArrayLiteral("-Infinity");
    }
    if (value == 0) {
        return std::signbit(value) ? QByteArrayLiteral("-0.0") : QByteArrayLiteral("0.0");
    }
    // Shortest round-trip digits, then Python's repr layout: fixed notation
    // when the decimal point position is in (-4, 16], scientific otherwise.
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), std::fabs(value), std::chars_format::scientific);
    const std::string text(buffer, result.ptr);
    const auto e = text.find('e');
    std::string digits = text.substr(0, e);
    digits.erase(std::remove(digits.begin(), digits.end(), '.'), digits.end());
    const int exponent = std::stoi(text.substr(e + 1));
    const int decimalPoint = exponent + 1;
    std::string out = std::signbit(value) ? "-" : "";
    if (decimalPoint > -4 && decimalPoint <= 16) {
        if (decimalPoint <= 0) {
            out += "0." + std::string(size_t(-decimalPoint), '0') + digits;
        } else if (size_t(decimalPoint) >= digits.size()) {
            out += digits + std::string(size_t(decimalPoint) - digits.size(), '0') + ".0";
        } else {
            out += digits.substr(0, size_t(decimalPoint)) + "." + digits.substr(size_t(decimalPoint));
        }
    } else {
        out += digits.substr(0, 1);
        if (digits.size() > 1) {
            out += "." + digits.substr(1);
        }
        const int shown = decimalPoint - 1;
        char exponentText[16];
        std::snprintf(exponentText, sizeof(exponentText), "e%c%02d", shown < 0 ? '-' : '+', std::abs(shown));
        out += exponentText;
    }
    return QByteArray::fromStdString(out);
}

namespace
{
void appendHex4(QByteArray &out, unsigned value)
{
    static const char hex[] = "0123456789abcdef";
    out += "\\u";
    out += hex[(value >> 12) & 0xF];
    out += hex[(value >> 8) & 0xF];
    out += hex[(value >> 4) & 0xF];
    out += hex[value & 0xF];
}

void appendString(QByteArray &out, const QString &value, bool ensureAscii)
{
    out += '"';
    for (qsizetype index = 0; index < value.size(); ++index) {
        const char16_t unit = value.at(index).unicode();
        switch (unit) {
        case u'"':
            out += "\\\"";
            continue;
        case u'\\':
            out += "\\\\";
            continue;
        case u'\n':
            out += "\\n";
            continue;
        case u'\r':
            out += "\\r";
            continue;
        case u'\t':
            out += "\\t";
            continue;
        case u'\b':
            out += "\\b";
            continue;
        case u'\f':
            out += "\\f";
            continue;
        default:
            break;
        }
        if (unit < 0x20) {
            appendHex4(out, unit);
        } else if (ensureAscii && unit > 0x7E) {
            // Python escapes each UTF-16 unit, giving surrogate pairs for
            // characters outside the BMP.
            appendHex4(out, unit);
        } else if (unit < 0x80) {
            out += char(unit);
        } else {
            // Raw UTF-8; combine a surrogate pair into one code point.
            if (QChar::isHighSurrogate(unit) && index + 1 < value.size() && QChar::isLowSurrogate(value.at(index + 1).unicode())) {
                out += QString(value.mid(index, 2)).toUtf8();
                ++index;
            } else {
                out += QString(QChar(unit)).toUtf8();
            }
        }
    }
    out += '"';
}

struct Encoder {
    const DumpOptions &options;
    QString itemSeparator;
    QString keySeparator;
    QString *error;

    bool encode(QByteArray &out, const Json &value, int level)
    {
        switch (value.type()) {
        case Json::Type::Null:
            out += "null";
            return true;
        case Json::Type::Bool:
            out += value.toBool() ? "true" : "false";
            return true;
        case Json::Type::Int:
            out += QByteArray::number(value.toInt());
            return true;
        case Json::Type::BigInt:
            out += value.bigIntDigits();
            return true;
        case Json::Type::Float: {
            const double number = value.toDouble();
            if (!options.allowNan && !std::isfinite(number)) {
                if (error) {
                    *error = QStringLiteral("Out of range float values are not JSON compliant: %1").arg(QString::fromLatin1(std::isnan(number) ? "nan" : (number > 0 ? "inf" : "-inf")));
                }
                return false;
            }
            out += floatRepr(number);
            return true;
        }
        case Json::Type::String:
            appendString(out, value.toString(), options.ensureAscii);
            return true;
        case Json::Type::Array: {
            const auto &items = value.items();
            if (items.isEmpty()) {
                out += "[]";
                return true;
            }
            out += '[';
            const QByteArray newline = indentFor(level + 1);
            const QByteArray separator = itemSeparator.toUtf8() + newline;
            out += newline;
            for (qsizetype index = 0; index < items.size(); ++index) {
                if (index) {
                    out += separator;
                }
                if (!encode(out, items.at(index), level + 1)) {
                    return false;
                }
            }
            out += indentFor(level);
            out += ']';
            return true;
        }
        case Json::Type::Object: {
            QList<Json::Member> members = value.members();
            if (members.isEmpty()) {
                out += "{}";
                return true;
            }
            if (options.sortKeys) {
                std::stable_sort(members.begin(), members.end(), [](const Json::Member &a, const Json::Member &b) { return codePointLess(a.first, b.first); });
            }
            out += '{';
            const QByteArray newline = indentFor(level + 1);
            const QByteArray separator = itemSeparator.toUtf8() + newline;
            out += newline;
            for (qsizetype index = 0; index < members.size(); ++index) {
                if (index) {
                    out += separator;
                }
                appendString(out, members.at(index).first, options.ensureAscii);
                out += keySeparator.toUtf8();
                if (!encode(out, members.at(index).second, level + 1)) {
                    return false;
                }
            }
            out += indentFor(level);
            out += '}';
            return true;
        }
        }
        return false;
    }

    QByteArray indentFor(int level) const
    {
        if (!options.indent) {
            return {};
        }
        return '\n' + QByteArray(qMax(0, *options.indent) * level, ' ');
    }
};
}

std::optional<QByteArray> dumps(const Json &value, const DumpOptions &options, QString *error)
{
    Encoder encoder {options, {}, {}, error};
    if (options.separators) {
        encoder.itemSeparator = options.separators->first;
        encoder.keySeparator = options.separators->second;
    } else {
        encoder.itemSeparator = options.indent ? QStringLiteral(",") : QStringLiteral(", ");
        encoder.keySeparator = QStringLiteral(": ");
    }
    QByteArray out;
    if (!encoder.encode(out, value, 0)) {
        return std::nullopt;
    }
    return out;
}

namespace
{
#if !defined(__APPLE__)
// A float literal std::from_chars found out of range: ±infinity when its first
// significant digit stands at a positive power of ten, else ±0. Only the
// position of that digit matters, so no locale is involved.
double outOfRangeFloatLiteral(const std::string &literal)
{
    const bool negative = !literal.empty() && literal.front() == '-';
    const size_t begin = negative ? 1 : 0;
    const size_t exponentAt = std::min(literal.find_first_of("eE", begin), literal.size());
    long long exponent = 0;
    if (exponentAt < literal.size()) {
        const char *first = literal.data() + exponentAt + 1;
        const char *last = literal.data() + literal.size();
        const bool negativeExponent = first < last && *first == '-';
        if (first < last && (*first == '+' || *first == '-')) {
            ++first;
        }
        if (std::from_chars(first, last, exponent).ec == std::errc::result_out_of_range) {
            exponent = std::numeric_limits<long long>::max() / 2;
        }
        if (negativeExponent) {
            exponent = -exponent;
        }
    }
    const size_t pointAt = std::min(literal.find('.', begin), exponentAt);
    const size_t digitAt = literal.find_first_of("123456789", begin);
    long long position = 0;
    if (digitAt < pointAt) {
        position = (long long)(pointAt - digitAt) - 1;
    } else if (digitAt < exponentAt) {
        position = -(long long)(digitAt - pointAt);
    } else {
        return negative ? -0.0 : 0.0;
    }
    const double magnitude = exponent + position > 0 ? HUGE_VAL : 0.0;
    return negative ? -magnitude : magnitude;
}
#endif

// A float literal as Decoder::parseNumber scanned it (JSON's grammar, ASCII
// only) as the double Python's float() reads: correctly rounded to nearest,
// ties to even, ±infinity past DBL_MAX, ±0 below the smallest subnormal.
// Sync and publication hashes depend on these exact bits.
double parseFloatLiteral(const std::string &literal)
{
#if defined(__APPLE__)
    // Apple's libc++ has floating-point std::from_chars only from macOS 26.0,
    // which would make that the app's minimum. strtod_l is correctly rounded
    // as well (gdtoa); in the C locale it reads '.' whatever LC_NUMERIC the
    // app runs with (Qt sets the user's locale on Unix), and it saturates and
    // underflows as float() does.
    return strtod_l(literal.c_str(), nullptr, LC_C_LOCALE);
#else
    double value = 0;
    const auto result = std::from_chars(literal.data(), literal.data() + literal.size(), value);
    if (result.ec == std::errc::result_out_of_range) {
        // Python's float() saturates to infinity or underflows to 0. Not with
        // strtod: it reads LC_NUMERIC, and with a decimal comma "1.5e400" is 1.
        return outOfRangeFloatLiteral(literal);
    }
    return value;
#endif
}

// Decodes over code points so error positions match Python's str indices.
struct Decoder {
    const std::u32string &text;
    const LoadOptions &options;
    QString *error;
    size_t pos = 0;

    bool fail(const QString &message, size_t at)
    {
        if (error) {
            size_t line = 1;
            size_t lastNewline = std::u32string::npos;
            for (size_t index = 0; index < at && index < text.size(); ++index) {
                if (text[index] == U'\n') {
                    ++line;
                    lastNewline = index;
                }
            }
            const size_t column = lastNewline == std::u32string::npos ? at + 1 : at - lastNewline;
            *error = QStringLiteral("%1: line %2 column %3 (char %4)").arg(message).arg(line).arg(column).arg(at);
        }
        return false;
    }

    void skipWhitespace()
    {
        while (pos < text.size() && (text[pos] == U' ' || text[pos] == U'\t' || text[pos] == U'\n' || text[pos] == U'\r')) {
            ++pos;
        }
    }

    bool startsWith(const char *literal) const
    {
        const size_t length = std::strlen(literal);
        if (pos + length > text.size()) {
            return false;
        }
        for (size_t index = 0; index < length; ++index) {
            if (text[pos + index] != char32_t(literal[index])) {
                return false;
            }
        }
        return true;
    }

    static QString fromCodePoints(const std::u32string &points)
    {
        QString out;
        for (char32_t point : points) {
            if (point > 0xFFFF) {
                out.append(QChar::highSurrogate(point));
                out.append(QChar::lowSurrogate(point));
            } else {
                out.append(QChar(char16_t(point)));
            }
        }
        return out;
    }

    bool parseString(QString *out)
    {
        // pos is just after the opening quote.
        const size_t begin = pos - 1;
        std::u32string chunk;
        while (true) {
            if (pos >= text.size()) {
                return fail(QStringLiteral("Unterminated string starting at"), begin);
            }
            const char32_t c = text[pos];
            if (c == U'"') {
                ++pos;
                break;
            }
            if (c < 0x20) {
                return fail(QStringLiteral("Invalid control character at"), pos);
            }
            if (c != U'\\') {
                chunk.push_back(c);
                ++pos;
                continue;
            }
            ++pos;
            if (pos >= text.size()) {
                return fail(QStringLiteral("Unterminated string starting at"), begin);
            }
            const char32_t escape = text[pos];
            switch (escape) {
            case U'"': chunk.push_back(U'"'); ++pos; continue;
            case U'\\': chunk.push_back(U'\\'); ++pos; continue;
            case U'/': chunk.push_back(U'/'); ++pos; continue;
            case U'b': chunk.push_back(U'\b'); ++pos; continue;
            case U'f': chunk.push_back(U'\f'); ++pos; continue;
            case U'n': chunk.push_back(U'\n'); ++pos; continue;
            case U'r': chunk.push_back(U'\r'); ++pos; continue;
            case U't': chunk.push_back(U'\t'); ++pos; continue;
            case U'u': break;
            default:
                return fail(QStringLiteral("Invalid \\escape"), pos - 1);
            }
            const auto hex4 = [this](size_t at, unsigned *value) {
                if (at + 4 > text.size()) {
                    return false;
                }
                unsigned result = 0;
                for (size_t index = 0; index < 4; ++index) {
                    const char32_t h = text[at + index];
                    result <<= 4;
                    if (h >= U'0' && h <= U'9') result |= unsigned(h - U'0');
                    else if (h >= U'a' && h <= U'f') result |= unsigned(h - U'a' + 10);
                    else if (h >= U'A' && h <= U'F') result |= unsigned(h - U'A' + 10);
                    else return false;
                }
                *value = result;
                return true;
            };
            unsigned unit = 0;
            if (!hex4(pos + 1, &unit)) {
                return fail(QStringLiteral("Invalid \\uXXXX escape"), pos);
            }
            pos += 5;
            char32_t point = unit;
            if (unit >= 0xD800 && unit <= 0xDBFF && pos + 1 < text.size() && text[pos] == U'\\' && text[pos + 1] == U'u') {
                unsigned low = 0;
                if (hex4(pos + 2, &low) && low >= 0xDC00 && low <= 0xDFFF) {
                    point = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                    pos += 6;
                }
            }
            chunk.push_back(point);
        }
        *out = fromCodePoints(chunk);
        return true;
    }

    bool parseNumber(Json *out)
    {
        const size_t begin = pos;
        size_t cursor = pos;
        if (cursor < text.size() && text[cursor] == U'-') ++cursor;
        if (cursor >= text.size() || text[cursor] < U'0' || text[cursor] > U'9') {
            return false;
        }
        if (text[cursor] == U'0') {
            ++cursor;
        } else {
            while (cursor < text.size() && text[cursor] >= U'0' && text[cursor] <= U'9') ++cursor;
        }
        bool isFloat = false;
        if (cursor + 1 < text.size() && text[cursor] == U'.' && text[cursor + 1] >= U'0' && text[cursor + 1] <= U'9') {
            isFloat = true;
            cursor += 2;
            while (cursor < text.size() && text[cursor] >= U'0' && text[cursor] <= U'9') ++cursor;
        }
        if (cursor < text.size() && (text[cursor] == U'e' || text[cursor] == U'E')) {
            size_t exponent = cursor + 1;
            if (exponent < text.size() && (text[exponent] == U'+' || text[exponent] == U'-')) ++exponent;
            if (exponent < text.size() && text[exponent] >= U'0' && text[exponent] <= U'9') {
                isFloat = true;
                cursor = exponent;
                while (cursor < text.size() && text[cursor] >= U'0' && text[cursor] <= U'9') ++cursor;
            }
        }
        std::string literal;
        for (size_t index = begin; index < cursor; ++index) literal.push_back(char(text[index]));
        pos = cursor;
        if (isFloat) {
            *out = Json(parseFloatLiteral(literal));
            return true;
        }
        qint64 value = 0;
        const auto result = std::from_chars(literal.data(), literal.data() + literal.size(), value);
        if (result.ec == std::errc::result_out_of_range) {
            *out = Json::bigInt(QByteArray::fromStdString(literal));
        } else {
            *out = Json(value);
        }
        return true;
    }

    bool parseValue(Json *out)
    {
        if (pos >= text.size()) {
            return fail(QStringLiteral("Expecting value"), pos);
        }
        const char32_t c = text[pos];
        if (c == U'"') {
            ++pos;
            QString value;
            if (!parseString(&value)) return false;
            *out = Json(value);
            return true;
        }
        if (c == U'{') {
            ++pos;
            return parseObject(out);
        }
        if (c == U'[') {
            ++pos;
            return parseArray(out);
        }
        if (startsWith("null")) { pos += 4; *out = Json(); return true; }
        if (startsWith("true")) { pos += 4; *out = Json(true); return true; }
        if (startsWith("false")) { pos += 5; *out = Json(false); return true; }
        if (startsWith("NaN")) { pos += 3; *out = Json(std::nan("")); return true; }
        if (startsWith("Infinity")) { pos += 8; *out = Json(HUGE_VAL); return true; }
        if (startsWith("-Infinity")) { pos += 9; *out = Json(-HUGE_VAL); return true; }
        if (parseNumber(out)) {
            return true;
        }
        return fail(QStringLiteral("Expecting value"), pos);
    }

    bool parseObject(Json *out)
    {
        QList<Json::Member> members;
        skipWhitespace();
        if (pos < text.size() && text[pos] == U'}') {
            ++pos;
            *out = Json::object();
            return true;
        }
        while (true) {
            if (pos >= text.size() || text[pos] != U'"') {
                return fail(QStringLiteral("Expecting property name enclosed in double quotes"), pos);
            }
            ++pos;
            QString key;
            if (!parseString(&key)) return false;
            skipWhitespace();
            if (pos >= text.size() || text[pos] != U':') {
                return fail(QStringLiteral("Expecting ':' delimiter"), pos);
            }
            ++pos;
            skipWhitespace();
            Json value;
            if (!parseValue(&value)) return false;
            bool replaced = false;
            for (auto &member : members) {
                if (member.first == key) {
                    if (options.rejectDuplicateKeys) {
                        if (error) *error = options.duplicateKeyMessage.arg(key);
                        if (options.duplicateKeyFound) *options.duplicateKeyFound = true;
                        return false;
                    }
                    member.second = value;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                members.append({key, value});
            }
            skipWhitespace();
            if (pos < text.size() && text[pos] == U'}') {
                ++pos;
                break;
            }
            if (pos >= text.size() || text[pos] != U',') {
                return fail(QStringLiteral("Expecting ',' delimiter"), pos);
            }
            const size_t comma = pos;
            ++pos;
            skipWhitespace();
            if (pos < text.size() && text[pos] == U'}') {
                return fail(QStringLiteral("Illegal trailing comma before end of object"), comma);
            }
        }
        *out = Json::object(std::move(members));
        return true;
    }

    bool parseArray(Json *out)
    {
        QList<Json> items;
        skipWhitespace();
        if (pos < text.size() && text[pos] == U']') {
            ++pos;
            *out = Json::array();
            return true;
        }
        while (true) {
            Json value;
            if (!parseValue(&value)) return false;
            items.append(value);
            skipWhitespace();
            if (pos < text.size() && text[pos] == U']') {
                ++pos;
                break;
            }
            if (pos >= text.size() || text[pos] != U',') {
                return fail(QStringLiteral("Expecting ',' delimiter"), pos);
            }
            const size_t comma = pos;
            ++pos;
            skipWhitespace();
            if (pos < text.size() && text[pos] == U']') {
                return fail(QStringLiteral("Illegal trailing comma before end of array"), comma);
            }
        }
        *out = Json::array(std::move(items));
        return true;
    }
};
}

std::optional<Json> loads(const QString &text, QString *error, const LoadOptions &options)
{
    std::u32string points;
    points.reserve(size_t(text.size()));
    for (qsizetype index = 0; index < text.size(); ++index) {
        const char16_t unit = text.at(index).unicode();
        if (QChar::isHighSurrogate(unit) && index + 1 < text.size() && QChar::isLowSurrogate(text.at(index + 1).unicode())) {
            points.push_back(QChar::surrogateToUcs4(unit, text.at(index + 1).unicode()));
            ++index;
        } else {
            points.push_back(unit);
        }
    }
    if (!points.empty() && points.front() == 0xFEFF) {
        if (error) {
            *error = QStringLiteral("Unexpected UTF-8 BOM (decode using utf-8-sig): line 1 column 1 (char 0)");
        }
        return std::nullopt;
    }
    Decoder decoder {points, options, error};
    decoder.skipWhitespace();
    Json value;
    if (!decoder.parseValue(&value)) {
        return std::nullopt;
    }
    const size_t end = decoder.pos;
    decoder.skipWhitespace();
    if (decoder.pos != points.size()) {
        decoder.fail(QStringLiteral("Extra data"), decoder.pos);
        Q_UNUSED(end)
        return std::nullopt;
    }
    return value;
}

std::optional<Json> loads(const QByteArray &utf8, QString *error, const LoadOptions &options)
{
    QByteArray bytes = utf8;
    if (bytes.startsWith("\xEF\xBB\xBF")) {
        bytes.remove(0, 3);
    }
    return loads(QString::fromUtf8(bytes), error, options);
}

}

namespace Thoughts::Py
{

QList<Json> Json::items() const &&
{
    return static_cast<const Json &>(*this).items();
}

QList<Json::Member> Json::members() const &&
{
    return static_cast<const Json &>(*this).members();
}

Json Json::value(const QString &key) const &&
{
    return static_cast<const Json &>(*this).value(key);
}

}

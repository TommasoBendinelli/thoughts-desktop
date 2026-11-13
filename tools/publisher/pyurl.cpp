// SPDX-License-Identifier: GPL-2.0-or-later
#include "pyurl.h"

#include "pyerror.h"
#include "pystr.h"

#include <QStringList>

#include <algorithm>

namespace Thoughts::Py
{
namespace
{
bool isAsciiAlpha(QChar c)
{
    return (c >= QLatin1Char('a') && c <= QLatin1Char('z')) || (c >= QLatin1Char('A') && c <= QLatin1Char('Z'));
}

bool isAsciiDigit(QChar c)
{
    return c >= QLatin1Char('0') && c <= QLatin1Char('9');
}

bool isHexDigit(QChar c)
{
    return isAsciiDigit(c) || (c >= QLatin1Char('a') && c <= QLatin1Char('f')) || (c >= QLatin1Char('A') && c <= QLatin1Char('F'));
}

bool parsesAsIpv4(const QString &address)
{
    // ipaddress.IPv4Address: four decimal octets, no leading zeros.
    const QStringList octets = address.split(QLatin1Char('.'));
    if (address.isEmpty() || octets.size() != 4) {
        return false;
    }
    for (const QString &octet : octets) {
        if (octet.isEmpty() || octet.size() > 3 || !std::all_of(octet.cbegin(), octet.cend(), isAsciiDigit)
            || (octet != QLatin1String("0") && octet.startsWith(QLatin1Char('0'))) || octet.toInt() > 255) {
            return false;
        }
    }
    return true;
}

bool parsesAsIpv6(const QString &input)
{
    // ipaddress.IPv6Address, with an optional %scope.
    QString address = input;
    const qsizetype percent = address.indexOf(QLatin1Char('%'));
    if (percent >= 0) {
        const QString scope = address.mid(percent + 1);
        if (scope.isEmpty() || scope.contains(QLatin1Char('%'))) {
            return false;
        }
        address = address.left(percent);
    }
    if (address.isEmpty() || address.size() > 45 || address.contains(QLatin1Char('/'))) {
        return false;
    }
    QStringList parts = address.split(QLatin1Char(':'));
    if (parts.size() < 3) {
        return false;
    }
    if (parts.last().contains(QLatin1Char('.'))) {
        if (!parsesAsIpv4(parts.last())) {
            return false;
        }
        parts.removeLast();
        parts << QStringLiteral("0") << QStringLiteral("0");
    }
    if (parts.size() > 9) {
        return false;
    }
    qsizetype skip = -1;
    for (qsizetype index = 1; index < parts.size() - 1; ++index) {
        if (parts.at(index).isEmpty()) {
            if (skip >= 0) {
                return false;
            }
            skip = index;
        }
    }
    qsizetype high = parts.size();
    qsizetype low = 0;
    if (skip >= 0) {
        high = skip;
        low = parts.size() - skip - 1;
        if (parts.first().isEmpty() && --high) {
            return false;
        }
        if (parts.last().isEmpty() && --low) {
            return false;
        }
        if (8 - (high + low) < 1) {
            return false;
        }
    } else if (parts.size() != 8 || parts.first().isEmpty() || parts.last().isEmpty()) {
        return false;
    }
    const auto validHextet = [](const QString &hextet) {
        return !hextet.isEmpty() && hextet.size() <= 4 && std::all_of(hextet.cbegin(), hextet.cend(), isHexDigit);
    };
    for (qsizetype index = 0; index < high; ++index) {
        if (!validHextet(parts.at(index))) {
            return false;
        }
    }
    for (qsizetype index = parts.size() - low; index < parts.size(); ++index) {
        if (!validHextet(parts.at(index))) {
            return false;
        }
    }
    return true;
}

void checkBracketedHost(const QString &hostname)
{
    if (hostname.startsWith(QLatin1Char('v'))) {
        // \Av[a-fA-F0-9]+\..+\z ("." does not match a newline)
        const qsizetype dot = hostname.indexOf(QLatin1Char('.'));
        bool valid = dot > 1 && dot + 1 < hostname.size() && !hostname.mid(dot + 1).contains(QLatin1Char('\n'));
        for (qsizetype index = 1; valid && index < dot; ++index) {
            valid = isHexDigit(hostname.at(index));
        }
        if (!valid) {
            raiseValueError(QStringLiteral("IPvFuture address is invalid"));
        }
        return;
    }
    if (parsesAsIpv4(hostname)) {
        raiseValueError(QStringLiteral("An IPv4 address cannot be in brackets"));
    }
    if (!parsesAsIpv6(hostname)) {
        raiseValueError(repr(hostname) + QStringLiteral(" does not appear to be an IPv4 or IPv6 address"));
    }
}

void checkBracketedNetloc(const QString &netloc)
{
    const QString hostAndPort = netloc.mid(netloc.lastIndexOf(QLatin1Char('@')) + 1);
    const qsizetype open = hostAndPort.indexOf(QLatin1Char('['));
    QString hostname;
    if (open >= 0) {
        if (open > 0) {
            raiseValueError(QStringLiteral("Invalid IPv6 URL"));
        }
        const QString bracketed = hostAndPort.mid(open + 1);
        const qsizetype close = bracketed.indexOf(QLatin1Char(']'));
        hostname = close < 0 ? bracketed : bracketed.left(close);
        const QString port = close < 0 ? QString() : bracketed.mid(close + 1);
        if (!port.isEmpty() && !port.startsWith(QLatin1Char(':'))) {
            raiseValueError(QStringLiteral("Invalid IPv6 URL"));
        }
    } else {
        const qsizetype colon = hostAndPort.indexOf(QLatin1Char(':'));
        hostname = colon < 0 ? hostAndPort : hostAndPort.left(colon);
    }
    checkBracketedHost(hostname);
}

void checkNetloc(const QString &netloc)
{
    bool ascii = true;
    for (const QChar c : netloc) {
        ascii = ascii && c.unicode() < 128;
    }
    if (netloc.isEmpty() || ascii) {
        return;
    }
    QString stripped = netloc;
    stripped.remove(QLatin1Char('@')).remove(QLatin1Char(':')).remove(QLatin1Char('#')).remove(QLatin1Char('?'));
    const QString normalized = stripped.normalized(QString::NormalizationForm_KC);
    if (normalized == stripped) {
        return;
    }
    for (const QChar c : QStringLiteral("/?#@:")) {
        if (normalized.contains(c)) {
            raiseValueError(QStringLiteral("netloc '") + netloc + QStringLiteral("' contains invalid characters under NFKC normalization"));
        }
    }
}

int hexValue(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// _unquote_impl on an ASCII run.
QByteArray unquoteBytes(const QByteArray &input)
{
    const QList<QByteArray> bits = input.split('%');
    if (bits.size() == 1) {
        return input;
    }
    QByteArray result = bits.first();
    for (qsizetype index = 1; index < bits.size(); ++index) {
        const QByteArray &item = bits.at(index);
        if (item.size() >= 2 && hexValue(item.at(0)) >= 0 && hexValue(item.at(1)) >= 0) {
            result.append(char(hexValue(item.at(0)) * 16 + hexValue(item.at(1))));
            result.append(item.mid(2));
        } else {
            result.append('%');
            result.append(item);
        }
    }
    return result;
}
}

SplitResult urlsplit(const QString &input)
{
    QString url = input;
    qsizetype start = 0;
    while (start < url.size() && url.at(start).unicode() <= 0x20) {
        ++start;
    }
    url = url.mid(start);
    url.remove(QLatin1Char('\t')).remove(QLatin1Char('\r')).remove(QLatin1Char('\n'));
    SplitResult result;
    const qsizetype colon = url.indexOf(QLatin1Char(':'));
    if (colon > 0 && isAsciiAlpha(url.at(0))) {
        bool valid = true;
        for (qsizetype index = 0; index < colon && valid; ++index) {
            const QChar c = url.at(index);
            valid = isAsciiAlpha(c) || isAsciiDigit(c) || c == QLatin1Char('+') || c == QLatin1Char('-') || c == QLatin1Char('.');
        }
        if (valid) {
            result.scheme = url.left(colon).toLower();
            url = url.mid(colon + 1);
        }
    }
    if (url.startsWith(QStringLiteral("//"))) {
        qsizetype delimiter = url.size();
        for (const QChar c : QStringLiteral("/?#")) {
            const qsizetype found = url.indexOf(c, 2);
            if (found >= 0) {
                delimiter = qMin(delimiter, found);
            }
        }
        result.netloc = url.mid(2, delimiter - 2);
        url = url.mid(delimiter);
        const bool open = result.netloc.contains(QLatin1Char('['));
        const bool close = result.netloc.contains(QLatin1Char(']'));
        if (open != close) {
            raiseValueError(QStringLiteral("Invalid IPv6 URL"));
        }
        if (open && close) {
            checkBracketedNetloc(result.netloc);
        }
    }
    const qsizetype hash = url.indexOf(QLatin1Char('#'));
    if (hash >= 0) {
        result.fragment = url.mid(hash + 1);
        url = url.left(hash);
    }
    const qsizetype question = url.indexOf(QLatin1Char('?'));
    if (question >= 0) {
        result.query = url.mid(question + 1);
        url = url.left(question);
    }
    checkNetloc(result.netloc);
    result.path = url;
    return result;
}

QString unquote(const QString &text)
{
    if (!text.contains(QLatin1Char('%'))) {
        return text;
    }
    QString result;
    qsizetype index = 0;
    while (index < text.size()) {
        qsizetype end = index;
        if (text.at(index).unicode() < 128) {
            while (end < text.size() && text.at(end).unicode() < 128) {
                ++end;
            }
            result += decodeUtf8Replacing(unquoteBytes(text.mid(index, end - index).toLatin1()));
        } else {
            while (end < text.size() && text.at(end).unicode() >= 128) {
                ++end;
            }
            result += text.mid(index, end - index);
        }
        index = end;
    }
    return result;
}

QString quote(const QString &text, const QString &safe)
{
    QString result;
    for (const char byte : text.toUtf8()) {
        const uchar value = uchar(byte);
        const bool unreserved = (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9')
            || value == '_' || value == '.' || value == '-' || value == '~';
        if (unreserved || (value < 128 && safe.contains(QLatin1Char(char(value))))) {
            result += QLatin1Char(char(value));
        } else {
            result += QStringLiteral("%%%1").arg(value, 2, 16, QLatin1Char('0')).toUpper();
        }
    }
    return result;
}

QString posixNormpath(const QString &path)
{
    if (path.isEmpty()) {
        return QStringLiteral(".");
    }
    int initialSlashes = path.startsWith(QLatin1Char('/')) ? 1 : 0;
    if (initialSlashes && path.startsWith(QStringLiteral("//")) && !path.startsWith(QStringLiteral("///"))) {
        initialSlashes = 2;
    }
    QStringList components;
    for (const QString &component : path.split(QLatin1Char('/'))) {
        if (component.isEmpty() || component == QLatin1String(".")) {
            continue;
        }
        if (component != QLatin1String("..") || (!initialSlashes && components.isEmpty())
            || (!components.isEmpty() && components.last() == QLatin1String(".."))) {
            components.append(component);
        } else if (!components.isEmpty()) {
            components.removeLast();
        }
    }
    const QString result = QString(initialSlashes, QLatin1Char('/')) + components.join(QLatin1Char('/'));
    return result.isEmpty() ? QStringLiteral(".") : result;
}

QString posixJoin(const QString &first, const QString &second)
{
    if (second.startsWith(QLatin1Char('/'))) {
        return second;
    }
    if (first.isEmpty() || first.endsWith(QLatin1Char('/'))) {
        return first + second;
    }
    return first + QLatin1Char('/') + second;
}

QString posixDirname(const QString &path)
{
    const qsizetype slash = path.lastIndexOf(QLatin1Char('/'));
    QString head = path.left(slash + 1);
    bool onlySlashes = true;
    for (const QChar c : head) {
        onlySlashes = onlySlashes && c == QLatin1Char('/');
    }
    if (!head.isEmpty() && !onlySlashes) {
        while (head.endsWith(QLatin1Char('/'))) {
            head.chop(1);
        }
    }
    return head;
}

QString decodeUtf8Replacing(const QByteArray &bytes)
{
    QString out;
    const auto *data = reinterpret_cast<const unsigned char *>(bytes.constData());
    const qsizetype size = bytes.size();
    qsizetype index = 0;
    while (index < size) {
        const unsigned char lead = data[index];
        if (lead < 0x80) {
            out.append(QChar(char16_t(lead)));
            ++index;
            continue;
        }
        int length = 0;
        char32_t point = 0;
        unsigned char low = 0x80;
        unsigned char high = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
            point = lead & 0x1F;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            point = lead & 0x0F;
            low = lead == 0xE0 ? 0xA0 : 0x80;
            high = lead == 0xED ? 0x9F : 0xBF;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            point = lead & 0x07;
            low = lead == 0xF0 ? 0x90 : 0x80;
            high = lead == 0xF4 ? 0x8F : 0xBF;
        } else {
            out.append(QChar(0xFFFD));
            ++index;
            continue;
        }
        int consumed = 1;
        bool valid = true;
        for (; consumed < length; ++consumed) {
            if (index + consumed >= size) {
                valid = false;
                break;
            }
            const unsigned char next = data[index + consumed];
            if (next < (consumed == 1 ? low : 0x80) || next > (consumed == 1 ? high : 0xBF)) {
                valid = false;
                break;
            }
            point = (point << 6) | (next & 0x3F);
        }
        if (!valid) {
            // One replacement for the maximal valid prefix.
            out.append(QChar(0xFFFD));
            index += consumed;
            continue;
        }
        out += fromCodePoints(std::u32string(1, point));
        index += length;
    }
    return out;
}

}

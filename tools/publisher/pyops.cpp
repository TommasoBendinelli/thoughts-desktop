// SPDX-License-Identifier: GPL-2.0-or-later
#include "pyops.h"

#include "pyerror.h"
#include "pystr.h"

#include <QAtomicInteger>

#include <cmath>

namespace Thoughts::Py
{

QString typeName(const Json &value)
{
    switch (value.type()) {
    case Json::Type::Null:
        return QStringLiteral("NoneType");
    case Json::Type::Bool:
        return QStringLiteral("bool");
    case Json::Type::Int:
    case Json::Type::BigInt:
        return QStringLiteral("int");
    case Json::Type::Float:
        return QStringLiteral("float");
    case Json::Type::String:
        return QStringLiteral("str");
    case Json::Type::Array:
        return QStringLiteral("list");
    case Json::Type::Object:
        return QStringLiteral("dict");
    }
    return {};
}

QList<Json> iterate(const Json &value)
{
    switch (value.type()) {
    case Json::Type::Array:
        return value.items();
    case Json::Type::Object: {
        QList<Json> keys;
        for (const QString &key : value.keys()) {
            keys.append(Json(key));
        }
        return keys;
    }
    case Json::Type::String: {
        QList<Json> characters;
        for (const char32_t codePoint : toCodePoints(value.toString())) {
            characters.append(Json(fromCodePoints(std::u32string(1, codePoint))));
        }
        return characters;
    }
    default:
        raise(QStringLiteral("TypeError"), QStringLiteral("'%1' object is not iterable").arg(typeName(value)));
    }
}

Json get(const Json &value, const QString &key, const Json &fallback)
{
    if (!value.isObject()) {
        raise(QStringLiteral("AttributeError"), QStringLiteral("'%1' object has no attribute 'get'").arg(typeName(value)));
    }
    return value.contains(key) ? value.value(key) : fallback;
}

Json subscript(const Json &value, const QString &key)
{
    switch (value.type()) {
    case Json::Type::Object:
        if (!value.contains(key)) {
            raise(QStringLiteral("KeyError"), repr(key));
        }
        return value.value(key);
    case Json::Type::String:
        raise(QStringLiteral("TypeError"), QStringLiteral("string indices must be integers, not 'str'"));
    case Json::Type::Array:
        raise(QStringLiteral("TypeError"), QStringLiteral("list indices must be integers or slices, not str"));
    default:
        raise(QStringLiteral("TypeError"), QStringLiteral("'%1' object is not subscriptable").arg(typeName(value)));
    }
}

QString hashKey(const Json &value, const char *container)
{
    switch (value.type()) {
    case Json::Type::Array:
    case Json::Type::Object:
        raise(QStringLiteral("TypeError"),
              QStringLiteral("cannot use '%1' as a %2 (unhashable type: '%1')").arg(typeName(value), QLatin1String(container)));
    case Json::Type::Null:
        return QStringLiteral("none");
    case Json::Type::String:
        return QStringLiteral("s:") + value.toString();
    case Json::Type::Bool:
        return value.toBool() ? QStringLiteral("n:1") : QStringLiteral("n:0");
    case Json::Type::Int:
        return QStringLiteral("n:") + QString::number(value.toInt());
    case Json::Type::BigInt:
        return QStringLiteral("n:") + QString::fromLatin1(value.bigIntDigits());
    case Json::Type::Float: {
        const double number = value.toDouble();
        if (std::isnan(number)) {
            // Each parsed NaN is its own object and equal to nothing.
            static QAtomicInteger<quint64> counter;
            return QStringLiteral("nan:") + QString::number(counter.fetchAndAddRelaxed(1));
        }
        if (std::isfinite(number) && number == std::floor(number)) {
            return QStringLiteral("n:") + QString::number(number, 'f', 0);
        }
        return QStringLiteral("f:") + QString::fromLatin1(floatRepr(number));
    }
    }
    return {};
}

QString requireString(const Json &value)
{
    if (!value.isString()) {
        raise(QStringLiteral("TypeError"), QStringLiteral("expected string or bytes-like object, got '%1'").arg(typeName(value)));
    }
    return value.toString();
}

bool truthy(const Json &value)
{
    switch (value.type()) {
    case Json::Type::Null:
        return false;
    case Json::Type::Bool:
        return value.toBool();
    case Json::Type::Int:
        return value.toInt() != 0;
    case Json::Type::BigInt:
        return true;
    case Json::Type::Float:
        return value.toDouble() != 0.0;
    case Json::Type::String:
        return !value.toString().isEmpty();
    case Json::Type::Array:
    case Json::Type::Object:
        return value.size() > 0;
    }
    return false;
}

qint64 toPythonInt(const Json &value)
{
    switch (value.type()) {
    case Json::Type::Bool:
        return value.toBool() ? 1 : 0;
    case Json::Type::Int:
        return value.toInt();
    case Json::Type::Float: {
        const double number = value.toDouble();
        if (std::isnan(number)) {
            raise(QStringLiteral("ValueError"), QStringLiteral("cannot convert float NaN to integer"));
        }
        if (std::isinf(number)) {
            raise(QStringLiteral("OverflowError"), QStringLiteral("cannot convert float infinity to integer"));
        }
        const double truncated = std::trunc(number);
        if (truncated < -9.2e18 || truncated > 9.2e18) {
            raise(QStringLiteral("OverflowError"), QStringLiteral("Python int too large to convert to C long"));
        }
        return qint64(truncated);
    }
    case Json::Type::BigInt:
        raise(QStringLiteral("OverflowError"), QStringLiteral("Python int too large to convert to C long"));
    case Json::Type::String: {
        // int(str): surrounding whitespace, a sign, digits with single underscores.
        const QString text = strip(value.toString());
        qsizetype index = 0;
        bool negative = false;
        if (index < text.size() && (text.at(index) == QLatin1Char('+') || text.at(index) == QLatin1Char('-'))) {
            negative = text.at(index) == QLatin1Char('-');
            ++index;
        }
        QString digits;
        bool valid = index < text.size();
        bool previousDigit = false;
        for (; valid && index < text.size(); ++index) {
            const QChar c = text.at(index);
            if (c == QLatin1Char('_') && previousDigit && index + 1 < text.size()) {
                previousDigit = false;
                continue;
            }
            if (!c.isDigit()) {
                valid = false;
                break;
            }
            digits += QChar(u'0' + c.digitValue());
            previousDigit = true;
        }
        bool ok = false;
        const qint64 parsed = digits.toLongLong(&ok);
        if (!valid || !previousDigit || !ok) {
            raise(QStringLiteral("ValueError"), QStringLiteral("invalid literal for int() with base 10: ") + repr(value.toString()));
        }
        return negative ? -parsed : parsed;
    }
    default:
        raise(QStringLiteral("TypeError"),
              QStringLiteral("int() argument must be a string, a bytes-like object or a real number, not '%1'").arg(typeName(value)));
    }
}

QString pythonStr(const Json &value)
{
    switch (value.type()) {
    case Json::Type::Null:
        return QStringLiteral("None");
    case Json::Type::Bool:
        return value.toBool() ? QStringLiteral("True") : QStringLiteral("False");
    case Json::Type::Int:
        return QString::number(value.toInt());
    case Json::Type::BigInt:
        return QString::fromLatin1(value.bigIntDigits());
    case Json::Type::Float: {
        const double number = value.toDouble();
        if (std::isnan(number)) {
            return QStringLiteral("nan");
        }
        if (std::isinf(number)) {
            return number > 0 ? QStringLiteral("inf") : QStringLiteral("-inf");
        }
        return QString::fromLatin1(floatRepr(number));
    }
    case Json::Type::String:
        return value.toString();
    default:
        // Python would print the repr of the container; JSON is close enough.
        return QString::fromUtf8(dumps(value).value_or(QByteArray()));
    }
}

}

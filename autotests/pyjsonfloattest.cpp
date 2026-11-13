// SPDX-License-Identifier: GPL-2.0-or-later
// Floating-point numbers in the publisher's JSON (tools/publisher/pyjson.cpp).
// Sync and publication hashes depend on reading every JSON number to the same
// double as Python's json.loads, which uses float(): correctly rounded to
// nearest, ties to even, ±infinity past DBL_MAX and ±0 below the smallest
// subnormal. The expected bits below are what Python 3 gives;
// tools/publisher/tests/pyjson_differential.py compares many more literals
// with Python itself.
#include "../tools/publisher/pyjson.h"

#include <QTest>

#include <cfloat>
#include <charconv>
#include <clocale>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <random>
#include <string>

using Thoughts::Py::Json;

namespace
{
quint64 bitsOf(double value)
{
    quint64 bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

double fromBits(quint64 bits)
{
    double value = 0;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

QByteArray hex(quint64 bits)
{
    return QByteArrayLiteral("0x") + QByteArray::number(bits, 16).rightJustified(16, '0');
}

// The double a JSON document holding only this literal reads to.
std::optional<double> parsed(const QByteArray &literal)
{
    QString error;
    const auto json = Thoughts::Py::loads(literal, &error);
    if (!json || json->type() != Json::Type::Float) {
        return std::nullopt;
    }
    return json->toDouble();
}

// Exact decimal digits of factor * base^power.
QByteArray exactDigits(quint64 factor, unsigned base, int power)
{
    std::string digits = std::to_string(factor); // most significant first
    for (int step = 0; step < power; ++step) {
        unsigned carry = 0;
        for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
            const unsigned product = unsigned(*it - '0') * base + carry;
            *it = char('0' + product % 10);
            carry = product / 10;
        }
        while (carry) {
            digits.insert(digits.begin(), char('0' + carry % 10));
            carry /= 10;
        }
    }
    return QByteArray::fromStdString(digits);
}

QByteArray minusOne(QByteArray digits)
{
    qsizetype index = digits.size() - 1;
    for (; digits[index] == '0'; --index) {
        digits[index] = '9';
    }
    --digits[index];
    return digits;
}

// factor * 2^-power exactly, as factor * 5^power * 10^-power.
QByteArray exactTimesTwoToMinus(quint64 factor, int power)
{
    return exactDigits(factor, 5, power) + "e-" + QByteArray::number(power);
}

// factor * 2^power exactly, written as a float literal.
QByteArray exactTimesTwoTo(quint64 factor, int power)
{
    return exactDigits(factor, 2, power) + ".0";
}

QByteArray shortest(double value)
{
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof buffer, value);
    return QByteArray(buffer, result.ptr - buffer);
}

QByteArray scientific(double value, int precision)
{
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof buffer, value, std::chars_format::scientific, precision);
    return QByteArray(buffer, result.ptr - buffer);
}

// Restores LC_NUMERIC, which a test changes to show the parser ignores it.
class NumericLocale
{
public:
    NumericLocale() : m_saved(std::setlocale(LC_NUMERIC, nullptr)) { }
    ~NumericLocale() { std::setlocale(LC_NUMERIC, m_saved.c_str()); }
    bool set(const char *name) { return std::setlocale(LC_NUMERIC, name) != nullptr; }

private:
    std::string m_saved;
};
}

class PyJsonFloatTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void literals_data()
    {
        QTest::addColumn<QByteArray>("literal");
        QTest::addColumn<quint64>("bits");
        const auto row = [](const char *name, const QByteArray &literal, quint64 bits) {
            QTest::newRow(name) << literal << bits;
        };
        // Zeros keep their sign, whatever the exponent.
        row("zero", "0.0", 0x0000000000000000);
        row("negative zero", "-0.0", 0x8000000000000000);
        row("negative zero with exponent", "-0e0", 0x8000000000000000);
        row("zero, huge exponent", "0e999999999999999999999", 0x0000000000000000);
        row("negative zero, huge negative exponent", "-0.0E-999999999999999999999", 0x8000000000000000);
        // Exponent forms.
        row("exponent only", "1e0", 0x3FF0000000000000);
        row("capital E and plus", "1E+2", 0x4059000000000000);
        row("negative exponent", "1e-7", 0x3E7AD7F29ABCAF48);
        row("exponent with leading zeros", "1e+0000000000000000000000000308", 0x7FE1CCF385EBC8A0);
        row("fraction and exponent", "0.1e1", 0x3FF0000000000000);
        row("1e22, exact", "1e22", 0x4480F0CF064DD592);
        row("1e23, inexact", "1e23", 0x44B52D02C7E14AF6);
        // 17 significant digits and rounding to nearest.
        row("0.1", "0.1", 0x3FB999999999999A);
        row("0.1 + 0.2", "0.30000000000000004", 0x3FD3333333333334);
        row("1/3, 17 digits", "0.33333333333333331", 0x3FD5555555555555);
        row("pi, 17 digits", "3.1415926535897931", 0x400921FB54442D18);
        row("-e, 17 digits", "-2.7182818284590451", 0xC005BF0A8B145769);
        row("next after 1", "1.0000000000000002", 0x3FF0000000000001);
        row("18 digits", "123456789012345678.0", 0x437B69B4BA630F35);
        row("2^53 + 1 ties to even (down)", "9007199254740993.0", 0x4340000000000000);
        row("2^53 + 3 ties to even (up)", "9007199254740995.0", 0x4340000000000002);
        row("exact tie after 1 goes down", exactTimesTwoToMinus((quint64(1) << 53) + 1, 53), 0x3FF0000000000000);
        row("just above that tie", exactDigits((quint64(1) << 53) + 1, 5, 53) + "1e-54", 0x3FF0000000000001);
        row("exact tie after next(1) goes up", exactTimesTwoToMinus((quint64(1) << 53) + 3, 53), 0x3FF0000000000002);
        row("800-digit mantissa", "0." + QByteArray(800, '1'), 0x3FBC71C71C71C71C);
        row("400-digit integer part", QByteArray(400, '9') + ".5e-100", 0x7E37E43C8800759C);
        // Subnormals and the bottom of the range.
        row("smallest subnormal", "5e-324", 0x0000000000000001);
        row("smallest subnormal, 17 digits", "4.9406564584124654e-324", 0x0000000000000001);
        row("negative smallest subnormal", "-5e-324", 0x8000000000000001);
        row("just above half the smallest subnormal", "2.4703282292062328e-324", 0x0000000000000001);
        row("just below half the smallest subnormal", "2.4703282292062327e-324", 0x0000000000000000);
        row("exactly half the smallest subnormal ties to 0", exactTimesTwoToMinus(1, 1075), 0x0000000000000000);
        row("negative exact half ties to -0", "-" + exactTimesTwoToMinus(1, 1075), 0x8000000000000000);
        row("one digit above the exact half", exactDigits(1, 5, 1075) + "1e-1076", 0x0000000000000001);
        row("subnormal", "1.23456789e-310", 0x000016B9F4D3C385);
        row("subnormal from the exponent", "12345e-315", 0x00000245C37E0D77);
        row("largest subnormal", "2.2250738585072009e-308", 0x000FFFFFFFFFFFFF);
        row("largest subnormal, 2.2250738585072011e-308", "2.2250738585072011e-308", 0x000FFFFFFFFFFFFF);
        row("smallest normal", "2.2250738585072014e-308", 0x0010000000000000);
        row("tie between largest subnormal and smallest normal", exactTimesTwoToMinus((quint64(1) << 53) - 1, 1075), 0x0010000000000000);
        row("underflow", "1e-400", 0x0000000000000000);
        row("negative underflow", "-1e-400", 0x8000000000000000);
        row("far underflow", "1e-999999999999999999999", 0x0000000000000000);
        // The top of the range: DBL_MAX, the tie with 2^1024 and overflow.
        row("DBL_MAX", "1.7976931348623157e308", 0x7FEFFFFFFFFFFFFF);
        row("negative DBL_MAX", "-1.7976931348623157E+308", 0xFFEFFFFFFFFFFFFF);
        row("half of 2^1024", "8.98846567431158e307", 0x7FE0000000000000);
        row("just below the DBL_MAX tie", "1.7976931348623158e308", 0x7FEFFFFFFFFFFFFF);
        row("just above the DBL_MAX tie", "1.7976931348623159e308", 0x7FF0000000000000);
        row("exact DBL_MAX tie overflows", exactTimesTwoTo((quint64(1) << 54) - 1, 970), 0x7FF0000000000000);
        row("one below the exact tie", minusOne(exactDigits((quint64(1) << 54) - 1, 2, 970)) + ".0", 0x7FEFFFFFFFFFFFFF);
        row("negative overflow", "-1.7976931348623159e308", 0xFFF0000000000000);
        row("overflow", "1e309", 0x7FF0000000000000);
        row("overflow with a fraction", "1.5e400", 0x7FF0000000000000);
        row("far overflow", "1e999999999999999999999", 0x7FF0000000000000);
    }

    void literals()
    {
        QFETCH(QByteArray, literal);
        QFETCH(quint64, bits);
        const auto value = parsed(literal);
        QVERIFY2(value, literal.left(80).constData());
        QCOMPARE(hex(bitsOf(*value)), hex(bits));
        // Inside a document, as Sync and the publisher read it.
        const auto document = Thoughts::Py::loads(QByteArray(QByteArray("{\"a\": [1, ") + literal + ", true]}"));
        QVERIFY(document);
        QCOMPARE(hex(bitsOf(document->value(QStringLiteral("a")).items().at(1).toDouble())), hex(bits));
    }

    void exactTiesAreTheExpectedNumbers()
    {
        // The helper's exact decimals, checked against values written out.
        QCOMPARE(exactTimesTwoToMinus(1, 3), QByteArray("125e-3"));
        QCOMPARE(exactTimesTwoTo(3, 4), QByteArray("48.0"));
        QVERIFY(exactTimesTwoToMinus(1, 1075).startsWith("24703282292062327208828439643411068618252990130716238221279284125033775363510437593264991818081799618989828234772285886546332835517796989819938739800539093906315035659515570226392290858392449105184435931802849936536152500319370457678249219365623669863658480757001585769269903706311928279558551332927834338409351978015531246597263579574622766465272827220056374006485499977096599470454020828166226237857393450736339007967761930577506740176324673600968951340535537458516661134223766678604162159680461914467291840300530057530849048765391711386591646239524912623653881879636239373280423891018672348497668235089863388587925628302755995657524455507255189313690836254779186948667994968324049705821028513185451396213837722826145437693412532098591327667236328125"));
        QCOMPARE(exactTimesTwoTo((quint64(1) << 54) - 1, 970).left(17), QByteArray("17976931348623158"));
    }

    void roundTrips()
    {
        // repr (what dumps writes) and 17 significant digits read back to the same bits.
        QList<double> values = {0.0, -0.0, 0.1, 1.0 / 3, -2.5, 1e16, 1e-5, 123456789012345678.0,
                                DBL_MAX, -DBL_MAX, std::nextafter(DBL_MAX, 0.0), DBL_MIN, -DBL_MIN,
                                std::nextafter(DBL_MIN, 0.0), std::nextafter(DBL_MIN, 1.0), DBL_TRUE_MIN, -DBL_TRUE_MIN,
                                2 * DBL_TRUE_MIN, fromBits(0x0008000000000000), 9007199254740993.0, HUGE_VAL, -HUGE_VAL};
        std::mt19937_64 random(20261111);
        for (int index = 0; index < 100000; ++index) {
            const double value = fromBits(random());
            if (std::isfinite(value)) values << value;
        }
        for (int index = 0; index < 20000; ++index) {
            values << fromBits(random() % 0x0010000000000000); // subnormals
            values << fromBits(0x7FEFFFFFFFFFFFFF - random() % 0x0000010000000000); // just below DBL_MAX
            values << fromBits(0x0010000000000000 + random() % 0x0000010000000000); // just above DBL_MIN
        }
        for (const double value : std::as_const(values)) {
            const auto dumped = Thoughts::Py::dumps(Json::array({Json(value)}));
            QVERIFY(dumped);
            const auto loaded = Thoughts::Py::loads(*dumped);
            QVERIFY(loaded);
            if (bitsOf(loaded->items().at(0).toDouble()) != bitsOf(value)) {
                QFAIL(qPrintable(QStringLiteral("%1 read back as %2").arg(QString::fromLatin1(*dumped), QString::number(loaded->items().at(0).toDouble(), 'g', 17))));
            }
            if (!std::isfinite(value)) continue;
            const QByteArray shortestText = shortest(value);
            const QByteArray texts[] = {Thoughts::Py::floatRepr(value), scientific(value, 16), scientific(value, 20),
                                        shortestText.contains('e') ? shortestText : QByteArray(shortestText + "e0")};
            for (const QByteArray &text : texts) {
                const auto back = parsed(text);
                QVERIFY2(back, text.constData());
                if (bitsOf(*back) != bitsOf(value)) {
                    QFAIL(qPrintable(QStringLiteral("%1 read back as %2 (%3, expected %4)").arg(QString::fromLatin1(text), QString::number(*back, 'g', 17), QString::fromLatin1(hex(bitsOf(*back))), QString::fromLatin1(hex(bitsOf(value))))));
                }
            }
        }
    }

    void matchesFromChars()
    {
        // On macOS the parser uses strtod_l instead of std::from_chars, which Apple's
        // libc++ has only from macOS 26.0. Where both exist, they agree on every literal.
#if defined(__APPLE__) && defined(_LIBCPP_AVAILABILITY_HAS_FROM_CHARS_FLOATING_POINT) && _LIBCPP_AVAILABILITY_HAS_FROM_CHARS_FLOATING_POINT
        NumericLocale locale;
        QVERIFY(locale.set("C")); // for the reference's strtod fallback
        std::mt19937_64 random(1111);
        const auto digit = [&random] { return char('0' + random() % 10); };
        int compared = 0;
        for (int index = 0; index < 400000; ++index) {
            std::string literal = random() % 2 ? "-" : "";
            if (random() % 4 == 0) {
                const double value = fromBits(random());
                if (!std::isfinite(value)) continue;
                literal += scientific(std::fabs(value), int(random() % 40)).toStdString();
                if (literal.find('e') == std::string::npos) literal += "e0";
            } else {
                const bool longer = random() % 3 == 0;
                literal += random() % 8 == 0 ? '0' : char('1' + random() % 9);
                if (literal.back() != '0') {
                    for (int count = int(random() % (longer ? 40 : 20)); count > 0; --count) literal += digit();
                }
                const bool fraction = random() % 2;
                if (fraction) {
                    literal += '.';
                    for (int count = 1 + int(random() % (longer ? 60 : 20)); count > 0; --count) literal += digit();
                }
                if (!fraction || random() % 3) {
                    literal += random() % 2 ? 'e' : 'E';
                    const int sign = int(random() % 3);
                    literal += sign == 0 ? "" : sign == 1 ? "+" : "-";
                    literal += std::to_string(random() % 400);
                }
            }
            double expected = 0;
            const auto result = std::from_chars(literal.data(), literal.data() + literal.size(), expected);
            QCOMPARE(result.ptr, literal.data() + literal.size());
            if (result.ec == std::errc::result_out_of_range) {
                expected = std::strtod(literal.c_str(), nullptr);
            }
            const auto value = parsed(QByteArray::fromStdString(literal));
            QVERIFY2(value, literal.c_str());
            if (bitsOf(*value) != bitsOf(expected)) {
                QFAIL(qPrintable(QStringLiteral("%1: %2, std::from_chars %3").arg(QString::fromStdString(literal), QString::fromLatin1(hex(bitsOf(*value))), QString::fromLatin1(hex(bitsOf(expected))))));
            }
            ++compared;
        }
        QVERIFY(compared > 300000);
#else
        QSKIP("std::from_chars for double is not available to compare with");
#endif
    }

    void independentOfTheLocale()
    {
        // Qt sets the user's locale on Unix; a decimal comma must not change any number.
        NumericLocale locale;
        bool comma = false;
        for (const char *name : {"de_DE.UTF-8", "de_CH.UTF-8", "fr_FR.UTF-8", "de_DE", "fr_FR", "German_Germany.1252"}) {
            if (locale.set(name) && std::strcmp(std::localeconv()->decimal_point, ",") == 0) {
                comma = true;
                break;
            }
        }
        if (!comma) {
            QSKIP("no locale with a decimal comma is installed");
        }
        QCOMPARE(hex(bitsOf(parsed("1.5").value_or(0))), hex(0x3FF8000000000000));
        QCOMPARE(hex(bitsOf(parsed("0.30000000000000004").value_or(0))), hex(0x3FD3333333333334));
        QCOMPARE(hex(bitsOf(parsed("2.4703282292062328e-324").value_or(0))), hex(0x0000000000000001));
        QCOMPARE(hex(bitsOf(parsed("1.5e400").value_or(0))), hex(0x7FF0000000000000));
        QCOMPARE(hex(bitsOf(parsed("-2.5e-400").value_or(1))), hex(0x8000000000000000));
        QCOMPARE(hex(bitsOf(parsed("1.7976931348623159e308").value_or(0))), hex(0x7FF0000000000000));
    }
};

QTEST_GUILESS_MAIN(PyJsonFloatTest)
#include "pyjsonfloattest.moc"

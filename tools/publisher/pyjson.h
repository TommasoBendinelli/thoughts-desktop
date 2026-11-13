// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>

#include <memory>
#include <optional>

// JSON exactly as Python's json module reads and writes it. The native
// publisher hashes and stores JSON that the Python publisher and the server
// also produce, so key order, number formatting and escaping must match
// json.dumps byte for byte; QJsonDocument sorts keys and formats numbers its
// own way.
namespace Thoughts::Py
{

class Json
{
public:
    enum class Type { Null, Bool, Int, BigInt, Float, String, Array, Object };
    using Member = QPair<QString, Json>;

    Json();
    Json(std::nullptr_t);
    Json(bool value);
    Json(int value);
    Json(qint64 value);
    Json(double value);
    Json(const char *value);
    Json(const QString &value);
    static Json array(QList<Json> items = {});
    static Json object(QList<Member> members = {});
    // An integer outside qint64, kept as Python's exact decimal digits.
    static Json bigInt(const QByteArray &digits);

    Type type() const { return m_type; }
    bool isNull() const { return m_type == Type::Null; }
    bool isBool() const { return m_type == Type::Bool; }
    bool isInt() const { return m_type == Type::Int; }
    bool isNumber() const { return m_type == Type::Int || m_type == Type::BigInt || m_type == Type::Float; }
    bool isString() const { return m_type == Type::String; }
    bool isArray() const { return m_type == Type::Array; }
    bool isObject() const { return m_type == Type::Object; }

    bool toBool(bool fallback = false) const;
    qint64 toInt(qint64 fallback = 0) const;
    double toDouble(double fallback = 0) const;
    // Decimal digits of a BigInt.
    QByteArray bigIntDigits() const { return m_bigInt; }
    QString toString(const QString &fallback = {}) const;
    // On a temporary these return copies, so a range-for over
    // f().items() never iterates a destroyed list.
    const QList<Json> &items() const &;
    QList<Json> items() const &&;
    QList<Json> &items() &;
    const QList<Member> &members() const &;
    QList<Member> members() const &&;
    QList<Member> &members() &;

    // Object access in insertion order, like a Python dict.
    bool contains(const QString &key) const;
    const Json &value(const QString &key) const &;
    Json value(const QString &key) const &&;
    const Json &operator[](const QString &key) const & { return value(key); }
    // Replaces the value in place, or appends a new key at the end.
    void insert(const QString &key, const Json &value);
    bool remove(const QString &key);
    QStringList keys() const;
    void append(const Json &value);
    qsizetype size() const;

    bool operator==(const Json &other) const;
    bool operator!=(const Json &other) const { return !(*this == other); }

private:
    Type m_type = Type::Null;
    bool m_bool = false;
    qint64 m_int = 0;
    double m_double = 0;
    QByteArray m_bigInt;
    QString m_string;
    std::shared_ptr<QList<Json>> m_items;
    std::shared_ptr<QList<Member>> m_members;
    void detach();
};

struct DumpOptions {
    // json.dumps(indent=...); nullopt is Python's None.
    std::optional<int> indent;
    // json.dumps(separators=...); nullopt picks Python's defaults for indent.
    std::optional<QPair<QString, QString>> separators;
    bool sortKeys = false;
    bool ensureAscii = true;
    bool allowNan = true;
};

// json.dumps(value, **options) as UTF-8. Fails (returns nullopt, with error)
// where Python raises, e.g. NaN with allow_nan=False.
std::optional<QByteArray> dumps(const Json &value, const DumpOptions &options = {}, QString *error = nullptr);
// Python's repr(float).
QByteArray floatRepr(double value);

struct LoadOptions {
    // object_pairs_hook rejecting duplicate keys; otherwise the last wins.
    bool rejectDuplicateKeys = false;
    // The hook's ValueError text (%1 is the key).
    QString duplicateKeyMessage = QStringLiteral("duplicate JSON object key: %1");
    // Set when parsing stopped at a duplicate key rather than a syntax error.
    bool *duplicateKeyFound = nullptr;
};

// json.loads(text). Error messages follow Python's JSONDecodeError wording.
std::optional<Json> loads(const QString &text, QString *error = nullptr, const LoadOptions &options = {});
// json.loads(bytes): Python decodes UTF-8 (rejecting a UTF-8 BOM in text
// mode is the caller's choice; bytes with a BOM are accepted by json.loads).
std::optional<Json> loads(const QByteArray &utf8, QString *error = nullptr, const LoadOptions &options = {});

// bytes.decode("utf-8") as Python does it: a leading BOM is kept as U+FEFF
// (Qt drops it), and invalid bytes fail with Python's UnicodeDecodeError text.
// surrogatePass accepts encoded surrogates (errors="surrogatepass").
std::optional<QString> decodeUtf8(const QByteArray &bytes, QString *error = nullptr, bool surrogatePass = false);

// Python's str ordering (by code point, not UTF-16 unit).
bool codePointLess(const QString &left, const QString &right);

}

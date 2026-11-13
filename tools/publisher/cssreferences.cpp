// SPDX-License-Identifier: GPL-2.0-or-later
#include "cssreferences.h"

#include "pystr.h"

#include <QList>

#include <string>

namespace Thoughts::Publisher
{
namespace
{
using Text = std::u32string;

struct Node {
    enum class Kind { Whitespace, Literal, Ident, AtKeyword, Hash, Number, UnicodeRange, String, Url, Error, Function, Curly, Square, Paren };
    Kind kind = Kind::Literal;
    Text value;
    QList<Node> children;
};

bool isHex(char32_t c)
{
    return (c >= U'0' && c <= U'9') || (c >= U'a' && c <= U'f') || (c >= U'A' && c <= U'F');
}

bool isDigit(char32_t c)
{
    return c >= U'0' && c <= U'9';
}

bool isWhitespace(char32_t c)
{
    return c == U' ' || c == U'\n' || c == U'\t';
}

Text asciiLower(const Text &text)
{
    Text result = text;
    for (char32_t &c : result) {
        if (c >= U'A' && c <= U'Z') {
            c += 32;
        }
    }
    return result;
}

// tinycss2.tokenizer.parse_component_value_list(css, skip_comments=True)
class Tokenizer
{
public:
    explicit Tokenizer(const QString &input)
    {
        const Text raw = Py::toCodePoints(input);
        for (size_t index = 0; index < raw.size(); ++index) {
            const char32_t c = raw[index];
            if (c == U'\0') {
                m_css.push_back(0xFFFD);
            } else if (c == U'\r') {
                m_css.push_back(U'\n');
                if (index + 1 < raw.size() && raw[index + 1] == U'\n') {
                    ++index;
                }
            } else if (c == U'\f') {
                m_css.push_back(U'\n');
            } else {
                m_css.push_back(c);
            }
        }
    }

    QList<Node> tokens()
    {
        QList<Node> root;
        // The open blocks: where their tokens go and what closes them.
        QList<QPair<QList<Node> *, char32_t>> stack;
        QList<Node> *tokens = &root;
        char32_t endChar = 0;
        const size_t length = m_css.size();
        size_t pos = 0;
        const auto startsWith = [this](size_t position, const char32_t *prefix) {
            const Text needle(prefix);
            return m_css.compare(position, needle.size(), needle) == 0 && position + needle.size() <= m_css.size();
        };
        const auto open = [&](Node::Kind kind, const Text &name, char32_t closing) {
            tokens->append(Node{kind, name, {}});
            stack.append({tokens, endChar});
            tokens = &tokens->last().children;
            endChar = closing;
        };
        while (pos < length) {
            const char32_t c = m_css[pos];
            if (isWhitespace(c)) {
                while (pos < length && isWhitespace(m_css[pos])) {
                    ++pos;
                }
                tokens->append(Node{Node::Kind::Whitespace, {}, {}});
                continue;
            }
            if ((c == U'U' || c == U'u') && pos + 2 < length && m_css[pos + 1] == U'+' && (isHex(m_css[pos + 2]) || m_css[pos + 2] == U'?')) {
                pos = consumeUnicodeRange(pos + 2);
                tokens->append(Node{Node::Kind::UnicodeRange, {}, {}});
                continue;
            }
            if (startsWith(pos, U"-->")) {
                tokens->append(Node{Node::Kind::Literal, U"-->", {}});
                pos += 3;
                continue;
            }
            if (isIdentStart(pos)) {
                Text value;
                pos = consumeIdent(pos, value);
                if (pos >= length || m_css[pos] != U'(') {
                    tokens->append(Node{Node::Kind::Ident, value, {}});
                    continue;
                }
                ++pos;
                if (asciiLower(value) == U"url") {
                    size_t urlPos = pos;
                    while (urlPos < length && isWhitespace(m_css[urlPos])) {
                        ++urlPos;
                    }
                    if (urlPos >= length || (m_css[urlPos] != U'"' && m_css[urlPos] != U'\'')) {
                        std::optional<Text> url;
                        bool error = false;
                        pos = consumeUrl(pos, url, error);
                        if (url) {
                            tokens->append(Node{Node::Kind::Url, *url, {}});
                        }
                        if (error) {
                            tokens->append(Node{Node::Kind::Error, {}, {}});
                        }
                        continue;
                    }
                }
                // Functions nest like blocks; this one is closed by ")".
                open(Node::Kind::Function, value, U')');
                continue;
            }
            if (const size_t end = numberEnd(pos); end != pos) {
                pos = end;
                if (pos < length && isIdentStart(pos)) {
                    Text unit;
                    pos = consumeIdent(pos, unit);
                } else if (pos < length && m_css[pos] == U'%') {
                    ++pos;
                }
                tokens->append(Node{Node::Kind::Number, {}, {}});
            } else if (c == U'@') {
                ++pos;
                if (pos < length && isIdentStart(pos)) {
                    Text value;
                    pos = consumeIdent(pos, value);
                    tokens->append(Node{Node::Kind::AtKeyword, value, {}});
                } else {
                    tokens->append(Node{Node::Kind::Literal, U"@", {}});
                }
            } else if (c == U'#') {
                ++pos;
                if (pos < length
                    && (isNameChar(m_css[pos]) || (m_css[pos] == U'\\' && !(pos + 1 < length && m_css[pos + 1] == U'\n')))) {
                    Text value;
                    pos = consumeIdent(pos, value);
                    tokens->append(Node{Node::Kind::Hash, value, {}});
                } else {
                    tokens->append(Node{Node::Kind::Literal, U"#", {}});
                }
            } else if (c == U'{') {
                open(Node::Kind::Curly, {}, U'}');
                ++pos;
            } else if (c == U'[') {
                open(Node::Kind::Square, {}, U']');
                ++pos;
            } else if (c == U'(') {
                open(Node::Kind::Paren, {}, U')');
                ++pos;
            } else if (endChar != 0 && c == endChar) {
                tokens = stack.last().first;
                endChar = stack.last().second;
                stack.removeLast();
                ++pos;
            } else if (c == U'}' || c == U']' || c == U')') {
                tokens->append(Node{Node::Kind::Error, {}, {}});
                ++pos;
            } else if (c == U'"' || c == U'\'') {
                std::optional<Text> value;
                bool error = false;
                pos = consumeQuotedString(pos, value, error);
                if (value) {
                    tokens->append(Node{Node::Kind::String, *value, {}});
                }
                if (error) {
                    tokens->append(Node{Node::Kind::Error, {}, {}});
                }
            } else if (startsWith(pos, U"/*")) {
                const size_t close = m_css.find(U"*/", pos + 2);
                if (close == Text::npos) {
                    break;
                }
                pos = close + 2;
            } else if (startsWith(pos, U"<!--")) {
                tokens->append(Node{Node::Kind::Literal, U"<!--", {}});
                pos += 4;
            } else if (startsWith(pos, U"||")) {
                tokens->append(Node{Node::Kind::Literal, U"||", {}});
                pos += 2;
            } else if (c == U'~' || c == U'|' || c == U'^' || c == U'$' || c == U'*') {
                ++pos;
                if (pos < length && m_css[pos] == U'=') {
                    ++pos;
                    tokens->append(Node{Node::Kind::Literal, Text{c, U'='}, {}});
                } else {
                    tokens->append(Node{Node::Kind::Literal, Text(1, c), {}});
                }
            } else {
                tokens->append(Node{Node::Kind::Literal, Text(1, c), {}});
                ++pos;
            }
        }
        return root;
    }

private:
    Text m_css;

    static bool isNameStartChar(char32_t c)
    {
        return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z') || c == U'_' || c > 0x7F;
    }

    static bool isNameChar(char32_t c)
    {
        return isNameStartChar(c) || isDigit(c) || c == U'-';
    }

    bool validEscapeAt(size_t pos) const
    {
        return pos < m_css.size() && m_css[pos] == U'\\' && !(pos + 1 < m_css.size() && m_css[pos + 1] == U'\n');
    }

    bool isIdentStart(size_t pos) const
    {
        const char32_t c = m_css[pos];
        if (isNameStartChar(c)) {
            return true;
        }
        if (c == U'-') {
            ++pos;
            return (pos < m_css.size() && (isNameStartChar(m_css[pos]) || m_css[pos] == U'-')) || validEscapeAt(pos);
        }
        if (c == U'\\') {
            return validEscapeAt(pos);
        }
        return false;
    }

    size_t consumeEscape(size_t pos, Text &out) const
    {
        size_t end = pos;
        while (end < m_css.size() && end - pos < 6 && isHex(m_css[end])) {
            ++end;
        }
        if (end > pos) {
            char32_t codePoint = 0;
            for (size_t index = pos; index < end; ++index) {
                const char32_t c = m_css[index];
                codePoint = codePoint * 16 + (isDigit(c) ? c - U'0' : (c >= U'a' ? c - U'a' + 10 : c - U'A' + 10));
            }
            out.push_back(codePoint > 0 && codePoint <= 0x10FFFF ? codePoint : char32_t(0xFFFD));
            if (end < m_css.size() && isWhitespace(m_css[end])) {
                ++end;
            }
            return end;
        }
        if (pos < m_css.size()) {
            out.push_back(m_css[pos]);
            return pos + 1;
        }
        out.push_back(0xFFFD);
        return pos;
    }

    size_t consumeIdent(size_t pos, Text &value) const
    {
        while (pos < m_css.size()) {
            const char32_t c = m_css[pos];
            if (isNameChar(c)) {
                value.push_back(c);
                ++pos;
            } else if (validEscapeAt(pos)) {
                pos = consumeEscape(pos + 1, value);
            } else {
                break;
            }
        }
        return pos;
    }

    size_t consumeQuotedString(size_t pos, std::optional<Text> &value, bool &error) const
    {
        const char32_t quote = m_css[pos];
        ++pos;
        Text chunks;
        while (pos < m_css.size()) {
            const char32_t c = m_css[pos];
            if (c == quote) {
                value = chunks;
                return pos + 1;
            }
            if (c == U'\\') {
                ++pos;
                if (pos < m_css.size()) {
                    if (m_css[pos] == U'\n') {
                        ++pos;
                    } else {
                        pos = consumeEscape(pos, chunks);
                    }
                }
            } else if (c == U'\n') {
                // Bad string: no token.
                error = true;
                return pos;
            } else {
                chunks.push_back(c);
                ++pos;
            }
        }
        value = chunks;
        error = true;
        return pos;
    }

    size_t consumeUrl(size_t pos, std::optional<Text> &value, bool &error) const
    {
        const size_t length = m_css.size();
        while (pos < length && isWhitespace(m_css[pos])) {
            ++pos;
        }
        if (pos >= length) {
            value = Text();
            error = true;
            return pos;
        }
        const char32_t first = m_css[pos];
        bool good = true;
        if (first == U'"' || first == U'\'') {
            pos = consumeQuotedString(pos, value, error);
            good = value.has_value();
        } else if (first == U')') {
            value = Text();
            return pos + 1;
        } else {
            Text chunks;
            for (;;) {
                if (pos >= length) {
                    value = chunks;
                    error = true;
                    return pos;
                }
                const char32_t c = m_css[pos];
                if (c == U')') {
                    value = chunks;
                    return pos + 1;
                }
                if (isWhitespace(c)) {
                    value = chunks;
                    ++pos;
                    break;
                }
                if (validEscapeAt(pos)) {
                    pos = consumeEscape(pos + 1, chunks);
                } else if (c == U'"' || c == U'\'' || c == U'(' || c <= 0x08 || c == 0x0B || (c >= 0x0E && c <= 0x1F) || c == 0x7F) {
                    good = false;
                    ++pos;
                    break;
                } else {
                    chunks.push_back(c);
                    ++pos;
                }
            }
        }
        if (good) {
            while (pos < length && isWhitespace(m_css[pos])) {
                ++pos;
            }
            if (pos < length) {
                if (m_css[pos] == U')') {
                    return pos + 1;
                }
            } else {
                error = true;
                return pos;
            }
        }
        // The remnants of a bad URL: no token.
        while (pos < length) {
            if (m_css[pos] == U'\\' && pos + 1 < length && m_css[pos + 1] == U')') {
                pos += 2;
            } else if (m_css[pos] == U')') {
                ++pos;
                break;
            } else {
                ++pos;
            }
        }
        value.reset();
        error = true;
        return pos;
    }

    size_t consumeUnicodeRange(size_t pos) const
    {
        const size_t length = m_css.size();
        size_t maximum = std::min(pos + 6, length);
        while (pos < maximum && isHex(m_css[pos])) {
            ++pos;
        }
        size_t marks = 0;
        while (pos < maximum && m_css[pos] == U'?') {
            ++pos;
            ++marks;
        }
        if (!marks && pos + 1 < length && m_css[pos] == U'-' && isHex(m_css[pos + 1])) {
            ++pos;
            maximum = std::min(pos + 6, length);
            while (pos < maximum && isHex(m_css[pos])) {
                ++pos;
            }
        }
        return pos;
    }

    // _NUMBER_RE: [-+]?([0-9]*\.)?[0-9]+([eE][+-]?[0-9]+)?
    size_t numberEnd(size_t pos) const
    {
        const size_t length = m_css.size();
        size_t p = pos;
        if (p < length && (m_css[p] == U'+' || m_css[p] == U'-')) {
            ++p;
        }
        size_t q = p;
        while (q < length && isDigit(m_css[q])) {
            ++q;
        }
        size_t end;
        if (q < length && m_css[q] == U'.' && q + 1 < length && isDigit(m_css[q + 1])) {
            end = q + 1;
            while (end < length && isDigit(m_css[end])) {
                ++end;
            }
        } else if (q > p) {
            end = q;
        } else {
            return pos;
        }
        if (end < length && (m_css[end] == U'e' || m_css[end] == U'E')) {
            size_t r = end + 1;
            if (r < length && (m_css[r] == U'+' || m_css[r] == U'-')) {
                ++r;
            }
            if (r < length && isDigit(m_css[r])) {
                while (r < length && isDigit(m_css[r])) {
                    ++r;
                }
                end = r;
            }
        }
        return end;
    }
};

bool isLiteral(const Node &node, const char32_t *value)
{
    return node.kind == Node::Kind::Literal && node.value == value;
}

struct Rule {
    bool atRule = false;
    Text keyword;
    QList<Node> prelude;
    std::optional<QList<Node>> content;
};

// parse_stylesheet(tokens, skip_comments=True, skip_whitespace=True)
QList<Rule> parseStylesheet(const QList<Node> &tokens)
{
    QList<Rule> rules;
    qsizetype index = 0;
    while (index < tokens.size()) {
        const Node &first = tokens.at(index++);
        if (first.kind == Node::Kind::Whitespace || isLiteral(first, U"<!--") || isLiteral(first, U"-->")) {
            continue;
        }
        Rule rule;
        if (first.kind == Node::Kind::AtKeyword) {
            rule.atRule = true;
            rule.keyword = first.value;
            while (index < tokens.size()) {
                const Node &token = tokens.at(index++);
                if (token.kind == Node::Kind::Curly) {
                    rule.content = token.children;
                    break;
                }
                if (isLiteral(token, U";")) {
                    break;
                }
                rule.prelude.append(token);
            }
            rules.append(rule);
            continue;
        }
        if (first.kind == Node::Kind::Curly) {
            rule.content = first.children;
            rules.append(rule);
            continue;
        }
        rule.prelude.append(first);
        bool closed = false;
        while (index < tokens.size()) {
            const Node &token = tokens.at(index++);
            if (token.kind == Node::Kind::Curly) {
                rule.content = token.children;
                closed = true;
                break;
            }
            rule.prelude.append(token);
        }
        // A qualified rule cut off by the end of the file is a parse error.
        if (closed) {
            rules.append(rule);
        }
    }
    return rules;
}

void collect(const QList<Node> &nodes, QStringList &references);

void collectNode(const Node &node, QStringList &references)
{
    if (node.kind == Node::Kind::Url) {
        references.append(Py::fromCodePoints(node.value));
    } else if (node.kind == Node::Kind::Function && asciiLower(node.value) == U"url") {
        for (const Node &argument : node.children) {
            if (argument.kind == Node::Kind::String) {
                references.append(Py::fromCodePoints(argument.value));
            }
        }
    }
    if (node.kind == Node::Kind::Function || node.kind == Node::Kind::Curly || node.kind == Node::Kind::Square
        || node.kind == Node::Kind::Paren) {
        collect(node.children, references);
    }
}

void collect(const QList<Node> &nodes, QStringList &references)
{
    for (const Node &node : nodes) {
        collectNode(node, references);
    }
}
}

QStringList stylesheetReferences(const QString &css)
{
    QStringList references;
    for (const Rule &rule : parseStylesheet(Tokenizer(css).tokens())) {
        if (rule.atRule && asciiLower(rule.keyword) == U"import") {
            for (const Node &token : rule.prelude) {
                if (token.kind == Node::Kind::String) {
                    references.append(Py::fromCodePoints(token.value));
                }
            }
        }
        collect(rule.prelude, references);
        if (rule.content) {
            collect(*rule.content, references);
        }
    }
    return references;
}

}

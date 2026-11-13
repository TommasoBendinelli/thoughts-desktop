// SPDX-License-Identifier: GPL-2.0-or-later
#include "thoughtignore.h"

#include "pyerror.h"
#include "pyjson.h"
#include "pystr.h"
#include "sourceowner.h"

#include <vector>

namespace Thoughts::Publisher
{
namespace
{
constexpr qint64 maximumSize = 4 * 1024 * 1024;

QList<ThoughtIgnore::Token> compile(const std::u32string &pattern)
{
    using Kind = ThoughtIgnore::Token::Kind;
    QList<ThoughtIgnore::Token> tokens;
    size_t index = 0;
    while (index < pattern.size()) {
        const char32_t character = pattern[index];
        if (character == U'*') {
            if (index + 1 < pattern.size() && pattern[index + 1] == U'*') {
                index += 2;
                if (index < pattern.size() && pattern[index] == U'/') {
                    tokens.append({Kind::DirectoryPrefix});
                    ++index;
                } else {
                    tokens.append({Kind::DotStar});
                }
                continue;
            }
            tokens.append({Kind::Star});
        } else if (character == U'?') {
            tokens.append({Kind::AnyChar});
        } else {
            tokens.append({Kind::Literal, character});
        }
        ++index;
    }
    return tokens;
}

// re.fullmatch of the translated pattern: "." stops at a newline, [^/] does not.
class Matcher
{
public:
    Matcher(const QList<ThoughtIgnore::Token> &tokens, const std::u32string &text)
        : m_tokens(tokens)
        , m_text(text)
        , m_memo((size_t(tokens.size()) + 1) * (text.size() + 1), -1)
    {
    }

    bool matches()
    {
        return match(0, 0);
    }

private:
    const QList<ThoughtIgnore::Token> &m_tokens;
    const std::u32string &m_text;
    std::vector<signed char> m_memo;

    bool match(qsizetype token, size_t position)
    {
        signed char &memo = m_memo[size_t(token) * (m_text.size() + 1) + position];
        if (memo >= 0) {
            return memo;
        }
        memo = compute(token, position);
        return memo;
    }

    bool compute(qsizetype token, size_t position)
    {
        using Kind = ThoughtIgnore::Token::Kind;
        if (token == m_tokens.size()) {
            return position == m_text.size();
        }
        const ThoughtIgnore::Token &current = m_tokens.at(token);
        switch (current.kind) {
        case Kind::Literal:
            return position < m_text.size() && m_text[position] == current.literal && match(token + 1, position + 1);
        case Kind::AnyChar:
            return position < m_text.size() && m_text[position] != U'/' && match(token + 1, position + 1);
        case Kind::Star:
            for (size_t end = position;; ++end) {
                if (match(token + 1, end)) {
                    return true;
                }
                if (end >= m_text.size() || m_text[end] == U'/') {
                    return false;
                }
            }
        case Kind::DotStar:
            for (size_t end = position;; ++end) {
                if (match(token + 1, end)) {
                    return true;
                }
                if (end >= m_text.size() || m_text[end] == U'\n') {
                    return false;
                }
            }
        case Kind::DirectoryPrefix:
            // (?:.*/)?
            if (match(token + 1, position)) {
                return true;
            }
            for (size_t end = position; end < m_text.size() && m_text[end] != U'\n'; ++end) {
                if (m_text[end] == U'/' && match(token + 1, end + 1)) {
                    return true;
                }
            }
            return false;
        }
        return false;
    }
};
}

ThoughtIgnore::ThoughtIgnore(const Py::Path &root)
{
    const Py::Path path = root / QStringLiteral(".ThoughtIgnore");
    if (Py::isSymlink(path) || (Py::exists(path) && !Py::isFile(path))) {
        Py::raiseValueError(QStringLiteral(".ThoughtIgnore must be a regular file"));
    }
    if (Py::isFile(path)) {
        const Py::FileStatus status = Py::status(path);
        if (status.kind == Py::FileStatus::Kind::Missing) {
            Py::raiseOsError(status.error, path.str());
        }
        if (status.size > maximumSize) {
            Py::raiseValueError(QStringLiteral(".ThoughtIgnore exceeds the 4 MiB limit"));
        }
        parse(Py::splitLines(Py::readText(path)));
    } else if (isDefaultFolder(root)) {
        // Build leftovers written inside a default folder are never sources (spec Rule 4).
        parse({QStringLiteral(".git/"), QStringLiteral(".DS_Store"), QStringLiteral("build/"), QStringLiteral("pdf/")});
    } else {
        parse({QStringLiteral(".git/")});
    }
}

ThoughtIgnore ThoughtIgnore::fromContents(const QByteArray &contents)
{
    if (contents.size() > maximumSize) {
        Py::raiseValueError(QStringLiteral(".ThoughtIgnore exceeds the 4 MiB limit"));
    }
    QString error;
    const std::optional<QString> text = Py::decodeUtf8(contents, &error);
    if (!text) {
        Py::raise(QStringLiteral("UnicodeDecodeError"), error);
    }
    ThoughtIgnore result;
    result.parse(Py::splitLines(*text));
    return result;
}

void ThoughtIgnore::parse(const QStringList &lines)
{
    for (const QString &raw : lines) {
        QString line = Py::strip(raw);
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
            continue;
        }
        Rule rule;
        rule.excluded = !line.startsWith(QLatin1Char('!'));
        if (!rule.excluded) {
            line = line.mid(1);
        }
        const bool anchored = line.startsWith(QLatin1Char('/'));
        rule.directoryOnly = line.endsWith(QLatin1Char('/'));
        const QString pattern = Py::strip(line, QStringLiteral("/"));
        if (pattern.isEmpty() || pattern == QLatin1String(".") || pattern == QLatin1String("..") || pattern.contains(QLatin1Char('\\'))) {
            Py::raiseValueError(QStringLiteral("invalid .ThoughtIgnore rule: ") + Py::repr(line));
        }
        rule.basename = !pattern.contains(QLatin1Char('/')) && !anchored;
        rule.tokens = compile(Py::toCodePoints(pattern));
        m_rules.append(rule);
    }
}

bool ThoughtIgnore::excludes(const QString &relative, bool isDirectory) const
{
    const QStringList parts = relative.split(QLatin1Char('/'));
    bool safe = !relative.isEmpty() && !relative.startsWith(QLatin1Char('/')) && !relative.contains(QLatin1Char('\\'));
    for (const QString &part : parts) {
        // PurePosixPath(relative).as_posix() == relative and no ".." part.
        if (!safe || part.isEmpty() || part == QLatin1String(".") || part == QLatin1String("..")) {
            safe = false;
            break;
        }
    }
    if (!safe) {
        Py::raiseValueError(QStringLiteral("unsafe .ThoughtIgnore path: ") + Py::repr(relative));
    }
    for (qsizetype end = 1; end <= parts.size(); ++end) {
        bool ignored = false;
        const bool directory = end < parts.size() || isDirectory;
        const std::u32string basename = Py::toCodePoints(parts.at(end - 1));
        const std::u32string prefix = Py::toCodePoints(parts.mid(0, end).join(QLatin1Char('/')));
        for (const Rule &rule : m_rules) {
            if (rule.directoryOnly && !directory) {
                continue;
            }
            if (Matcher(rule.tokens, rule.basename ? basename : prefix).matches()) {
                ignored = rule.excluded;
            }
        }
        if (ignored) {
            return true;
        }
    }
    return false;
}

}

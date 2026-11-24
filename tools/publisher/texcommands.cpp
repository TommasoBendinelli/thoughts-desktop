// SPDX-License-Identifier: GPL-2.0-or-later
#include "texcommands.h"

#include "pystr.h"

#include <algorithm>
#include <optional>
#include <string>

namespace Thoughts::Publisher
{
namespace
{
// Positions are code points, as in Python.
using Text = std::u32string;

bool isAsciiLetterOrAt(char32_t c)
{
    return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z') || c == U'@';
}

size_t afterComment(const Text &source, size_t position)
{
    const size_t newline = source.find(U'\n', position + 1);
    return newline == Text::npos ? source.size() : newline + 1;
}

size_t controlSequenceEnd(const Text &source, size_t start)
{
    size_t end = start + 1;
    if (end >= source.size()) {
        return end;
    }
    if (isAsciiLetterOrAt(source[end])) {
        ++end;
        while (end < source.size() && isAsciiLetterOrAt(source[end])) {
            ++end;
        }
        return end;
    }
    return end + 1;
}

size_t skipSpaceAndComments(const Text &source, size_t start)
{
    size_t position = start;
    while (position < source.size()) {
        if (Py::isSpace(source[position])) {
            ++position;
        } else if (source[position] == U'%') {
            position = afterComment(source, position);
        } else {
            break;
        }
    }
    return position;
}

std::optional<std::pair<Text, size_t>> bracedArgument(const Text &source, size_t start)
{
    const size_t opening = skipSpaceAndComments(source, start);
    if (opening >= source.size() || source[opening] != U'{') {
        return std::nullopt;
    }
    int depth = 1;
    size_t position = opening + 1;
    while (position < source.size()) {
        const char32_t character = source[position];
        if (character == U'%') {
            position = afterComment(source, position);
            continue;
        }
        if (character == U'\\') {
            position = controlSequenceEnd(source, position);
            continue;
        }
        if (character == U'{') {
            ++depth;
        } else if (character == U'}') {
            if (--depth == 0) {
                return std::make_pair(source.substr(opening + 1, position - opening - 1), position + 1);
            }
        }
        ++position;
    }
    return std::nullopt;
}

size_t inlineVerbEnd(const Text &source, size_t start)
{
    size_t position = start;
    if (position < source.size() && source[position] == U'*') {
        ++position;
    }
    if (position >= source.size() || Py::isSpace(source[position])) {
        return position;
    }
    const size_t closing = source.find(source[position], position + 1);
    return closing == Text::npos ? source.size() : closing + 1;
}

QString stripped(const Text &text)
{
    return Py::strip(Py::fromCodePoints(text));
}
}

namespace
{
// Calls found(name, start, commandEnd) for each active control sequence named in
// names, skipping what TeX would not execute; scanning continues after the
// control sequence, so commands inside arguments are found too.
template<typename Found>
void scanActiveCommands(const Text &source, const QStringList &names, Found found)
{
    static const QStringList literalCommands{QStringLiteral("code"), QStringLiteral("detokenize"), QStringLiteral("mcode"), QStringLiteral("nolinkurl"),
                                             QStringLiteral("path"), QStringLiteral("titlecode"), QStringLiteral("url")};
    static const QStringList literalEnvironments{QStringLiteral("DocCode"),    QStringLiteral("Verbatim"), QStringLiteral("lstlisting"),
                                                 QStringLiteral("minted"),     QStringLiteral("verbatim"), QStringLiteral("verbatim*")};
    size_t position = 0;
    while (position < source.size()) {
        const char32_t character = source[position];
        if (character == U'%') {
            position = afterComment(source, position);
            continue;
        }
        if (character != U'\\') {
            ++position;
            continue;
        }
        const size_t commandEnd = controlSequenceEnd(source, position);
        const QString command = Py::fromCodePoints(source.substr(position + 1, commandEnd - position - 1));
        if (literalCommands.contains(command)) {
            if (const auto argument = bracedArgument(source, commandEnd)) {
                position = argument->second;
                continue;
            }
        }
        if (command == QLatin1String("verb")) {
            position = inlineVerbEnd(source, commandEnd);
            continue;
        }
        if (command == QLatin1String("begin")) {
            const auto argument = bracedArgument(source, commandEnd);
            if (argument && literalEnvironments.contains(stripped(argument->first))) {
                const Text marker = Py::toCodePoints(QStringLiteral("\\end{") + stripped(argument->first) + QLatin1Char('}'));
                const size_t closing = source.find(marker, argument->second);
                position = closing == Text::npos ? source.size() : closing + marker.size();
                continue;
            }
        }
        if (command == QLatin1String("string")) {
            const size_t escaped = skipSpaceAndComments(source, commandEnd);
            position = escaped < source.size() && source[escaped] == U'\\' ? controlSequenceEnd(source, escaped) : escaped;
            continue;
        }
        if (names.contains(command)) {
            found(command, position, commandEnd);
        }
        position = commandEnd;
    }
}
}

QStringList activePdfLinkKeys(const QString &input)
{
    const Text source = Py::toCodePoints(input);
    QStringList keys;
    scanActiveCommands(source, {QStringLiteral("PdfLink")}, [&](const QString &, size_t, size_t commandEnd) {
        if (const auto argument = bracedArgument(source, commandEnd)) {
            keys.append(stripped(argument->first));
        }
    });
    return keys;
}

QList<LinkCommand> activeLinkCommands(const QString &input)
{
    return activeCommands(input, linkCommandNames(), 3);
}

QList<LinkCommand> activeCommands(const QString &input, const QStringList &names, int arguments)
{
    const Text source = Py::toCodePoints(input);
    QList<LinkCommand> commands;
    scanActiveCommands(source, names, [&](const QString &name, size_t start, size_t commandEnd) {
        LinkCommand command;
        command.name = name;
        command.line = 1 + int(std::count(source.cbegin(), source.cbegin() + start, U'\n'));
        size_t end = commandEnd;
        for (int index = 0; index < arguments; ++index) {
            const auto argument = bracedArgument(source, end);
            if (!argument) {
                break;
            }
            command.arguments.append(stripped(argument->first));
            end = argument->second;
        }
        command.text = Py::fromCodePoints(source.substr(start, end - start));
        commands.append(command);
    });
    return commands;
}

}

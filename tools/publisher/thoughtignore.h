// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "pyfs.h"

#include <QList>
#include <QString>

#include <string>

// A Project folder's .ThoughtIgnore (thoughts_server.thought_ignore): ordered
// Git-style rules with *, ? and **, relative to the folder. Without the file
// the rules are ".git/" (".git/", ".DS_Store", "build/" and "pdf/" in a
// Project's default folder); an existing file replaces that default.
namespace Thoughts::Publisher
{

class ThoughtIgnore
{
public:
    // Reads root/.ThoughtIgnore; raises ValueError or the read error.
    explicit ThoughtIgnore(const Py::Path &root);
    // Rules from given file contents instead.
    static ThoughtIgnore fromContents(const QByteArray &contents);

    // Whether a normalised folder-relative POSIX path is excluded; raises
    // "unsafe .ThoughtIgnore path" for anything else.
    bool excludes(const QString &relative, bool isDirectory = false) const;

    struct Token {
        enum class Kind { Literal, AnyChar, Star, DotStar, DirectoryPrefix };
        Kind kind;
        char32_t literal = 0;
    };
    struct Rule {
        QList<Token> tokens;
        bool basename = false;
        bool directoryOnly = false;
        bool excluded = true;
    };

private:
    ThoughtIgnore() = default;
    void parse(const QStringList &lines);
    QList<Rule> m_rules;
};

}

// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include <optional>

// The command line of thoughts-publish-sources as Python's argparse parses
// it: unique-prefix abbreviations, --opt=value, last value wins, and the
// same usage and error texts with exit status 2.
namespace Thoughts::Publisher::ArgParse
{

struct Option {
    enum class Kind { Value, Flag, Append, Int, Float };
    QString name;
    Kind kind = Kind::Value;
    bool required = false;
    // Options sharing a group number are mutually exclusive.
    int group = 0;
};

// One argparse.ArgumentParser: its prog and fixed 80-column texts, and the
// parser that reports unrecognized arguments (the root one for subcommands).
struct Program {
    QString prog;
    QByteArray usage;
    QByteArray help;
    QString extrasProg;
    QByteArray extrasUsage;
    // Mutually exclusive groups that require one member.
    QList<int> requiredGroups;
};

struct Arguments {
    QHash<QString, QString> values;
    QHash<QString, QStringList> lists;
    QSet<QString> flags;

    bool has(const QString &name) const { return values.contains(name); }
    QString value(const QString &name) const { return values.value(name); }
    bool flag(const QString &name) const { return flags.contains(name); }
};

struct Outcome {
    std::optional<Arguments> arguments;
    // Set when parsing ended the program (help, or an error already printed).
    int exitCode = 0;
};

// Parses the arguments after the subcommand name.
Outcome parse(const QString &command, const QList<Option> &options, const QStringList &arguments);
Outcome parse(const Program &program, const QList<Option> &options, const QStringList &arguments);
// python -m thoughts_server.source_reconciliation and thoughts-watch-build.
Program reconcileProgram();
QList<Option> reconcileOptions();
Program watchProgram();
QList<Option> watchOptions();
// parser.error(message) for a program.
int programError(const Program &program, const QString &message);
// The options each subcommand defines, in definition order.
QList<Option> commandOptions(const QString &command);
// parser.error(message) on the top-level parser: usage and message on
// stderr; returns the exit status 2.
int rootError(const QString &message);

}

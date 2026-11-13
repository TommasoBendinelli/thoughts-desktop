// SPDX-License-Identifier: GPL-2.0-or-later
#include "argparse.h"

#include "pyerror.h"
#include "pyio.h"
#include "pyops.h"
#include "pystr.h"

#include <QMap>

namespace Thoughts::Publisher::ArgParse
{
namespace
{
struct CommandTexts {
    const char *command;
    const char *usage;
    const char *help;
};
#include "argparse_texts.inc"

QString program_name()
{
    return QStringLiteral("thoughts-publish-sources");
}
constexpr int helpAction = 0;
constexpr int noAction = -1;

struct Match {
    int action = noAction;
    QString optionString;
    std::optional<QString> separator;
    std::optional<QString> explicitArgument;
};

struct Failure {
    QString message;
};

const CommandTexts *textsFor(const QString &command)
{
    for (const CommandTexts &texts : commandTexts) {
        if (command == QLatin1String(texts.command)) {
            return &texts;
        }
    }
    return nullptr;
}

std::pair<QString, std::optional<QString>> partition(const QString &text)
{
    const qsizetype equals = text.indexOf(QLatin1Char('='));
    if (equals < 0) {
        return {text, std::nullopt};
    }
    return {text.left(equals), text.mid(equals + 1)};
}

class Parser
{
public:
    Parser(const Program &program, const QList<Option> &options)
        : m_program(program)
        , m_options(options)
    {
        m_strings.append({QStringLiteral("-h"), helpAction});
        m_strings.append({QStringLiteral("--help"), helpAction});
        for (qsizetype index = 0; index < options.size(); ++index) {
            m_strings.append({options.at(index).name, int(index) + 1});
        }
    }

    Outcome run(const QStringList &arguments)
    {
        try {
            return parse(arguments);
        } catch (const Failure &failure) {
            Py::writeError(m_program.usage + QStringLiteral("%1: error: %2\n").arg(m_program.prog, failure.message).toUtf8());
            return {std::nullopt, 2};
        }
    }

private:
    Program m_program;
    QHash<int, int> m_groupSeen;
    QList<Option> m_options;
    QList<QPair<QString, int>> m_strings;
    Arguments m_result;
    QSet<int> m_seen;
    bool m_helpRequested = false;

    std::optional<int> actionFor(const QString &optionString) const
    {
        for (const auto &[string, action] : m_strings) {
            if (string == optionString) {
                return action;
            }
        }
        return std::nullopt;
    }

    QString actionName(int action) const
    {
        return action == helpAction ? QStringLiteral("-h/--help") : m_options.at(action - 1).name;
    }

    bool takesValue(int action) const
    {
        return action != helpAction && m_options.at(action - 1).kind != Option::Kind::Flag;
    }

    // _get_values: type=int / type=float conversions.
    QString converted(int action, const QString &value) const
    {
        const Option &option = m_options.at(action - 1);
        if (option.kind == Option::Kind::Int) {
            try {
                return QString::number(Py::toPythonInt(Py::Json(value)));
            } catch (const Py::Exception &) {
                fail(action, QStringLiteral("invalid int value: ") + Py::repr(value));
            }
        }
        if (option.kind == Option::Kind::Float) {
            bool ok = false;
            const double number = Py::strip(value).toDouble(&ok);
            if (!ok) {
                fail(action, QStringLiteral("invalid float value: ") + Py::repr(value));
            }
            return QString::number(number, 'g', 17);
        }
        return value;
    }

    [[noreturn]] void fail(int action, const QString &message) const
    {
        throw Failure{action == noAction ? message : QStringLiteral("argument %1: %2").arg(actionName(action), message)};
    }

    QList<Match> optionTuples(const QString &argument) const
    {
        QList<Match> result;
        if (argument.at(1) == QLatin1Char('-')) {
            auto [prefix, explicitArgument] = partition(argument);
            const std::optional<QString> separator = explicitArgument ? std::optional<QString>(QStringLiteral("=")) : std::nullopt;
            for (const auto &[string, action] : m_strings) {
                if (string.startsWith(prefix)) {
                    result.append({action, string, separator, explicitArgument});
                }
            }
        } else {
            auto [prefix, explicitArgument] = partition(argument);
            const std::optional<QString> separator = explicitArgument ? std::optional<QString>(QStringLiteral("=")) : std::nullopt;
            const QString shortPrefix = argument.left(2);
            const QString shortExplicit = argument.mid(2);
            for (const auto &[string, action] : m_strings) {
                if (string == shortPrefix) {
                    result.append({action, string, QString(QLatin1String("")), shortExplicit});
                } else if (string.startsWith(prefix)) {
                    result.append({action, string, separator, explicitArgument});
                }
            }
        }
        return result;
    }

    std::optional<QList<Match>> classify(const QString &argument) const
    {
        if (argument.isEmpty() || argument.at(0) != QLatin1Char('-')) {
            return std::nullopt;
        }
        if (const std::optional<int> action = actionFor(argument)) {
            return QList<Match>{{*action, argument, std::nullopt, std::nullopt}};
        }
        if (argument.size() == 1) {
            return std::nullopt;
        }
        const auto [optionString, explicitArgument] = partition(argument);
        if (explicitArgument) {
            if (const std::optional<int> action = actionFor(optionString)) {
                return QList<Match>{{*action, optionString, QStringLiteral("="), explicitArgument}};
            }
        }
        const QList<Match> tuples = optionTuples(argument);
        if (!tuples.isEmpty()) {
            return tuples;
        }
        // _negative_number_matcher: -\.?\d at the start.
        const qsizetype digit = argument.size() > 2 && argument.at(1) == QLatin1Char('.') ? 2 : 1;
        if (digit < argument.size() && argument.at(digit).isDigit()) {
            return std::nullopt;
        }
        if (argument.contains(QLatin1Char(' '))) {
            return std::nullopt;
        }
        return QList<Match>{{noAction, argument, std::nullopt, std::nullopt}};
    }

    void take(int action, const QStringList &values)
    {
        m_seen.insert(action);
        if (action == helpAction) {
            Py::writeOut(m_program.help);
            m_helpRequested = true;
            return;
        }
        const Option &option = m_options.at(action - 1);
        if (option.group) {
            const auto other = m_groupSeen.constFind(option.group);
            if (other != m_groupSeen.constEnd() && *other != action) {
                fail(action, QStringLiteral("not allowed with argument ") + actionName(*other));
            }
            m_groupSeen.insert(option.group, action);
        }
        switch (option.kind) {
        case Option::Kind::Flag:
            m_result.flags.insert(option.name);
            break;
        case Option::Kind::Value:
        case Option::Kind::Int:
        case Option::Kind::Float:
            m_result.values.insert(option.name, converted(action, values.value(0)));
            break;
        case Option::Kind::Append:
            m_result.lists[option.name].append(values.value(0));
            break;
        }
    }

    Outcome parse(const QStringList &arguments)
    {
        QString pattern;
        QMap<qsizetype, QList<Match>> optionIndices;
        for (qsizetype index = 0; index < arguments.size(); ++index) {
            if (arguments.at(index) == QLatin1String("--")) {
                pattern += QLatin1Char('-');
                pattern += QString(arguments.size() - index - 1, QLatin1Char('A'));
                break;
            }
            if (const std::optional<QList<Match>> matches = classify(arguments.at(index))) {
                optionIndices.insert(index, *matches);
                pattern += QLatin1Char('O');
            } else {
                pattern += QLatin1Char('A');
            }
        }
        QStringList extras;
        qsizetype start = 0;
        const qsizetype lastOption = optionIndices.isEmpty() ? -1 : optionIndices.lastKey();
        while (start <= lastOption) {
            qsizetype next = start;
            while (next <= lastOption && !optionIndices.contains(next)) {
                ++next;
            }
            extras += arguments.mid(start, next - start);
            start = consumeOptional(arguments, pattern, optionIndices, next, extras);
            if (m_helpRequested) {
                return {std::nullopt, 0};
            }
        }
        extras += arguments.mid(start);
        QStringList missing;
        for (qsizetype index = 0; index < m_options.size(); ++index) {
            if (m_options.at(index).required && !m_seen.contains(int(index) + 1)) {
                missing.append(m_options.at(index).name);
            }
        }
        if (!missing.isEmpty()) {
            fail(noAction, QStringLiteral("the following arguments are required: ") + missing.join(QStringLiteral(", ")));
        }
        for (const int group : m_program.requiredGroups) {
            if (!m_groupSeen.contains(group)) {
                QStringList names;
                for (const Option &option : m_options) {
                    if (option.group == group) {
                        names.append(option.name);
                    }
                }
                fail(noAction, QStringLiteral("one of the arguments %1 is required").arg(names.join(QLatin1Char(' '))));
            }
        }
        if (!extras.isEmpty()) {
            Py::writeError(m_program.extrasUsage
                           + QStringLiteral("%1: error: unrecognized arguments: %2\n").arg(m_program.extrasProg, extras.join(QLatin1Char(' '))).toUtf8());
            return {std::nullopt, 2};
        }
        return {m_result, 0};
    }

    qsizetype consumeOptional(const QStringList &arguments, const QString &pattern, const QMap<qsizetype, QList<Match>> &optionIndices,
                              qsizetype index, QStringList &extras)
    {
        const QList<Match> &matches = optionIndices[index];
        if (matches.size() > 1) {
            QStringList names;
            for (const Match &match : matches) {
                names.append(match.optionString);
            }
            fail(noAction, QStringLiteral("ambiguous option: %1 could match %2").arg(arguments.at(index), names.join(QStringLiteral(", "))));
        }
        Match match = matches.first();
        QList<QPair<int, QStringList>> taken;
        qsizetype stop = index + 1;
        for (;;) {
            if (match.action == noAction) {
                extras.append(arguments.at(index));
                return index + 1;
            }
            if (match.explicitArgument) {
                const int count = takesValue(match.action) ? 1 : 0;
                const QString explicitArgument = *match.explicitArgument;
                if (count == 0 && match.optionString.at(1) != QLatin1Char('-') && !explicitArgument.isEmpty()) {
                    if ((match.separator && !match.separator->isEmpty()) || explicitArgument.at(0) == QLatin1Char('-')) {
                        fail(match.action, QStringLiteral("ignored explicit argument ") + Py::repr(explicitArgument));
                    }
                    taken.append({match.action, QStringList()});
                    const QString combined = match.optionString.left(1) + explicitArgument.at(0);
                    if (const std::optional<int> action = actionFor(combined)) {
                        match.action = *action;
                        match.optionString = combined;
                        QString rest = explicitArgument.mid(1);
                        if (rest.isEmpty()) {
                            match.separator.reset();
                            match.explicitArgument.reset();
                        } else if (rest.at(0) == QLatin1Char('=')) {
                            match.separator = QStringLiteral("=");
                            match.explicitArgument = rest.mid(1);
                        } else {
                            match.separator = QString(QLatin1String(""));
                            match.explicitArgument = rest;
                        }
                        continue;
                    }
                    extras.append(match.optionString.left(1) + explicitArgument);
                    stop = index + 1;
                    break;
                }
                if (count == 1) {
                    taken.append({match.action, {explicitArgument}});
                    stop = index + 1;
                    break;
                }
                fail(match.action, QStringLiteral("ignored explicit argument ") + Py::repr(explicitArgument));
            }
            if (takesValue(match.action)) {
                if (index + 1 >= pattern.size() || pattern.at(index + 1) != QLatin1Char('A')) {
                    fail(match.action, QStringLiteral("expected one argument"));
                }
                taken.append({match.action, {arguments.at(index + 1)}});
                stop = index + 2;
            } else {
                taken.append({match.action, QStringList()});
                stop = index + 1;
            }
            break;
        }
        for (const auto &[action, values] : std::as_const(taken)) {
            take(action, values);
            if (m_helpRequested) {
                break;
            }
        }
        return stop;
    }
};
}

Outcome parse(const Program &program, const QList<Option> &options, const QStringList &arguments)
{
    return Parser(program, options).run(arguments);
}

Outcome parse(const QString &command, const QList<Option> &options, const QStringList &arguments)
{
    const CommandTexts *texts = textsFor(command);
    Program program;
    program.prog = program_name() + QLatin1Char(' ') + command;
    program.usage = QByteArray(texts ? texts->usage : "");
    program.help = QByteArray(texts ? texts->help : "");
    program.extrasProg = program_name();
    program.extrasUsage = QByteArray(rootUsage);
    return parse(program, options, arguments);
}

Program reconcileProgram()
{
    const QString prog = QString::fromLatin1(reconcileProgramText);
    return {prog, QByteArray(reconcileUsage), QByteArray(reconcileHelp), prog, QByteArray(reconcileUsage), {}};
}

QList<Option> reconcileOptions()
{
    using Kind = Option::Kind;
    return {{QStringLiteral("--root"), Kind::Value, true},
            {QStringLiteral("--manifest"), Kind::Value, true},
            {QStringLiteral("--state"), Kind::Value, true},
            {QStringLiteral("--bootstrap-state"), Kind::Value, false},
            {QStringLiteral("--server"), Kind::Value, true},
            {QStringLiteral("--through"), Kind::Int, true}};
}

Program watchProgram()
{
    const QString prog = QStringLiteral("thoughts-watch-build");
    return {prog, QByteArray(watchUsage), QByteArray(watchHelp), prog, QByteArray(watchUsage), {1}};
}

QList<Option> watchOptions()
{
    using Kind = Option::Kind;
    return {{QStringLiteral("--server"), Kind::Value, true},
            {QStringLiteral("--run-directory"), Kind::Value, false, 1},
            {QStringLiteral("--build-id"), Kind::Value, false, 1},
            {QStringLiteral("--target"), Kind::Value, false},
            {QStringLiteral("--token"), Kind::Value, false},
            {QStringLiteral("--poll-interval"), Kind::Float, false}};
}

int programError(const Program &program, const QString &message)
{
    Py::writeError(program.usage + QStringLiteral("%1: error: %2\n").arg(program.prog, message).toUtf8());
    return 2;
}

int rootError(const QString &message)
{
    Py::writeError(QByteArray(rootUsage) + QStringLiteral("%1: error: %2\n").arg(program_name(), message).toUtf8());
    return 2;
}

QList<Option> commandOptions(const QString &command)
{
    using Kind = Option::Kind;
    const Option progress{QStringLiteral("--desktop-progress-jsonl"), Kind::Flag, false};
    if (command == QLatin1String("generate-manifest")) {
        return {{QStringLiteral("--docs-root"), Kind::Value, true}, {QStringLiteral("--output"), Kind::Value, true}, progress};
    }
    if (command == QLatin1String("refresh-project-manifest")) {
        return {{QStringLiteral("--docs-root"), Kind::Value, true},
                {QStringLiteral("--manifest"), Kind::Value, true},
                {QStringLiteral("--project"), Kind::Value, true},
                {QStringLiteral("--output"), Kind::Value, true},
                progress};
    }
    if (command == QLatin1String("migrate-manifest")) {
        return {{QStringLiteral("--docs-root"), Kind::Value, true}, {QStringLiteral("--manifest"), Kind::Value, true}};
    }
    if (command == QLatin1String("list-candidates")) {
        return {{QStringLiteral("--docs-root"), Kind::Value, true}};
    }
    if (command == QLatin1String("inspect-build-inputs")) {
        // --also-target is a native extension (no Python counterpart): more
        // PDFs of the same Project, inspected with one scan of its folder.
        return {{QStringLiteral("--root"), Kind::Value, true},
                {QStringLiteral("--manifest"), Kind::Value, true},
                {QStringLiteral("--target"), Kind::Value, true},
                {QStringLiteral("--also-target"), Kind::Append, false},
                {QStringLiteral("--project"), Kind::Value, false},
                progress};
    }
    if (command == QLatin1String("inspect-library-build-inputs")) {
        return {{QStringLiteral("--root"), Kind::Value, true}, {QStringLiteral("--manifest"), Kind::Value, true}, progress};
    }
    if (command == QLatin1String("publish")) {
        return {{QStringLiteral("--root"), Kind::Value, true},
                {QStringLiteral("--manifest"), Kind::Value, true},
                {QStringLiteral("--state"), Kind::Value, true},
                {QStringLiteral("--server"), Kind::Value, true},
                {QStringLiteral("--token"), Kind::Value, false},
                {QStringLiteral("--target"), Kind::Value, false},
                {QStringLiteral("--force-build"), Kind::Flag, false},
                {QStringLiteral("--scope-target"), Kind::Value, false},
                // Native extension: more PDFs of the --scope-target's Project,
                // published in the same run with one source frontier check.
                {QStringLiteral("--also-scope-target"), Kind::Append, false},
                {QStringLiteral("--project"), Kind::Value, false},
                {QStringLiteral("--path"), Kind::Append, false},
                {QStringLiteral("--frozen-inputs"), Kind::Value, false},
                {QStringLiteral("--adopt-remote-base"), Kind::Append, false},
                {QStringLiteral("--bootstrap-state"), Kind::Value, false}};
    }
    return {};
}

}

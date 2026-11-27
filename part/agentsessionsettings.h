// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <KConfigGroup>
#include <KSharedConfig>

#include <QDir>
#include <QString>
#include <QUuid>

// Whether this computer starts the agent sessions the owner asks for from the phone (the phone's
// "Start a new study session"; shell/sessionstarter.h). One choice per computer, in this app's
// config file: Settings > Agent prompts shows it, and the question the computer asks the first
// time ("Start", "Always", "Not on this computer") changes it.
namespace Thoughts::AgentSessions
{
enum class PhoneStart {
    Ask,  // the default: a question on this computer for each request, until Always or Never
    Always,
    Never,
};

inline QString configGroup() { return QStringLiteral("AgentSessions"); }

// Where those sessions run: a folder of their own that Claude Code must trust once (it never
// trusts the home folder), ~/Thoughts Sessions (Windows: %USERPROFILE%\Thoughts Sessions).
inline QString sessionsFolder() { return QDir::home().filePath(QStringLiteral("Thoughts Sessions")); }
inline QString phoneStartKey() { return QStringLiteral("StartFromPhone"); }

inline QString phoneStartToString(PhoneStart choice)
{
    switch (choice) {
    case PhoneStart::Always:
        return QStringLiteral("always");
    case PhoneStart::Never:
        return QStringLiteral("never");
    case PhoneStart::Ask:
        break;
    }
    return QStringLiteral("ask");
}

// Anything else (nothing saved, a word of a later version) is Ask.
inline PhoneStart phoneStartFromString(const QString &text)
{
    const QString word = text.trimmed().toLower();
    if (word == QLatin1String("always")) return PhoneStart::Always;
    if (word == QLatin1String("never")) return PhoneStart::Never;
    return PhoneStart::Ask;
}

inline PhoneStart phoneStartChoice()
{
    return phoneStartFromString(KConfigGroup(KSharedConfig::openConfig(), configGroup()).readEntry(phoneStartKey(), QString()));
}

// Ask, the default, removes the entry.
inline void setPhoneStartChoice(PhoneStart choice)
{
    KConfigGroup group(KSharedConfig::openConfig(), configGroup());
    if (choice == PhoneStart::Ask) group.deleteEntry(phoneStartKey());
    else group.writeEntry(phoneStartKey(), phoneStartToString(choice));
    group.sync();
}

// This computer among the person's computers, for a page only it may act on (Send to Agent's "New
// session on this computer": origin.where "one_computer", origin.computer this id). A random id
// made the first time it is asked for: it names nothing about the computer or its owner.
inline QString oneComputerWhere() { return QStringLiteral("one_computer"); }
inline QString computerIdKey() { return QStringLiteral("ComputerId"); }
inline QString computerId()
{
    KConfigGroup group(KSharedConfig::openConfig(), configGroup());
    QString id = group.readEntry(computerIdKey(), QString());
    if (id.isEmpty()) {
        id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        group.writeEntry(computerIdKey(), id);
        group.sync();
    }
    return id;
}

// Set up Claude Code (Settings > Agent prompts) registered the Thoughts tools of this installation:
// what it registered (ClaudeCode::BundledTools::fingerprint()), empty before it ever did. An
// update that moves the tools registers them again.
inline QString registeredToolsKey() { return QStringLiteral("ClaudeCodeTools"); }
inline QString registeredTools()
{
    return KConfigGroup(KSharedConfig::openConfig(), configGroup()).readEntry(registeredToolsKey(), QString());
}
inline void setRegisteredTools(const QString &fingerprint)
{
    KConfigGroup group(KSharedConfig::openConfig(), configGroup());
    if (fingerprint.isEmpty()) group.deleteEntry(registeredToolsKey());
    else group.writeEntry(registeredToolsKey(), fingerprint);
    group.sync();
}

// "Let Claude Code sessions on this computer answer": the app runs the reply waker for them
// (Windows; shell/wakersupervisor.h), which lists them in Send to Agent and Ask Agent and brings
// them pages and replies. Without a choice of the person's: on once Claude Code is set up.
inline QString answerHereKey() { return QStringLiteral("AnswerOnThisComputer"); }
inline bool answerHereByDefault() { return !registeredTools().isEmpty(); }
inline bool answerHere()
{
    const KConfigGroup group(KSharedConfig::openConfig(), configGroup());
    return group.hasKey(answerHereKey()) ? group.readEntry(answerHereKey(), false) : answerHereByDefault();
}
// The default removes the entry, so the choice keeps following Set up Claude Code.
inline void setAnswerHere(bool answer)
{
    KConfigGroup group(KSharedConfig::openConfig(), configGroup());
    if (answer == answerHereByDefault()) group.deleteEntry(answerHereKey());
    else group.writeEntry(answerHereKey(), answer);
    group.sync();
}
}

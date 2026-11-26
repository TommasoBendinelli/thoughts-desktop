// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <KConfigGroup>
#include <KSharedConfig>

#include <QDir>
#include <QString>

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
}

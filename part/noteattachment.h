// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QChar>
#include <QList>
#include <QString>
#include <QStringList>

// What Thoughts does with a note's attachment. Its name and its bytes come
// from the server (another device, or whoever can write to the account), so
// the name is checked before it becomes a file name and only passive kinds of
// file are handed to their application; anything else is shown in the file
// manager, where opening it is the reader's own, visible decision.
namespace Thoughts::NoteAttachment
{

// Whether a character may appear in an attachment's name. Control characters
// never may. Format characters (Unicode category Cf) are invisible and can
// change how the name reads: the right-to-left override shows
// "report<U+202E>fdp.exe" as "reportexe.pdf". Line and paragraph separators
// and unpaired surrogates are refused with them.
inline bool characterIsSafe(char32_t character)
{
    switch (QChar::category(character)) {
    case QChar::Other_Control:
    case QChar::Other_Format:
    case QChar::Other_Surrogate:
    case QChar::Separator_Line:
    case QChar::Separator_Paragraph:
        return false;
    default:
        return true;
    }
}

// The name's characters as code points; an unpaired surrogate is U+D800..DFFF itself.
inline QList<char32_t> codePoints(const QString &name)
{
    QList<char32_t> characters;
    characters.reserve(name.size());
    for (qsizetype index = 0; index < name.size(); ++index) {
        const QChar unit = name.at(index);
        if (unit.isHighSurrogate() && index + 1 < name.size() && name.at(index + 1).isLowSurrogate()) {
            characters.append(QChar::surrogateToUcs4(unit, name.at(++index)));
        } else {
            characters.append(unit.unicode());
        }
    }
    return characters;
}

#ifdef Q_OS_WIN
inline constexpr bool windowsNamesByDefault = true;
#else
inline constexpr bool windowsNamesByDefault = false;
#endif

// Whether `name` can be the attachment's file name: one path segment of at
// most 255 characters without the characters above. With `windowsNames` it
// must also be a name Windows keeps as it is: none of < > : " | ? * (a colon
// would write an alternate data stream of another file), no trailing dot or
// space (Windows drops them, which changes the extension), and no device
// name such as CON or NUL.
inline bool nameIsSafe(const QString &name, bool windowsNames = windowsNamesByDefault)
{
    if (name.isEmpty() || name.size() > 255 || name == QLatin1String(".") || name == QLatin1String("..") || name.contains(QLatin1Char('/'))
        || name.contains(QLatin1Char('\\'))) {
        return false;
    }
    const QList<char32_t> characters = codePoints(name);
    for (const char32_t character : characters) {
        if (!characterIsSafe(character)) {
            return false;
        }
    }
    if (windowsNames) {
        for (const QChar character : name) {
            if (QStringLiteral("<>:\"|?*").contains(character)) {
                return false;
            }
        }
        if (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) {
            return false;
        }
        static const QStringList devices {QStringLiteral("con"),  QStringLiteral("prn"),  QStringLiteral("aux"),  QStringLiteral("nul"),  QStringLiteral("com1"), QStringLiteral("com2"),
                                          QStringLiteral("com3"), QStringLiteral("com4"), QStringLiteral("com5"), QStringLiteral("com6"), QStringLiteral("com7"), QStringLiteral("com8"),
                                          QStringLiteral("com9"), QStringLiteral("lpt1"), QStringLiteral("lpt2"), QStringLiteral("lpt3"), QStringLiteral("lpt4"), QStringLiteral("lpt5"),
                                          QStringLiteral("lpt6"), QStringLiteral("lpt7"), QStringLiteral("lpt8"), QStringLiteral("lpt9")};
        if (devices.contains(name.section(QLatin1Char('.'), 0, 0).trimmed().toLower())) {
            return false;
        }
    }
    return true;
}

// The name as text that is safe to show, for a button or a message: the
// characters a file name may not contain are replaced, so even a refused
// name cannot be displayed as something else.
inline QString displayName(const QString &name)
{
    QString shown;
    const QList<char32_t> characters = codePoints(name);
    for (const char32_t character : characters) {
        shown += characterIsSafe(character) ? QString::fromUcs4(&character, 1) : QString(QChar(0xFFFD));
    }
    return shown;
}

// The kinds of file Thoughts opens itself: documents, pictures and plain
// text, which their applications display without running anything.
inline QStringList passiveSuffixes()
{
    return {QStringLiteral("pdf"), QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("gif"), QStringLiteral("webp"),
            QStringLiteral("txt"), QStringLiteral("md"),  QStringLiteral("tex"), QStringLiteral("bib"),  QStringLiteral("csv"), QStringLiteral("json")};
}

enum class Action {
    // Open with the default application.
    Open,
    // Save, and show the saved file in the file manager.
    Reveal,
};

// The extension decides, as it does for the system that would open the
// file: the text after the last dot, whatever its case. A name without one,
// or with any other one (an application, a script, a disk image, an HTML
// page, a shortcut, an archive), is revealed.
inline Action actionFor(const QString &name)
{
    const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot < 0 || dot == name.size() - 1) {
        return Action::Reveal;
    }
    return passiveSuffixes().contains(name.mid(dot + 1).toLower()) ? Action::Open : Action::Reveal;
}

}

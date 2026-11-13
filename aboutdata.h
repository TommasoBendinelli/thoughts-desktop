/*
    SPDX-FileCopyrightText: 2007 Pino Toscano <pino@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef _ABOUTDATA_H_
#define _ABOUTDATA_H_

#include <KAboutData>

#include "core/version.h"

#include <KLocalizedString>

inline KAboutData okularAboutData()
{
    // Thoughts was originally built on top of Okular. The author list names Thoughts' author; the
    // copyright statement keeps Okular's notice, and the credit thanks the Okular developers.
    KAboutData about(QStringLiteral("thoughts"),
                     i18n("Thoughts"),
                     QStringLiteral(OKULAR_VERSION_STRING),
                     i18n("Thoughts, a source-linked document workspace"),
                     KAboutLicense::GPL,
                     i18n("(C) 2026 Tommaso Bendinelli\n"
                          "Built on Okular: (C) 2002 Wilco Greven, Christophe Devriese, (C) 2004-2005 Enrico Ros, "
                          "(C) 2005 Piotr Szymanski, (C) 2004-2017 Albert Astals Cid, (C) 2006-2009 Pino Toscano "
                          "and the Okular developers"),
                     i18n("Thoughts was originally built on top of Okular, KDE's universal document viewer "
                          "(https://invent.kde.org/graphics/okular). We are very grateful to the Okular developers "
                          "and the KDE community."),
                     QStringLiteral("https://github.com/TommasoBendinelli/thoughts-desktop"));

    about.addAuthor(i18n("Tommaso Bendinelli"), i18n("Author"), QString(), QStringLiteral("https://github.com/TommasoBendinelli"));
    about.addCredit(i18n("The Okular developers and the KDE community"),
                    i18n("Thoughts is built on Okular. Thank you!"),
                    QString(),
                    QStringLiteral("https://invent.kde.org/graphics/okular"));

    return about;
}

#endif

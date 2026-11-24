// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QStringList>

// The small TeX scanner of thoughts_server.tex_commands: control sequences
// TeX would execute, skipping comments, \verb, literal braced commands and
// verbatim environments.
namespace Thoughts::Publisher
{

// The stripped arguments of every active \PdfLink{...} in source.
QStringList activePdfLinkKeys(const QString &source);

// The semantic link commands of thoughts_server.renderer_tools.doc_links,
// and the retired \TexLink and \CrossTexLink, which no longer resolve: they
// are found only so that a proposal adding one is told what replaces it.
inline const QStringList &linkCommandNames()
{
    static const QStringList names{QStringLiteral("PdfLink"),         QStringLiteral("CrossPdfLink"), QStringLiteral("SourceLink"),
                                   QStringLiteral("CrossSourceLink"), QStringLiteral("TexLink"),      QStringLiteral("CrossTexLink")};
    return names;
}

// One active link command: its name, its braced arguments (up to three,
// stripped; fewer when the source has fewer), its one-based line and the
// command's source text.
struct LinkCommand {
    QString name;
    QStringList arguments;
    int line = 0;
    QString text;
};

// Every active link command in source, found by the scanner that finds the
// \PdfLink keys the build inputs declare.
QList<LinkCommand> activeLinkCommands(const QString &source);

// Every active command of names in source with up to arguments braced
// arguments, found by the same scanner: e.g. the \href commands of a source.
QList<LinkCommand> activeCommands(const QString &source, const QStringList &names, int arguments);

}

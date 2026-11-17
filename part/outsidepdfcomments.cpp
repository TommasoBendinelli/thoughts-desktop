// SPDX-License-Identifier: GPL-2.0-or-later
#include "outsidepdfcomments.h"

#include "thoughtsplatform.h"

#include <QDateTime>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace Thoughts::OutsidePdfComments
{
namespace
{
const QString pathAttribute = QStringLiteral("pdf_path");
const QString shaAttribute = QStringLiteral("pdf_sha256");

bool validSha256(const QString &value)
{
    if (value.size() != 64) return false;
    for (const QChar c : value) {
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) || (c >= QLatin1Char('a') && c <= QLatin1Char('f')))) return false;
    }
    return true;
}

QString normalizedPath(const QString &path)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}

QDomElement readRoot(const QString &commentsFile, QDomDocument *document)
{
    QFile file(commentsFile);
    if (!file.open(QIODevice::ReadOnly) || !document->setContent(&file)) return {};
    const QDomElement root = document->documentElement();
    return root.tagName() == QLatin1String("documentInfo") ? root : QDomElement();
}
}

QString directory()
{
    return QDir(Platform::applicationDataRoot()).filePath(QStringLiteral("outside-pdf-comments"));
}

QString fileFor(const QString &sha256)
{
    return validSha256(sha256) ? QDir(directory()).filePath(sha256 + QStringLiteral(".xml")) : QString();
}

QString recordedPath(const QString &commentsFile)
{
    QDomDocument document;
    return readRoot(commentsFile, &document).attribute(pathAttribute);
}

QString find(const QString &pdfPath, const QString &sha256)
{
    if (const QString byContent = fileFor(sha256); !byContent.isEmpty() && QFileInfo::exists(byContent)) return byContent;
    // The paper changed at the same place: its comments are the last ones saved for that path.
    const QString path = normalizedPath(pdfPath);
    QString newest;
    QDateTime newestTime;
    const QFileInfoList files = QDir(directory()).entryInfoList({QStringLiteral("*.xml")}, QDir::Files);
    for (const QFileInfo &file : files) {
        if (recordedPath(file.absoluteFilePath()) != path) continue;
        if (newest.isEmpty() || file.lastModified() > newestTime) {
            newest = file.absoluteFilePath();
            newestTime = file.lastModified();
        }
    }
    return newest;
}

bool save(const QString &pdfPath, const QString &sha256, const QByteArray &xml, QString *error)
{
    const QString target = fileFor(sha256);
    if (target.isEmpty()) {
        if (error) *error = QStringLiteral("the PDF could not be read");
        return false;
    }
    QDomDocument document;
    if (xml.isEmpty()) {
        if (!QFileInfo::exists(target)) return true;
        document.appendChild(document.createProcessingInstruction(QStringLiteral("xml"), QStringLiteral("version=\"1.0\" encoding=\"utf-8\"")));
        QDomElement root = document.createElement(QStringLiteral("documentInfo"));
        root.appendChild(document.createElement(QStringLiteral("pageList")));
        document.appendChild(root);
    } else if (!document.setContent(xml) || document.documentElement().tagName() != QLatin1String("documentInfo")) {
        if (error) *error = QStringLiteral("the comments could not be written down");
        return false;
    }
    QDomElement root = document.documentElement();
    root.setAttribute(pathAttribute, normalizedPath(pdfPath));
    root.setAttribute(shaAttribute, sha256);
    if (!QDir().mkpath(directory())) {
        if (error) *error = QStringLiteral("the folder %1 could not be created").arg(directory());
        return false;
    }
    QSaveFile file(target);
    if (!file.open(QIODevice::WriteOnly) || file.write(document.toByteArray()) < 0 || !file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}
}

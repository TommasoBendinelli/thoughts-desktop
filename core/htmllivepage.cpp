// SPDX-License-Identifier: GPL-2.0-or-later
#include "htmllivepage.h"
#include "../part/thoughtsplatform.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStringDecoder>

#include <algorithm>

namespace
{
using namespace Thoughts;

// One tag of an HTML text: what the snapshot and stub rules need of the HTML tokenizer.
struct Tag {
    qsizetype start = 0; // its '<'
    qsizetype end = 0; // one past its '>'
    QString name; // lower case
    bool closing = false;
    QStringView attributes;
};

bool isSpace(QChar c)
{
    return c == QLatin1Char(' ') || c == QLatin1Char('\t') || c == QLatin1Char('\n') || c == QLatin1Char('\r') || c == QLatin1Char('\f');
}

bool isAsciiLetter(QChar c)
{
    return (c >= QLatin1Char('a') && c <= QLatin1Char('z')) || (c >= QLatin1Char('A') && c <= QLatin1Char('Z'));
}

// Elements whose content is text up to their closing tag (no tags inside).
bool isRawText(const QString &name)
{
    static const QStringList names{QStringLiteral("script"), QStringLiteral("style"), QStringLiteral("textarea"), QStringLiteral("title"),
                                   QStringLiteral("xmp"), QStringLiteral("iframe"), QStringLiteral("noembed"), QStringLiteral("noframes"),
                                   QStringLiteral("noscript"), QStringLiteral("plaintext")};
    return names.contains(name);
}

// Visits the tags of @p html in order: comments, doctypes and the text of raw-text elements are
// stepped over (their closing tags are visited). @p visit returns false to stop.
template<typename Visit>
void forEachTag(QStringView html, Visit visit)
{
    const qsizetype n = html.size();
    qsizetype i = 0;
    while (i < n) {
        const qsizetype lt = html.indexOf(QLatin1Char('<'), i);
        if (lt < 0) return;
        const QStringView rest = html.mid(lt);
        if (rest.startsWith(QLatin1String("<!--"))) {
            const qsizetype end = html.indexOf(QLatin1String("-->"), lt + 4);
            i = end < 0 ? n : end + 3;
            continue;
        }
        if (rest.startsWith(QLatin1String("<!")) || rest.startsWith(QLatin1String("<?"))) {
            const qsizetype end = html.indexOf(QLatin1Char('>'), lt);
            i = end < 0 ? n : end + 1;
            continue;
        }
        qsizetype j = lt + 1;
        Tag tag;
        tag.start = lt;
        if (j < n && html[j] == QLatin1Char('/')) {
            tag.closing = true;
            ++j;
        }
        if (j >= n || !isAsciiLetter(html[j])) {
            i = lt + 1;
            continue;
        }
        const qsizetype nameStart = j;
        while (j < n && !isSpace(html[j]) && html[j] != QLatin1Char('/') && html[j] != QLatin1Char('>')) ++j;
        tag.name = html.mid(nameStart, j - nameStart).toString().toLower();
        const qsizetype attributesStart = j;
        // To the '>' that ends the tag: a quote opens a value only after '='.
        QChar quote;
        QChar last;
        while (j < n) {
            const QChar c = html[j];
            if (!quote.isNull()) {
                if (c == quote) quote = QChar();
            } else if ((c == QLatin1Char('"') || c == QLatin1Char('\'')) && last == QLatin1Char('=')) {
                quote = c;
            } else if (c == QLatin1Char('>')) {
                break;
            }
            if (!isSpace(c)) last = c;
            ++j;
        }
        tag.attributes = html.mid(attributesStart, j - attributesStart);
        tag.end = j < n ? j + 1 : n;
        if (!visit(tag)) return;
        i = tag.end;
        if (!tag.closing && isRawText(tag.name)) {
            // The element's text ends at its closing tag.
            const QString closing = QStringLiteral("</") + tag.name;
            qsizetype k = i;
            while (true) {
                k = html.indexOf(closing, k, Qt::CaseInsensitive);
                if (k < 0) return;
                const qsizetype after = k + closing.size();
                if (after >= n || isSpace(html[after]) || html[after] == QLatin1Char('>') || html[after] == QLatin1Char('/')) break;
                k = after;
            }
            i = k;
        }
    }
}

QString decodeEntities(QStringView text)
{
    if (!text.contains(QLatin1Char('&'))) return text.toString();
    static const QHash<QString, QString> named{{QStringLiteral("amp"), QStringLiteral("&")},   {QStringLiteral("lt"), QStringLiteral("<")},
                                               {QStringLiteral("gt"), QStringLiteral(">")},    {QStringLiteral("quot"), QStringLiteral("\"")},
                                               {QStringLiteral("apos"), QStringLiteral("'")}, {QStringLiteral("nbsp"), QString(QChar(0xa0))}};
    QString out;
    out.reserve(text.size());
    qsizetype i = 0;
    while (i < text.size()) {
        const QChar c = text[i];
        const qsizetype semicolon = c == QLatin1Char('&') ? text.indexOf(QLatin1Char(';'), i + 1) : -1;
        if (semicolon > i + 1 && semicolon - i <= 12) {
            const QStringView entity = text.mid(i + 1, semicolon - i - 1);
            QString decoded;
            if (entity.startsWith(QLatin1Char('#'))) {
                bool ok = false;
                const bool hex = entity.size() > 1 && (entity[1] == QLatin1Char('x') || entity[1] == QLatin1Char('X'));
                const uint code = (hex ? entity.mid(2) : entity.mid(1)).toUInt(&ok, hex ? 16 : 10);
                if (ok && code > 0 && code <= 0x10ffff && !(code >= 0xd800 && code <= 0xdfff)) {
                    const char32_t point = code;
                    decoded = QString::fromUcs4(&point, 1);
                }
            } else {
                decoded = named.value(entity.toString());
            }
            if (!decoded.isEmpty()) {
                out += decoded;
                i = semicolon + 1;
                continue;
            }
        }
        out += c;
        ++i;
    }
    return out;
}

// The attributes of a tag (names in lower case, values decoded; the first of a name wins).
QHash<QString, QString> attributesOf(QStringView text)
{
    QHash<QString, QString> out;
    const qsizetype n = text.size();
    qsizetype i = 0;
    while (i < n) {
        while (i < n && (isSpace(text[i]) || text[i] == QLatin1Char('/'))) ++i;
        if (i >= n) break;
        const qsizetype nameStart = i;
        while (i < n && !isSpace(text[i]) && text[i] != QLatin1Char('=') && text[i] != QLatin1Char('>') && text[i] != QLatin1Char('/')) ++i;
        const QString name = text.mid(nameStart, i - nameStart).toString().toLower();
        while (i < n && isSpace(text[i])) ++i;
        QString value;
        if (i < n && text[i] == QLatin1Char('=')) {
            ++i;
            while (i < n && isSpace(text[i])) ++i;
            if (i < n && (text[i] == QLatin1Char('"') || text[i] == QLatin1Char('\''))) {
                const QChar quote = text[i];
                const qsizetype end = text.indexOf(quote, i + 1);
                const qsizetype stop = end < 0 ? n : end;
                value = decodeEntities(text.mid(i + 1, stop - i - 1));
                i = end < 0 ? n : end + 1;
            } else {
                const qsizetype valueStart = i;
                while (i < n && !isSpace(text[i]) && text[i] != QLatin1Char('>')) ++i;
                value = decodeEntities(text.mid(valueStart, i - valueStart));
            }
        }
        if (!name.isEmpty() && !out.contains(name)) out.insert(name, value);
    }
    return out;
}

QString escaped(const QString &text)
{
    QString out = text.toHtmlEscaped();
    out.replace(QLatin1Char('\''), QStringLiteral("&#39;"));
    return out;
}

QString headText(const QByteArray &html, qint64 scanLimit)
{
    return QString::fromUtf8(html.left(scanLimit));
}

QByteArray readHead(const QString &file, qint64 limit)
{
    QFile page(file);
    return page.open(QIODevice::ReadOnly) ? page.read(limit) : QByteArray();
}

bool validUtf8(const QByteArray &bytes)
{
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString text = decoder.decode(bytes);
    Q_UNUSED(text)
    return !decoder.hasError();
}

QString sha256Of(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QString sha256OfFile(const QString &path, qint64 limit)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 1 || file.size() > limit) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return hash.addData(&file) ? QString::fromLatin1(hash.result().toHex()) : QString();
}

QString writesRoot()
{
    return QDir(Platform::applicationDataRoot()).filePath(QStringLiteral("html-page-writes"));
}

QJsonObject readWrites()
{
    QFile file(QDir(writesRoot()).filePath(QStringLiteral("writes.json")));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4000000) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

bool storeWrites(const QJsonObject &writes)
{
    QDir().mkpath(writesRoot());
    QSaveFile file(QDir(writesRoot()).filePath(QStringLiteral("writes.json")));
    return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(writes).toJson()) >= 0 && file.commit();
}

void forgetWrite(const QString &path)
{
    QJsonObject writes = readWrites();
    const QString sha = writes.value(path).toString();
    if (!writes.contains(path)) return;
    writes.remove(path);
    storeWrites(writes);
    // Its copy, unless another page was written with the same bytes.
    for (auto it = writes.constBegin(); it != writes.constEnd(); ++it) {
        if (it.value().toString() == sha) return;
    }
    if (QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(sha).hasMatch()) QFile::remove(QDir(writesRoot()).filePath(sha));
}

const QString AllowedOriginsKey = QStringLiteral("html-live-pages/allowed-origins");
}

namespace Thoughts::LivePage
{
QUrl validAddress(const QString &text, QString *error)
{
    const auto fail = [error](const QString &why) {
        if (error) *error = why;
        return QUrl();
    };
    const QString address = text.trimmed();
    if (address.isEmpty()) return fail(QStringLiteral("The live page names no address."));
    if (address.size() > AddressLimit) return fail(QStringLiteral("The address is longer than %1 characters.").arg(AddressLimit));
    if (std::any_of(address.cbegin(), address.cend(), [](QChar c) { return c.unicode() <= 32 || c.unicode() == 127; }))
        return fail(QStringLiteral("The address has spaces or control characters in it."));
    const QUrl url(address, QUrl::StrictMode);
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || url.isRelative()) return fail(QStringLiteral("The address is not an absolute URL."));
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https")) return fail(QStringLiteral("The address is not an http or https URL."));
    if (url.host().isEmpty()) return fail(QStringLiteral("The address has no host."));
    if (url.authority(QUrl::FullyEncoded).contains(QLatin1Char('@')) || !url.userName().isEmpty() || !url.password().isEmpty())
        return fail(QStringLiteral("The address has a user name or password in it."));
    if (error) error->clear();
    return url;
}

QString metaContent(const QByteArray &html, const QString &name, qint64 scanLimit)
{
    const QString text = headText(html, scanLimit);
    QString found;
    forEachTag(text, [&](const Tag &tag) {
        if (tag.closing || tag.name != QLatin1String("meta")) return true;
        const auto attributes = attributesOf(tag.attributes);
        if (attributes.value(QStringLiteral("name")).compare(name, Qt::CaseInsensitive) != 0) return true;
        found = attributes.value(QStringLiteral("content"));
        if (found.isNull()) found = QLatin1String("");
        return false;
    });
    return found;
}

QString title(const QByteArray &html, qint64 scanLimit)
{
    const QString text = headText(html, scanLimit);
    QString found;
    forEachTag(text, [&](const Tag &tag) {
        if (tag.closing || tag.name != QLatin1String("title")) return true;
        const qsizetype close = text.indexOf(QLatin1String("</title"), tag.end, Qt::CaseInsensitive);
        found = decodeEntities(QStringView(text).mid(tag.end, (close < 0 ? text.size() : close) - tag.end)).simplified();
        return false;
    });
    return found;
}

QUrl stubAddress(const QByteArray &html)
{
    // A malformed stub is shown as the embedded page it is (sealed off), never loaded.
    if (html.isEmpty() || html.size() > StubLimit || !validUtf8(html) || html.toLower().contains("<script")) return {};
    const QString content = metaContent(html, QStringLiteral("thoughts-live-page"), StubLimit);
    return content.isNull() ? QUrl() : validAddress(content);
}

QUrl stubAddressOfFile(const QString &file)
{
    if (QFileInfo(file).size() > StubLimit) return {};
    return stubAddress(readHead(file, StubLimit + 1));
}

QUrl snapshotSource(const QByteArray &html)
{
    if (stubAddress(html).isValid()) return {};
    const QString content = metaContent(html, QStringLiteral("thoughts-live-source"));
    return content.isNull() ? QUrl() : validAddress(content);
}

QUrl snapshotSourceOfFile(const QString &file)
{
    return snapshotSource(readHead(file, 256 * 1024));
}

QString origin(const QUrl &address)
{
    const QUrl url = validAddress(address.toString(QUrl::FullyEncoded));
    if (!url.isValid()) return {};
    QUrl out;
    out.setScheme(url.scheme().toLower());
    out.setHost(url.host(QUrl::FullyEncoded).toLower());
    const int fallback = out.scheme() == QLatin1String("https") ? 443 : 80;
    const int port = url.port(fallback);
    out.setPort(port == fallback ? -1 : port);
    return out.toString(QUrl::FullyEncoded);
}

bool navigationStays(const QUrl &address, const QUrl &target, bool mainFrame)
{
    const QString scheme = target.scheme().toLower();
    if (scheme == QLatin1String("about")) return true;
    if (!mainFrame) return scheme == QLatin1String("http") || scheme == QLatin1String("https") || scheme == QLatin1String("data") || scheme == QLatin1String("blob");
    const QString here = origin(address);
    if (here.isEmpty()) return false;
    if (origin(target) == here) return true;
    // An http address its server moves to https on the same host (a common redirect).
    const QUrl from = validAddress(address.toString(QUrl::FullyEncoded));
    const QUrl to = validAddress(target.toString(QUrl::FullyEncoded));
    return from.scheme().toLower() == QLatin1String("http") && to.scheme().toLower() == QLatin1String("https")
        && from.host(QUrl::FullyEncoded).toLower() == to.host(QUrl::FullyEncoded).toLower();
}

bool isBrowserLink(const QUrl &target)
{
    const QString scheme = target.scheme().toLower();
    return target.isValid() && (scheme == QLatin1String("http") || scheme == QLatin1String("https") || scheme == QLatin1String("mailto"));
}

QByteArray stubHtml(const QUrl &address, const QString &title)
{
    const QUrl url = validAddress(address.toString(QUrl::FullyEncoded));
    if (!url.isValid()) return {};
    const QString href = escaped(url.toString(QUrl::FullyEncoded));
    QString name = title.simplified().left(200);
    if (name.isEmpty()) name = url.host();
    const QString head = QStringLiteral("<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">\n<meta name=\"thoughts-live-page\" content=\"%1\">\n<title>%2</title></head>\n")
                             .arg(href, escaped(name));
    QByteArray stub = (head + QStringLiteral("<body><p>Live page: <a href=\"%1\">%1</a>.\nIt opens in Thoughts, on a device that can reach this address.</p></body></html>\n").arg(href)).toUtf8();
    // A long address with much to escape: the stub without the link still says it is live.
    if (stub.size() > StubLimit) stub = (head + QStringLiteral("<body><p>Live page. It opens in Thoughts.</p></body></html>\n")).toUtf8();
    return stub.size() <= StubLimit ? stub : QByteArray();
}

QByteArray finishSnapshot(const QString &serialized, const QUrl &address, const QString &title, const QDateTime &takenAt, const QString &shownTime, QString *error)
{
    struct Edit {
        qsizetype start;
        qsizetype end;
        QString text;
    };
    QList<Edit> edits;
    qsizetype headInsert = -1;
    qsizetype htmlInsert = -1;
    qsizetype headClose = -1;
    qsizetype bodyInsert = -1;
    bool inHead = false;
    const QString source = escaped(address.toString(QUrl::FullyEncoded));
    const bool ownTitle = !title.simplified().isEmpty();
    static const QStringList dropped{QStringLiteral("thoughts-live-page"), QStringLiteral("thoughts-live-source"), QStringLiteral("thoughts-snapshot-at")};
    // What was only part of the live page: its scripts, base address, charset and http-equiv
    // instructions (refresh, CSP), and earlier live or snapshot marks.
    forEachTag(serialized, [&](const Tag &tag) {
        if (tag.closing) {
            if (tag.name == QLatin1String("head") && headClose < 0) {
                headClose = tag.start;
                inHead = false;
            }
            return true;
        }
        if (tag.name == QLatin1String("html") && htmlInsert < 0) {
            htmlInsert = tag.end;
        } else if (tag.name == QLatin1String("head") && headInsert < 0) {
            headInsert = tag.end;
            inHead = true;
        } else if (tag.name == QLatin1String("body") && bodyInsert < 0) {
            bodyInsert = tag.end;
            inHead = false;
        } else if (tag.name == QLatin1String("script") || (tag.name == QLatin1String("title") && inHead && ownTitle)) {
            const QString closing = QStringLiteral("</") + tag.name;
            qsizetype close = serialized.indexOf(closing, tag.end, Qt::CaseInsensitive);
            qsizetype end = close < 0 ? serialized.size() : serialized.indexOf(QLatin1Char('>'), close);
            end = end < 0 ? serialized.size() : end + 1;
            edits.append({tag.start, end, QString()});
        } else if (tag.name == QLatin1String("base")) {
            edits.append({tag.start, tag.end, QString()});
        } else if (tag.name == QLatin1String("meta")) {
            const auto attributes = attributesOf(tag.attributes);
            if (attributes.contains(QStringLiteral("charset")) || attributes.contains(QStringLiteral("http-equiv"))
                || dropped.contains(attributes.value(QStringLiteral("name")).toLower()))
                edits.append({tag.start, tag.end, QString()});
        }
        return true;
    });
    const QString utc = takenAt.toUTC().toString(Qt::ISODate);
    QString head = QStringLiteral("<meta charset=\"utf-8\">\n<meta name=\"thoughts-live-source\" content=\"%1\">\n<meta name=\"thoughts-snapshot-at\" content=\"%2\">\n")
                       .arg(source, escaped(utc));
    if (ownTitle) head += QStringLiteral("<title>%1</title>\n").arg(escaped(title.simplified()));
    const QString line = QStringLiteral(
                             "<div data-thoughts-snapshot=\"\" style=\"all:initial;display:block;box-sizing:border-box;margin:0;padding:6px 12px;"
                             "font:13px/1.4 -apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;color:#3c3c3c;background:#f1f1f1;border-bottom:1px solid #d6d6d6\">"
                             "Snapshot of <a href=\"%1\" style=\"color:inherit\">%1</a>, %2</div>\n")
                             .arg(source, escaped(shownTime));
    if (headInsert >= 0) {
        edits.append({headInsert, headInsert, QStringLiteral("\n") + head});
    } else if (htmlInsert >= 0) {
        edits.append({htmlInsert, htmlInsert, QStringLiteral("<head>\n") + head + QStringLiteral("</head>")});
    } else {
        edits.append({0, 0, QStringLiteral("<head>\n") + head + QStringLiteral("</head>")});
    }
    if (bodyInsert >= 0) {
        edits.append({bodyInsert, bodyInsert, QStringLiteral("\n") + line});
    } else {
        // No <body> tag: the parser starts the body after the head.
        qsizetype at = headClose >= 0 ? serialized.indexOf(QLatin1Char('>'), headClose) + 1 : headInsert >= 0 ? headInsert : htmlInsert >= 0 ? htmlInsert : 0;
        edits.append({at, at, line});
    }
    // In order; an insertion at a removed range's start goes before the removal.
    std::stable_sort(edits.begin(), edits.end(), [](const Edit &a, const Edit &b) { return a.start < b.start || (a.start == b.start && a.end == a.start && b.end != b.start); });
    QString out = QStringLiteral("<!doctype html>\n");
    out.reserve(serialized.size() + 2048);
    qsizetype at = 0;
    for (const Edit &edit : std::as_const(edits)) {
        if (edit.start < at) continue; // inside a range already removed
        out += QStringView(serialized).mid(at, edit.start - at);
        out += edit.text;
        at = edit.end;
    }
    out += QStringView(serialized).mid(at);
    QByteArray bytes = out.toUtf8();
    if (bytes.size() > PageLimit) {
        if (error) *error = QStringLiteral("The snapshot is larger than 100 MB, the limit of a page.");
        return {};
    }
    if (error) error->clear();
    return bytes;
}

bool originAllowed(const QString &origin)
{
    return !origin.isEmpty() && QSettings().value(AllowedOriginsKey).toStringList().contains(origin);
}

void allowOrigin(const QString &origin)
{
    if (origin.isEmpty()) return;
    QSettings settings;
    QStringList origins = settings.value(AllowedOriginsKey).toStringList();
    if (origins.contains(origin)) return;
    origins.append(origin);
    settings.setValue(AllowedOriginsKey, origins);
}

void forgetOrigins()
{
    QSettings().remove(AllowedOriginsKey);
}

bool rememberWrite(const QString &path, const QByteArray &bytes, QString *error)
{
    const QString sha = sha256Of(bytes);
    const QString copy = QDir(writesRoot()).filePath(sha);
    if (sha256OfFile(copy, PageLimit) != sha) {
        QDir().mkpath(writesRoot());
        QSaveFile file(copy);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
            if (error) *error = QStringLiteral("Could not keep a copy of the page: %1").arg(file.errorString());
            return false;
        }
    }
    QJsonObject writes = readWrites();
    const QString previous = writes.value(path).toString();
    writes.insert(path, sha);
    if (!storeWrites(writes)) {
        if (error) *error = QStringLiteral("Could not remember the written page.");
        return false;
    }
    // The copy of what this page was written with before (unless still in use).
    if (!previous.isEmpty() && previous != sha) {
        bool used = false;
        for (auto it = writes.constBegin(); it != writes.constEnd(); ++it) used = used || it.value().toString() == previous;
        if (!used && QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(previous).hasMatch()) QFile::remove(QDir(writesRoot()).filePath(previous));
    }
    return true;
}

HtmlAttachmentTarget writtenPage(const QString &path, const HtmlAttachmentTarget &built, const QString &docsRoot)
{
    const QString sha = readWrites().value(path).toString();
    if (sha.isEmpty()) return {};
    // Built (the PDFs carry it now), or the page changed since (another device, Sync): the bundle's.
    if ((built.isValid() && built.sha256 == sha) || docsRoot.isEmpty() || sha256OfFile(QDir(docsRoot).filePath(path), PageLimit) != sha) {
        forgetWrite(path);
        return {};
    }
    const QString copy = QDir(writesRoot()).filePath(sha);
    if (sha256OfFile(copy, PageLimit) != sha) {
        forgetWrite(path);
        return {};
    }
    HtmlAttachmentTarget target;
    target.path = path;
    target.file = copy;
    target.sha256 = sha;
    return target;
}

void forgetWrites()
{
    QDir(writesRoot()).removeRecursively();
}

bool writeLibraryPage(const QString &docsRoot, const QString &path, const QByteArray &bytes, QString *error)
{
    const auto fail = [error](const QString &why) {
        if (error) *error = why;
        return false;
    };
    if (!isHtmlSourcePath(path)) return fail(QStringLiteral("This is not a page of a source folder."));
    if (bytes.isEmpty() || bytes.size() > PageLimit || !validUtf8(bytes)) return fail(QStringLiteral("A page must be a UTF-8 HTML file no larger than 100 MB."));
    const QString folder = htmlSourceFolder(path);
    if (docsRoot.isEmpty() || !QFileInfo(QDir(docsRoot).filePath(folder)).isDir())
        return fail(QStringLiteral("The page's folder %1 is not on this computer.").arg(folder));
    const QString file = QDir(docsRoot).filePath(path);
    if (!QDir().mkpath(QFileInfo(file).absolutePath())) return fail(QStringLiteral("Could not make the folder of %1.").arg(path));
    QSaveFile save(file);
    if (!save.open(QIODevice::WriteOnly) || save.write(bytes) != bytes.size() || !save.commit())
        return fail(QStringLiteral("Could not write %1: %2").arg(path, save.errorString()));
    if (error) error->clear();
    return true;
}
}

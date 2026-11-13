/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "librarysearch.h"

#include "../part/librarysourceinventory.h"
#include "agentproposalparser.h"

#include <KLocalizedString>
#include <algorithm>
#include <limits>
#include <QAbstractTextDocumentLayout>
#include <QActionGroup>
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QKeyEvent>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringDecoder>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QtMath>
#include <QUuid>
#include <QtConcurrent/QtConcurrentMap>
#include <poppler-qt6.h>

namespace Thoughts
{
namespace
{
constexpr auto SchemaVersion = "3";

// Opens a connection named for this call only; the caller removes it.
bool openDatabase(const QString &path, const QString &connection, QString *error)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
    db.setDatabaseName(path);
    db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    if (!db.open()) {
        if (error) *error = db.lastError().text();
        return false;
    }
    return true;
}

bool exec(QSqlDatabase &db, const QString &sql, QString *error)
{
    QSqlQuery query(db);
    if (!query.exec(sql)) {
        if (error) *error = query.lastError().text();
        return false;
    }
    return true;
}

bool ensureSchema(QSqlDatabase &db, QString *error)
{
    if (!exec(db, QStringLiteral("PRAGMA journal_mode=WAL"), error)
        || !exec(db, QStringLiteral("CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT NOT NULL)"), error)) {
        return false;
    }
    QSqlQuery version(db);
    version.exec(QStringLiteral("SELECT value FROM meta WHERE key = 'schema'"));
    const bool current = version.next() && version.value(0).toString() == QLatin1String(SchemaVersion);
    // An open statement would lock the tables dropped below.
    version.finish();
    if (current) {
        return true;
    }
    // An older layout is only a cache: rebuild it.
    return exec(db, QStringLiteral("DROP TABLE IF EXISTS entries"), error) && exec(db, QStringLiteral("DROP TABLE IF EXISTS indexed_pdfs"), error)
        && exec(db, QStringLiteral("DROP TABLE IF EXISTS titles"), error)
        && exec(db, QStringLiteral("CREATE TABLE indexed_pdfs(pdf_path TEXT PRIMARY KEY, identity TEXT NOT NULL, metadata_title TEXT NOT NULL DEFAULT '')"), error)
        && exec(db, QStringLiteral("CREATE TABLE titles(pdf_path TEXT PRIMARY KEY, title TEXT NOT NULL)"), error)
        && exec(db,
                QStringLiteral("CREATE VIRTUAL TABLE entries USING fts5(kind UNINDEXED, ref UNINDEXED, pdf_path UNINDEXED, page UNINDEXED, "
                               "context UNINDEXED, title, body, tokenize = 'porter unicode61 remove_diacritics 2', prefix = '2 3')"),
                error)
        && exec(db, QStringLiteral("INSERT OR REPLACE INTO meta(key, value) VALUES ('schema', '%1')").arg(QLatin1String(SchemaVersion)), error);
}

constexpr auto SourceSchemaVersion = "1";

// Source files live in their own tables, so neither index rebuilds the other.
bool ensureSourceSchema(QSqlDatabase &db, QString *error)
{
    if (!exec(db, QStringLiteral("PRAGMA journal_mode=WAL"), error)
        || !exec(db, QStringLiteral("CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT NOT NULL)"), error)) {
        return false;
    }
    QSqlQuery version(db);
    version.exec(QStringLiteral("SELECT value FROM meta WHERE key = 'source_schema'"));
    const bool current = version.next() && version.value(0).toString() == QLatin1String(SourceSchemaVersion);
    version.finish();
    if (current) {
        return true;
    }
    return exec(db, QStringLiteral("DROP TABLE IF EXISTS source_entries"), error) && exec(db, QStringLiteral("DROP TABLE IF EXISTS indexed_sources"), error)
        && exec(db,
                QStringLiteral("CREATE TABLE indexed_sources(path TEXT PRIMARY KEY, identity TEXT NOT NULL, sha1 TEXT NOT NULL, "
                               "relative_path TEXT NOT NULL, project TEXT NOT NULL)"),
                error)
        // kind "source" rows match the path (title); "source-line" rows a block of lines (body) starting at line.
        && exec(db,
                QStringLiteral("CREATE VIRTUAL TABLE source_entries USING fts5(kind UNINDEXED, path UNINDEXED, line UNINDEXED, context UNINDEXED, "
                               "title, body, tokenize = 'porter unicode61 remove_diacritics 2', prefix = '2 3')"),
                error)
        && exec(db, QStringLiteral("INSERT OR REPLACE INTO meta(key, value) VALUES ('source_schema', '%1')").arg(QLatin1String(SourceSchemaVersion)), error);
}

// Larger files are found by their path only.
constexpr qint64 MaxIndexedSourceBytes = 4 * 1024 * 1024;
// Lines of one indexed block: a paragraph, split when longer.
constexpr int MaxBlockLines = 24;

struct SourceBlock {
    int line = 0;
    QString text;
};

struct ReadSource {
    QString path;
    QString sha1;
    QList<SourceBlock> blocks;
};

ReadSource readSource(const QString &path)
{
    ReadSource read{path, {}, {}};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return read;
    const QByteArray bytes = file.size() <= MaxIndexedSourceBytes ? file.read(MaxIndexedSourceBytes + 1) : QByteArray();
    read.sha1 = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex()) + QLatin1Char(':') + QString::number(file.size());
    if (bytes.isEmpty() || bytes.size() > MaxIndexedSourceBytes || bytes.contains('\0')) return read;
    QStringDecoder decoder(QStringDecoder::Utf8);
    QString text = decoder.decode(bytes);
    if (decoder.hasError()) text = QString::fromLatin1(bytes);
    const QStringList lines = LibrarySearchIndex::visibleSourceText(path, text).split(QLatin1Char('\n'));
    SourceBlock block;
    const auto flush = [&] {
        if (!block.text.trimmed().isEmpty()) read.blocks.append(block);
        block = {};
    };
    int count = 0;
    for (int index = 0; index < lines.size(); ++index) {
        QString line = lines.at(index);
        if (line.endsWith(QLatin1Char('\r'))) line.chop(1);
        if (line.trimmed().isEmpty() || count == MaxBlockLines) {
            flush();
            count = 0;
            if (line.trimmed().isEmpty()) continue;
        }
        if (count == 0) block.line = index + 1;
        else block.text += QLatin1Char('\n');
        block.text += line;
        ++count;
    }
    flush();
    return read;
}

struct ExtractedPdf {
    QString pdfPath;
    QStringList pages;
    QString metadataTitle;
};

// A PDF's embedded title, unless it is empty, generic or a file name.
QString usableMetadataTitle(const QString &raw)
{
    static const QRegularExpression spaces(QStringLiteral("[\\s\\x{00a0}]+"));
    static const QRegularExpression fileName(QStringLiteral("\\.(?:pdf|tex|dvi|docx?|pptx?|md)$"), QRegularExpression::CaseInsensitiveOption);
    const QString title = QString(raw).replace(spaces, QStringLiteral(" ")).trimmed();
    static const QStringList generic{QStringLiteral("untitled"), QStringLiteral("untitled document"), QStringLiteral("document"), QStringLiteral("paper"),
                                     QStringLiteral("main"), QStringLiteral("slides"), QStringLiteral("presentation")};
    if (title.size() < 3 || generic.contains(title.toLower()) || fileName.match(title).hasMatch()) return {};
    for (const QChar c : title) {
        if (c.unicode() < 32) return {};
    }
    return title;
}

ExtractedPdf extractText(const QString &pdfPath)
{
    ExtractedPdf extracted{pdfPath, {}, {}};
    // Each worker owns its document; Poppler documents are not shared across threads.
    std::unique_ptr<Poppler::Document> document = Poppler::Document::load(pdfPath);
    if (!document || document->isLocked()) {
        return extracted;
    }
    extracted.metadataTitle = usableMetadataTitle(document->info(QStringLiteral("Title")));
    for (int index = 0; index < document->numPages(); ++index) {
        std::unique_ptr<Poppler::Page> page = document->page(index);
        extracted.pages.append(page ? page->text(QRectF()) : QString());
    }
    return extracted;
}

bool insertEntry(QSqlQuery &insert, const QString &kind, const QString &ref, const QString &pdfPath, int page, const QString &context,
                 const QString &title, const QString &body, QString *error)
{
    insert.bindValue(0, kind);
    insert.bindValue(1, ref);
    insert.bindValue(2, pdfPath);
    insert.bindValue(3, page);
    insert.bindValue(4, context);
    insert.bindValue(5, title);
    insert.bindValue(6, body);
    if (!insert.exec()) {
        if (error) *error = insert.lastError().text();
        return false;
    }
    return true;
}
}

LibrarySearchIndex::LibrarySearchIndex(QString databasePath)
    : m_path(std::move(databasePath))
{
}

QStringList LibrarySearchIndex::queryWords(const QString &query)
{
    static const QRegularExpression separators(QStringLiteral("[^\\p{L}\\p{N}]+"));
    return query.split(separators, Qt::SkipEmptyParts);
}

QString LibrarySearchIndex::matchExpression(const QString &query)
{
    // Only the word being typed matches as a prefix; finished words match by
    // stem ("prefills" finds "prefill"), so "the" does not find "themselves".
    QStringList terms;
    const QStringList words = queryWords(query);
    for (int index = 0; index < words.size(); ++index) {
        terms.append(QStringLiteral("\"%1\"%2").arg(words.at(index), index == words.size() - 1 ? QStringLiteral("*") : QString()));
    }
    return terms.join(QLatin1Char(' '));
}

bool LibrarySearchIndex::update(const QList<LibrarySearchPdf> &pdfs, const QList<LibrarySearchRecord> &records, const QStringList &replaceKinds,
                                LibrarySearchUpdate *summary, QString *error) const
{
    const QString connection = QStringLiteral("thoughts-library-search-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool ok = false;
    {
        if (!openDatabase(m_path, connection, error)) {
            QSqlDatabase::removeDatabase(connection);
            return false;
        }
        QSqlDatabase db = QSqlDatabase::database(connection, false);
        if (ensureSchema(db, error)) {
            QHash<QString, QString> indexed, metadataTitles, storedTitles;
            QSqlQuery existing(db);
            existing.exec(QStringLiteral("SELECT pdf_path, identity, metadata_title FROM indexed_pdfs"));
            while (existing.next()) {
                indexed.insert(existing.value(0).toString(), existing.value(1).toString());
                metadataTitles.insert(existing.value(0).toString(), existing.value(2).toString());
            }
            existing.exec(QStringLiteral("SELECT pdf_path, title FROM titles"));
            while (existing.next()) {
                storedTitles.insert(existing.value(0).toString(), existing.value(1).toString());
            }
            QHash<QString, LibrarySearchPdf> wanted;
            QStringList changed;
            for (const LibrarySearchPdf &pdf : pdfs) {
                wanted.insert(pdf.pdfPath, pdf);
                if (indexed.value(pdf.pdfPath) != pdf.identity) {
                    changed.append(pdf.pdfPath);
                }
            }
            QStringList removed;
            for (auto it = indexed.cbegin(); it != indexed.cend(); ++it) {
                if (!wanted.contains(it.key())) removed.append(it.key());
            }

            QElapsedTimer timer;
            timer.start();
            const QList<ExtractedPdf> extracted = QtConcurrent::blockingMapped<QList<ExtractedPdf>>(changed, extractText);
            const qint64 extractMs = timer.restart();

            ok = db.transaction();
            QSqlQuery remove(db);
            remove.prepare(QStringLiteral("DELETE FROM entries WHERE pdf_path = ? AND kind IN ('document', 'page')"));
            QSqlQuery forget(db);
            forget.prepare(QStringLiteral("DELETE FROM indexed_pdfs WHERE pdf_path = ?"));
            QSqlQuery insert(db);
            insert.prepare(QStringLiteral("INSERT INTO entries(kind, ref, pdf_path, page, context, title, body) VALUES (?, ?, ?, ?, ?, ?, ?)"));
            QSqlQuery remember(db);
            remember.prepare(QStringLiteral("INSERT OR REPLACE INTO indexed_pdfs(pdf_path, identity, metadata_title) VALUES (?, ?, ?)"));
            QSqlQuery removeDocument(db);
            removeDocument.prepare(QStringLiteral("DELETE FROM entries WHERE pdf_path = ? AND kind = 'document'"));
            // The Library's name wins; otherwise the PDF's own title; otherwise the caller's.
            const auto titleOf = [&](const LibrarySearchPdf &pdf) {
                if (!pdf.libraryTitle.isEmpty()) return pdf.libraryTitle;
                const QString metadata = metadataTitles.value(pdf.pdfPath);
                return metadata.isEmpty() ? pdf.title : metadata;
            };
            int pages = 0;
            for (const QString &path : removed + changed) {
                remove.bindValue(0, path);
                forget.bindValue(0, path);
                ok = ok && remove.exec() && forget.exec();
            }
            QSet<QString> extractedPaths;
            for (const ExtractedPdf &pdf : extracted) {
                if (!ok) break;
                extractedPaths.insert(pdf.pdfPath);
                metadataTitles.insert(pdf.pdfPath, pdf.metadataTitle);
                // Page rows match text only; their document's name is looked up when searching.
                for (int index = 0; ok && index < pdf.pages.size(); ++index) {
                    if (pdf.pages.at(index).trimmed().isEmpty()) continue;
                    ok = insertEntry(insert, QStringLiteral("page"), pdf.pdfPath, pdf.pdfPath, index + 1, QString(), QString(), pdf.pages.at(index), error);
                    ++pages;
                }
                remember.bindValue(0, pdf.pdfPath);
                remember.bindValue(1, wanted[pdf.pdfPath].identity);
                remember.bindValue(2, pdf.metadataTitle);
                ok = ok && remember.exec();
            }
            // Document rows match names, so they follow renames even when the PDF is unchanged.
            ok = ok && exec(db, QStringLiteral("DELETE FROM titles"), error);
            QSqlQuery storeTitle(db);
            storeTitle.prepare(QStringLiteral("INSERT INTO titles(pdf_path, title) VALUES (?, ?)"));
            for (const LibrarySearchPdf &pdf : pdfs) {
                if (!ok) break;
                const QString title = titleOf(pdf);
                if (extractedPaths.contains(pdf.pdfPath) || storedTitles.value(pdf.pdfPath) != title) {
                    removeDocument.bindValue(0, pdf.pdfPath);
                    ok = removeDocument.exec()
                        && insertEntry(insert, QStringLiteral("document"), pdf.pdfPath, pdf.pdfPath, 0, pdf.project, title, pdf.project, error);
                }
                storeTitle.bindValue(0, pdf.pdfPath);
                storeTitle.bindValue(1, title);
                ok = ok && storeTitle.exec();
            }
            QSqlQuery clear(db);
            clear.prepare(QStringLiteral("DELETE FROM entries WHERE kind = ?"));
            for (const QString &kind : replaceKinds) {
                clear.bindValue(0, kind);
                ok = ok && clear.exec();
            }
            for (const LibrarySearchRecord &record : records) {
                if (!ok) break;
                if (!replaceKinds.contains(record.kind)) continue;
                ok = insertEntry(insert, record.kind, record.ref, record.pdfPath, record.page, record.context, record.title, record.body, error);
            }
            if (ok) {
                ok = db.commit();
            } else {
                db.rollback();
            }
            if (!ok && error && error->isEmpty()) *error = db.lastError().text();
            if (summary) {
                *summary = {int(extracted.size()), int(removed.size()), pages, int(records.size()), extractMs, timer.elapsed()};
            }
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return ok;
}

bool LibrarySearchIndex::isTextSource(const QString &path)
{
    static const QSet<QString> text{QStringLiteral("tex"), QStringLiteral("ltx"), QStringLiteral("sty"), QStringLiteral("cls"), QStringLiteral("bib"),
                                    QStringLiteral("bst"), QStringLiteral("bbx"), QStringLiteral("cbx"), QStringLiteral("dtx"), QStringLiteral("ins"),
                                    QStringLiteral("def"), QStringLiteral("cfg"), QStringLiteral("md"), QStringLiteral("markdown"), QStringLiteral("txt"),
                                    QStringLiteral("rst"), QStringLiteral("org"), QStringLiteral("py"), QStringLiteral("r"), QStringLiteral("jl"),
                                    QStringLiteral("lua"), QStringLiteral("sh"), QStringLiteral("js"), QStringLiteral("ts"), QStringLiteral("css"),
                                    QStringLiteral("json"), QStringLiteral("yaml"), QStringLiteral("yml"), QStringLiteral("toml"), QStringLiteral("ini"),
                                    QStringLiteral("patch"), QStringLiteral("diff"), QStringLiteral("mk")};
    const QString name = QFileInfo(path).fileName();
    const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
    // Makefile, latexmkrc, .ThoughtIgnore and other names without an extension are text too.
    if (dot <= 0) return !name.isEmpty();
    return text.contains(name.mid(dot + 1).toLower());
}

QString LibrarySearchIndex::visibleSourceText(const QString &path, const QString &text)
{
    QString visible = text;
    const auto blank = [&](qsizetype first, qsizetype last) {
        for (qsizetype i = qMax<qsizetype>(0, first); i < qMin(last, visible.size()); ++i) {
            if (visible.at(i) != QLatin1Char('\n') && visible.at(i) != QLatin1Char('\r')) visible[i] = QLatin1Char(' ');
        }
    };
    const AgentProposalFormat format = agentProposalFormat(path);
    if (format == AgentProposalFormat::Latex) {
        // What TeX shows of each proposal: its replacement. The command, its
        // metadata, explanation and original text are hidden.
        bool incomplete = false;
        for (const auto &[first, last] : latexAgentProposalHiddenRanges(text, &incomplete)) {
            // A broken wrapper would hide the rest of the file; search keeps it.
            if (incomplete && last >= text.size()) continue;
            blank(first, last);
        }
        // The braces around a valid proposal's replacement are syntax too.
        for (const AgentProposedEdit &proposal : parseLatexAgentProposals(text).proposals) {
            blank(proposal.replacementOffset - 1, proposal.replacementOffset);
            blank(proposal.replacementOffset + proposal.replacementLength, proposal.replacementOffset + proposal.replacementLength + 1);
        }
        static const QRegularExpression marker(QStringLiteral(R"(^[ \t]*\\ReviewCardMask(?:Start|End)\{[^}\n]*\}\{[0-9]+\}%?[ \t]*\r?$)"),
                                               QRegularExpression::MultilineOption);
        for (auto it = marker.globalMatch(text); it.hasNext();) {
            const auto match = it.next();
            blank(match.capturedStart(), match.capturedEnd());
        }
    } else if (format == AgentProposalFormat::Markdown) {
        // Thoughts markers and the opening comment of a proposal (its metadata,
        // explanation and original text) are HTML comments a reader never sees.
        static const QRegularExpression marker(QStringLiteral(R"(<!--\s*thoughts:.*?-->)"), QRegularExpression::DotMatchesEverythingOption);
        for (auto it = marker.globalMatch(text); it.hasNext();) {
            const auto match = it.next();
            blank(match.capturedStart(), match.capturedEnd());
        }
    }
    return visible;
}

QList<LibrarySearchSource> LibrarySearchIndex::listSources(const QString &docsRoot, const QList<LibrarySearchSourceFolder> &folders)
{
    QList<LibrarySearchSource> sources;
    // The inventory Sync and the Sources tree use: same .ThoughtIgnore rules,
    // directory listings reused while unchanged. Its cache is only read here.
    LibrarySourceInventory inventory(docsRoot);
    const QDir root(docsRoot);
    QSet<QString> seen;
    for (const LibrarySearchSourceFolder &folder : folders) {
        if (seen.contains(folder.owner)) continue;
        seen.insert(folder.owner);
        const QJsonObject location = inventory.refresh(folder.owner);
        // Default folders sit below .thoughts/projects/<id>; show their files by project only.
        const QString prefix = SourceOwner::isDefaultFolder(folder.owner) ? QString() : folder.owner + QLatin1Char('/');
        for (const QJsonValue &value : location.value(QStringLiteral("files")).toArray()) {
            const QString relative = value.toObject().value(QStringLiteral("relative_path")).toString();
            if (relative.isEmpty() || !isTextSource(relative)) continue;
            const QFileInfo file(root.filePath(folder.owner + QLatin1Char('/') + relative));
            if (!file.isFile()) continue;
            sources.append({file.absoluteFilePath(), prefix + relative, folder.project,
                            QStringLiteral("%1:%2").arg(file.size()).arg(file.lastModified().toMSecsSinceEpoch())});
        }
    }
    return sources;
}

bool LibrarySearchIndex::updateSources(const QList<LibrarySearchSource> &sources, LibrarySearchSourceUpdate *summary, QString *error) const
{
    const QString connection = QStringLiteral("thoughts-library-search-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool ok = false;
    {
        if (!openDatabase(m_path, connection, error)) {
            QSqlDatabase::removeDatabase(connection);
            return false;
        }
        QSqlDatabase db = QSqlDatabase::database(connection, false);
        if (ensureSourceSchema(db, error)) {
            struct Stored {
                QString identity;
                QString sha1;
                QString relativePath;
                QString project;
            };
            QHash<QString, Stored> stored;
            QSqlQuery existing(db);
            existing.exec(QStringLiteral("SELECT path, identity, sha1, relative_path, project FROM indexed_sources"));
            while (existing.next()) {
                stored.insert(existing.value(0).toString(),
                              {existing.value(1).toString(), existing.value(2).toString(), existing.value(3).toString(), existing.value(4).toString()});
            }
            existing.finish();
            QHash<QString, LibrarySearchSource> wanted;
            QStringList changed;
            for (const LibrarySearchSource &source : sources) {
                if (wanted.contains(source.path)) continue;
                wanted.insert(source.path, source);
                const auto known = stored.constFind(source.path);
                if (known == stored.cend() || known->identity != source.identity || known->relativePath != source.relativePath
                    || known->project != source.project) {
                    changed.append(source.path);
                }
            }
            QStringList removed;
            for (auto it = stored.cbegin(); it != stored.cend(); ++it) {
                if (!wanted.contains(it.key())) removed.append(it.key());
            }

            QElapsedTimer timer;
            timer.start();
            const QList<ReadSource> read = QtConcurrent::blockingMapped<QList<ReadSource>>(changed, readSource);
            const qint64 readMs = timer.restart();

            ok = db.transaction();
            QSqlQuery remove(db);
            remove.prepare(QStringLiteral("DELETE FROM source_entries WHERE path = ?"));
            QSqlQuery forget(db);
            forget.prepare(QStringLiteral("DELETE FROM indexed_sources WHERE path = ?"));
            QSqlQuery insert(db);
            insert.prepare(QStringLiteral("INSERT INTO source_entries(kind, path, line, context, title, body) VALUES (?, ?, ?, ?, ?, ?)"));
            QSqlQuery remember(db);
            remember.prepare(QStringLiteral("INSERT OR REPLACE INTO indexed_sources(path, identity, sha1, relative_path, project) VALUES (?, ?, ?, ?, ?)"));
            const auto add = [&](const QString &kind, const QString &path, int line, const QString &context, const QString &title, const QString &body) {
                insert.bindValue(0, kind);
                insert.bindValue(1, path);
                insert.bindValue(2, line);
                insert.bindValue(3, context);
                insert.bindValue(4, title);
                insert.bindValue(5, body);
                if (insert.exec()) return true;
                if (error) *error = insert.lastError().text();
                return false;
            };
            const auto store = [&](const LibrarySearchSource &source, const QString &sha1) {
                remember.bindValue(0, source.path);
                remember.bindValue(1, source.identity);
                remember.bindValue(2, sha1);
                remember.bindValue(3, source.relativePath);
                remember.bindValue(4, source.project);
                return remember.exec();
            };
            for (const QString &path : std::as_const(removed)) {
                remove.bindValue(0, path);
                forget.bindValue(0, path);
                ok = ok && remove.exec() && forget.exec();
            }
            int unchanged = 0;
            int chunks = 0;
            for (const ReadSource &file : read) {
                if (!ok) break;
                const LibrarySearchSource &source = wanted[file.path];
                const auto known = stored.constFind(file.path);
                // Touched but not edited (a checkout, a Sync rewrite): keep its rows.
                if (known != stored.cend() && known->sha1 == file.sha1 && known->relativePath == source.relativePath && known->project == source.project) {
                    ++unchanged;
                    ok = store(source, file.sha1);
                    continue;
                }
                remove.bindValue(0, file.path);
                ok = remove.exec() && add(QStringLiteral("source"), file.path, 0, source.project, source.relativePath, QString());
                for (const SourceBlock &block : file.blocks) {
                    if (!ok) break;
                    ok = add(QStringLiteral("source-line"), file.path, block.line, QString(), QString(), block.text);
                    ++chunks;
                }
                ok = ok && store(source, file.sha1);
            }
            ok = ok && exec(db, QStringLiteral("INSERT OR REPLACE INTO meta(key, value) VALUES ('sources_indexed', '1')"), error);
            if (ok) {
                ok = db.commit();
            } else {
                db.rollback();
            }
            if (!ok && error && error->isEmpty()) *error = db.lastError().text();
            if (summary) {
                *summary = {int(wanted.size()), int(read.size()), unchanged, int(removed.size()), chunks, readMs, timer.elapsed()};
            }
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return ok;
}

bool LibrarySearchIndex::hasSources() const
{
    if (!QFileInfo::exists(m_path)) return false;
    const QString connection = QStringLiteral("thoughts-library-search-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool indexed = false;
    {
        if (openDatabase(m_path, connection, nullptr)) {
            QSqlDatabase db = QSqlDatabase::database(connection, false);
            QSqlQuery query(db);
            indexed = query.exec(QStringLiteral("SELECT value FROM meta WHERE key = 'sources_indexed'")) && query.next();
            query.finish();
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return indexed;
}

bool LibrarySearchIndex::isEmpty() const
{
    if (!QFileInfo::exists(m_path)) return true;
    const QString connection = QStringLiteral("thoughts-library-search-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool empty = true;
    {
        if (openDatabase(m_path, connection, nullptr)) {
            QSqlDatabase db = QSqlDatabase::database(connection, false);
            QSqlQuery count(db);
            empty = !(count.exec(QStringLiteral("SELECT count(*) FROM indexed_pdfs")) && count.next() && count.value(0).toInt() > 0);
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return empty;
}

QList<LibrarySearchHit> LibrarySearchIndex::search(const QString &query, QString *error) const
{
    return search(query, LibrarySearchScope::Documents, error);
}

namespace
{
// The line of a highlighted block with the most matched words, as a one-line
// snippet that starts shortly before the first match.
void locateLine(const QString &highlighted, int firstLine, int *line, QString *snippet)
{
    const QStringList lines = highlighted.split(QLatin1Char('\n'));
    int best = 0;
    qsizetype bestCount = 0;
    for (int index = 0; index < lines.size(); ++index) {
        const qsizetype count = lines.at(index).count(QChar(1));
        if (count > bestCount) {
            best = index;
            bestCount = count;
        }
    }
    *line = firstLine + best;
    QString text = lines.value(best);
    const qsizetype first = text.indexOf(QChar(1));
    const qsizetype cut = first > 60 ? text.lastIndexOf(QLatin1Char(' '), first - 40) : -1;
    if (cut > 0) text = QStringLiteral("…") + text.mid(cut + 1);
    *snippet = text.trimmed();
}

bool nameMatchesAll(const QString &relativePath, const QStringList &words)
{
    const QString name = QFileInfo(relativePath).fileName();
    return !words.isEmpty() && std::all_of(words.cbegin(), words.cend(), [&name](const QString &word) { return name.contains(word, Qt::CaseInsensitive); });
}
}

namespace
{
bool matchesAsTyped(const QString &text, const QList<QRegularExpression> &asTyped)
{
    return !asTyped.isEmpty() && std::all_of(asTyped.cbegin(), asTyped.cend(), [&text](const QRegularExpression &word) { return word.match(text).hasMatch(); });
}

// How well a document matches. A tier is lower for a better match: the expression that found it
// (the phrase, finished words, the word being typed), and within that, the words as typed.
struct DocumentMatch {
    static constexpr int NoMatch = std::numeric_limits<int>::max();
    QString name;
    QString project;
    int titleTier = NoMatch;
    double titleRank = 0;
    int pageTier = NoMatch;
    QList<qint64> pageRows; // Of the pages at pageTier, the best first.
    int page = 0;
    int pageExpression = 0;
    double pageRank = 0;

    int tier() const
    {
        return std::min(titleTier, pageTier);
    }
    double rank() const
    {
        return pageTier == NoMatch ? titleRank : pageRank;
    }
};

// One row for each document: its name, how many pages match best and how often the words occur on
// them, and the best of those pages.
// The document search panel lists every match of the document that is opened from it.
QList<LibrarySearchHit> documentHits(const QSqlDatabase &db, const QStringList &expressions, const QList<QRegularExpression> &asTyped,
                                     const QHash<QString, QString> &titles, int limit, double *bestRank, bool *phraseMatched, QString *error)
{
    QHash<QString, DocumentMatch> matches;
    for (int expression = 0; expression < expressions.size(); ++expression) {
        if (expressions.at(expression).isEmpty()) continue;
        QSqlQuery named(db);
        named.prepare(QStringLiteral("SELECT pdf_path, context, title, rank FROM entries WHERE entries MATCH ? AND kind = 'document' ORDER BY rank LIMIT 500"));
        named.addBindValue(expressions.at(expression));
        if (!named.exec() && error) *error = named.lastError().text();
        while (named.next()) {
            DocumentMatch &match = matches[named.value(0).toString()];
            match.project = named.value(1).toString();
            match.name = named.value(2).toString();
            const int tier = expression * 2 + (matchesAsTyped(match.name, asTyped) ? 0 : 1);
            if (tier < match.titleTier) {
                match.titleTier = tier;
                match.titleRank = named.value(3).toDouble();
            }
        }
    }

    // The text of a page is read only to tell the words as typed.
    const QString pagesQuery = QStringLiteral("SELECT rowid, pdf_path, page, rank%1 FROM entries WHERE entries MATCH ? AND kind = 'page' ORDER BY rank LIMIT 5000")
                                   .arg(asTyped.isEmpty() ? QString() : QStringLiteral(", body"));
    QSet<qint64> seen;
    for (int expression = 0; expression < expressions.size(); ++expression) {
        if (expressions.at(expression).isEmpty()) continue;
        QSqlQuery pages(db);
        pages.prepare(pagesQuery);
        pages.addBindValue(expressions.at(expression));
        if (!pages.exec() && error) *error = pages.lastError().text();
        while (pages.next()) {
            const qint64 row = pages.value(0).toLongLong();
            if (seen.contains(row)) continue;
            seen.insert(row);
            const int tier = expression * 2 + (!asTyped.isEmpty() && matchesAsTyped(pages.value(4).toString(), asTyped) ? 0 : 1);
            DocumentMatch &match = matches[pages.value(1).toString()];
            if (tier < match.pageTier) {
                match.pageTier = tier;
                match.pageRows = {row};
                match.page = pages.value(2).toInt();
                match.pageExpression = expression;
                match.pageRank = pages.value(3).toDouble();
            } else if (tier == match.pageTier) {
                match.pageRows.append(row);
            }
        }
    }

    QList<QString> order = matches.keys();
    std::sort(order.begin(), order.end(), [&matches](const QString &a, const QString &b) {
        const DocumentMatch &left = matches[a];
        const DocumentMatch &right = matches[b];
        if (left.tier() != right.tier()) return left.tier() < right.tier();
        // Within a tier: its name holds the words, then the most pages, then the best page.
        if ((left.titleTier == left.tier()) != (right.titleTier == right.tier())) return left.titleTier == left.tier();
        if (left.pageRows.size() != right.pageRows.size()) return left.pageRows.size() > right.pageRows.size();
        if (left.rank() != right.rank()) return left.rank() < right.rank();
        return a < b;
    });

    QSqlQuery snippet(db);
    snippet.prepare(QStringLiteral("SELECT snippet(entries, -1, char(1), char(2), '…', 14) FROM entries WHERE entries MATCH ? AND rowid = ?"));
    QSqlQuery marked(db);
    marked.prepare(QStringLiteral("SELECT highlight(entries, 6, char(1), char(2)) FROM entries WHERE entries MATCH ? AND rowid = ?"));
    QList<LibrarySearchHit> hits;
    for (const QString &path : std::as_const(order)) {
        if (hits.size() == limit) break;
        const DocumentMatch &match = matches[path];
        LibrarySearchHit hit;
        hit.kind = QStringLiteral("document");
        hit.ref = path;
        hit.pdfPath = path;
        hit.page = match.page;
        hit.title = titles.value(path, match.name);
        hit.context = match.project;
        hit.matchingPages = int(match.pageRows.size());
        if (!match.pageRows.isEmpty()) {
            const QString &expression = expressions.at(match.pageExpression);
            snippet.addBindValue(expression);
            snippet.addBindValue(match.pageRows.constFirst());
            if (snippet.exec() && snippet.next()) hit.snippet = snippet.value(0).toString();
            // Pages found for the words as typed count only those, not any case of them.
            const bool typed = match.pageTier % 2 == 0;
            for (const qint64 row : match.pageRows) {
                marked.addBindValue(expression);
                marked.addBindValue(row);
                if (!marked.exec() || !marked.next()) continue;
                const QString text = marked.value(0).toString();
                for (qsizetype start = text.indexOf(QChar(1)); start >= 0; start = text.indexOf(QChar(1), start + 1)) {
                    const qsizetype end = text.indexOf(QChar(2), start);
                    if (end < 0) break;
                    hit.matches += !typed || matchesAsTyped(text.mid(start + 1, end - start - 1), asTyped) ? 1 : 0;
                }
            }
            marked.finish();
        }
        hits.append(hit);
    }
    // The count is on the row, so within a tier the rows follow it.
    std::stable_sort(hits.begin(), hits.end(), [&matches](const LibrarySearchHit &a, const LibrarySearchHit &b) {
        const DocumentMatch &left = matches[a.pdfPath];
        const DocumentMatch &right = matches[b.pdfPath];
        if (left.tier() != right.tier() || (left.titleTier == left.tier()) != (right.titleTier == right.tier())) return false;
        return a.matches > b.matches;
    });
    if (!order.isEmpty()) {
        const DocumentMatch &best = matches[order.constFirst()];
        *bestRank = best.rank();
        *phraseMatched = best.tier() < 2;
    }
    return hits;
}
}

QList<LibrarySearchHit> LibrarySearchIndex::search(const QString &query, LibrarySearchScope scope, QString *error) const
{
    const QString match = matchExpression(query);
    if (match.isEmpty() || !QFileInfo::exists(m_path)) return {};
    // The words in order, the last one still being typed.
    const QStringList words = queryWords(query);
    const QString phrase = words.size() > 1 ? QStringLiteral("\"%1\"*").arg(words.join(QLatin1Char(' '))) : QString();
    // The last word as a finished one: "bar" finds "bar" before "barrier".
    const QString finished = match.chopped(1);
    // Words typed with capitals mean those letters: BAR is not any bar.
    QList<QRegularExpression> asTyped;
    for (const QString &word : words) {
        if (word != word.toLower()) {
            asTyped.append(QRegularExpression(QStringLiteral("(?<![\\p{L}\\p{N}])%1(?![\\p{L}\\p{N}])").arg(QRegularExpression::escape(word))));
        }
    }
    struct Group {
        bool source;
        QString filter;
        int limit;
        QList<LibrarySearchHit> hits;
        double score = 0;
        bool perDocument = false;
    };
    const bool documents = scope != LibrarySearchScope::Sources;
    const bool sources = scope != LibrarySearchScope::Documents;
    QList<Group> groups;
    if (sources) groups.append({true, QStringLiteral("kind = 'source'"), 6, {}});
    if (documents) {
        groups.append({false, QString(), 20, {}, 0, true});
    }
    if (sources) groups.append({true, QStringLiteral("kind = 'source-line'"), documents ? 12 : 30, {}});
    if (documents) {
        groups.append({false, QStringLiteral("kind IN ('comment', 'note', 'diary')"), 8, {}});
        groups.append({false, QStringLiteral("kind = 'card'"), 8, {}});
    }
    const QString connection = QStringLiteral("thoughts-library-search-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        if (openDatabase(m_path, connection, error)) {
            QSqlDatabase db = QSqlDatabase::database(connection, false);
            QHash<QString, QString> titles;
            QHash<QString, QPair<QString, QString>> sourceFiles; // path -> relative path, project
            QSqlQuery names(db);
            if (documents) {
                names.exec(QStringLiteral("SELECT pdf_path, title FROM titles"));
                while (names.next()) {
                    titles.insert(names.value(0).toString(), names.value(1).toString());
                }
            }
            if (sources) {
                names.exec(QStringLiteral("SELECT path, relative_path, project FROM indexed_sources"));
                while (names.next()) {
                    sourceFiles.insert(names.value(0).toString(), {names.value(1).toString(), names.value(2).toString()});
                }
            }
            names.finish();
            for (Group &group : groups) {
                QSet<QString> seen;
                bool phraseMatched = false;
                double best = 0;
                if (group.perDocument) {
                    group.hits = documentHits(db, {phrase, finished, match}, asTyped, titles, group.limit, &best, &phraseMatched, error);
                    group.score = best - (phraseMatched ? 1000.0 : 0.0);
                    continue;
                }
                const bool files = group.source && group.filter.contains(QLatin1String("'source'"));
                // File names are re-ranked below; read more candidates for that.
                const int candidates = files ? group.limit * 4 : group.limit;
                QList<LibrarySearchHit> found;
                // 0: every word as typed, 1: finished words, 2: the word being typed.
                QHash<QString, int> tiers;
                // Exact phrase matches first, then finished words, then the word being typed.
                for (const QString &expression : {phrase, finished, match}) {
                    if (expression.isEmpty() || found.size() >= candidates) continue;
                    QSqlQuery select(db);
                    select.prepare(group.source
                                       ? QStringLiteral("SELECT kind, path, line, context, title, snippet(source_entries, -1, char(1), char(2), '…', 14), rank, "
                                                        "highlight(source_entries, 5, char(1), char(2)) "
                                                        "FROM source_entries WHERE source_entries MATCH ? AND %1 ORDER BY rank LIMIT %2")
                                             .arg(group.filter)
                                             .arg(candidates)
                                       : QStringLiteral("SELECT kind, ref, pdf_path, page, context, title, snippet(entries, -1, char(1), char(2), '…', 14), rank, body "
                                                        "FROM entries WHERE entries MATCH ? AND %1 ORDER BY rank LIMIT %2")
                                             .arg(group.filter)
                                             .arg(candidates));
                    select.addBindValue(expression);
                    if (!select.exec()) {
                        // Sources that were never indexed have no table yet.
                        if (error) *error = select.lastError().text();
                        continue;
                    }
                    while (select.next() && found.size() < candidates) {
                        LibrarySearchHit hit;
                        if (group.source) {
                            const QString path = select.value(1).toString();
                            const auto file = sourceFiles.value(path);
                            hit.kind = select.value(0).toString();
                            hit.ref = path;
                            hit.sourcePath = path;
                            hit.page = select.value(2).toInt();
                            hit.title = file.first;
                            hit.context = file.second;
                            if (hit.kind == QLatin1String("source")) {
                                hit.snippet = select.value(5).toString();
                            } else {
                                locateLine(select.value(7).toString(), hit.page, &hit.line, &hit.snippet);
                            }
                        } else {
                            hit = {select.value(0).toString(), select.value(1).toString(), select.value(2).toString(), select.value(3).toInt(),
                                   select.value(5).toString(), select.value(4).toString(), select.value(6).toString()};
                        }
                        const QString key = hit.kind + QLatin1Char('\n') + hit.ref + QLatin1Char('\n') + QString::number(hit.page);
                        if (seen.contains(key)) continue;
                        seen.insert(key);
                        if (!group.source) {
                            tiers.insert(key, matchesAsTyped(select.value(8).toString(), asTyped) ? 0 : expression == match ? 2 : 1);
                        }
                        if (found.isEmpty()) {
                            best = select.value(group.source ? 6 : 7).toDouble();
                            phraseMatched = expression == phrase;
                        }
                        // Comments, cards and pages are shown under their document's current name.
                        const QString document = titles.value(hit.pdfPath);
                        if (!group.source && !document.isEmpty() && hit.kind != QLatin1String("document") && hit.kind != QLatin1String("note")
                            && hit.kind != QLatin1String("diary")) {
                            hit.context = hit.kind == QLatin1String("card") && hit.page > 0 ? i18n("%1 · p. %2", document, hit.page) : document;
                        }
                        found.append(hit);
                    }
                }
                bool nameMatched = false;
                if (files) {
                    // A file whose name holds every word beats one matched by its folders.
                    std::stable_sort(found.begin(), found.end(), [&words](const LibrarySearchHit &a, const LibrarySearchHit &b) {
                        return nameMatchesAll(a.title, words) && !nameMatchesAll(b.title, words);
                    });
                    nameMatched = !found.isEmpty() && nameMatchesAll(found.constFirst().title, words);
                }
                if (!group.source) {
                    QList<LibrarySearchHit> byTier[3];
                    for (const LibrarySearchHit &hit : std::as_const(found)) {
                        byTier[tiers.value(hit.kind + QLatin1Char('\n') + hit.ref + QLatin1Char('\n') + QString::number(hit.page))].append(hit);
                    }
                    found.clear();
                    for (const QList<LibrarySearchHit> &tier : byTier) found += tier;
                }
                group.hits = found.mid(0, group.limit);
                // bm25 ranks are negative, lower is better; a phrase match outranks any other
                // and a file found by its name outranks both.
                group.score = best - (phraseMatched ? 1000.0 : 0.0) - (nameMatched ? 2000.0 : 0.0);
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    // The group holding the best match comes first; ties keep the fixed order.
    std::stable_sort(groups.begin(), groups.end(), [](const Group &a, const Group &b) {
        if (a.hits.isEmpty() != b.hits.isEmpty()) return !a.hits.isEmpty();
        return a.score < b.score;
    });
    QList<LibrarySearchHit> hits;
    for (const Group &group : std::as_const(groups)) hits += group.hits;
    return hits;
}

namespace
{
enum Roles { HitRole = Qt::UserRole, HeaderRole, HtmlRole };

QString snippetHtml(const QString &snippet)
{
    QString html = snippet.simplified().toHtmlEscaped();
    html.replace(QChar(1), QStringLiteral("<span style=\"background-color: rgba(250, 210, 60, 0.55);\">"));
    html.replace(QChar(2), QStringLiteral("</span>"));
    return html;
}

QString kindLabel(const LibrarySearchHit &hit)
{
    if (hit.kind == QLatin1String("source")) return i18n("File");
    if (hit.kind == QLatin1String("source-line")) return i18n("Line %1", hit.line);
    if (hit.kind == QLatin1String("document")) {
        return hit.matchingPages > 0 ? i18n("%1 · %2", i18np("%1 match", "%1 matches", hit.matches), i18np("%1 page", "%1 pages", hit.matchingPages)) : i18n("Document");
    }
    if (hit.kind == QLatin1String("note")) return i18n("Note");
    if (hit.kind == QLatin1String("diary")) return i18n("Diary");
    if (hit.kind == QLatin1String("card")) return i18n("Review card");
    return hit.page > 0 ? i18n("Comment · p. %1", hit.page) : i18n("Comment");
}

QString groupLabel(const QString &kind)
{
    if (kind == QLatin1String("document")) return i18n("Documents");
    if (kind == QLatin1String("card")) return i18n("Review cards");
    if (kind == QLatin1String("source")) return i18n("Source files");
    if (kind == QLatin1String("source-line")) return i18n("Source text");
    return i18n("Comments and notes");
}

QString groupOf(const QString &kind)
{
    return kind == QLatin1String("note") || kind == QLatin1String("diary") ? QStringLiteral("comment") : kind;
}

void layoutDocument(QTextDocument &document, const QStyleOptionViewItem &option, const QModelIndex &index)
{
    document.setDefaultFont(option.font);
    document.setDocumentMargin(index.data(HeaderRole).toBool() ? 4 : 6);
    // One line each for the heading and the snippet keeps every row the same
    // height whether or not the list shows a scroll bar; long text is clipped.
    QTextOption noWrap;
    noWrap.setWrapMode(QTextOption::NoWrap);
    document.setDefaultTextOption(noWrap);
    document.setHtml(index.data(HtmlRole).toString());
    // Size hints are asked without a row rectangle; rows span the list's width.
    const auto *view = qobject_cast<const QAbstractItemView *>(option.widget);
    const int width = option.rect.width() > 0 ? option.rect.width() : view ? view->viewport()->width() : 400;
    document.setTextWidth(qMax(80, width));
}
}

void LibrarySearchHitDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QStyleOptionViewItem item = option;
    initStyleOption(&item, index);
    item.text.clear();
    QStyle *style = option.widget ? option.widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &item, painter, option.widget);
    QTextDocument document;
    layoutDocument(document, option, index);
    painter->save();
    painter->setClipRect(option.rect.adjusted(0, 0, -4, 0));
    painter->translate(option.rect.topLeft());
    QAbstractTextDocumentLayout::PaintContext context;
    context.palette = option.palette;
    context.palette.setColor(QPalette::Text, option.palette.color(option.state & QStyle::State_Selected ? QPalette::HighlightedText : QPalette::Text));
    document.documentLayout()->draw(painter, context);
    painter->restore();
}

QSize LibrarySearchHitDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QTextDocument document;
    layoutDocument(document, option, index);
    return QSize(option.rect.width(), qCeil(document.size().height()));
}

QString LibrarySearchField::scopeName(LibrarySearchScope scope)
{
    switch (scope) {
    case LibrarySearchScope::Sources:
        return i18nc("@item:inmenu search scope", "Sources");
    case LibrarySearchScope::All:
        return i18nc("@item:inmenu search scope", "All");
    case LibrarySearchScope::Documents:
        break;
    }
    return i18nc("@item:inmenu search scope", "Documents");
}

QString LibrarySearchField::scopeKey(LibrarySearchScope scope)
{
    switch (scope) {
    case LibrarySearchScope::Sources:
        return QStringLiteral("sources");
    case LibrarySearchScope::All:
        return QStringLiteral("all");
    case LibrarySearchScope::Documents:
        break;
    }
    return QStringLiteral("documents");
}

LibrarySearchScope LibrarySearchField::scopeFromKey(const QString &key)
{
    if (key == QLatin1String("sources")) return LibrarySearchScope::Sources;
    if (key == QLatin1String("all")) return LibrarySearchScope::All;
    return LibrarySearchScope::Documents;
}

LibrarySearchField::LibrarySearchField(QString databasePath, QWidget *parent)
    : QLineEdit(parent)
    , m_index(std::move(databasePath))
{
    setClearButtonEnabled(true);
    // The scope sits inside the field on the left, like a search field's menu
    // in macOS: "Documents ▾" opens Documents / Sources / All.
    m_scopeButton = new QToolButton(this);
    m_scopeButton->setObjectName(QStringLiteral("librarySearchScope"));
    m_scopeButton->setAutoRaise(true);
    m_scopeButton->setFocusPolicy(Qt::NoFocus);
    m_scopeButton->setCursor(Qt::ArrowCursor);
    m_scopeButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_scopeButton->setAccessibleName(i18n("Search scope"));
    auto *menu = new QMenu(m_scopeButton);
    menu->setObjectName(QStringLiteral("librarySearchScopeMenu"));
    auto *group = new QActionGroup(menu);
    const QList<QPair<LibrarySearchScope, QString>> scopes{
        {LibrarySearchScope::Documents, i18n("Document names, PDF text, comments, notes and review cards")},
        {LibrarySearchScope::Sources, i18n("Source files of the Library's projects: their names and text")},
        {LibrarySearchScope::All, i18n("Documents and sources together")}};
    for (int index = 0; index < scopes.size(); ++index) {
        QAction *action = menu->addAction(scopeName(scopes.at(index).first));
        action->setObjectName(QStringLiteral("librarySearchScope-") + scopeKey(scopes.at(index).first));
        action->setCheckable(true);
        action->setActionGroup(group);
        action->setToolTip(scopes.at(index).second);
        action->setStatusTip(scopes.at(index).second);
        // Shown as a hint; the field itself handles the keys while focused.
        action->setShortcut(QKeySequence(Qt::CTRL | Qt::Key(Qt::Key_1 + index)));
        action->setShortcutContext(Qt::WidgetShortcut);
        const LibrarySearchScope scope = scopes.at(index).first;
        connect(action, &QAction::triggered, this, [this, scope] {
            // Picking a scope continues the search in it.
            setFocus(Qt::OtherFocusReason);
            setScope(scope);
        });
        m_scopeActions.append(action);
    }
    menu->setToolTipsVisible(true);
    connect(m_scopeButton, &QToolButton::clicked, this, [this, menu] {
        menu->popup(m_scopeButton->mapToGlobal(QPoint(0, m_scopeButton->height())));
    });
    updateScopeButton();
    m_delay = new QTimer(this);
    m_delay->setSingleShot(true);
    m_delay->setInterval(60);
    connect(m_delay, &QTimer::timeout, this, &LibrarySearchField::runSearch);
    connect(this, &QLineEdit::textEdited, m_delay, qOverload<>(&QTimer::start));
    connect(this, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (text.trimmed().isEmpty()) hideResults();
    });
}

void LibrarySearchField::setIndexingStatus(const QString &status)
{
    m_indexingStatus = status;
    if (m_results && m_results->isVisible()) runSearch();
}

void LibrarySearchField::setSourceIndexingStatus(const QString &status)
{
    m_sourceIndexingStatus = status;
    if (m_results && m_results->isVisible()) runSearch();
}

QString LibrarySearchField::currentStatus() const
{
    QStringList status;
    if (m_scope != LibrarySearchScope::Sources && !m_indexingStatus.isEmpty()) status.append(m_indexingStatus);
    if (m_scope != LibrarySearchScope::Documents && !m_sourceIndexingStatus.isEmpty()) status.append(m_sourceIndexingStatus);
    return status.join(QLatin1Char(' '));
}

void LibrarySearchField::setScope(LibrarySearchScope scope)
{
    const bool changed = scope != m_scope;
    m_scope = scope;
    updateScopeButton();
    if (!changed) return;
    Q_EMIT scopeChanged(scope);
    if (!LibrarySearchIndex::queryWords(text()).isEmpty() && hasFocus()) runSearch();
    else refreshResults();
}

void LibrarySearchField::updateScopeButton()
{
    for (QAction *action : std::as_const(m_scopeActions)) {
        action->setChecked(action->objectName() == QStringLiteral("librarySearchScope-") + scopeKey(m_scope));
    }
    m_scopeButton->setText(scopeName(m_scope) + QStringLiteral(" \u25BE"));
    m_scopeButton->setToolTip(i18n("Search %1. Click to choose what to search (%2, %3, %4).", scopeName(m_scope).toLower(),
                                   QKeySequence(Qt::CTRL | Qt::Key_1).toString(QKeySequence::NativeText),
                                   QKeySequence(Qt::CTRL | Qt::Key_2).toString(QKeySequence::NativeText),
                                   QKeySequence(Qt::CTRL | Qt::Key_3).toString(QKeySequence::NativeText)));
    QFont font = this->font();
    if (font.pointSizeF() > 0) font.setPointSizeF(font.pointSizeF() * 0.9);
    m_scopeButton->setFont(font);
    m_scopeButton->adjustSize();
    const int buttonHeight = qMin(m_scopeButton->sizeHint().height(), qMax(16, height() - 4));
    m_scopeButton->setFixedHeight(buttonHeight);
    m_scopeButton->move(2, (height() - buttonHeight) / 2);
    const QMargins margins = textMargins();
    setTextMargins(m_scopeButton->width() + 2, margins.top(), margins.right(), margins.bottom());
}

void LibrarySearchField::resizeEvent(QResizeEvent *event)
{
    QLineEdit::resizeEvent(event);
    updateScopeButton();
}

void LibrarySearchField::changeEvent(QEvent *event)
{
    QLineEdit::changeEvent(event);
    if (m_scopeButton && (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)) updateScopeButton();
}

bool LibrarySearchField::event(QEvent *event)
{
    if (event->type() == QEvent::ShortcutOverride) {
        // ⌘1, ⌘2 and ⌘3 choose the scope while the field has focus.
        const auto *key = static_cast<QKeyEvent *>(event);
        if ((key->modifiers() & ~Qt::KeypadModifier) == Qt::ControlModifier && key->key() >= Qt::Key_1 && key->key() <= Qt::Key_3) {
            event->accept();
            return true;
        }
    }
    return QLineEdit::event(event);
}

void LibrarySearchField::refreshResults()
{
    if (m_results && m_results->isVisible()) runSearch();
}

void LibrarySearchField::runSearch()
{
    const QString query = text().trimmed();
    if (LibrarySearchIndex::queryWords(query).isEmpty()) {
        hideResults();
        return;
    }
    if (!m_results) {
        // A child of the window, drawn over the tabs below the field, so the
        // field keeps keyboard focus while results are shown.
        m_results = new QListWidget(window());
        m_results->setObjectName(QStringLiteral("librarySearchResults"));
        m_results->setFocusPolicy(Qt::NoFocus);
        m_results->setItemDelegate(new LibrarySearchHitDelegate(m_results));
        m_results->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        m_results->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_results->setFrameShape(QFrame::StyledPanel);
        connect(m_results, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) { activateRow(m_results->row(item)); });
        window()->installEventFilter(this);
    }
    QElapsedTimer timer;
    timer.start();
    m_hits = m_index.search(query, m_scope);
    m_results->clear();
    QString group;
    for (int index = 0; index < m_hits.size(); ++index) {
        const LibrarySearchHit &hit = m_hits.at(index);
        if (groupOf(hit.kind) != group) {
            group = groupOf(hit.kind);
            auto *header = new QListWidgetItem(m_results);
            header->setFlags(Qt::NoItemFlags);
            header->setData(HeaderRole, true);
            header->setData(HtmlRole, QStringLiteral("<span style=\"font-size: small; font-weight: 600; color: gray;\">%1</span>").arg(groupLabel(group).toHtmlEscaped()));
        }
        const QString plainSnippet = QString(hit.snippet).remove(QChar(1)).remove(QChar(2)).simplified();
        QString heading = (hit.title.isEmpty() ? hit.context : hit.title).simplified().toHtmlEscaped();
        QString detail = snippetHtml(hit.snippet);
        if (hit.kind == QLatin1String("document") && hit.matchingPages == 0) {
            // The match is in the name or the project; show both plainly.
            detail = hit.context.toHtmlEscaped();
        } else if (hit.kind == QLatin1String("source")) {
            // The path with its matched words, then the project.
            heading = snippetHtml(hit.snippet);
            detail = hit.context.toHtmlEscaped();
        } else if (hit.kind == QLatin1String("source-line")) {
            // The file name, then where it lives: its folder and project.
            const QString folder = hit.title.contains(QLatin1Char('/')) ? hit.title.section(QLatin1Char('/'), 0, -2) : QString();
            const QString where = QStringList{folder, hit.context}.filter(QRegularExpression(QStringLiteral("\\S"))).join(QStringLiteral(" · "));
            heading = QStringLiteral("%1 <span style=\"color: gray; font-weight: normal;\">%2</span>")
                          .arg(QFileInfo(hit.title).fileName().toHtmlEscaped(), where.toHtmlEscaped());
        } else if (hit.kind == QLatin1String("card") && hit.title.simplified().contains(plainSnippet)) {
            // Matched in the question: highlight it there and name the document below.
            heading = snippetHtml(hit.snippet);
            detail = hit.context.toHtmlEscaped();
        }
        auto *item = new QListWidgetItem(m_results);
        item->setData(HitRole, index);
        item->setData(HtmlRole, QStringLiteral("<span style=\"color: gray; font-size: small;\">%1</span> &nbsp;<b>%2</b><br>%3")
                                    .arg(kindLabel(hit).toHtmlEscaped(), heading, detail));
        item->setToolTip(hit.kind.startsWith(QLatin1String("source")) ? (hit.context.isEmpty() ? hit.title : i18n("%1 · %2", hit.context, hit.title))
                         : hit.title.isEmpty()                    ? hit.context
                                                                  : hit.title);
    }
    const QString status = currentStatus();
    if (m_hits.isEmpty()) {
        auto *empty = new QListWidgetItem(m_results);
        empty->setFlags(Qt::NoItemFlags);
        const QString none = m_scope == LibrarySearchScope::Documents ? i18n("No matches") : i18n("No matches in %1", scopeName(m_scope).toLower());
        empty->setData(HtmlRole, QStringLiteral("<span style=\"color: gray;\">%1</span>").arg((status.isEmpty() ? none : status).toHtmlEscaped()));
    } else if (!status.isEmpty()) {
        auto *statusItem = new QListWidgetItem(m_results);
        statusItem->setFlags(Qt::NoItemFlags);
        statusItem->setData(HtmlRole, QStringLiteral("<span style=\"color: gray; font-size: small;\">%1</span>").arg(status.toHtmlEscaped()));
    }
    setProperty("lastSearchMs", timer.elapsed());
    placeResults();
    moveSelection(1);
}

void LibrarySearchField::placeResults()
{
    if (!m_results) return;
    QWidget *host = window();
    const QPoint origin = mapTo(host, QPoint(0, height() + 4));
    m_results->setFixedWidth(qMax(200, qMin(qMax(width(), 620), host->width() - origin.x() - 12)));
    m_results->ensurePolished();
    m_results->viewport()->resize(m_results->width() - 2 * m_results->frameWidth(), m_results->viewport()->height());
    int contentHeight = 2 * m_results->frameWidth();
    for (int row = 0; row < m_results->count(); ++row) {
        contentHeight += m_results->sizeHintForRow(row);
    }
    m_results->setFixedHeight(qMin(contentHeight, qMax(120, host->height() - origin.y() - 16)));
    m_results->move(origin);
    m_results->raise();
    m_results->show();
}

void LibrarySearchField::hideResults()
{
    if (m_results) m_results->hide();
}

void LibrarySearchField::moveSelection(int step)
{
    if (!m_results || !m_results->isVisible()) return;
    int row = m_results->currentRow();
    if (row < 0) row = step > 0 ? -1 : m_results->count();
    for (int next = row + step; next >= 0 && next < m_results->count(); next += step) {
        if (m_results->item(next)->flags() & Qt::ItemIsEnabled) {
            m_results->setCurrentRow(next);
            m_results->scrollToItem(m_results->item(next));
            return;
        }
    }
    if (row >= 0 && row < m_results->count()) {
        // Keep the first result visible together with its group heading.
        m_results->scrollToItem(m_results->item(qMax(0, row - 1)));
    }
}

void LibrarySearchField::activateRow(int row)
{
    if (!m_results || row < 0 || row >= m_results->count()) return;
    const QVariant hitIndex = m_results->item(row)->data(HitRole);
    if (!hitIndex.isValid() || hitIndex.toInt() < 0 || hitIndex.toInt() >= m_hits.size()) return;
    const LibrarySearchHit hit = m_hits.at(hitIndex.toInt());
    hideResults();
    Q_EMIT hitActivated(hit, text().trimmed());
}

void LibrarySearchField::keyPressEvent(QKeyEvent *event)
{
    const bool showing = m_results && m_results->isVisible();
    if ((event->modifiers() & ~Qt::KeypadModifier) == Qt::ControlModifier && event->key() >= Qt::Key_1 && event->key() <= Qt::Key_3) {
        setScope(event->key() == Qt::Key_1 ? LibrarySearchScope::Documents : event->key() == Qt::Key_2 ? LibrarySearchScope::Sources : LibrarySearchScope::All);
        return;
    }
    if (event->modifiers() & Qt::AltModifier && event->key() == Qt::Key_Down) {
        // ⌥↓ opens the scope menu.
        m_scopeButton->click();
        return;
    }
    switch (event->key()) {
    case Qt::Key_Down:
        if (!showing) runSearch();
        else moveSelection(1);
        return;
    case Qt::Key_Up:
        moveSelection(-1);
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (showing) {
            activateRow(m_results->currentRow());
        } else {
            runSearch();
        }
        return;
    case Qt::Key_Escape:
        if (showing) hideResults();
        else clear();
        return;
    default:
        QLineEdit::keyPressEvent(event);
    }
}

void LibrarySearchField::focusOutEvent(QFocusEvent *event)
{
    QLineEdit::focusOutEvent(event);
    // The scope menu (or the field's own context menu) opens over the results.
    if (event->reason() == Qt::PopupFocusReason) return;
    // Clicking a result does not take focus (the list accepts none), so losing
    // focus means the user moved elsewhere.
    hideResults();
}

bool LibrarySearchField::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == window() && m_results && m_results->isVisible()) {
        if (event->type() == QEvent::Resize) placeResults();
        if (event->type() == QEvent::WindowDeactivate) hideResults();
    }
    return QLineEdit::eventFilter(watched, event);
}
}

#include "moc_librarysearch.cpp"

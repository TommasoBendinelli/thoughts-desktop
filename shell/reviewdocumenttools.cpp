#include "../part/documentidentity.h"
#include "../part/thoughtsplatform.h"
// SPDX-License-Identifier: GPL-2.0-or-later
#include "reviewdocumenttools.h"
#include "agentproposalparser.h"
#include "agentdocumentdescription.h"
#include "../part/agentprompts.h"
#include "../part/reviewcardfeedback.h"
#include "part/sourceannotationindex.h"
#include "part/docscommentsynchronizer.h"
#include <poppler-qt6.h>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QHash>
#include <QMap>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>
#include <limits>
#include <QtEndian>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <memory>
#include <functional>
namespace Thoughts {
namespace {
QByteArray read(const QString &path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }
QString digest(const QByteArray &bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); }
QString fingerprint(Poppler::Page &page) {
    const QSizeF size = page.pageSizeF() * (96.0 / 72.0);
    if (size.width() <= 0 || size.height() <= 0 || size.width() * size.height() > 32000000) return {};
    const QImage rgb = page.renderToImage(96, 96).convertToFormat(QImage::Format_RGB888);
    if (rgb.isNull()) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArray(reviewFingerprintMethod));
    const quint32 dimensions[] {qToBigEndian(quint32(rgb.width())), qToBigEndian(quint32(rgb.height()))};
    hash.addData(reinterpret_cast<const char *>(dimensions), sizeof(dimensions));
    for (int y = 0; y < rgb.height(); ++y) hash.addData(reinterpret_cast<const char *>(rgb.constScanLine(y)), rgb.width() * 3);
    return QString::fromLatin1(hash.result().toHex());
}
std::unique_ptr<Poppler::Document> document(const QByteArray &bytes) {
    auto pdf = Poppler::Document::loadFromData(bytes);
    if (pdf && !pdf->isLocked()) {
        pdf->setRenderHint(Poppler::Document::Antialiasing);
        pdf->setRenderHint(Poppler::Document::TextAntialiasing);
        return pdf;
    }
    return {};
}
}
bool isBasicReviewCard(const QJsonObject &card) {
    const auto method = card.value(QStringLiteral("generation_method")).toString();
    return method == QLatin1String("basic_half_page") || method == QLatin1String("basic_passages_v1") || method == QLatin1String("basic_passages_v2");
}
QString basicReviewSnapshotPath(const QString &docsRoot, const QJsonObject &card) {
    const auto primary = card.value(QStringLiteral("primary")).toObject();
    const auto sha = primary.value(QStringLiteral("pdf_sha256")).toString();
    const auto stem = primary.value(QStringLiteral("pdf_stem")).toString();
    if (!QRegularExpression(QStringLiteral("^[a-f0-9]{64}$")).match(sha).hasMatch()
        || !QRegularExpression(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]*$")).match(stem).hasMatch()) return {};
    return QDir(docsRoot).filePath(QStringLiteral(".runtime/review-pdfs/%1/%2.pdf").arg(sha, stem));
}
QString reviewCardPdfPath(const QString &docsRoot, const QJsonObject &card) {
    const auto primary = card.value(QStringLiteral("primary")).toObject();
    const DocumentIdentityStore identities(docsRoot);
    const auto id = primary.value(QStringLiteral("pdf_document_id")).toString(primary.value(QStringLiteral("document_id")).toString());
    const auto readable = identities.readablePath(id);
    if (!readable.isEmpty()) return readable;
    const auto entry = identities.byId(id);
    return entry.isEmpty() ? QString() : QDir(docsRoot).filePath(QStringLiteral("current_pdfs/") + entry.value(QStringLiteral("filename")).toString());
}
QJsonArray limitBasicCardExtent(const QJsonArray &candidates, const QString &pdfSha256) {
    QJsonArray result;
    for (const auto &value : candidates) {
        const auto candidate = value.toObject();
        if (!candidate.value(QStringLiteral("source_anchor")).toObject().isEmpty()) {
            result.append(candidate); continue;
        }
        QMap<int, QJsonArray> pages;
        for (const auto &region : candidate.value(QStringLiteral("regions")).toArray())
            pages[region.toObject().value(QStringLiteral("page")).toInt()].append(region.toObject().value(QStringLiteral("rect")));
        double extent = 0;
        QMap<int, QPair<double, double>> bounds;
        for (auto page = pages.cbegin(); page != pages.cend(); ++page) {
            double top = 1, bottom = 0;
            for (const auto &value : page.value()) {
                const auto r = value.toArray();
                top = std::min(top, r[1].toDouble()); bottom = std::max(bottom, r[3].toDouble());
            }
            bounds[page.key()] = {top, bottom}; extent += bottom - top;
        }
        if (extent <= .5 + 1e-9) { result.append(candidate); continue; }
        // A clipped passage cannot retain an anchor to the entire source block.
        // Keep its immutable PDF identity instead of expanding it during reflow.
        for (auto page = pages.cbegin(); page != pages.cend(); ++page) {
            const auto [top, bottom] = bounds[page.key()];
            const int count = std::max(1, int(std::ceil((bottom-top)/.5 - 1e-9)));
            for (int i=0; i<count; ++i) {
                const double a = top+(bottom-top)*i/count, b = top+(bottom-top)*(i+1)/count;
                QJsonArray clipped;
                for (const auto &value : page.value()) {
                    const auto r = value.toArray();
                    if (r[1].toDouble() < b && r[3].toDouble() > a)
                        clipped.append(QJsonObject{{QStringLiteral("page"),page.key()}, {QStringLiteral("rect"),QJsonArray{r[0],std::max(a,r[1].toDouble()),r[2],std::min(b,r[3].toDouble())}}});
                }
                if (clipped.isEmpty()) continue;
                const QJsonArray identity {QStringLiteral("basic-card-limit-v1"),pdfSha256,candidate.value(QStringLiteral("key")),clipped};
                result.append(QJsonObject{{QStringLiteral("key"),digest(QJsonDocument(identity).toJson(QJsonDocument::Compact))}, {QStringLiteral("regions"),clipped}});
            }
        }
    }
    return result;
}
QVariantMap prepareBasicReviewCards(const QString &pdfPath, const QString &bundleRoot) {
    const auto bytes = read(pdfPath);
    auto pdf = document(bytes);
    if (!pdf || pdf->numPages() < 1)
        return {{QStringLiteral("error"), QStringLiteral("The PDF could not be read.")}};
    const QString root = bundleRoot.isEmpty() ? QDir(Thoughts::Platform::applicationDataRoot()).filePath(QStringLiteral("thoughts-server/source-bundle")) : bundleRoot;
    const auto pointer = QJsonDocument::fromJson(read(QDir(root).filePath(QStringLiteral("documents/%1.json").arg(QFileInfo(pdfPath).fileName())))).object();
    const auto artifactSha = pointer.value(QStringLiteral("artifact_sha256")).toString();
    const auto artifact = QRegularExpression(QStringLiteral("^[a-f0-9]{64}$")).match(artifactSha).hasMatch() ? read(QDir(root).filePath(QStringLiteral("objects/%1").arg(artifactSha))) : QByteArray();
    const auto manifest = QJsonDocument::fromJson(artifact).object();
    const auto plan = manifest.value(QStringLiteral("pdf")).toObject().value(QStringLiteral("basic_review")).toObject();
    QJsonArray candidates;
    if (digest(artifact) == artifactSha && plan.value(QStringLiteral("schema_version")).toInt() == 1
        && plan.value(QStringLiteral("pdf_sha256")).toString() == digest(bytes)) candidates = plan.value(QStringLiteral("cards")).toArray();
    const bool sourcePlan = !candidates.isEmpty();
    if (!sourcePlan) {
        const auto directory = QFileInfo(pdfPath).absoluteDir();
        const auto sourceIndex = QJsonDocument::fromJson(read(directory.filePath(QStringLiteral(".thoughts-source-index.json")))).object();
        const auto library = QJsonDocument::fromJson(read(directory.filePath(QStringLiteral(".thoughts-library-index.json")))).object();
        bool needsSource = !pointer.isEmpty() && (digest(artifact) != artifactSha
            || !manifest.value(QStringLiteral("sources")).toArray().isEmpty());
        for (const auto &value : sourceIndex.value(QStringLiteral("pdfs")).toArray())
            if (value.toObject().value(QStringLiteral("filename")).toString() == QFileInfo(pdfPath).fileName()) needsSource = true;
        for (const auto &value : library.value(QStringLiteral("pdfs")).toArray()) {
            const auto entry = value.toObject();
            if (entry.value(QStringLiteral("filename")).toString() == QFileInfo(pdfPath).fileName()
                && entry.value(QStringLiteral("kind")).toString() != QLatin1String("unlinked")) needsSource = true;
        }
        if (needsSource) return {{QStringLiteral("error"), QStringLiteral("The verified source passage plan is unavailable. Sync before generating source-backed cards.")}};
    }
    QVariantList pages;
    for (int index = 0; index < pdf->numPages(); ++index) {
        auto page = pdf->page(index);
        const auto sha = page ? fingerprint(*page) : QString();
        if (sha.isEmpty())
            return {{QStringLiteral("error"), QStringLiteral("Page %1 could not be rendered.").arg(index + 1)}};
        pages.append(sha);
        if (!sourcePlan) {
            // A whitespace cut cannot bisect a word. With no paragraph-sized gap,
            // keep the page whole instead of inventing a half-page boundary.
            const auto size = page->pageSizeF();
            QList<QPair<double, double>> lines;
            for (const auto &box : page->textList()) {
                const auto r = box->boundingBox();
                if (!box->text().trimmed().isEmpty()) lines.append({r.top()/size.height(), r.bottom()/size.height()});
            }
            std::sort(lines.begin(), lines.end());
            QList<QPair<double,double>> bands;
            for (const auto &line : lines) {
                if (!bands.isEmpty() && line.first <= bands.last().second + .004) bands.last().second = std::max(bands.last().second, line.second);
                else bands.append(line);
            }
            const auto raster = page->renderToImage(36, 36).convertToFormat(QImage::Format_RGB32);
            double cut = -1;
            for (int i=1; i<bands.size(); ++i) {
                const double middle = (bands[i-1].second + bands[i].first)/2;
                if (middle > .25 && middle < .75 && bands[i].first-bands[i-1].second >= .012
                    && (cut < 0 || std::abs(middle-.5)<std::abs(cut-.5))) {
                    bool clear = !raster.isNull();
                    const int y = int(middle * raster.height());
                    for (int row = std::max(0, y-1); clear && row <= std::min(raster.height()-1, y+1); ++row) {
                        for (int x=0; x<raster.width(); ++x) {
                            const auto pixel = raster.pixel(x,row);
                            if (qRed(pixel)<240 || qGreen(pixel)<240 || qBlue(pixel)<240) { clear=false; break; }
                        }
                    }
                    if (clear) cut=middle;
                }
            }
            QList<double> cuts {0.0}; if (cut > 0) cuts.append(cut); cuts.append(1.0);
            for (int i=1; i<cuts.size(); ++i) {
                QJsonArray regions;
                const int n = std::max(1, int(std::ceil((cuts[i]-cuts[i-1])/.5)));
                for (int j=0; j<n; ++j) regions.append(QJsonObject{{QStringLiteral("page"),index+1},
                    {QStringLiteral("rect"),QJsonArray{0.0,cuts[i-1]+(cuts[i]-cuts[i-1])*j/n,1.0,cuts[i-1]+(cuts[i]-cuts[i-1])*(j+1)/n}}});
                candidates.append(QJsonObject {{QStringLiteral("key"), digest(digest(bytes).toUtf8()+QJsonDocument(regions).toJson(QJsonDocument::Compact))}, {QStringLiteral("regions"), regions}});
            }
        }
    }
    return {{QStringLiteral("pdf_path"), pdfPath},
            {QStringLiteral("pdf_stem"), QFileInfo(pdfPath).completeBaseName()},
            {QStringLiteral("pdf_sha256"), digest(bytes)},
            {QStringLiteral("page_count"), pdf->numPages()},
            {QStringLiteral("basic_page_fingerprints"), pages},
            {QStringLiteral("basic_candidates"), limitBasicCardExtent(candidates, digest(bytes)).toVariantList()}};
}
QVariantMap fingerprintReviewPage(const QString &pdfPath, int page, const QString &expectedPdfSha256) {
    const auto bytes = read(pdfPath);
    if (bytes.isEmpty() || (!expectedPdfSha256.isEmpty() && digest(bytes) != expectedPdfSha256))
        return {{QStringLiteral("error"), QStringLiteral("The PDF changed or is unavailable. Reopen it before creating the card.")}};
    auto pdf = document(bytes);
    auto selected = pdf && page > 0 && page <= pdf->numPages() ? pdf->page(page - 1) : nullptr;
    const QString sha = selected ? fingerprint(*selected) : QString();
    if (sha.isEmpty()) return {{QStringLiteral("error"), QStringLiteral("The selected PDF page could not be read.")}};
    return {{QStringLiteral("page_visual_sha256"), sha}, {QStringLiteral("method"), QString::fromLatin1(reviewFingerprintMethod)}};
}
QVariantMap locateReviewPage(const QString &pdfPath, const QString &wanted) {
    auto pdf = document(read(pdfPath));
    int match = 0;
    if (pdf && QRegularExpression(QStringLiteral("^[a-f0-9]{64}$")).match(wanted).hasMatch()) {
        for (int index = 0; index < pdf->numPages(); ++index) {
            auto page = pdf->page(index);
            if (page && fingerprint(*page) == wanted) {
                if (match) return {{QStringLiteral("error"), QStringLiteral("More than one page matches this card. Sync to retrieve verified answer locations.")}};
                match = index + 1;
            }
        }
    }
    if (!match) return {{QStringLiteral("error"), QStringLiteral("The card’s page could not be identified uniquely. Sync to retrieve verified answer locations.")}};
    return {{QStringLiteral("page"), match}};
}
// Marks each inclusive line range as one segment, in file order. Only complete, visible
// top-level source lines are marked: never a macro argument, proposal, comment, preamble, or
// table cell.
// How markTexRanges treats what TeX does not show inside a range. A new card's passage (the
// default) holds no definition, no other card's marker and no pending proposal. A passage that
// already is a card's (ExistingPassage: reading a card's markers, a merge moving them) may hold
// definitions: the card exists and builds, and its markers expand to nothing. With
// HeldProposals it may also hold pending agent proposals whole. A passage may hold other cards'
// passages whole, never part of one, when it already is a card's or WrapsCards (a section card:
// its section holds the cards made from its paragraphs); the server accepts such nesting.
enum TexMarkMode { NewPassage = 0, ExistingPassage = 1, HeldProposals = 2, WrapsCards = 4 };
// The definitions a new card's passage must not hold (whole command names; \def also as the
// start of a longer name, as before).
static const QRegularExpression &texDefinition()
{
    static const QRegularExpression pattern(QStringLiteral(R"(\\(def[A-Za-z@]*|newcommand|renewcommand|providecommand|DeclareRobustCommand)(?![A-Za-z@]))"));
    return pattern;
}
// @p lineNumbers: the line of the caller's source shown for each line of @p source (one-based), when they differ.
static ReviewAnchorPlan markTexRanges(const QString &path, const QByteArray &source, QList<QPair<int, int>> ranges,
                                      const QString &cardId, int *failedRange, int mode = NewPassage, const QList<int> &lineNumbers = {}) {
    const bool aroundPendingProposals = mode & HeldProposals;
    ReviewAnchorPlan plan;
    if (failedRange) *failedRange = -1;
    const auto fail = [&plan, failedRange](const QString &error, int range = -1) {
        plan.error = error;
        if (failedRange) *failedRange = range;
        return plan;
    };
    if (QUuid(cardId).isNull() || ranges.isEmpty() || source.contains(cardId.toUtf8())) return fail(QStringLiteral("The source selection or card identity is invalid."));
    const QString text = QString::fromUtf8(source);
    if (text.toUtf8() != source) return fail(QStringLiteral("The source is not valid UTF-8."));
    const auto lines = source.split('\n');
    QList<int> order(ranges.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&ranges](int a, int b) { return ranges[a].first < ranges[b].first; });
    for (int i = 0; i < order.size(); ++i) {
        const auto &range = ranges[order[i]];
        if (range.first < 1 || range.second > lines.size() || range.first > range.second
            || (i > 0 && range.first <= ranges[order[i - 1]].second))
            return fail(QStringLiteral("The source line range is invalid."), order[i]);
    }
    if (ranges.size() > 20) return fail(QStringLiteral("The selection contains too many disjoint source ranges."));
    // A pending proposal keeps its original next to its replacement. TeX shows
    // only the replacement, so scan structure as TeX reads it: blank each
    // wrapper's hidden part (keeping line breaks), and never mark inside it.
    QString scanText = text;
    QSet<int> inProposal;
    const auto lineOfOffset = [&text](int offset) { return int(QStringView(text).left(offset).count(QLatin1Char('\n'))) + 1; };
    for (const auto &[from, to] : latexAgentProposalHiddenRanges(text)) {
        for (int line = lineOfOffset(from); line <= lineOfOffset(to); ++line) inProposal.insert(line);
        for (int k = from; k < to; ++k)
            if (scanText.at(k) != QLatin1Char('\n')) scanText[k] = QLatin1Char(' ');
    }
    // A merge re-marks passages that hold pending proposals: the lines of a valid proposal that
    // one range holds whole are no obstacle, since markers go on lines of their own outside it.
    // A broken one, or one a range holds only in part, still is.
    if (aroundPendingProposals && !inProposal.isEmpty()) {
        const AgentProposalParse parsed = parseAgentProposals(path, text);
        QSet<int> broken;
        for (const auto &issue : parsed.issues)
            for (int line = lineOfOffset(issue.offset); line <= lineOfOffset(issue.offset + issue.length); ++line) broken.insert(line);
        for (const auto &proposal : parsed.proposals) {
            const int first = lineOfOffset(proposal.offset), last = lineOfOffset(proposal.offset + proposal.length);
            const bool held = std::any_of(ranges.cbegin(), ranges.cend(), [&](const auto &range) { return range.first <= first && last <= range.second; });
            if (!held) continue;
            for (int line = first; line <= last; ++line)
                if (!broken.contains(line)) inProposal.remove(line);
        }
    }
    const auto scanLines = scanText.toUtf8().split('\n');
    int depth = 0; bool visible = !scanText.contains(QStringLiteral("\\begin{document}"));
    QStringList environments;
    QSet<int> safeBefore, safeAfter;
    QHash<int, QString> hidden;  // line -> why TeX does not show it
    // Other cards' marker lines as Thoughts writes them: line -> (card:segment, start?).
    QHash<int, QPair<QString, bool>> otherMarkers;
    QHash<QString, QPair<int, int>> otherPassages; // card:segment -> (start line, end line)
    static const QRegularExpression canonicalMarker(QStringLiteral(R"(^\\ReviewCardMask(Start|End)\{([0-9A-Fa-f-]{36})\}\{([0-9]+)\}%$)"));
    const QSet<QString> allowed {QStringLiteral("document"), QStringLiteral("itemize"), QStringLiteral("enumerate"), QStringLiteral("description")};
    for (int index = 0; index < scanLines.size(); ++index) {
        const auto line = scanLines[index];
        bool plainContext = depth == 0 && visible;
        for (const auto &env : environments) plainContext = plainContext && allowed.contains(env);
        QByteArray active;
        for (int j = 0; j < line.size(); ++j) {
            if (line[j] == '%') break;
            active += line[j];
            if (line[j] == '\\' && j + 1 < line.size()) { active += line[++j]; continue; }
            if (line[j] == '{') ++depth;
            if (line[j] == '}') --depth;
        }
        static const QRegularExpression envPattern(QStringLiteral(R"(\\(begin|end)\{([^}]+)\})"));
        auto matches = envPattern.globalMatch(QString::fromUtf8(active));
        if (plainContext) safeBefore.insert(index + 1);
        while (matches.hasNext()) { const auto m = matches.next();
            if (m.captured(1) == QLatin1String("begin")) environments.append(m.captured(2));
            else if (!environments.isEmpty() && environments.last() == m.captured(2)) environments.removeLast();
            else return fail(QStringLiteral("The source environments are not balanced."));
            if (m.captured(2) == QLatin1String("document")) visible = m.captured(1) == QLatin1String("begin");
        }
        bool after = depth == 0 && visible;
        for (const auto &env : environments) after = after && allowed.contains(env);
        if (after) safeAfter.insert(index + 1);
        const QString activeText = QString::fromUtf8(active);
        const auto canonical = canonicalMarker.match(QString::fromUtf8(line));
        if (canonical.hasMatch() && !inProposal.contains(index + 1)) {
            const QString key = canonical.captured(2).toLower() + QLatin1Char(':') + canonical.captured(3);
            const bool start = canonical.captured(1) == QLatin1String("Start");
            otherMarkers.insert(index + 1, {key, start});
            auto &passage = otherPassages[key];
            (start ? passage.first : passage.second) = index + 1;
        } else if (activeText.contains(QLatin1String("\\ReviewCardMask")))
            hidden.insert(index + 1, QStringLiteral("holds another review card's marker; cards cannot be nested"));
        else if (inProposal.contains(index + 1))
            hidden.insert(index + 1, mode & ExistingPassage
                ? QStringLiteral("holds part of a pending agent proposal that runs past the passage; decide or withdraw it first")
                : QStringLiteral("holds a pending agent proposal; resolve it before creating a card from this passage"));
        else if (!(mode & ExistingPassage))
            if (const auto definition = texDefinition().match(activeText); definition.hasMatch())
                hidden.insert(index + 1, QStringLiteral("holds a definition (\\%1), which the PDF does not show; select the passage without it")
                                             .arg(definition.captured(1)));
        if (depth < 0) return fail(QStringLiteral("The source braces are not balanced."));
    }
    for (const int index : std::as_const(order)) {
        const auto &range = ranges[index];
        if (!safeBefore.contains(range.first) || !safeAfter.contains(range.second))
            return fail(QStringLiteral("Select a complete source passage, including its surrounding equation or figure structure."), index);
        for (int line = range.first; line <= range.second; ++line) {
            if (hidden.contains(line))
                return fail(QStringLiteral("Line %1 %2.").arg(QString::number(lineNumbers.value(line - 1, line)), hidden.value(line)), index);
            if (!otherMarkers.contains(line)) continue;
            if (!(mode & (ExistingPassage | WrapsCards)))
                return fail(QStringLiteral("Line %1 holds another review card's marker; cards cannot be nested.").arg(QString::number(lineNumbers.value(line - 1, line))), index);
            const auto passage = otherPassages.value(otherMarkers.value(line).first);
            if (!passage.first || !passage.second || passage.first >= passage.second || passage.first < range.first || passage.second > range.second)
                return fail(QStringLiteral("Line %1 holds a marker of another review card whose passage runs past this one; a card can hold other cards only whole.")
                                .arg(QString::number(lineNumbers.value(line - 1, line))), index);
        }
    }
    QByteArray output; QJsonArray segments; int range = 0;
    for (int index = 0; index < lines.size(); ++index) {
        if (range < order.size() && index + 1 == ranges[order[range]].first) {
            const auto start = QByteArray("\\ReviewCardMaskStart{") + cardId.toUtf8() + "}{" + QByteArray::number(range) + "}";
            const auto bodyStart = output.count('\n') + 1;
            output += start + "%\n";
            QByteArray body("%\n");
            for (; index + 1 <= ranges[order[range]].second; ++index) { body += lines[index] + '\n'; output += lines[index] + '\n'; }
            segments.append(QJsonObject {{QStringLiteral("segment_id"), QString::number(range)}, {QStringLiteral("source_file"), path},
                {QStringLiteral("content_sha256"), digest(body)}, {QStringLiteral("line_start"), bodyStart}, {QStringLiteral("line_end"), output.count('\n') + 1}});
            output += QByteArray("\\ReviewCardMaskEnd{") + cardId.toUtf8() + "}{" + QByteArray::number(range) + "}%\n";
            --index; ++range;
        } else { output += lines[index]; if (index + 1 < lines.size()) output += '\n'; }
    }
    for (int i = 0; i < segments.size(); ++i) { auto s = segments[i].toObject(); s.insert(QStringLiteral("source_sha256"), digest(output)); segments[i] = s; }
    plan.path = path; plan.original = source; plan.replacement = output;
    plan.anchor = {{QStringLiteral("version"), 2}, {QStringLiteral("segments"), segments}};
    return plan;
}
static ReviewAnchorPlan markReviewSourceLinesImpl(const QString &path, const QByteArray &source, const QList<int> &selectedLines, const QString &cardId) {
    ReviewAnchorPlan plan;
    if (QUuid(cardId).isNull() || selectedLines.isEmpty() || source.contains(cardId.toUtf8())) {
        plan.error = QStringLiteral("The source selection or card identity is invalid.");
        return plan;
    }
    const auto lines = source.split('\n');
    QList<int> selected = selectedLines; std::sort(selected.begin(), selected.end()); selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
    if (selected.first() < 1 || selected.last() > lines.size()) { plan.error = QStringLiteral("The source line range is invalid."); return plan; }
    for (int line = selected.first(); line <= selected.last(); ++line) {
        if (!selected.contains(line) && !lines[line - 1].trimmed().isEmpty()) {
            plan.error = QStringLiteral("A card must contain one continuous source passage. Create separate cards for separated passages.");
            return plan;
        }
    }
    return markTexRanges(path, source, {{selected.first(), selected.last()}}, cardId, nullptr);
}
ReviewAnchorPlan markReviewSourceLines(const QString &path, const QByteArray &source, const QList<int> &selectedLines, const QString &cardId) {
    if (path.endsWith(QLatin1String(".md"), Qt::CaseInsensitive) || path.endsWith(QLatin1String(".markdown"), Qt::CaseInsensitive)) {
        ReviewAnchorPlan invalid;
        if (selectedLines.isEmpty() || QUuid(cardId).isNull() || source.contains(cardId.toUtf8())) {
            invalid.error = QStringLiteral("The source selection or card identity is invalid.");
            return invalid;
        }
        const auto bounds = std::minmax_element(selectedLines.cbegin(), selectedLines.cend());
        const auto lines = source.split('\n');
        if (*bounds.first < 1 || *bounds.second > lines.size()) {
            invalid.error = QStringLiteral("The source line range is invalid.");
            return invalid;
        }
        for (int line = *bounds.first; line <= *bounds.second; ++line) {
            if (!selectedLines.contains(line) && !lines[line - 1].trimmed().isEmpty()) {
                invalid.error = QStringLiteral("A card must contain one continuous source passage.");
                return invalid;
            }
        }
        return markReviewSourceSelection(path, source, *bounds.first, *bounds.second + 1, cardId);
    }
    return markReviewSourceLinesImpl(path, source, selectedLines, cardId);
}
// Why lines [first, end) of a source cannot become one answer passage of @p cardId, or empty:
// a pending or broken proposal or another card's markers in it, or for Markdown a boundary
// that is not between complete top-level blocks.
// With @p wrapCards (a section card, a merge) other cards' passages that lie wholly inside are no obstacle.
static QString sourceSelectionError(const QString &path, const QByteArray &source, int first, int end, const QString &cardId,
                                   bool aroundPendingProposals = false, bool wrapCards = false) {
    const QString text = QString::fromUtf8(source);
    const auto lines = source.split('\n');
    const int lineCount = lines.size() - int(source.endsWith('\n'));
    if (text.toUtf8() != source || source.contains('\0') || first < 1 || end <= first || end > lineCount + 1)
        return QStringLiteral("Select complete source lines in a UTF-8 file.");
    int startOffset = 0, endOffset = 0;
    const auto textLines = text.split(QLatin1Char('\n'));
    for (int i = 0; i < end - 1; ++i) {
        if (i < first - 1) startOffset += textLines[i].size() + 1;
        endOffset += textLines[i].size() + (i + 1 < textLines.size() ? 1 : 0);
    }
    if (text.mid(startOffset, endOffset - startOffset).trimmed().isEmpty()) return QStringLiteral("Select an answer passage.");
    // Broken proposals occupy their span too; elsewhere they do not block a card.
    const AgentProposalParse parsed = parseAgentProposals(path, text);
    // A merge re-marks passages that hold pending proposals whole (aroundPendingProposals).
    QList<QPair<int, int>> heldProposals;
    for (const auto &p : parsed.proposals) {
        if (!(startOffset < p.offset + p.length && p.offset < endOffset)) continue;
        if (!aroundPendingProposals) return QStringLiteral("Resolve the pending proposal before creating a card from this passage.");
        if (p.offset < startOffset || p.offset + p.length > endOffset)
            return QStringLiteral("The pending proposal on line %1 starts inside this passage and ends outside it, or the other way round. Decide or withdraw it first.").arg(p.line);
        heldProposals.append({p.offset, p.offset + p.length});
    }
    for (const auto &issue : parsed.issues)
        if (startOffset < issue.offset + issue.length && issue.offset < endOffset)
            return QStringLiteral("Fix or remove the broken proposal before creating a card from this passage: %1").arg(agentProposalIssueMessage(issue));
    const auto locations = scanAnnotationLocations(path, source, {}).value(QStringLiteral("locations")).toObject();
    for (auto it = locations.begin(); it != locations.end(); ++it) {
        // The card's own other passages are no obstacle; only other cards' are.
        if (!it.key().startsWith(QLatin1String("review_card\n")) || it.key().endsWith(QLatin1Char('\n') + cardId)) continue;
        for (const auto &value : it.value().toArray()) {
            const auto range = value.toObject();
            const int start = range.value(QStringLiteral("start_line")).toInt(), last = range.value(QStringLiteral("end_line")).toInt();
            if (wrapCards && first <= start && last < end) continue;
            if (first <= last && start < end)
                return QStringLiteral("A review card already covers part of this passage.");
        }
    }
    if (path.endsWith(QLatin1String(".tex"), Qt::CaseInsensitive)) return {};
    if (!path.endsWith(QLatin1String(".md"), Qt::CaseInsensitive) && !path.endsWith(QLatin1String(".markdown"), Qt::CaseInsensitive))
        return QStringLiteral("Choose a LaTeX or Markdown source file.");
    // Only top-level block boundaries are safe for Markdown HTML markers.
    // Reject literal/HTML contexts and unfinished blocks rather than changing
    // paragraph, list, table, or code structure as a side effect of anchoring.
    QByteArray fence; bool html = false;
    QSet<int> safe;
    const QRegularExpression fenceStart(QStringLiteral(R"(^ {0,3}(`{3,}|~{3,}))"));
    for (int i = 0; i <= lineCount; ++i) {
        if (fence.isEmpty() && !html) safe.insert(i + 1);
        if (i == lineCount) break;
        const auto line = lines[i]; const auto trimmed = line.trimmed();
        if (!fence.isEmpty()) {
            int n = 0; while (n < trimmed.size() && trimmed[n] == fence[0]) ++n;
            if (n >= fence.size() && trimmed.mid(n).isEmpty()) fence.clear();
        } else if (const auto match = fenceStart.match(QString::fromUtf8(line)); match.hasMatch()) {
            fence = match.captured(1).toLatin1();
        } else {
            if (line.contains("<!--")) html = true;
            if (line.contains("-->")) html = false;
        }
    }
    const auto heading = [](const QByteArray &line) { return QRegularExpression(QStringLiteral("^#{1,6}\\s")).match(QString::fromUtf8(line)).hasMatch(); };
    if (!safe.contains(first) || !safe.contains(end)
        || (first > 1 && !lines[first - 2].trimmed().isEmpty() && !lines[first - 1].trimmed().isEmpty() && !heading(lines[first - 1])
            && !heading(lines[first - 2]))
        || (end <= lineCount && !lines[end - 2].trimmed().isEmpty() && !lines[end - 1].trimmed().isEmpty() && !heading(lines[end - 1]))
        || lines[first - 1].startsWith(' ') || lines[first - 1].startsWith('\t'))
        return QStringLiteral("Select complete Markdown blocks, including the full paragraph, list, table, or code block.");
    // Boundaries inside HTML blocks, front matter, nested lists and blockquotes
    // cannot be proven neutral by this local editor path.
    int lineOffset = startOffset;
    for (int i = first - 1; i < end - 1; ++i) {
        const int at = lineOffset;
        lineOffset += int(textLines[i].size()) + 1;
        // A pending proposal held whole is a block of its own; its marker lines are not HTML.
        if (std::any_of(heldProposals.cbegin(), heldProposals.cend(), [at](const auto &span) { return span.first <= at && at < span.second; })) continue;
        // Another card's marker line, of a passage held whole (checked above).
        static const QRegularExpression reviewMarker(QStringLiteral(R"(^ {0,3}<!-- thoughts:v1:(start|end) review:[0-9A-Fa-f-]{36}:answer:[0-9]+ -->\r?$)"));
        if (wrapCards && reviewMarker.match(QString::fromUtf8(lines[i])).hasMatch()) continue;
        const auto trimmed = lines[i].trimmed();
        if (trimmed.startsWith('<') || trimmed.startsWith('>') || (i == 0 && (trimmed == "---" || trimmed == "+++")))
            return QStringLiteral("Select rendered Markdown content outside HTML, front matter, and blockquotes.");
    }
    for (int i : {first - 2, end - 1}) {
        const int direction = i < first - 1 ? -1 : 1;
        // An unindented heading terminates a preceding list, even when the
        // list's last item is the nearest nonblank line above the selection.
        if (direction < 0 && heading(lines[first - 1])) continue;
        while (i >= 0 && i < lineCount && lines[i].trimmed().isEmpty()) i += direction;
        if (i >= 0 && i < lineCount && (lines[i].startsWith(' ') || lines[i].startsWith('\t')
            || QRegularExpression(QStringLiteral(R"(^([-+*]|[0-9]+[.)])\s)")).match(QString::fromUtf8(lines[i])).hasMatch()))
            return QStringLiteral("Include the entire Markdown list in the selection.");
    }
    return {};
}
static bool isMarkdownPath(const QString &path) {
    return path.endsWith(QLatin1String(".md"), Qt::CaseInsensitive) || path.endsWith(QLatin1String(".markdown"), Qt::CaseInsensitive);
}
// Inclusive line ranges, each wrapped in its own answer:n marker pair (n in file order).
static ReviewAnchorPlan markMarkdownRanges(const QString &path, const QByteArray &source, const QList<QPair<int, int>> &ranges,
                                           const QString &cardId, int *failedRange, bool aroundPendingProposals = false, bool wrapCards = false) {
    ReviewAnchorPlan invalid;
    if (failedRange) *failedRange = -1;
    const auto fail = [&](const QString &message, int range = -1) {
        invalid.error = message;
        if (failedRange) *failedRange = range;
        return invalid;
    };
    if (QUuid(cardId).isNull() || ranges.isEmpty() || source.contains(cardId.toUtf8()))
        return fail(QStringLiteral("The source selection or card identity is invalid."));
    if (ranges.size() > 20) return fail(QStringLiteral("The selection contains too many disjoint source ranges."));
    QList<int> order(ranges.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&ranges](int a, int b) { return ranges[a].first < ranges[b].first; });
    for (int i = 0; i < order.size(); ++i) {
        const auto &range = ranges[order[i]];
        if (range.first > range.second || (i > 0 && range.first <= ranges[order[i - 1]].second))
            return fail(QStringLiteral("The source line range is invalid."), order[i]);
        const QString error = sourceSelectionError(path, source, range.first, range.second + 1, cardId, aroundPendingProposals,
                                                   wrapCards || aroundPendingProposals);
        if (!error.isEmpty()) return fail(error, order[i]);
    }
    const QString text = QString::fromUtf8(source);
    const auto textLines = text.split(QLatin1Char('\n'));
    const auto offsetOfLine = [&textLines](int line) {
        qsizetype offset = 0;
        for (int i = 0; i < line - 1 && i < textLines.size(); ++i) offset += textLines[i].size() + 1;
        return offset;
    };
    QString marked;
    qsizetype copied = 0;
    for (int segment = 0; segment < order.size(); ++segment) {
        const auto &range = ranges[order[segment]];
        const qsizetype startOffset = offsetOfLine(range.first);
        const qsizetype endOffset = std::min<qsizetype>(offsetOfLine(range.second + 1), text.size());
        QString body = text.mid(startOffset, endOffset - startOffset);
        if (!body.endsWith(QLatin1Char('\n'))) body += QLatin1Char('\n');
        marked += text.mid(copied, startOffset - copied);
        marked += QStringLiteral("<!-- thoughts:v1:start review:%1:answer:%2 -->\n").arg(cardId).arg(segment);
        marked += body;
        marked += QStringLiteral("<!-- thoughts:v1:end review:%1:answer:%2 -->\n").arg(cardId).arg(segment);
        copied = endOffset;
    }
    marked += text.mid(copied);
    auto plan = readReviewSourceAnchor(path, marked.toUtf8(), cardId);
    if (!plan.error.isEmpty()) return fail(plan.error);
    if (plan.anchor.value(QStringLiteral("segments")).toArray().size() != ranges.size())
        return fail(QStringLiteral("The source line range is invalid."));
    plan.original = source; plan.replacement = marked.toUtf8();
    return plan;
}
ReviewAnchorPlan markReviewSourceRanges(const QString &path, const QByteArray &source, const QList<QPair<int, int>> &ranges,
                                        const QString &cardId, int *failedRange, bool existingPassages, bool wrapOtherCards) {
    if (isMarkdownPath(path)) return markMarkdownRanges(path, source, ranges, cardId, failedRange, existingPassages, wrapOtherCards);
    if (!path.endsWith(QLatin1String(".tex"), Qt::CaseInsensitive)) {
        if (failedRange) *failedRange = -1;
        ReviewAnchorPlan invalid;
        invalid.error = QStringLiteral("Choose a LaTeX or Markdown source file.");
        return invalid;
    }
    return markTexRanges(path, source, ranges, cardId, failedRange,
                         (existingPassages ? ExistingPassage | HeldProposals : NewPassage) | (wrapOtherCards ? WrapsCards : NewPassage));
}
ReviewAnchorPlan markReviewSourceSelection(const QString &path, const QByteArray &source, int first, int end, const QString &cardId) {
    ReviewAnchorPlan invalid;
    const QString error = sourceSelectionError(path, source, first, end, cardId);
    if (!error.isEmpty()) { invalid.error = error; return invalid; }
    if (path.endsWith(QLatin1String(".tex"), Qt::CaseInsensitive)) {
        QList<int> selected;
        for (int line = first; line < end; ++line) selected.append(line);
        return markReviewSourceLines(path, source, selected, cardId);
    }
    return markMarkdownRanges(path, source, {{first, end - 1}}, cardId, nullptr);
}
QJsonObject reviewPdfComponentOrigin(const QString &bundleRoot, const QString &pdfPath,
                                     const QString &pdfSha256, int page) {
    if (page < 1 || digest(read(pdfPath)) != pdfSha256) return {};
    const auto pointer = QJsonDocument::fromJson(read(QDir(bundleRoot).filePath(
        QStringLiteral("documents/%1.json").arg(QFileInfo(pdfPath).fileName())))).object();
    const auto artifactSha = pointer.value(QStringLiteral("artifact_sha256")).toString();
    if (artifactSha.size() != 64) return {};
    const auto artifact = read(QDir(bundleRoot).filePath(QStringLiteral("objects/%1").arg(artifactSha)));
    if (digest(artifact) != artifactSha) return {};
    const auto manifest = QJsonDocument::fromJson(artifact).object();
    const auto pdf = manifest.value(QStringLiteral("pdf")).toObject();
    if (pdf.value(QStringLiteral("pdf_sha256")).toString() != pdfSha256) return {};
    QJsonObject found;
    for (const auto &value : pdf.value(QStringLiteral("segments")).toArray()) {
        const auto segment = value.toObject();
        const int first = segment.value(QStringLiteral("start_page")).toInt();
        const int count = segment.value(QStringLiteral("page_count")).toInt();
        if (first < 1 || count < 1 || page < first || page >= first + count) continue;
        if (!found.isEmpty() || segment.value(QStringLiteral("source_available")).toBool(true)) return {};
        const QString id = segment.value(QStringLiteral("source_id")).toString();
        const QString componentSha = segment.value(QStringLiteral("fingerprint")).toString();
        if (id.isEmpty() || componentSha.size() != 64) return {};
        found = {{QStringLiteral("source_id"), id},
                 {QStringLiteral("component_pdf_sha256"), componentSha},
                 {QStringLiteral("local_page"), page - first + 1}};
    }
    return found;
}
ReviewAnchorPlan planReviewSourceAnchor(const QString &docsRoot, const QString &bundleRoot, const QString &pdfPath,
                                      const QString &pdfSha256, int pageNumber, const QRectF &region, const QString &cardId) {
    return planReviewSourceAnchors(docsRoot, bundleRoot, pdfPath, pdfSha256, {{pageNumber, region, false}}, cardId);
}

namespace {
// Floats are typeset away from their source position and hold content (drawings, generated
// tables) that maps to no authored line, so an area around one stands for all of it.
struct FloatEnvironment { int begin = 0, end = 0; };
QList<FloatEnvironment> texFloatEnvironments(const QByteArray &source) {
    static const QSet<QString> floats{QStringLiteral("table"), QStringLiteral("table*"), QStringLiteral("figure"), QStringLiteral("figure*"),
        QStringLiteral("algorithm"), QStringLiteral("wrapfigure"), QStringLiteral("wraptable"), QStringLiteral("sidewaystable"),
        QStringLiteral("sidewaysfigure")};
    static const QRegularExpression environment(QStringLiteral(R"(\\(begin|end)\s*\{([^}]+)\})"));
    QList<FloatEnvironment> found;
    QList<QPair<QString, int>> open;
    const auto lines = source.split('\n');
    for (int index = 0; index < lines.size(); ++index) {
        QByteArray active;
        const auto &line = lines[index];
        for (int j = 0; j < line.size(); ++j) {
            if (line[j] == '%') break;
            active += line[j];
            if (line[j] == '\\' && j + 1 < line.size()) active += line[++j];
        }
        auto matches = environment.globalMatch(QString::fromUtf8(active));
        while (matches.hasNext()) {
            const auto match = matches.next();
            const QString name = match.captured(2).trimmed();
            if (match.captured(1) == QLatin1String("begin")) {
                open.append({name, index + 1});
            } else {
                for (int i = open.size() - 1; i >= 0; --i) {
                    if (open[i].first != name) continue;
                    if (floats.contains(name)) found.append({open[i].second, index + 1});
                    open.erase(open.begin() + i, open.end());
                    break;
                }
            }
        }
    }
    return found;
}
// The smallest float environment around every line, or {0, 0}.
FloatEnvironment enclosingFloat(const QList<FloatEnvironment> &floats, const QList<int> &lines) {
    FloatEnvironment best;
    if (lines.isEmpty()) return best;
    const auto [low, high] = std::minmax_element(lines.cbegin(), lines.cend());
    for (const auto &candidate : floats) {
        if (candidate.begin <= *low && *high <= candidate.end && (!best.begin || candidate.end - candidate.begin < best.end - best.begin))
            best = candidate;
    }
    return best;
}
// A page's text in visual lines, top to bottom: boxes whose vertical centres fall in one band.
struct PageTextLine { qreal top = 0, bottom = 0; QList<const Poppler::TextBox *> boxes; };
QList<PageTextLine> pageTextLines(const std::vector<std::unique_ptr<Poppler::TextBox>> &boxes) {
    QList<const Poppler::TextBox *> sorted;
    for (const auto &box : boxes) if (!box->text().trimmed().isEmpty()) sorted.append(box.get());
    std::stable_sort(sorted.begin(), sorted.end(), [](const Poppler::TextBox *a, const Poppler::TextBox *b) {
        return a->boundingBox().center().y() < b->boundingBox().center().y();
    });
    QList<PageTextLine> lines;
    for (const auto *box : std::as_const(sorted)) {
        const QRectF bounds = box->boundingBox();
        if (!lines.isEmpty() && bounds.center().y() >= lines.last().top && bounds.center().y() <= lines.last().bottom) {
            lines.last().top = std::min(lines.last().top, bounds.top());
            lines.last().bottom = std::max(lines.last().bottom, bounds.bottom());
            lines.last().boxes.append(box);
        } else {
            lines.append({bounds.top(), bounds.bottom(), {box}});
        }
    }
    return lines;
}
// The words of a line that hold a letter, lower case and without punctuation.
QSet<QString> letteredWords(const PageTextLine &line) {
    QSet<QString> words;
    for (const auto *box : line.boxes) {
        QString word;
        for (const QChar c : box->text()) if (c.isLetterOrNumber()) word += c.toLower();
        if (std::any_of(word.cbegin(), word.cend(), [](QChar c) { return c.isLetter(); })) words.insert(word);
    }
    return words;
}
// Two pages show the same running header or footer line: at the same height, with the same
// words (at least half of them; a section title or date in it may change), or both without
// letters (page numbers).
bool sameFurnitureLine(const PageTextLine &a, const PageTextLine &b) {
    if (qAbs((a.top + a.bottom) / 2 - (b.top + b.bottom) / 2) > 3) return false;
    const QSet<QString> first = letteredWords(a), second = letteredWords(b);
    if (first.isEmpty() || second.isEmpty()) return first.isEmpty() && second.isEmpty();
    return 2 * QSet<QString>(first).intersect(second).size() >= std::max(first.size(), second.size());
}
// Page furniture: the running header, footer and page number the page layout prints. They are
// typeset by the page's output routine, so SyncTeX ties them to whatever source line TeX was
// reading at the page break, not to their text. They are the page's first or last lines (at
// most two at each edge, in its top or bottom fifth) that its neighbouring pages repeat at the
// same height.
QSet<const Poppler::TextBox *> pageFurniture(const QList<PageTextLine> &lines, qreal pageHeight, const QList<QList<PageTextLine>> &neighbours) {
    QSet<const Poppler::TextBox *> furniture;
    const auto repeated = [&neighbours](const PageTextLine &line) {
        return std::any_of(neighbours.cbegin(), neighbours.cend(), [&line](const QList<PageTextLine> &other) {
            return std::any_of(other.cbegin(), other.cend(), [&line](const PageTextLine &candidate) { return sameFurnitureLine(line, candidate); });
        });
    };
    int top = 0;
    for (; top < std::min<qsizetype>(2, lines.size()); ++top) {
        const auto &line = lines[top];
        if ((line.top + line.bottom) / 2 > pageHeight / 5 || !repeated(line)) break;
        for (const auto *box : line.boxes) furniture.insert(box);
    }
    for (int index = int(lines.size()) - 1, taken = 0; index >= top && taken < 2; --index, ++taken) {
        const auto &line = lines[index];
        if ((line.top + line.bottom) / 2 < pageHeight * 4 / 5 || !repeated(line)) break;
        for (const auto *box : line.boxes) furniture.insert(box);
    }
    return furniture;
}
bool ignorableSourceLine(const QByteArray &line, bool tex) {
    const auto trimmed = line.trimmed();
    return trimmed.isEmpty() || (tex && trimmed.startsWith('%'));
}

// Placing an area by its text (locateReviewTextInSource). Letters and digits a match needs,
// so a few words cannot match by chance (the phone's rule).
constexpr int minTextMatchLetters = 24;
// Prose pieces with fewer letters are not looked up alone, only allowed for between the others.
constexpr int minPieceLetters = 8;
// Source letters outside math that the PDF text does not explain: between two matched pieces,
// and on the passage's first line before it and last line after it.
constexpr int gapSlackLetters = 12;
constexpr int edgeSlackLetters = 3;

// The rendered letters and digits of a TeX or Markdown source, NFKD-decomposed and case-folded:
// command names, comments, card markers and the arguments of commands that print nothing
// (\label, \cite, link targets, ...) are left out. Math is kept but flagged, because its PDF
// text (Xi,Yi∈{0,1} for \(X_i, Y_i \in \{0,1\}\)) has the same letters in an unknown order.
struct SourceLetters {
    QString text;
    QString letters;
    QList<int> line, offset;
    QList<bool> math;
    QList<int> prose, all; // prefix counts of letters (not digits): outside math, and anywhere
    QList<QPair<int, int>> displays; // line spans of display math
    QList<int> lineStarts;           // offset of each line
    int lineOf(int offset) const { return int(std::upper_bound(lineStarts.cbegin(), lineStarts.cend(), offset) - lineStarts.cbegin()); }
    int indexOfOffset(int at) const { return int(std::lower_bound(offset.cbegin(), offset.cend(), at) - offset.cbegin()); }
    int indexOfLine(int number) const { return int(std::lower_bound(line.cbegin(), line.cend(), number) - line.cbegin()); }
    int proseBetween(int from, int to) const { return from < to ? prose[to] - prose[from] : 0; }
    int lettersBetween(int from, int to) const { return from < to ? all[to] - all[from] : 0; }
    bool wordStart(int index) const { return offset[index] == 0 || !text[offset[index] - 1].isLetterOrNumber(); }
    bool wordEnd(int index) const { return offset[index] + 1 >= text.size() || !text[offset[index] + 1].isLetterOrNumber(); }
    // Every boundary-respecting start of @p wanted (at most @p cap).
    QList<int> find(const QString &wanted, int cap = 4096) const {
        QList<int> found;
        for (int at = letters.indexOf(wanted); at >= 0 && found.size() < cap; at = letters.indexOf(wanted, at + 1))
            if (wordStart(at) && wordEnd(at + wanted.size() - 1)) found.append(at);
        return found;
    }
};
QString foldedLetters(QStringView text) {
    QString folded;
    for (const QChar c : text) {
        if (c.unicode() < 128) { if (c.isLetterOrNumber()) folded += c.toCaseFolded(); continue; }
        // The modifier apostrophes (ʻ ʼ ʽ, which PDF text may carry for ') count as letters in
        // Unicode, but not here.
        if (c.unicode() >= 0x02BB && c.unicode() <= 0x02BD) continue;
        for (const QChar d : QString(c).normalized(QString::NormalizationForm_KD))
            if (d.isLetterOrNumber()) folded += d.toCaseFolded();
    }
    return folded;
}
SourceLetters sourceLetters(const QString &path, const QString &text) {
    static const QSet<QString> mathEnvironments{QStringLiteral("equation"), QStringLiteral("equation*"), QStringLiteral("align"),
        QStringLiteral("align*"), QStringLiteral("gather"), QStringLiteral("gather*"), QStringLiteral("multline"), QStringLiteral("multline*"),
        QStringLiteral("eqnarray"), QStringLiteral("eqnarray*"), QStringLiteral("flalign"), QStringLiteral("flalign*"),
        QStringLiteral("alignat"), QStringLiteral("alignat*"), QStringLiteral("displaymath"), QStringLiteral("math")};
    // Commands whose first n arguments print nothing.
    static const QHash<QString, int> silent{{QStringLiteral("label"), 1}, {QStringLiteral("ref"), 1}, {QStringLiteral("eqref"), 1},
        {QStringLiteral("cref"), 1}, {QStringLiteral("Cref"), 1}, {QStringLiteral("autoref"), 1}, {QStringLiteral("pageref"), 1},
        {QStringLiteral("cite"), 1}, {QStringLiteral("citep"), 1}, {QStringLiteral("citet"), 1}, {QStringLiteral("citealp"), 1},
        {QStringLiteral("nocite"), 1}, {QStringLiteral("SourceLabel"), 1}, {QStringLiteral("includegraphics"), 1},
        {QStringLiteral("input"), 1}, {QStringLiteral("include"), 1}, {QStringLiteral("href"), 1}, {QStringLiteral("vspace"), 1},
        {QStringLiteral("hspace"), 1}, {QStringLiteral("setlength"), 2}, {QStringLiteral("usepackage"), 1},
        {QStringLiteral("documentclass"), 1}, {QStringLiteral("bibliography"), 1}, {QStringLiteral("bibliographystyle"), 1},
        {QStringLiteral("PdfLink"), 2}, {QStringLiteral("CrossPdfLink"), 2}, {QStringLiteral("SourceLink"), 2},
        {QStringLiteral("CrossSourceLink"), 2}, {QStringLiteral("ReviewCardMaskStart"), 2}, {QStringLiteral("ReviewCardMaskEnd"), 2},
        // Retired on 2026-10-26, but older notes still use them.
        {QStringLiteral("TexLink"), 2}, {QStringLiteral("CrossTexLink"), 2}};
    const bool tex = path.endsWith(QLatin1String(".tex"), Qt::CaseInsensitive);
    SourceLetters s;
    s.text = text;
    s.lineStarts.append(0);
    for (int i = 0; i < text.size(); ++i) if (text[i] == QLatin1Char('\n')) s.lineStarts.append(i + 1);
    const int n = text.size();
    bool inlineMath = false;
    int displayStart = 0;
    QString displayKind;
    const auto openDisplay = [&](int at, const QString &kind) { if (!displayStart) { displayStart = s.lineOf(at); displayKind = kind; } };
    const auto closeDisplay = [&](int at, const QString &kind) {
        if (displayStart && displayKind == kind) { s.displays.append({displayStart, s.lineOf(at)}); displayStart = 0; }
    };
    const auto skipBlanks = [&](int at) { while (at < n && (text[at] == QLatin1Char(' ') || text[at] == QLatin1Char('\t'))) ++at; return at; };
    const auto skipGroup = [&](int at, QChar open, QChar close) {
        for (int depth = 0; at < n; ++at) {
            if (text[at] == QLatin1Char('\\')) { ++at; continue; }
            if (text[at] == open) ++depth;
            else if (text[at] == close && --depth == 0) return at + 1;
        }
        return n;
    };
    for (int i = 0; i < n;) {
        const QChar c = text[i];
        if (tex && c == QLatin1Char('%')) { while (i < n && text[i] != QLatin1Char('\n')) ++i; continue; }
        if (!tex && QStringView(text).mid(i, 4) == QLatin1String("<!--")) {
            const int end = text.indexOf(QLatin1String("-->"), i + 4);
            i = end < 0 ? n : end + 3;
            continue;
        }
        if (c == QLatin1Char('\\') && i + 1 < n) {
            const QChar next = text[i + 1];
            if ((next.unicode() < 128 && next.isLetter()) || next == QLatin1Char('@')) {
                int j = i + 1;
                while (j < n && ((text[j].unicode() < 128 && text[j].isLetter()) || text[j] == QLatin1Char('@'))) ++j;
                const QString name = text.mid(i + 1, j - i - 1);
                if (j < n && text[j] == QLatin1Char('*')) ++j;
                i = j;
                if (name == QLatin1String("begin") || name == QLatin1String("end")) {
                    const int open = skipBlanks(i);
                    const int close = open < n && text[open] == QLatin1Char('{') ? text.indexOf(QLatin1Char('}'), open) : -1;
                    if (close < 0) continue;
                    const QString environment = text.mid(open + 1, close - open - 1).trimmed();
                    if (mathEnvironments.contains(environment)) {
                        if (name == QLatin1String("begin")) openDisplay(i, environment);
                        else closeDisplay(i, environment);
                    }
                    i = close + 1;
                } else if (const int count = silent.value(name)) {
                    for (int argument = 0; argument < count; ++argument) {
                        int at = skipBlanks(i);
                        while (at < n && text[at] == QLatin1Char('[')) at = skipBlanks(skipGroup(at, QLatin1Char('['), QLatin1Char(']')));
                        if (at >= n || text[at] != QLatin1Char('{')) break;
                        i = skipGroup(at, QLatin1Char('{'), QLatin1Char('}'));
                    }
                }
                continue;
            }
            if (next == QLatin1Char('(')) inlineMath = true;
            else if (next == QLatin1Char(')')) inlineMath = false;
            else if (next == QLatin1Char('[')) openDisplay(i, QStringLiteral("\\["));
            else if (next == QLatin1Char(']')) closeDisplay(i, QStringLiteral("\\["));
            i += 2;
            continue;
        }
        if (c == QLatin1Char('$')) {
            if (i + 1 < n && text[i + 1] == QLatin1Char('$')) {
                if (displayStart) closeDisplay(i, QStringLiteral("$$")); else openDisplay(i, QStringLiteral("$$"));
                i += 2;
            } else {
                inlineMath = !inlineMath;
                ++i;
            }
            continue;
        }
        const QString folded = foldedLetters(QStringView(text).mid(i, 1));
        for (const QChar letter : folded) {
            s.letters += letter;
            s.line.append(s.lineOf(i));
            s.offset.append(i);
            s.math.append(inlineMath || displayStart);
        }
        ++i;
    }
    s.prose.append(0);
    s.all.append(0);
    for (int i = 0; i < s.letters.size(); ++i) {
        s.prose.append(s.prose.last() + (!s.math[i] && s.letters[i].isLetter()));
        s.all.append(s.all.last() + s.letters[i].isLetter());
    }
    return s;
}

// One word of the area's PDF text. Prose is Latin letters with inner hyphens or apostrophes;
// anything else but punctuation (Xi,Yi∈{0,1}, =, p=0.03, θ) is treated as math.
struct PdfWord {
    QString letters; // letters and digits, folded like SourceLetters
    enum Kind { Neutral, Prose, Math } kind = Neutral;
    bool sentenceEnd = false;
    int latinLetters = 0;
};
PdfWord pdfWord(const QString &word) {
    PdfWord result;
    result.letters = foldedLetters(word);
    const QString decomposed = word.normalized(QString::NormalizationForm_KD);
    int first = 0, last = decomposed.size();
    while (first < last && decomposed[first].isPunct()) ++first;
    while (last > first && (decomposed[last - 1].isPunct() || decomposed[last - 1].category() == QChar::Mark_NonSpacing)) {
        if (QStringLiteral(".!?:;").contains(decomposed[last - 1])) result.sentenceEnd = true;
        --last;
    }
    const QStringView core = QStringView(decomposed).mid(first, last - first);
    if (core.isEmpty()) return result;
    bool latin = true;
    for (const QChar c : core) {
        if (c.isLetter() && c.script() == QChar::Script_Latin) ++result.latinLetters;
        else if (c.category() != QChar::Mark_NonSpacing && c != QLatin1Char('-') && c != QLatin1Char('\'') && c != QChar(0x2019) && c != QChar(0x02BC)) latin = false;
    }
    // Punctuation alone (a dash) joins prose; a symbol (=, \u2208, \u2211) is math.
    result.kind = latin && result.latinLetters >= 2 ? PdfWord::Prose : PdfWord::Math;
    return result;
}
}

ReviewTextPlacement locateReviewTextInSource(const QString &path, const QByteArray &source, const QStringList &words) {
    ReviewTextPlacement placement;
    QList<PdfWord> pdf;
    QString wanted;
    for (const QString &word : words) {
        for (const QString &part : word.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts)) {
            pdf.append(pdfWord(part));
            wanted += pdf.last().letters;
        }
    }
    if (wanted.size() < minTextMatchLetters) return placement;
    const QString text = QString::fromUtf8(source);
    const SourceLetters s = sourceLetters(path, text);
    const bool tex = path.endsWith(QLatin1String(".tex"), Qt::CaseInsensitive);
    // A passage [from, to) of s is found once only if every match gives the same lines.
    struct Found { int from = 0, to = 0, before = 0, after = 0, leading = 0, trailing = 0; };
    QList<Found> found;
    const auto distinctLines = [&s](const QList<Found> &matches) {
        QSet<QPair<int, int>> lines;
        for (const auto &match : matches) lines.insert({s.line[match.from], s.line[match.to - 1]});
        return lines.size();
    };

    // 1. The exact text, apart from whitespace and TeX comments.
    {
        QString searchable;
        QList<int> offsets;
        for (int i = 0; i < text.size(); ++i) {
            if (tex && text[i] == QLatin1Char('%') && (i == 0 || text[i - 1] != QLatin1Char('\\'))) {
                while (i + 1 < text.size() && text[i + 1] != QLatin1Char('\n')) ++i;
                continue;
            }
            if (text[i].isSpace()) {
                if (!searchable.isEmpty() && searchable.back() != QLatin1Char(' ')) { searchable += QLatin1Char(' '); offsets.append(i); }
                continue;
            }
            searchable += text[i];
            offsets.append(i);
        }
        const QString selected = words.join(QLatin1Char(' ')).simplified();
        for (int at = selected.isEmpty() ? -1 : searchable.indexOf(selected); at >= 0; at = searchable.indexOf(selected, at + 1)) {
            const int end = at + selected.size();
            if ((at > 0 && searchable[at - 1].isLetterOrNumber()) || (end < searchable.size() && searchable[end].isLetterOrNumber())) continue;
            const int from = s.indexOfOffset(offsets[at]), to = s.indexOfOffset(offsets[end - 1] + 1);
            if (from < to) found.append(Found{from, to});
        }
    }
    // 2. Its letters and digits only: PDF text hyphenates words at line ends, joins source lines
    //    and drops TeX markup.
    if (found.isEmpty()) {
        for (const int at : s.find(wanted)) found.append(Found{at, at + int(wanted.size())});
    }
    // 3. Its prose pieces one by one, in order, in one region of the source: the math between
    //    them may be typeset in any order. Every piece has to be found; shorter prose and
    //    single letters only allow for source prose between the pieces.
    if (found.isEmpty()) {
        struct Piece { QString letters; int firstWord = 0, lastWord = 0, before = 0; };
        QList<Piece> pieces;
        Piece current;
        int pending = 0, currentLatin = 0;
        const auto close = [&] {
            if (current.letters.size() >= minPieceLetters) { current.before = pending; pieces.append(current); pending = 0; }
            else pending += currentLatin;
            current = {};
            currentLatin = 0;
        };
        for (int w = 0; w < pdf.size(); ++w) {
            const auto &word = pdf[w];
            if (word.kind == PdfWord::Prose) {
                if (current.letters.isEmpty()) current.firstWord = w;
                current.letters += word.letters;
                current.lastWord = w;
                currentLatin += word.latinLetters;
                if (word.sentenceEnd) close();
            } else if (word.kind == PdfWord::Math) {
                close();
                if (word.latinLetters == 1 && word.letters.size() == 1) pending += 1;
            }
        }
        close();
        int matched = 0;
        for (const auto &piece : std::as_const(pieces)) matched += piece.letters.size();
        if (pieces.isEmpty() || matched < minTextMatchLetters) return placement;
        const auto lettersOf = [&pdf](int from, int to) {
            int count = 0;
            for (int w = from; w < to; ++w) count += int(std::count_if(pdf[w].letters.cbegin(), pdf[w].letters.cend(), [](QChar c) { return c.isLetter(); }));
            return count;
        };
        const int leading = lettersOf(0, pieces.first().firstWord), trailing = lettersOf(pieces.last().lastWord + 1, pdf.size());
        // Text drawn inside a picture (a figure's labels and axes) is no source text, and may even
        // repeat words of the caption. Where the source has a picture between two pieces, the
        // chain may therefore jump over the pieces between them; the first and the last piece
        // are always matched, and nowhere else may a piece be left out.
        const auto pictureBetween = [&s](int from, int to) {
            if (from <= 0 || to <= from || to >= s.offset.size()) return false;
            const int begin = s.offset[from - 1], end = s.offset[to];
            return QStringView(s.text).mid(begin, end - begin).contains(u"\\includegraphics");
        };
        // Each node is an occurrence of a piece, with the starts of the chains reaching it.
        struct Node { int at = 0; QList<int> origins; };
        QList<QList<Node>> occurrences(pieces.size());
        for (int j = 0; j < pieces.size(); ++j)
            for (const int at : s.find(pieces[j].letters)) occurrences[j].append(Node{at, {}});
        if (occurrences.first().isEmpty() || occurrences.last().isEmpty()) return placement;
        for (auto &node : occurrences.first()) node.origins.append(node.at);
        constexpr int maximumJump = 40; // pieces drawn inside one picture
        for (int j = 1; j < pieces.size(); ++j) {
            for (auto &node : occurrences[j]) {
                int skippedBefore = 0;
                for (int i = j - 1; i >= 0 && i >= j - 1 - maximumJump; --i) {
                    const bool across = i < j - 1;
                    if (across) skippedBefore += pieces[i + 1].before;
                    const int length = pieces[i].letters.size();
                    const int allowance = pieces[j].before + skippedBefore + gapSlackLetters;
                    for (const auto &from : std::as_const(occurrences[i])) {
                        if (from.origins.isEmpty()) continue;
                        const int fromEnd = from.at + length;
                        if (node.at < fromEnd || s.proseBetween(fromEnd, node.at) > allowance) continue;
                        if (across && !pictureBetween(fromEnd, node.at)) continue;
                        for (const int origin : from.origins)
                            if (!node.origins.contains(origin) && node.origins.size() < 4) node.origins.append(origin);
                    }
                }
            }
        }
        const QList<Node> &previous = occurrences.last();
        const int lastLength = pieces.last().letters.size();
        for (const auto &node : std::as_const(previous))
            for (const int origin : node.origins)
                if (found.size() < 8) found.append(Found{origin, node.at + lastLength, pieces.first().before, pending, leading, trailing});
    }
    if (found.isEmpty()) return placement;
    if (distinctLines(found) > 1) { placement.ambiguous = true; return placement; }
    const Found match = found.first();
    int first = s.line[match.from], last = s.line[match.to - 1];
    // The area must hold whole source lines: prose on them outside the match is not in it.
    const int lineBegin = s.indexOfLine(first), lineEnd = s.indexOfLine(last + 1);
    if (s.proseBetween(lineBegin, match.from) > match.before + edgeSlackLetters
        || s.proseBetween(match.to, lineEnd) > match.after + edgeSlackLetters) return placement;
    // Never cut display math. Math the area starts or ends with that the matched lines do not
    // hold is the display next to them.
    const auto lines = text.split(QLatin1Char('\n'));
    const auto blank = [&](int number) {
        const QString trimmed = lines.value(number - 1).trimmed();
        return trimmed.isEmpty() || (tex && trimmed.startsWith(QLatin1Char('%')));
    };
    for (const auto &display : s.displays) {
        if (display.first < first && first <= display.second) first = display.first;
        if (display.first <= last && last < display.second) last = display.second;
    }
    int unexplained = match.leading - s.lettersBetween(lineBegin, match.from);
    while (unexplained >= 2) {
        int above = first - 1;
        while (above > 0 && blank(above)) --above;
        const auto display = std::find_if(s.displays.cbegin(), s.displays.cend(), [above](const auto &d) { return d.second == above; });
        if (above < 1 || display == s.displays.cend()) break;
        unexplained -= s.lettersBetween(s.indexOfLine(display->first), s.indexOfLine(display->second + 1));
        first = display->first;
    }
    unexplained = match.trailing - s.lettersBetween(match.to, lineEnd);
    while (unexplained >= 2) {
        int below = last + 1;
        while (below <= lines.size() && blank(below)) ++below;
        const auto display = std::find_if(s.displays.cbegin(), s.displays.cend(), [below](const auto &d) { return d.first == below; });
        if (below > lines.size() || display == s.displays.cend()) break;
        unexplained -= s.lettersBetween(s.indexOfLine(display->first), s.indexOfLine(display->second + 1));
        last = display->second;
    }
    placement.first = first;
    placement.last = last;
    return placement;
}

ReviewAnchorPlan planReviewSourceAnchors(const QString &docsRoot, const QString &bundleRoot, const QString &pdfPath,
                                         const QString &pdfSha256, const QList<ReviewArea> &areas, const QString &cardId,
                                         bool replaceCardMarkers) {
    ReviewAnchorPlan plan;
    const auto fail = [&plan](const QString &error, int area = -1) { plan.error = error; plan.failedArea = area; return plan; };
    const auto bytes = read(pdfPath);
    plan.invalidPdf = true;
    if (areas.isEmpty()) return fail(QStringLiteral("Draw at least one answer area."));
    if (bytes.isEmpty() || digest(bytes) != pdfSha256) return fail(QStringLiteral("The PDF changed before source anchoring."));
    auto pdf = document(bytes);
    if (!pdf) return fail(QStringLiteral("The review page is unavailable."));
    QHash<int, std::shared_ptr<Poppler::Page>> pages;
    QHash<int, std::shared_ptr<std::vector<std::unique_ptr<Poppler::TextBox>>>> pageBoxes;
    const auto pageAt = [&](int number) -> Poppler::Page * {
        if (number < 1 || number > pdf->numPages()) return nullptr;
        if (!pages.contains(number)) pages.insert(number, std::shared_ptr<Poppler::Page>(pdf->page(number - 1).release()));
        return pages.value(number).get();
    };
    const auto boxesAt = [&](int number) -> const std::vector<std::unique_ptr<Poppler::TextBox>> & {
        if (!pageBoxes.contains(number))
            pageBoxes.insert(number, std::make_shared<std::vector<std::unique_ptr<Poppler::TextBox>>>(pageAt(number)->textList()));
        return *pageBoxes.value(number);
    };
    // Each page's running header, footer and page number (pageFurniture): never mapped to the
    // source, never part of an answer.
    QHash<int, QSet<const Poppler::TextBox *>> furnitureOf;
    const auto furnitureAt = [&](int number) -> const QSet<const Poppler::TextBox *> & {
        if (!furnitureOf.contains(number)) {
            QList<QList<PageTextLine>> neighbours;
            for (const int other : {number - 1, number + 1})
                if (other >= 1 && other <= pdf->numPages() && pageAt(other)) neighbours.append(pageTextLines(boxesAt(other)));
            furnitureOf.insert(number, pageFurniture(pageTextLines(boxesAt(number)), pageAt(number)->pageSizeF().height(), neighbours));
        }
        return furnitureOf[number];
    };
    const auto isFurniture = [&](int number, const std::unique_ptr<Poppler::TextBox> &box) { return furnitureAt(number).contains(box.get()); };
    QList<QRectF> selections;
    QStringList answers;
    for (int i = 0; i < areas.size(); ++i) {
        auto *page = pageAt(areas[i].page);
        const QRectF rect = areas[i].rect.normalized();
        if (!page || rect.isEmpty() || rect.left() < -1e-9 || rect.top() < -1e-9 || rect.right() > 1 + 1e-9 || rect.bottom() > 1 + 1e-9)
            return fail(QStringLiteral("The review page is unavailable."), i);
        const auto size = page->pageSizeF();
        selections.append(QRectF(rect.x()*size.width(), rect.y()*size.height(), rect.width()*size.width(), rect.height()*size.height()));
        // The answer's text without the page furniture the area may include at its edges.
        QRectF answerRect = selections.last();
        qreal bodyTop = std::numeric_limits<qreal>::max(), bodyBottom = std::numeric_limits<qreal>::lowest();
        bool furnished = false;
        for (const auto &box : boxesAt(areas[i].page)) {
            if (box->text().trimmed().isEmpty() || !answerRect.intersects(box->boundingBox())) continue;
            if (isFurniture(areas[i].page, box)) { furnished = true; continue; }
            bodyTop = std::min(bodyTop, box->boundingBox().top());
            bodyBottom = std::max(bodyBottom, box->boundingBox().bottom());
        }
        if (furnished && bodyTop <= bodyBottom)
            answerRect = QRectF(answerRect.left(), std::max(answerRect.top(), bodyTop - 0.5), answerRect.width(),
                                std::min(answerRect.bottom(), bodyBottom + 0.5) - std::max(answerRect.top(), bodyTop - 0.5));
        const QString text = furnished && bodyTop > bodyBottom ? QString() : page->text(answerRect).simplified();
        if (!text.isEmpty()) answers.append(text);
    }
    plan.invalidPdf = false;
    const QString answer = answers.join(QLatin1Char(' '));
    if (!answer.isEmpty()) plan.answerAnchor = {{QStringLiteral("version"), 1}, {QStringLiteral("normalized_text"), answer}, {QStringLiteral("text_sha256"), digest(answer.toUtf8())}};
    const auto region = [&areas](int i) {
        const QRectF r = areas[i].rect.normalized();
        return QJsonObject{{QStringLiteral("page"), areas[i].page}, {QStringLiteral("rect"), QJsonArray{r.left(), r.top(), r.right(), r.bottom()}}};
    };
    const auto finishWithoutSource = [&] {
        plan.withoutSource = true;
        plan.componentOrigin = reviewPdfComponentOrigin(bundleRoot, pdfPath, pdfSha256, areas.first().page);
        for (int i = 0; i < areas.size(); ++i) plan.regions.append(region(i));
        if (auto *first = pageAt(areas.first().page)) plan.pageVisualSha256 = fingerprint(*first);
        return plan;
    };
    const auto registered = DocumentIdentityStore(docsRoot).forFile(pdfPath, pdfSha256);
    if (registered.value(QStringLiteral("kind")).toString() == QLatin1String("unlinked")) return finishWithoutSource();
    const auto pointer = QJsonDocument::fromJson(read(QDir(bundleRoot).filePath(QStringLiteral("documents/%1.json").arg(QFileInfo(pdfPath).fileName())))).object();
    const auto artifactSha = pointer.value(QStringLiteral("artifact_sha256")).toString();
    plan.sourceMapUnavailable = true;
    if (!QRegularExpression(QStringLiteral("^[a-f0-9]{64}$")).match(artifactSha).hasMatch()) return fail(QStringLiteral("Sync this document to install its verified source information."));
    const auto artifact = read(QDir(bundleRoot).filePath(QStringLiteral("objects/%1").arg(artifactSha)));
    const auto manifest = QJsonDocument::fromJson(artifact).object();
    if (digest(artifact) != artifactSha || manifest.value(QStringLiteral("pdf")).toObject().value(QStringLiteral("pdf_sha256")).toString() != pdfSha256)
        return fail(QStringLiteral("The source information belongs to a different PDF."));
    if (manifest.value(QStringLiteral("schema_version")).toInt() != 1
        || manifest.value(QStringLiteral("kind")).toString() != QLatin1String("thoughts-document-source-artifact"))
        return fail(QStringLiteral("The source information is invalid."));
    plan.sourceMapUnavailable = false;
    const auto pdfSegments = manifest.value(QStringLiteral("pdf")).toObject().value(QStringLiteral("segments")).toArray();
    QList<bool> withoutSource;
    for (const auto &area : areas) {
        bool without = false;
        for (const auto &value : pdfSegments) {
            const auto segment = value.toObject();
            const int first = segment.value(QStringLiteral("start_page")).toInt();
            if (area.page >= first && area.page < first + segment.value(QStringLiteral("page_count")).toInt()
                && segment.value(QStringLiteral("source_available")).isBool() && !segment.value(QStringLiteral("source_available")).toBool())
                without = true;
        }
        withoutSource.append(without);
    }
    if (std::all_of(withoutSource.cbegin(), withoutSource.cend(), [](bool without) { return without; })) return finishWithoutSource();
    if (manifest.value(QStringLiteral("sources")).isArray() && manifest.value(QStringLiteral("sources")).toArray().isEmpty()
        && manifest.value(QStringLiteral("pdf")).toObject().value(QStringLiteral("project")).toString().isEmpty())
        return finishWithoutSource();
    for (int i = 0; i < areas.size(); ++i) {
        if (withoutSource[i]) return fail(QStringLiteral("This page has no editable source, unlike the card's other areas. Make a separate card for it."), i);
    }
    Okular::DocsCommentSynchronizer::Options options; options.docsRoot = docsRoot; options.sourceBundleRoot = bundleRoot; options.allowLegacySourceResolution = false; options.cacheSourceResolution = true;
    Okular::DocsCommentSynchronizer resolver(options);
    const QString stem = QFileInfo(pdfPath).completeBaseName();
    // Poppler text boxes already use PDF points; only saved regions are normalized.
    const auto resolveBox = [&](int pageNumber, const QRectF &selection, const std::unique_ptr<Poppler::TextBox> &box) {
        const auto center = box->boundingBox().center();
        auto mapped = resolver.resolveSource({stem, pageNumber, center.x(), center.y(), box->text(), pdfPath, true});
        if (!mapped.success && mapped.problem == Okular::DocsCommentSynchronizer::SourceResolutionResult::Problem::AmbiguousLocation) {
            // A single word often cannot disambiguate a shifted or structural
            // SyncTeX line. Supply its visual line as context, without widening
            // the answer region or relaxing the source resolver's checks.
            QStringList context;
            for (const auto &neighbor : boxesAt(pageNumber)) {
                const auto bounds = neighbor->boundingBox();
                if (bounds.top() <= center.y() && bounds.bottom() >= center.y()
                    && selection.intersects(bounds)) context.append(neighbor->text());
            }
            if (context.size() > 1) {
                const auto contextual = resolver.resolveSource({stem, pageNumber, center.x(), center.y(), context.join(QLatin1Char(' ')), pdfPath, true});
                const auto lines = QString::fromUtf8(read(contextual.sourcePath)).split(QLatin1Char('\n'));
                if (contextual.success && contextual.sourcePath == mapped.fallbackSourcePath
                    && contextual.line > 0 && contextual.line <= lines.size()
                    && lines[contextual.line - 1].contains(box->text(), Qt::CaseInsensitive)) mapped = contextual;
            }
        }
        return mapped;
    };
    QHash<QString, QByteArray> sources;
    const auto sourceAt = [&](const QString &path) -> const QByteArray & {
        if (!sources.contains(path)) sources.insert(path, read(path));
        return sources[path];
    };
    // The build's entry for a source: its digest and snapshot object.
    const auto builtEntry = [&manifest](const QString &id) {
        for (const auto &value : manifest.value(QStringLiteral("sources")).toArray()) {
            const auto entry = value.toObject();
            if (entry.value(QStringLiteral("path")).toString() == id)
                return qMakePair(entry.value(QStringLiteral("sha256")).toString(), entry.value(QStringLiteral("object")).toString());
        }
        return qMakePair(QString(), QString());
    };
    // Whether the PDF was built from the source as it is now (apart from the card's own markers
    // when they move). Otherwise its lines name the build's lines, not current ones.
    QHash<QString, bool> builtFromCurrent;
    const auto builtFrom = [&](const QString &path, const QString &id) {
        if (builtFromCurrent.contains(path)) return builtFromCurrent.value(path);
        const auto [expected, builtObject] = builtEntry(id);
        const auto &source = sourceAt(path);
        bool same = !expected.isEmpty() && digest(source) == expected;
        if (!same && !expected.isEmpty() && replaceCardMarkers) {
            const auto strip = stripReviewCardMarkers(id, source, cardId);
            const auto built = QRegularExpression(QStringLiteral("^[a-f0-9]{64}$")).match(builtObject).hasMatch()
                ? read(QDir(bundleRoot).filePath(QStringLiteral("objects/%1").arg(builtObject))) : QByteArray();
            const auto builtStrip = !built.isEmpty() && digest(built) == expected ? stripReviewCardMarkers(id, built, cardId) : ReviewMarkerStrip{};
            // Broken markers of the card are reported when they are removed, as before.
            same = !strip.error.isEmpty() || (!built.isEmpty() && builtStrip.error.isEmpty() && builtStrip.text == strip.text);
        }
        builtFromCurrent.insert(path, same);
        return same;
    };
    // The manifest's name of a source file the resolver found but could not place.
    const auto sourceIdOf = [&](const QString &path) {
        const QString wanted = QFileInfo(path).canonicalFilePath();
        for (const auto &value : manifest.value(QStringLiteral("sources")).toArray()) {
            const QString id = value.toObject().value(QStringLiteral("path")).toString();
            if (!id.isEmpty() && !wanted.isEmpty() && QFileInfo(QDir(docsRoot).filePath(id)).canonicalFilePath() == wanted) return id;
        }
        return QString();
    };
    const QString staleFailure = replaceCardMarkers
        ? QStringLiteral("The source changed since this PDF was built. Sync before placing the card.")
        : QStringLiteral("The TeX changed since this PDF was built. Sync before creating the card.");
    // byText: placed by its text in the current source, not by the build's lines.
    struct AreaLines { QString path, id; QList<int> lines; int first = 0, last = 0; bool whole = false, byText = false; };
    QList<AreaLines> mappedAreas;
    for (int i = 0; i < areas.size(); ++i) {
        const int pageNumber = areas[i].page;
        const QRectF &selection = selections[i];
        QString firstFailure, generatedError;
        QMap<QString, QList<int>> mapped;
        QHash<QString, QString> ids;
        // The area's words in reading order, and the editable files the resolver found for them.
        QStringList words, furniture;
        QSet<QString> files;
        for (const auto &box : boxesAt(pageNumber)) {
            if (box->text().trimmed().isEmpty() || !selection.intersects(box->boundingBox())) continue;
            // A running header, footer or page number at the area's edge is not the answer's.
            if (isFurniture(pageNumber, box)) { furniture.append(box->text()); continue; }
            if (!selection.adjusted(-0.5,-0.5,0.5,0.5).contains(box->boundingBox()))
                return fail(QStringLiteral("Select complete words when creating a source-linked card."), i);
            words.append(box->text());
            const auto result = resolveBox(pageNumber, selection, box);
            if (result.success) files.insert(result.sourcePath);
            else if (result.problem == Okular::DocsCommentSynchronizer::SourceResolutionResult::Problem::AmbiguousLocation
                     && !result.fallbackSourcePath.isEmpty()) files.insert(result.fallbackSourcePath);
            // Citation numbers, reference labels and footnote marks ("[3],", "(2)")
            // are typeset from macros, so their text never occurs in the source.
            // Skip such letterless words; the real words around them still have to
            // map, and the marked passage spans every line between them.
            const auto text = box->text();
            if (!result.success && result.problem == Okular::DocsCommentSynchronizer::SourceResolutionResult::Problem::AmbiguousLocation
                && std::none_of(text.cbegin(), text.cend(), [](QChar c) { return c.isLetter(); })) {
                if (generatedError.isEmpty()) generatedError = QStringLiteral("%1 (selected text: %2)").arg(result.failureReason, text);
                if (!result.fallbackSourcePath.isEmpty() && plan.fallbackSource.isEmpty()) plan.fallbackSource = QDir(docsRoot).relativeFilePath(result.fallbackSourcePath);
                continue;
            }
            if (!result.success) {
                if (!result.fallbackSourcePath.isEmpty()) plan.fallbackSource = QDir(docsRoot).relativeFilePath(result.fallbackSourcePath);
                if (firstFailure.isEmpty()) firstFailure = QStringLiteral("%1 (selected text: %2)").arg(result.failureReason, text);
                continue;
            }
            plan.fallbackSource = result.sourceId;
            mapped[result.sourcePath].append(result.line);
            ids.insert(result.sourcePath, result.sourceId);
        }
        if (words.isEmpty() && !furniture.isEmpty())
            return fail(QStringLiteral("it holds only the page's running header, footer or page number (“%1”), which the page layout prints, "
                                       "not the note's source. Select text of the note itself.").arg(furniture.join(QLatin1Char(' '))), i);
        const QString mappingFailure = !firstFailure.isEmpty() ? firstFailure : !generatedError.isEmpty() ? generatedError
            : QStringLiteral("The selected region has no verifiable source-linked text.");
        // When the build's lines cannot place the area, its whole text placed once in the
        // current source still can. Never a guess: a missing, short or repeated text keeps the
        // line mapping's error.
        const QString textFile = files.size() == 1 ? *files.cbegin() : QString();
        const QString textId = textFile.isEmpty() ? QString() : ids.contains(textFile) ? ids.value(textFile) : sourceIdOf(textFile);
        AreaLines lines;
        const auto placeByText = [&] {
            if (textId.isEmpty()) return false;
            const auto found = locateReviewTextInSource(textId, sourceAt(textFile), words);
            if (!found.first) return false;
            lines = {textFile, textId, {}, found.first, found.last, false, true};
            for (int line = found.first; line <= found.last; ++line) lines.lines.append(line);
            if (!areas[i].textSelection && textId.endsWith(QLatin1String(".tex"), Qt::CaseInsensitive)) {
                const auto environment = enclosingFloat(texFloatEnvironments(sourceAt(textFile)), lines.lines);
                if (environment.begin) { lines.first = environment.begin; lines.last = environment.end; lines.whole = true; }
            }
            return true;
        };
        // The area's lines from the build's line mapping, or the reason they cannot be used.
        const auto placeByLines = [&]() -> QString {
            if (mapped.isEmpty()) return mappingFailure;
            // A rectangle around a float stands for the whole environment; words in it from
            // generated or unmapped content are then part of the float, not a failure.
            QString file;
            FloatEnvironment around;
            if (!areas[i].textSelection) {
                int candidates = 0;
                for (auto it = mapped.cbegin(); it != mapped.cend(); ++it) {
                    if (!ids.value(it.key()).endsWith(QLatin1String(".tex"), Qt::CaseInsensitive)) continue;
                    const auto environment = enclosingFloat(texFloatEnvironments(sourceAt(it.key())), it.value());
                    if (!environment.begin) continue;
                    ++candidates; file = it.key(); around = environment;
                }
                if (candidates != 1) { file.clear(); around = {}; }
            }
            if (!around.begin) {
                if (mapped.size() > 1) return QStringLiteral("Select content from one TeX source file.");
                if (!firstFailure.isEmpty()) return firstFailure;
                file = mapped.firstKey();
            }
            lines = {file, ids.value(file), mapped.value(file)};
            std::sort(lines.lines.begin(), lines.lines.end());
            lines.lines.erase(std::unique(lines.lines.begin(), lines.lines.end()), lines.lines.end());
            if (around.begin) {
                lines.first = around.begin; lines.last = around.end; lines.whole = true;
                return {};
            }
            lines.first = lines.lines.first(); lines.last = lines.lines.last();
            const auto sourceLines = sourceAt(file).split('\n');
            for (int line = lines.first; line <= lines.last; ++line) {
                if (line < 1 || line > sourceLines.size()) return QStringLiteral("The source line range is invalid.");
                if (!lines.lines.contains(line) && !ignorableSourceLine(sourceLines[line - 1], file.endsWith(QLatin1String(".tex"), Qt::CaseInsensitive)))
                    return areas.size() > 1
                        ? QStringLiteral("The area covers separate source passages. Draw one area for each passage.")
                        : QStringLiteral("A card must contain one continuous source passage. Create separate cards for separated passages.");
            }
            return {};
        };
        if (!textId.isEmpty() && !builtFrom(textFile, textId)) {
            // The source changed since the build (a proposal accepted without a rebuild): the
            // build's lines name lines of another text, so only the text can place the area.
            if (!placeByText()) return fail(mapped.isEmpty() || !firstFailure.isEmpty() ? mappingFailure : staleFailure, i);
        } else if (const QString error = placeByLines(); !error.isEmpty() && !placeByText()) {
            return fail(error, i);
        }
        if (i > 0 && lines.path != mappedAreas.first().path)
            return fail(QStringLiteral("Its text is in %1, but the first area's is in %2. A card's areas must come from one source file.")
                .arg(lines.id, mappedAreas.first().id), i);
        mappedAreas.append(lines);
    }
    const QString sourcePath = mappedAreas.first().path, sourceId = mappedAreas.first().id;
    // Every rendering of a marked line must be inside the card's areas: elsewhere on an area's
    // page the passage would be cut, and on another page it would be shown during review.
    // Areas placed by their text are not checked: the build's lines of other words are what
    // could not place them, and their whole lines hold no text outside the area.
    QHash<int, int> lineOwner;
    for (int i = 0; i < mappedAreas.size(); ++i) {
        if (mappedAreas[i].byText) continue;
        for (const int line : std::as_const(mappedAreas[i].lines)) if (!lineOwner.contains(line)) lineOwner.insert(line, i);
    }
    QSet<int> areaPages;
    for (const auto &area : areas) areaPages.insert(area.page);
    for (const int number : std::as_const(areaPages)) {
        if (lineOwner.isEmpty()) break;
        for (const auto &box : boxesAt(number)) {
            // Page furniture maps to the line read at the page break, not to its own text.
            if (box->text().trimmed().isEmpty() || isFurniture(number, box)) continue;
            bool inside = false;
            for (int i = 0; i < areas.size() && !inside; ++i)
                inside = areas[i].page == number && selections[i].adjusted(-0.5,-0.5,0.5,0.5).contains(box->boundingBox());
            if (inside) continue;
            const auto center = box->boundingBox().center();
            const auto mapped = resolver.resolveSource({stem, number, center.x(), center.y(), box->text(), pdfPath, true});
            if (mapped.success && mapped.sourcePath == sourcePath && lineOwner.contains(mapped.line))
                return fail(QStringLiteral("The selection covers only part of a source line. Select the complete passage."), lineOwner.value(mapped.line));
        }
    }
    for (const auto &value : pdfSegments) {
        const auto segment = value.toObject();
        bool usesSource = false;
        for (const auto &input : segment.value(QStringLiteral("inputs")).toArray()) usesSource = usesSource || input.toObject().value(QStringLiteral("source_file")).toString() == sourceId;
        if (!usesSource || lineOwner.isEmpty()) continue;
        const int first = segment.value(QStringLiteral("start_page")).toInt();
        const int count = segment.value(QStringLiteral("page_count")).toInt();
        for (int number = first; number < first + count; ++number) {
            if (areaPages.contains(number) || number < 1 || number > pdf->numPages()) continue;
            if (!pageAt(number)) return fail(QStringLiteral("An associated PDF page is unavailable."));
            for (const auto &box : boxesAt(number)) {
                if (box->text().trimmed().isEmpty() || isFurniture(number, box)) continue;
                const auto center = box->boundingBox().center();
                const auto mapped = resolver.resolveSource({stem, number, center.x(), center.y(), box->text(), pdfPath, true});
                if (mapped.success && mapped.sourcePath == sourcePath && lineOwner.contains(mapped.line))
                    return fail(areas.size() > 1
                        ? QStringLiteral("Its source text also appears on page %1. Add an area there too, or choose a passage that appears once.").arg(number)
                        : QStringLiteral("The selected source also renders on another page. Select a passage with one complete occurrence."),
                        lineOwner.value(mapped.line));
            }
        }
    }
    const auto source = read(sourcePath); QString expected, builtObject;
    for (const auto &value : manifest.value(QStringLiteral("sources")).toArray()) {
        const auto entry = value.toObject();
        if (entry.value(QStringLiteral("path")).toString() != sourceId) continue;
        expected = entry.value(QStringLiteral("sha256")).toString();
        builtObject = entry.value(QStringLiteral("object")).toString();
    }
    QByteArray base = source;
    QList<int> lineMap;
    for (int i = 0; i < source.count('\n') + 1; ++i) lineMap.append(i + 1);
    // Text placement read the source as it is now: then it must still be that, and the build
    // may be older.
    const bool byText = std::any_of(mappedAreas.cbegin(), mappedAreas.cend(), [](const AreaLines &area) { return area.byText; });
    if (byText && source != sourceAt(sourcePath)) return fail(staleFailure);
    const bool stale = byText && !builtFrom(sourcePath, sourceId);
    if (replaceCardMarkers) {
        // The card's own markers move: the source without them must be the build's source
        // without them, whether or not the PDF was built after they were placed.
        const auto strip = stripReviewCardMarkers(sourceId, source, cardId);
        if (!strip.error.isEmpty()) return fail(strip.error);
        if (!stale && expected.isEmpty()) return fail(QStringLiteral("The source changed since this PDF was built. Sync before placing the card."));
        if (!stale && digest(source) != expected) {
            const auto built = QRegularExpression(QStringLiteral("^[a-f0-9]{64}$")).match(builtObject).hasMatch()
                ? read(QDir(bundleRoot).filePath(QStringLiteral("objects/%1").arg(builtObject))) : QByteArray();
            const auto builtStrip = digest(built) == expected ? stripReviewCardMarkers(sourceId, built, cardId) : ReviewMarkerStrip{};
            if (built.isEmpty() || digest(built) != expected || !builtStrip.error.isEmpty() || builtStrip.text != strip.text)
                return fail(QStringLiteral("The source changed since this PDF was built. Sync before placing the card."));
        }
        base = strip.text;
        lineMap = strip.lineMap;
    } else if (!stale && (expected.isEmpty() || digest(source) != expected)) {
        return fail(QStringLiteral("The TeX changed since this PDF was built. Sync before creating the card."));
    }
    const auto baseLine = [&lineMap](int line) { return line >= 1 && line <= lineMap.size() ? lineMap[line - 1] : 0; };
    // One segment per passage: areas whose lines overlap mark one passage together.
    QList<int> order(mappedAreas.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&mappedAreas](int a, int b) { return mappedAreas[a].first < mappedAreas[b].first; });
    struct Passage { int first = 0, last = 0; QList<int> areas; };
    QList<Passage> passages;
    for (const int i : std::as_const(order)) {
        if (!passages.isEmpty() && mappedAreas[i].first <= passages.last().last) {
            passages.last().last = std::max(passages.last().last, mappedAreas[i].last);
            passages.last().areas.append(i);
        } else {
            passages.append({mappedAreas[i].first, mappedAreas[i].last, {i}});
        }
    }
    QList<QPair<int, int>> ranges;
    for (const auto &passage : std::as_const(passages)) {
        const int first = baseLine(passage.first), last = baseLine(passage.last);
        if (!first || !last) return fail(QStringLiteral("The source line range is invalid."), passage.areas.first());
        ranges.append({first, last});
    }
    int failedRange = -1;
    const auto answerAnchor = plan.answerAnchor;
    const auto fallbackSource = plan.fallbackSource;
    auto marked = markReviewSourceRanges(sourceId, base, ranges, cardId, &failedRange);
    if (!marked.error.isEmpty()) return fail(marked.error, failedRange >= 0 ? passages[failedRange].areas.first() : -1);
    plan.path = sourcePath;
    plan.original = source;
    plan.replacement = marked.replacement;
    plan.anchor = marked.anchor;
    plan.answerAnchor = answerAnchor;
    plan.fallbackSource = fallbackSource;
    for (const auto &passage : std::as_const(passages)) {
        QList<int> members = passage.areas;
        std::sort(members.begin(), members.end());
        for (const int i : std::as_const(members)) plan.regions.append(region(i));
    }
    if (auto *first = pageAt(plan.regions.first().toObject().value(QStringLiteral("page")).toInt())) plan.pageVisualSha256 = fingerprint(*first);
    return plan;
}

// ---- Review cards for a whole section ----
namespace {
// A TeX line as TeX reads it: without its comment.
QString texActiveText(const QByteArray &line) {
    QByteArray active;
    for (int j = 0; j < line.size(); ++j) {
        if (line[j] == '%') break;
        active += line[j];
        if (line[j] == '\\' && j + 1 < line.size()) active += line[++j];
    }
    return QString::fromUtf8(active);
}
const QRegularExpression &texHeadingPattern() {
    static const QRegularExpression pattern(QStringLiteral(R"(^\s*\\(part|chapter|section|subsection|subsubsection|paragraph|subparagraph)\*?\s*(\[[^\]]*\]\s*)?\{)"));
    return pattern;
}
int texHeadingLevel(const QString &name) {
    static const QHash<QString, int> levels{{QStringLiteral("part"), -1}, {QStringLiteral("chapter"), 0}, {QStringLiteral("section"), 1},
        {QStringLiteral("subsection"), 2}, {QStringLiteral("subsubsection"), 3}, {QStringLiteral("paragraph"), 4}, {QStringLiteral("subparagraph"), 5}};
    return levels.value(name, 100);
}
// Where a TeX document's sections end: nothing after it belongs to the last one.
const QRegularExpression &texSectionsEnd() {
    static const QRegularExpression pattern(QStringLiteral(
        R"(^\s*\\(end\s*\{document\}|appendix(?![A-Za-z@])|bibliography\s*\{|bibliographystyle\s*\{|printbibliography(?![A-Za-z@])|begin\s*\{thebibliography\}))"));
    return pattern;
}
// A line, or the rest of the heading's line, that belongs to the heading: labels, anchors and
// contents entries print nothing of the section's text.
bool texHeadingCompanion(const QString &active) {
    static const QRegularExpression companion(QStringLiteral(
        R"(^\s*((\\(label|SourceLabel|index|hypertarget|phantomsection|addcontentsline|pdfbookmark|markboth|markright|nopagebreak)\*?\s*(\[[^\]]*\]\s*)*(\{[^{}]*\}\s*)*)\s*)*$)"));
    return companion.match(active).hasMatch();
}
QString plainTexTitle(QString title) {
    title.replace(QRegularExpression(QStringLiteral(R"(\\[A-Za-z@]+\*?)")), QStringLiteral(" "));
    title.replace(QRegularExpression(QStringLiteral(R"(\\([^A-Za-z@]))")), QStringLiteral("\\1"));
    title.remove(QRegularExpression(QStringLiteral(R"([{}$])")));
    title.replace(QLatin1Char('~'), QLatin1Char(' '));
    return title.simplified();
}
QString plainMarkdownTitle(QString title) {
    title.replace(QRegularExpression(QStringLiteral(R"(\s*\{[^}]*\}\s*$)")), QString());
    title.replace(QRegularExpression(QStringLiteral(R"(!?\[([^\]]*)\]\([^)]*\))")), QStringLiteral("\\1"));
    title.remove(QRegularExpression(QStringLiteral(R"([*_`])")));
    return title.simplified();
}
const QRegularExpression &texCardMarkerLine() {
    static const QRegularExpression pattern(QStringLiteral(R"(^\\ReviewCardMask(Start|End)\{([0-9A-Fa-f-]{36})\}\{([0-9]+)\}%$)"));
    return pattern;
}
const QRegularExpression &markdownCardMarkerLine() {
    static const QRegularExpression pattern(QStringLiteral(R"(^ {0,3}<!-- thoughts:v1:(start|end) review:([0-9A-Fa-f-]{36}):answer:([0-9]+) -->\r?$)"));
    return pattern;
}
// The body's edges: blank (and TeX comment) lines stay out, and so do the markers of a card
// whose passage reaches past the edge (an end marker at the start, a start marker at the end).
void trimSectionBody(const QList<QByteArray> &lines, bool tex, int *first, int *last) {
    const auto &marker = tex ? texCardMarkerLine() : markdownCardMarkerLine();
    const auto blank = [&](int line) { return ignorableSourceLine(lines[line - 1], tex); };
    const auto markerOf = [&](int line) { return marker.match(QString::fromUtf8(lines[line - 1])); };
    const auto partner = [&](int line, const QRegularExpressionMatch &match, int direction) {
        const bool start = match.captured(1).compare(QLatin1String("start"), Qt::CaseInsensitive) == 0;
        for (int other = line + direction; other >= 1 && other <= int(lines.size()); other += direction) {
            const auto candidate = markerOf(other);
            if (candidate.hasMatch() && candidate.captured(2).compare(match.captured(2), Qt::CaseInsensitive) == 0
                && candidate.captured(3) == match.captured(3) && (candidate.captured(1).compare(QLatin1String("start"), Qt::CaseInsensitive) == 0) != start)
                return other;
        }
        return 0;
    };
    for (bool changed = true; changed && *first <= *last;) {
        changed = false;
        while (*first <= *last && blank(*first)) { ++*first; changed = true; }
        while (*last >= *first && blank(*last)) { --*last; changed = true; }
        if (*first <= *last) {
            const auto opening = markerOf(*first);
            if (opening.hasMatch() && opening.captured(1).compare(QLatin1String("end"), Qt::CaseInsensitive) == 0) { ++*first; changed = true; continue; }
            const auto closing = markerOf(*last);
            if (closing.hasMatch() && closing.captured(1).compare(QLatin1String("start"), Qt::CaseInsensitive) == 0) { --*last; changed = true; continue; }
            // A start marker whose end lies after the body, or an end marker whose start lies before it.
            if (opening.hasMatch()) {
                const int end = partner(*first, opening, 1);
                if (!end || end > *last) { ++*first; changed = true; continue; }
            }
            if (closing.hasMatch()) {
                const int start = partner(*last, closing, -1);
                if (!start || start < *first) { --*last; changed = true; continue; }
            }
        }
    }
}
}

ReviewSourceSection reviewSourceSection(const QString &path, const QByteArray &source, const QList<int> &headingLines) {
    ReviewSourceSection section;
    const auto fail = [&section](const QString &error) { section.error = error; return section; };
    const bool tex = path.endsWith(QLatin1String(".tex"), Qt::CaseInsensitive);
    if (!tex && !isMarkdownPath(path)) return fail(QStringLiteral("Choose a LaTeX or Markdown source file."));
    if (QString::fromUtf8(source).toUtf8() != source) return fail(QStringLiteral("The source is not valid UTF-8."));
    QList<QByteArray> lines = source.split('\n');
    if (source.endsWith('\n')) lines.removeLast();
    const int count = int(lines.size());
    QList<int> wanted = headingLines;
    wanted.erase(std::remove_if(wanted.begin(), wanted.end(), [count](int line) { return line < 1 || line > count; }), wanted.end());
    if (wanted.isEmpty()) return fail(QStringLiteral("The heading's source line was not found."));
    struct Heading { int line = 0, endLine = 0, level = 0; QString name, title, rest; };
    QList<Heading> headings;
    QList<int> ends; // lines where every section ends (TeX: appendix, bibliography, end of document)
    if (tex) {
        QStringList active;
        for (const auto &line : std::as_const(lines)) active.append(texActiveText(line));
        int documentStart = 0;
        for (int i = 0; i < count; ++i)
            if (active[i].contains(QRegularExpression(QStringLiteral(R"(\\begin\s*\{document\})")))) { documentStart = i + 1; break; }
        for (int i = documentStart; i < count; ++i) {
            if (texSectionsEnd().match(active[i]).hasMatch()) { ends.append(i + 1); continue; }
            const auto match = texHeadingPattern().match(active[i]);
            if (!match.hasMatch()) continue;
            Heading heading{i + 1, i + 1, texHeadingLevel(match.captured(1)), match.captured(1), {}, {}};
            // The title's braces, over at most six lines.
            int depth = 1, line = i, at = int(match.capturedEnd());
            QString title;
            while (depth > 0 && line < count && line < i + 6) {
                const QString &text = active[line];
                for (; at < text.size(); ++at) {
                    const QChar c = text[at];
                    if (c == QLatin1Char('\\') && at + 1 < text.size()) { title += c; title += text[++at]; continue; }
                    if (c == QLatin1Char('{')) ++depth;
                    if (c == QLatin1Char('}') && --depth == 0) { heading.endLine = line + 1; heading.rest = text.mid(at + 1); break; }
                    title += c;
                }
                if (depth > 0) { title += QLatin1Char(' '); ++line; at = 0; }
            }
            if (depth > 0) continue;
            heading.title = plainTexTitle(title);
            headings.append(heading);
        }
    } else {
        static const QRegularExpression fenceStart(QStringLiteral(R"(^ {0,3}(`{3,}|~{3,}))"));
        static const QRegularExpression atx(QStringLiteral(R"(^ {0,3}(#{1,6})(?:[ \t]+(.*?))?[ \t]*$)"));
        QString fence;
        for (int i = 0; i < count; ++i) {
            const QString text = QString::fromUtf8(lines[i]);
            const QString trimmed = text.trimmed();
            if (!fence.isEmpty()) {
                int length = 0;
                while (length < trimmed.size() && trimmed.at(length) == fence.at(0)) ++length;
                if (length >= fence.size() && trimmed.mid(length).isEmpty()) fence.clear();
                continue;
            }
            if (const auto match = fenceStart.match(text); match.hasMatch()) { fence = match.captured(1); continue; }
            const auto match = atx.match(text);
            if (!match.hasMatch()) continue;
            QString title = match.captured(2);
            title.remove(QRegularExpression(QStringLiteral(R"((^|[ \t]+)#+$)")));
            headings.append({i + 1, i + 1, int(match.captured(1).size()), QString(), plainMarkdownTitle(title), {}});
        }
    }
    const Heading *chosen = nullptr;
    for (const auto &heading : std::as_const(headings))
        if (std::any_of(wanted.cbegin(), wanted.cend(), [&heading](int line) { return heading.line <= line && line <= heading.endLine; })) { chosen = &heading; break; }
    if (!chosen) return fail(QStringLiteral("Line %1 of %2 is not a section heading.").arg(QString::number(wanted.first()), path));
    section.title = chosen->title;
    section.level = chosen->level;
    section.headingLine = chosen->line;
    section.kind = tex ? chosen->name
        : chosen->level <= 1 ? QStringLiteral("section") : chosen->level == 2 ? QStringLiteral("subsection") : QStringLiteral("subsubsection");
    if (tex && chosen->level > 3)
        return fail(QStringLiteral("“%1” is a paragraph heading, which runs into its text. Select the passage instead.").arg(chosen->title));
    section.isHeading = true;
    if (tex && !texHeadingCompanion(chosen->rest))
        return fail(QStringLiteral("The text of “%1” starts on its heading's line (line %2 of %3). Put the heading on a line of its own to make a card of its section.")
                        .arg(chosen->title, QString::number(chosen->endLine), path));
    int first = chosen->endLine + 1;
    // Blank and comment lines, labels and anchors below the heading are the heading's.
    if (tex) while (first <= count && texHeadingCompanion(texActiveText(lines[first - 1]))
                    && !texCardMarkerLine().match(QString::fromUtf8(lines[first - 1])).hasMatch()) ++first;
    int next = 0;
    for (const auto &heading : std::as_const(headings))
        if (heading.line > chosen->endLine && heading.level <= chosen->level) { next = heading.line; break; }
    for (const int end : std::as_const(ends))
        if (end > chosen->endLine && (!next || end < next)) { next = end; break; }
    section.nextHeadingLine = next;
    int last = next ? next - 1 : count;
    trimSectionBody(lines, tex, &first, &last);
    if (first > last) return fail(QStringLiteral("The section “%1” has no text below its heading.").arg(chosen->title));
    section.firstLine = first;
    section.lastLine = last;
    return section;
}

namespace {
// A PDF's text by visual lines in reading order, page by page and, on a page set in two
// columns, column by column; the page's running header, footer and page number left out.
struct LayoutLine {
    int column = 0;
    QRectF rect;
    QString text;
    qreal glyphHeight = 0; // the median height of its words
    QList<const Poppler::TextBox *> boxes;
};
class PdfLayout {
public:
    explicit PdfLayout(Poppler::Document *pdf) : m_pdf(pdf) {}
    int pageCount() const { return m_pdf->numPages(); }
    Poppler::Page *page(int number) {
        if (number < 1 || number > m_pdf->numPages()) return nullptr;
        if (!m_pages.contains(number)) m_pages.insert(number, std::shared_ptr<Poppler::Page>(m_pdf->page(number - 1).release()));
        return m_pages.value(number).get();
    }
    QSizeF size(int number) { auto *p = page(number); return p ? p->pageSizeF() : QSizeF(); }
    const std::vector<std::unique_ptr<Poppler::TextBox>> &boxes(int number) {
        if (!m_boxes.contains(number))
            m_boxes.insert(number, std::make_shared<std::vector<std::unique_ptr<Poppler::TextBox>>>(page(number)->textList()));
        return *m_boxes.value(number);
    }
    const QSet<const Poppler::TextBox *> &furniture(int number) {
        if (!m_furniture.contains(number)) {
            QList<QList<PageTextLine>> neighbours;
            for (const int other : {number - 1, number + 1})
                if (other >= 1 && other <= m_pdf->numPages() && page(other)) neighbours.append(pageTextLines(boxes(other)));
            m_furniture.insert(number, pageFurniture(pageTextLines(boxes(number)), size(number).height(), neighbours));
        }
        return m_furniture[number];
    }
    int columns(int number) { lines(number); return m_columns.value(number, 1); }
    // The page's lines in reading order (column, then top to bottom), without page furniture.
    const QList<LayoutLine> &lines(int number) {
        if (m_lines.contains(number)) return m_lines[number];
        QList<LayoutLine> &result = m_lines[number];
        if (!page(number)) return result;
        const auto &skip = furniture(number);
        QList<const Poppler::TextBox *> body;
        for (const auto &box : boxes(number))
            if (!box->text().trimmed().isEmpty() && !skip.contains(box.get())) body.append(box.get());
        // Two columns: most lines leave a wide gap at the page's middle, and both halves hold text.
        const qreal middle = size(number).width() / 2;
        int spanning = 0, left = 0, right = 0;
        QList<QList<const Poppler::TextBox *>> bands;
        {
            QList<const Poppler::TextBox *> sorted = body;
            std::stable_sort(sorted.begin(), sorted.end(), [](auto *a, auto *b) { return a->boundingBox().center().y() < b->boundingBox().center().y(); });
            qreal top = 0, bottom = -1;
            for (auto *box : std::as_const(sorted)) {
                const QRectF bounds = box->boundingBox();
                if (!bands.isEmpty() && bounds.center().y() >= top && bounds.center().y() <= bottom) {
                    bands.last().append(box);
                    top = std::min(top, bounds.top()); bottom = std::max(bottom, bounds.bottom());
                } else {
                    bands.append({box}); top = bounds.top(); bottom = bounds.bottom();
                }
            }
        }
        for (const auto &band : std::as_const(bands)) {
            bool crosses = false, hasLeft = false, hasRight = false;
            for (auto *box : band) {
                const QRectF bounds = box->boundingBox();
                if (bounds.left() < middle - 4 && bounds.right() > middle + 4) crosses = true;
                if (bounds.right() <= middle + 4) hasLeft = true;
                if (bounds.left() >= middle - 4) hasRight = true;
            }
            // Words on both sides of the middle without a gap of at least 8 points between them
            // belong to one line of one column.
            if (!crosses && hasLeft && hasRight) {
                qreal leftEdge = 0, rightEdge = size(number).width();
                for (auto *box : band) {
                    const QRectF bounds = box->boundingBox();
                    if (bounds.center().x() < middle) leftEdge = std::max(leftEdge, bounds.right());
                    else rightEdge = std::min(rightEdge, bounds.left());
                }
                if (rightEdge - leftEdge < 8) crosses = true;
            }
            if (crosses) ++spanning;
            if (hasLeft && !crosses) ++left;
            if (hasRight && !crosses) ++right;
        }
        const bool twoColumns = bands.size() >= 10 && spanning * 5 <= int(bands.size()) && left * 4 >= int(bands.size()) && right * 4 >= int(bands.size());
        m_columns.insert(number, twoColumns ? 2 : 1);
        for (int column = 0; column < (twoColumns ? 2 : 1); ++column) {
            QList<const Poppler::TextBox *> mine;
            for (const auto &band : std::as_const(bands)) {
                // A line across the middle (a title, a wide figure's caption) reads with the left column.
                bool crosses = false;
                for (auto *box : band) crosses = crosses || (box->boundingBox().left() < middle - 4 && box->boundingBox().right() > middle + 4);
                for (auto *box : band) {
                    const int at = !twoColumns || crosses || box->boundingBox().center().x() < middle ? 0 : 1;
                    if (at == column) mine.append(box);
                }
            }
            std::stable_sort(mine.begin(), mine.end(), [](auto *a, auto *b) { return a->boundingBox().center().y() < b->boundingBox().center().y(); });
            QList<LayoutLine> columnLines;
            for (auto *box : std::as_const(mine)) {
                const QRectF bounds = box->boundingBox();
                if (!columnLines.isEmpty() && bounds.center().y() >= columnLines.last().rect.top() && bounds.center().y() <= columnLines.last().rect.bottom()) {
                    columnLines.last().rect |= bounds;
                    columnLines.last().boxes.append(box);
                } else {
                    columnLines.append({column, bounds, {}, 0, {box}});
                }
            }
            for (auto &line : columnLines) {
                std::stable_sort(line.boxes.begin(), line.boxes.end(), [](auto *a, auto *b) { return a->boundingBox().left() < b->boundingBox().left(); });
                QStringList words;
                QList<qreal> heights;
                for (auto *box : std::as_const(line.boxes)) { words.append(box->text()); heights.append(box->boundingBox().height()); }
                std::sort(heights.begin(), heights.end());
                line.text = words.join(QLatin1Char(' '));
                line.glyphHeight = heights.value(heights.size() / 2);
            }
            result.append(columnLines);
        }
        return result;
    }
    // A column's horizontal extent on a page: from its leftmost to its rightmost text.
    QPair<qreal, qreal> columnExtent(int number, int column) {
        qreal left = std::numeric_limits<qreal>::max(), right = std::numeric_limits<qreal>::lowest();
        for (const auto &line : lines(number)) {
            if (line.column != column) continue;
            left = std::min(left, line.rect.left()); right = std::max(right, line.rect.right());
        }
        if (left > right) return {0, size(number).width()};
        return {left, right};
    }
    // The median word height of a page's text.
    qreal bodyGlyphHeight(int number) {
        QList<qreal> heights;
        for (const auto &line : lines(number)) for (auto *box : line.boxes) heights.append(box->boundingBox().height());
        std::sort(heights.begin(), heights.end());
        return heights.value(heights.size() / 2);
    }
private:
    Poppler::Document *m_pdf;
    QHash<int, std::shared_ptr<Poppler::Page>> m_pages;
    QHash<int, std::shared_ptr<std::vector<std::unique_ptr<Poppler::TextBox>>>> m_boxes;
    QHash<int, QSet<const Poppler::TextBox *>> m_furniture;
    QHash<int, QList<LayoutLine>> m_lines;
    QHash<int, int> m_columns;
};
// A place in reading order: a page, a column on it, and a height in points.
struct ReadingPlace {
    int page = 0, column = 0;
    qreal y = 0;
    bool operator<(const ReadingPlace &other) const {
        return page != other.page ? page < other.page : column != other.column ? column < other.column : y < other.y;
    }
};
// A heading's number: 2, 2.1, A.1, A., II. (a letter or a Roman numeral only with a dot after it).
struct HeadingNumber { QList<int> parts; bool letter = false, roman = false; bool valid() const { return !parts.isEmpty(); } };
int romanValue(const QString &text) {
    static const QHash<QChar, int> values{{QLatin1Char('I'), 1}, {QLatin1Char('V'), 5}, {QLatin1Char('X'), 10}, {QLatin1Char('L'), 50}, {QLatin1Char('C'), 100}};
    int total = 0;
    for (int i = 0; i < text.size(); ++i) {
        const int value = values.value(text[i]);
        if (!value) return 0;
        total += i + 1 < text.size() && values.value(text[i + 1]) > value ? -value : value;
    }
    return total;
}
HeadingNumber headingNumber(const QString &text, QString *rest = nullptr) {
    static const QRegularExpression pattern(QStringLiteral(R"(^\s*(?:(\d+(?:\.\d+)*)\.?|([A-Z])(?:(\.\d+)+\.?|\.)|([IVXLC]+)\.)\s+(\S.*)$)"));
    const auto match = pattern.match(text);
    HeadingNumber number;
    if (!match.hasMatch()) return number;
    if (rest) *rest = match.captured(5);
    if (!match.captured(1).isEmpty()) {
        for (const auto &part : match.captured(1).split(QLatin1Char('.'))) number.parts.append(part.toInt());
    } else if (!match.captured(2).isEmpty()) {
        number.letter = true;
        number.parts.append(match.captured(2).at(0).unicode() - 'A' + 1);
        for (const auto &part : match.captured(3).split(QLatin1Char('.'), Qt::SkipEmptyParts)) number.parts.append(part.toInt());
    } else {
        number.roman = true;
        number.parts.append(romanValue(match.captured(4)));
        if (!number.parts.first()) number.parts.clear();
    }
    return number;
}
QString headingNumberText(const HeadingNumber &number) {
    QStringList parts;
    for (int i = 0; i < number.parts.size(); ++i)
        parts.append(i == 0 && number.letter ? QString(QChar('A' + number.parts[i] - 1)) : QString::number(number.parts[i]));
    return parts.join(QLatin1Char('.'));
}
// Whether @p candidate numbers the next heading of the same or a higher level after @p heading:
// 2.1 is followed by 2.2 or 3; appendices (A, B) follow numbered sections.
bool nextHeadingNumber(const HeadingNumber &heading, const HeadingNumber &candidate) {
    if (!candidate.valid() || candidate.parts.size() > heading.parts.size() || candidate.roman != heading.roman) return false;
    if (candidate.letter && !heading.letter) return candidate.parts.size() == 1 && candidate.parts.first() == 1;
    if (candidate.letter != heading.letter) return false;
    const int level = int(candidate.parts.size());
    for (int i = 0; i < level - 1; ++i) if (candidate.parts[i] != heading.parts[i]) return false;
    return candidate.parts[level - 1] == heading.parts[level - 1] + 1;
}
bool closingHeading(const QString &text) {
    static const QSet<QString> closing{QStringLiteral("references"), QStringLiteral("bibliography"), QStringLiteral("acknowledgments"),
        QStringLiteral("acknowledgements"), QStringLiteral("acknowledgment"), QStringLiteral("acknowledgement"), QStringLiteral("appendix"), QStringLiteral("appendices")};
    QString rest = text;
    if (headingNumber(text, &rest).valid()) return closing.contains(foldedLetters(rest));
    return closing.contains(foldedLetters(text));
}
int wordCount(const QString &text) { return int(text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).size()); }
// Whether a PDF line shows the heading @p title: the same letters, perhaps with a number before
// either ("2.2 Model" for Model, Model for "2.2 Model"), or the start of a title that runs on to
// the next line. A line of text that merely mentions the title ("... before the results.") does not.
bool showsTitle(const QString &line, const QString &title) {
    QString titleRest, lineRest;
    QStringList wanted{foldedLetters(title)}, shown{foldedLetters(line)};
    if (headingNumber(title, &titleRest).valid()) wanted.append(foldedLetters(titleRest));
    if (headingNumber(line, &lineRest).valid()) shown.append(foldedLetters(lineRest));
    for (const QString &w : std::as_const(wanted)) {
        if (w.size() < 2) continue;
        for (const QString &s : std::as_const(shown))
            if (s == w || (s.size() >= 3 && w.startsWith(s))) return true;
    }
    return false;
}
// The areas between two places in reading order: per page and column, from just below @p start
// to just above @p end, as wide as the column's text.
QList<ReviewArea> areasBetween(PdfLayout &layout, const ReadingPlace &start, const ReadingPlace &end) {
    QList<ReviewArea> areas;
    for (int number = start.page; number <= std::min(end.page, layout.pageCount()); ++number) {
        const QSizeF size = layout.size(number);
        if (size.isEmpty()) continue;
        for (int column = 0; column < layout.columns(number); ++column) {
            const ReadingPlace blockStart{number, column, -1}, blockEnd{number, column, size.height() + 1};
            if (blockEnd < start || end < blockStart) continue;
            const bool first = number == start.page && column == start.column, last = number == end.page && column == end.column;
            qreal top = std::numeric_limits<qreal>::max(), bottom = std::numeric_limits<qreal>::lowest();
            for (const auto &line : layout.lines(number)) {
                if (line.column != column) continue;
                const qreal centre = line.rect.center().y();
                if ((first && centre <= start.y) || (last && centre >= end.y)) continue;
                top = std::min(top, line.rect.top()); bottom = std::max(bottom, line.rect.bottom());
            }
            if (top > bottom) continue;
            // From just below the heading to just above the next one; elsewhere around the text.
            top = first ? start.y + 0.5 : top - 1;
            bottom = last ? end.y - 0.5 : bottom + 1;
            top = std::max<qreal>(0, top); bottom = std::min<qreal>(size.height(), bottom);
            const auto [left, right] = layout.columnExtent(number, column);
            const QRectF rect(QPointF(std::max<qreal>(0, left - 2) / size.width(), top / size.height()),
                              QPointF(std::min<qreal>(size.width(), right + 2) / size.width(), bottom / size.height()));
            if (rect.height() * size.height() >= 2 && rect.width() > 0) areas.append({number, rect, false});
        }
    }
    return areas;
}
// The lines of a page an area shows (a word's centre inside it).
QList<int> linesIn(PdfLayout &layout, const ReviewArea &area) {
    QList<int> found;
    const QSizeF size = layout.size(area.page);
    const QRectF rect(area.rect.left() * size.width() - 1, area.rect.top() * size.height() - 1,
                      area.rect.width() * size.width() + 2, area.rect.height() * size.height() + 2);
    const auto &lines = layout.lines(area.page);
    for (int i = 0; i < lines.size(); ++i)
        if (std::any_of(lines[i].boxes.cbegin(), lines[i].boxes.cend(), [&rect](auto *box) { return rect.contains(box->boundingBox().center()); })) found.append(i);
    return found;
}
}

ReviewPdfSection reviewPdfSection(const QString &pdfPath, const ReviewArea &heading, bool identifyOnly) {
    ReviewPdfSection section;
    const auto fail = [&section](const QString &error) { section.error = error; return section; };
    auto pdf = document(read(pdfPath));
    if (!pdf) return fail(QStringLiteral("The PDF could not be read."));
    PdfLayout layout(pdf.get());
    if (!layout.page(heading.page)) return fail(QStringLiteral("The heading's page is unavailable."));
    const auto &pageLines = layout.lines(heading.page);
    const QList<int> chosen = linesIn(layout, heading);
    if (chosen.isEmpty()) return fail(QStringLiteral("Select the heading's text."));
    QString lineText, selected;
    const QSizeF size = layout.size(heading.page);
    const QRectF selection(heading.rect.left() * size.width() - 1, heading.rect.top() * size.height() - 1,
                           heading.rect.width() * size.width() + 2, heading.rect.height() * size.height() + 2);
    for (const int i : chosen) {
        lineText += (lineText.isEmpty() ? QString() : QStringLiteral(" ")) + pageLines[i].text;
        for (auto *box : pageLines[i].boxes) if (selection.contains(box->boundingBox().center())) selected += box->text() + QLatin1Char(' ');
    }
    const QString lineLetters = foldedLetters(lineText), selectedLetters = foldedLetters(selected);
    int headingLast = chosen.last();
    // 1. The PDF outline: the entry on this page whose title the heading line shows.
    struct Entry { QString title; int depth = 0, page = 0; qreal top = -1; };
    QList<Entry> entries;
    const std::function<void(const QList<Poppler::OutlineItem> &, int)> flatten = [&](const QList<Poppler::OutlineItem> &items, int depth) {
        for (const auto &item : items) {
            Entry entry{item.name().simplified(), depth, 0, -1};
            if (const auto destination = item.destination()) {
                entry.page = destination->pageNumber();
                if (destination->isChangeTop()) entry.top = destination->top();
            }
            entries.append(entry);
            if (item.hasChildren()) flatten(item.children(), depth + 1);
        }
    };
    flatten(pdf->outline(), 1);
    int found = -1;
    for (int i = 0; i < entries.size(); ++i) {
        const QString title = foldedLetters(entries[i].title);
        if (entries[i].page != heading.page || title.size() < 2 || lineLetters.size() < 2) continue;
        // The heading alone (with its number), not a line of text that mentions it.
        if (!showsTitle(lineText, entries[i].title)) continue;
        if (!selectedLetters.isEmpty() && !title.contains(selectedLetters) && !selectedLetters.contains(title)) continue;
        const auto distance = [&](int index) {
            return entries[index].top < 0 ? 0.0 : qAbs(entries[index].top * size.height() - pageLines[chosen.first()].rect.top());
        };
        if (found < 0 || distance(i) < distance(found)) found = i;
    }
    ReadingPlace end{layout.pageCount() + 1, 0, 0};
    if (found >= 0) {
        const Entry &entry = entries[found];
        section.isHeading = true;
        section.fromOutline = true;
        section.level = entry.depth;
        QString rest = entry.title;
        const auto number = headingNumber(entry.title, &rest);
        section.number = number.valid() ? headingNumberText(number) : QString();
        section.title = number.valid() ? rest : entry.title;
        if (!number.valid()) {
            QString lineRest;
            const auto shown = headingNumber(lineText, &lineRest);
            if (shown.valid()) section.number = headingNumberText(shown);
        }
        // A title over several lines: the lines below that continue it.
        QString shown = foldedLetters(lineText);
        const QString title = foldedLetters(entry.title);
        while (headingLast + 1 < pageLines.size() && shown.size() < title.size() && pageLines[headingLast + 1].column == pageLines[headingLast].column) {
            const QString more = shown + foldedLetters(pageLines[headingLast + 1].text);
            if (!title.contains(more) && !more.contains(title)) break;
            shown = more;
            ++headingLast;
        }
        if (identifyOnly) return section;
        for (int i = found + 1; i < entries.size(); ++i) {
            if (entries[i].depth > entry.depth || entries[i].page < 1) continue;
            // Its heading line, from its destination on (a line may show only the title's start).
            const QSizeF nextSize = layout.size(entries[i].page);
            const auto &lines = layout.lines(entries[i].page);
            for (const auto &line : lines) {
                if (!showsTitle(line.text, entries[i].title)) continue;
                if (entries[i].top >= 0 && line.rect.bottom() < entries[i].top * nextSize.height() - 2) continue;
                const ReadingPlace place{entries[i].page, line.column, line.rect.top()};
                if (entries[i].page == heading.page && !(ReadingPlace{heading.page, pageLines[headingLast].column, pageLines[headingLast].rect.bottom()} < place)) continue;
                end = place;
                break;
            }
            if (end.page > layout.pageCount())
                end = {entries[i].page, 0, entries[i].top >= 0 ? entries[i].top * nextSize.height() : 0};
            break;
        }
    } else {
        // 2. Without an outline entry: a heading line, numbered or set larger than the text.
        const LayoutLine &line = pageLines[chosen.first()];
        QString rest;
        const auto number = headingNumber(lineText, &rest);
        const qreal height = line.glyphHeight, body = layout.bodyGlyphHeight(heading.page);
        const bool larger = height >= 1.08 * body;
        const QString trimmed = lineText.trimmed();
        // A heading over two lines only when both are set as large (never a heading and its text).
        const bool alike = std::all_of(chosen.cbegin(), chosen.cend(), [&](int i) { return qAbs(pageLines[i].glyphHeight - height) <= 0.05 * height; });
        const bool shortLine = (chosen.size() == 1 || (chosen.size() == 2 && larger && alike)) && wordCount(lineText) <= 15 && trimmed.size() <= 120;
        const bool sentence = trimmed.endsWith(QLatin1Char(',')) || trimmed.endsWith(QLatin1Char(';')) || (trimmed.endsWith(QLatin1Char('.')) && wordCount(trimmed) > 8);
        if (!shortLine || sentence || height < 0.95 * body || (!number.valid() && !larger))
            return fail(QStringLiteral("This is not a section heading: the PDF has no outline entry for it, and it is neither numbered nor set larger than the text."));
        section.isHeading = true;
        section.number = number.valid() ? headingNumberText(number) : QString();
        section.title = number.valid() ? rest.trimmed() : trimmed;
        section.level = number.valid() ? int(number.parts.size()) : 1;
        if (identifyOnly) return section;
        bool past = false;
        for (int p = heading.page; p <= layout.pageCount() && end.page > layout.pageCount(); ++p) {
            const auto &lines = layout.lines(p);
            for (int i = 0; i < lines.size(); ++i) {
                if (p == heading.page && !past) { past = i == headingLast; continue; }
                const auto &candidate = lines[i];
                const QString text = candidate.text.trimmed();
                if (wordCount(text) > 20 || candidate.glyphHeight < 0.9 * height) continue;
                // A numbered line of text ("3 of them ...") is no heading: a heading's title starts
                // with a capital, and its line is short or set larger than the text.
                const auto [columnLeft, columnRight] = layout.columnExtent(p, candidate.column);
                const bool headingLike = candidate.rect.width() <= 0.8 * (columnRight - columnLeft) || candidate.glyphHeight >= 1.05 * layout.bodyGlyphHeight(p);
                QString title;
                const auto candidateNumber = headingNumber(text, &title);
                const bool next = number.valid()
                    ? headingLike && !title.isEmpty() && title.at(0).isUpper() && nextHeadingNumber(number, candidateNumber)
                    : candidate.glyphHeight >= 0.95 * height && !text.endsWith(QLatin1Char('.'));
                if (next || (headingLike && closingHeading(text))) { end = {p, candidate.column, candidate.rect.top()}; break; }
            }
        }
    }
    const ReadingPlace start{heading.page, pageLines[headingLast].column, pageLines[headingLast].rect.bottom()};
    // Without a next heading the section runs to the end of the document (end stays past its last page).
    section.areas = areasBetween(layout, start, end);
    if (section.areas.isEmpty()) return fail(QStringLiteral("The section “%1” has no text below its heading.").arg(section.title));
    return section;
}

QString ReviewSectionPlan::name() const {
    const QString what = kind.isEmpty() ? QStringLiteral("section") : kind;
    return number.isEmpty() ? QStringLiteral("%1 “%2”").arg(what, title) : QStringLiteral("%1 %2 “%3”").arg(what, number, title);
}

ReviewSectionPlan planReviewSectionCard(const QString &docsRoot, const QString &bundleRoot, const QString &pdfPath,
                                        const QString &pdfSha256, const ReviewArea &heading, const QString &cardId, bool identifyOnly) {
    ReviewSectionPlan result;
    const auto fail = [&result](const QString &error) { result.error = error; return result; };
    const auto bytes = read(pdfPath);
    if (bytes.isEmpty() || digest(bytes) != pdfSha256) return fail(QStringLiteral("The PDF changed. Select the heading again."));
    auto pdf = document(bytes);
    if (!pdf) return fail(QStringLiteral("The PDF could not be read."));
    PdfLayout layout(pdf.get());
    if (!layout.page(heading.page) || heading.rect.isEmpty()) return fail(QStringLiteral("The heading's page is unavailable."));
    const auto pageRange = [&result](const QList<ReviewArea> &areas) {
        for (const auto &area : areas) {
            if (!result.firstPage || area.page < result.firstPage) result.firstPage = area.page;
            result.lastPage = std::max(result.lastPage, area.page);
        }
    };
    // Built from source or not, as planReviewSourceAnchors decides for the heading's page.
    bool withoutSource = DocumentIdentityStore(docsRoot).forFile(pdfPath, pdfSha256).value(QStringLiteral("kind")).toString() == QLatin1String("unlinked");
    QJsonObject manifest;
    if (!withoutSource) {
        const auto pointer = QJsonDocument::fromJson(read(QDir(bundleRoot).filePath(QStringLiteral("documents/%1.json").arg(QFileInfo(pdfPath).fileName())))).object();
        const auto artifactSha = pointer.value(QStringLiteral("artifact_sha256")).toString();
        const auto artifact = artifactSha.size() == 64 ? read(QDir(bundleRoot).filePath(QStringLiteral("objects/%1").arg(artifactSha))) : QByteArray();
        manifest = QJsonDocument::fromJson(artifact).object();
        if (artifact.isEmpty() || digest(artifact) != artifactSha || manifest.value(QStringLiteral("pdf")).toObject().value(QStringLiteral("pdf_sha256")).toString() != pdfSha256
            || manifest.value(QStringLiteral("kind")).toString() != QLatin1String("thoughts-document-source-artifact")) {
            // Whether it is a heading still shows in the PDF; its card needs the source information.
            const auto shown = reviewPdfSection(pdfPath, heading, true);
            result.isHeading = shown.isHeading;
            result.title = shown.title;
            return fail(shown.isHeading ? QStringLiteral("Sync this document to install its verified source information.") : shown.error);
        }
        for (const auto &value : manifest.value(QStringLiteral("pdf")).toObject().value(QStringLiteral("segments")).toArray()) {
            const auto segment = value.toObject();
            const int first = segment.value(QStringLiteral("start_page")).toInt();
            if (heading.page >= first && heading.page < first + segment.value(QStringLiteral("page_count")).toInt()
                && segment.value(QStringLiteral("source_available")).isBool() && !segment.value(QStringLiteral("source_available")).toBool())
                withoutSource = true;
        }
        if (manifest.value(QStringLiteral("sources")).isArray() && manifest.value(QStringLiteral("sources")).toArray().isEmpty()
            && manifest.value(QStringLiteral("pdf")).toObject().value(QStringLiteral("project")).toString().isEmpty())
            withoutSource = true;
    }
    if (withoutSource) {
        const auto section = reviewPdfSection(pdfPath, heading, identifyOnly);
        result.isHeading = section.isHeading;
        result.title = section.title;
        result.number = section.number;
        result.level = section.level;
        result.kind = section.level <= 1 ? QStringLiteral("section") : section.level == 2 ? QStringLiteral("subsection") : QStringLiteral("subsubsection");
        if (!section.error.isEmpty()) return fail(section.error);
        if (identifyOnly) return result;
        result.plan = planReviewSourceAnchors(docsRoot, bundleRoot, pdfPath, pdfSha256, section.areas, cardId);
        if (!result.plan.error.isEmpty()) return fail(result.plan.error);
        if (!result.plan.withoutSource) return fail(QStringLiteral("The section's pages have editable source, unlike its heading's page. Select its passages instead."));
        pageRange(section.areas);
        return result;
    }
    // From source: the heading's source lines, then its section there.
    Okular::DocsCommentSynchronizer::Options options; options.docsRoot = docsRoot; options.sourceBundleRoot = bundleRoot; options.allowLegacySourceResolution = false; options.cacheSourceResolution = true;
    Okular::DocsCommentSynchronizer resolver(options);
    const QString stem = QFileInfo(pdfPath).completeBaseName();
    const auto resolveLine = [&](int number, const LayoutLine &line, int tries) {
        QList<const Poppler::TextBox *> boxes = line.boxes;
        std::stable_sort(boxes.begin(), boxes.end(), [](auto *a, auto *b) { return foldedLetters(a->text()).size() > foldedLetters(b->text()).size(); });
        Okular::DocsCommentSynchronizer::SourceResolutionResult found;
        for (int i = 0; i < boxes.size() && i < tries && !found.success; ++i) {
            const auto centre = boxes[i]->boundingBox().center();
            found = resolver.resolveSource({stem, number, centre.x(), centre.y(), boxes[i]->text(), pdfPath, true});
        }
        return found;
    };
    const QList<int> chosen = linesIn(layout, heading);
    QHash<QString, QList<int>> mapped;
    QHash<QString, QString> ids;
    for (const int i : chosen) {
        const auto found = resolveLine(heading.page, layout.lines(heading.page)[i], 6);
        if (!found.success) continue;
        mapped[found.sourcePath].append(found.line);
        ids.insert(found.sourcePath, found.sourceId);
    }
    if (mapped.size() != 1) return fail(QStringLiteral("The heading's source line was not found."));
    const QString sourcePath = mapped.cbegin().key(), sourceId = ids.value(sourcePath);
    const QByteArray source = read(sourcePath);
    const auto section = reviewSourceSection(sourceId, source, mapped.cbegin().value());
    result.isHeading = section.isHeading;
    result.title = section.title;
    result.kind = section.kind;
    result.level = section.level;
    result.fromSource = true;
    result.sourceId = sourceId;
    if (!section.error.isEmpty()) return fail(section.error);
    // The heading alone: a selection that runs on into the text makes an ordinary card.
    if (std::any_of(mapped.cbegin().value().cbegin(), mapped.cbegin().value().cend(),
                    [&section](int line) { return line < section.headingLine || line >= section.firstLine; })) {
        result.isHeading = false;
        return fail(QStringLiteral("The selection holds more than the heading."));
    }
    // A numbered heading shows its number in the PDF.
    {
        QString lineText;
        for (const int i : chosen) lineText += layout.lines(heading.page)[i].text + QLatin1Char(' ');
        const auto number = headingNumber(lineText.trimmed());
        if (number.valid()) result.number = headingNumberText(number);
    }
    result.firstLine = section.firstLine;
    result.lastLine = section.lastLine;
    QString expected;
    for (const auto &value : manifest.value(QStringLiteral("sources")).toArray())
        if (value.toObject().value(QStringLiteral("path")).toString() == sourceId) expected = value.toObject().value(QStringLiteral("sha256")).toString();
    if (expected.isEmpty() || digest(source) != expected)
        return fail(QStringLiteral("%1 changed since this PDF was built. Sync, then make the card.").arg(QFileInfo(sourceId).fileName()));
    {
        QSet<QString> wrapped;
        const auto lines = source.split('\n');
        const auto &marker = sourceId.endsWith(QLatin1String(".tex"), Qt::CaseInsensitive) ? texCardMarkerLine() : markdownCardMarkerLine();
        for (int line = section.firstLine; line <= section.lastLine && line <= lines.size(); ++line)
            if (const auto match = marker.match(QString::fromUtf8(lines[line - 1])); match.hasMatch()) wrapped.insert(match.captured(2).toLower());
        result.wrappedCards = int(wrapped.size());
    }
    if (identifyOnly) return result;
    const auto marked = markReviewSourceRanges(sourceId, source, {{section.firstLine, section.lastLine}}, cardId, nullptr, false, true);
    if (!marked.error.isEmpty()) return fail(marked.error);
    // Where the body is rendered: every PDF line, from the heading's page on, whose source line is
    // in it, with the unmapped lines (display math, drawings) between them; the pages of the
    // build that use this source, until a page past the next section holds none of it.
    enum Where { Unknown, Before, Body, After, Foreign };
    struct Placed { int page = 0; int index = 0; Where where = Unknown; };
    QList<Placed> placed;
    int lastPage = layout.pageCount();
    for (const auto &value : manifest.value(QStringLiteral("pdf")).toObject().value(QStringLiteral("segments")).toArray()) {
        const auto segment = value.toObject();
        const int first = segment.value(QStringLiteral("start_page")).toInt(), count = segment.value(QStringLiteral("page_count")).toInt();
        if (heading.page >= first && heading.page < first + count) lastPage = std::min(lastPage, first + count - 1);
    }
    bool pastSection = false;
    for (int number = heading.page; number <= lastPage; ++number) {
        const auto &lines = layout.lines(number);
        bool holdsBody = false;
        for (int i = 0; i < lines.size(); ++i) {
            const auto found = resolveLine(number, lines[i], 4);
            Where where = Unknown;
            if (found.success) {
                where = found.sourcePath != sourcePath ? Foreign
                    : found.line < section.firstLine ? Before : found.line > section.lastLine ? After : Body;
            }
            holdsBody = holdsBody || where == Body;
            pastSection = pastSection || where == After;
            placed.append({number, i, where});
        }
        if (pastSection && !holdsBody) break;
    }
    // An unmapped line is the body's when it lies between the body and its edges: after body or
    // heading, before body or the next section.
    QList<bool> included(placed.size(), false);
    for (int i = 0; i < placed.size(); ++i) {
        if (placed[i].where == Body) { included[i] = true; continue; }
        if (placed[i].where != Unknown) continue;
        Where previous = Unknown, next = Unknown;
        for (int j = i - 1; j >= 0 && previous == Unknown; --j) previous = placed[j].where;
        for (int j = i + 1; j < placed.size() && next == Unknown; ++j) next = placed[j].where;
        included[i] = (previous == Body || next == Body) && (previous == Body || previous == Before) && (next == Body || next == After || next == Unknown);
    }
    QList<ReviewArea> areas;
    QStringList answer;
    for (int i = 0; i < placed.size();) {
        if (!included[i]) { ++i; continue; }
        int j = i;
        while (j + 1 < placed.size() && included[j + 1] && placed[j + 1].page == placed[i].page) ++j;
        const int number = placed[i].page;
        const auto &lines = layout.lines(number);
        const QSizeF size = layout.size(number);
        // From just below the line above (the heading) to just above the line below; at a page's
        // top or bottom, around the text.
        const auto &firstLine = lines[placed[i].index], &lastLine = lines[placed[j].index];
        const bool above = placed[i].index > 0 && lines[placed[i].index - 1].column == firstLine.column;
        const bool below = placed[j].index + 1 < lines.size() && lines[placed[j].index + 1].column == lastLine.column;
        const qreal top = above ? lines[placed[i].index - 1].rect.bottom() + 0.5 : firstLine.rect.top() - 2;
        const qreal bottom = below ? lines[placed[j].index + 1].rect.top() - 0.5 : lastLine.rect.bottom() + 2;
        qreal left = std::numeric_limits<qreal>::max(), right = std::numeric_limits<qreal>::lowest();
        for (const auto &line : lines) { left = std::min(left, line.rect.left()); right = std::max(right, line.rect.right()); }
        for (int k = i; k <= j; ++k) answer.append(lines[placed[k].index].text);
        const QRectF rect(QPointF(std::max<qreal>(0, left - 2) / size.width(), std::max<qreal>(0, top) / size.height()),
                          QPointF(std::min<qreal>(size.width(), right + 2) / size.width(), std::min<qreal>(size.height(), bottom) / size.height()));
        if (rect.width() > 0 && rect.height() > 0) areas.append({number, rect, false});
        i = j + 1;
    }
    if (areas.isEmpty()) return fail(QStringLiteral("The section's text was not found in this PDF. Sync, then try again."));
    ReviewAnchorPlan plan;
    plan.path = sourcePath;
    plan.original = source;
    plan.replacement = marked.replacement;
    plan.anchor = marked.anchor;
    plan.fallbackSource = sourceId;
    const QString text = answer.join(QLatin1Char(' ')).simplified();
    if (!text.isEmpty()) plan.answerAnchor = {{QStringLiteral("version"), 1}, {QStringLiteral("normalized_text"), text}, {QStringLiteral("text_sha256"), digest(text.toUtf8())}};
    for (const auto &area : std::as_const(areas))
        plan.regions.append(QJsonObject{{QStringLiteral("page"), area.page},
                                        {QStringLiteral("rect"), QJsonArray{area.rect.left(), area.rect.top(), area.rect.right(), area.rect.bottom()}}});
    if (auto *first = layout.page(areas.first().page)) plan.pageVisualSha256 = fingerprint(*first);
    result.plan = plan;
    pageRange(areas);
    return result;
}

ReviewAnchorPlan readReviewSourceAnchor(const QString &path, const QByteArray &source, const QString &cardId, bool aroundPendingProposals) {
    ReviewAnchorPlan invalid;
    invalid.error = QStringLiteral("Place one complete start/end marker pair around the answer in the source and save it.");
    if (QUuid(cardId).isNull() || source.isEmpty()) return invalid;
    if (isMarkdownPath(path)) {
        if (QString::fromUtf8(source).toUtf8() != source) return invalid;
        // One start/end pair per answer segment, numbered 0..n-1 in file order, never nested.
        const QRegularExpression marker(QStringLiteral(R"(^ {0,3}<!-- thoughts:v1:(start|end) review:%1:answer:([0-9]+) -->\r?$)").arg(QRegularExpression::escape(cardId)));
        const QRegularExpression fenceStart(QStringLiteral(R"(^ {0,3}(`{3,}|~{3,}))"));
        const auto lines = source.split('\n');
        QByteArray fence;
        QJsonArray segments;
        int startLine = 0, bodyStart = -1, offset = 0;
        for (int i = 0; i < lines.size(); ++i) {
            const QByteArray &line = lines.at(i);
            const QString text = QString::fromUtf8(line);
            const QString trimmed = text.trimmed();
            if (!fence.isEmpty()) {
                int fenceLength = 0;
                while (fenceLength < trimmed.size() && trimmed.at(fenceLength).toLatin1() == fence.at(0)) ++fenceLength;
                if (fenceLength >= fence.size() && trimmed.mid(fenceLength).isEmpty()) fence.clear();
                else if (line.contains(cardId.toUtf8())) return invalid;
            } else if (const auto match = fenceStart.match(text); match.hasMatch()) {
                fence = match.captured(1).toLatin1();
            } else if (const auto match = marker.match(text); match.hasMatch()) {
                if (match.captured(2) != QString::number(segments.size())) return invalid;
                if (match.captured(1) == QLatin1String("start")) {
                    if (startLine) return invalid;
                    startLine = i + 1;
                    bodyStart = offset + line.size() + (i + 1 < lines.size() ? 1 : 0);
                } else {
                    if (!startLine) return invalid;
                    const int bodyEnd = offset;
                    if (bodyEnd <= bodyStart || source.mid(bodyStart, bodyEnd - bodyStart).trimmed().isEmpty()) return invalid;
                    segments.append(QJsonObject{{QStringLiteral("segment_id"), QString::number(segments.size())}, {QStringLiteral("source_file"), path},
                        {QStringLiteral("source_sha256"), digest(source)}, {QStringLiteral("content_sha256"), digest(source.mid(bodyStart, bodyEnd - bodyStart))},
                        {QStringLiteral("line_start"), startLine + 1}, {QStringLiteral("line_end"), i + 1}});
                    startLine = 0;
                }
            } else if (line.contains(cardId.toUtf8())) return invalid;
            offset += line.size() + (i + 1 < lines.size() ? 1 : 0);
        }
        if (startLine || segments.isEmpty()) return invalid;
        ReviewAnchorPlan plan;
        plan.path = path;
        plan.original = source;
        plan.replacement = source;
        plan.anchor = {{QStringLiteral("version"), 2}, {QStringLiteral("segments"), segments}};
        return plan;
    }
    if (!path.endsWith(QLatin1String(".tex"), Qt::CaseInsensitive)) return invalid;
    const QRegularExpression marker(QStringLiteral("^\\\\ReviewCardMask(Start|End)\\{%1\\}\\{([0-9]+)\\}%$").arg(QRegularExpression::escape(cardId)));
    QByteArray unmarked;
    QList<QPair<int, int>> ranges;
    QList<int> lineNumbers;  // the source line of each unmarked line, for messages
    bool inside = false;
    int segment = 0, unmarkedLine = 0;
    const auto lines = source.split('\n');
    for (int i = 0; i < lines.size(); ++i) {
        const auto &line = lines[i];
        const auto match = marker.match(QString::fromUtf8(line));
        if (!match.hasMatch()) lineNumbers.append(i + 1);
        if (match.hasMatch()) {
            if (match.captured(2) != QString::number(segment)) return invalid;
            if (match.captured(1) == QLatin1String("Start")) {
                if (inside) return invalid;
                inside = true;
                ranges.append({unmarkedLine + 1, unmarkedLine});
            } else {
                if (!inside || ranges.last().second < ranges.last().first) return invalid;
                inside = false;
                ++segment;
            }
        } else {
            if (line.contains(cardId.toUtf8())) return invalid;
            ++unmarkedLine;
            if (inside) ranges.last().second = unmarkedLine;
            unmarked += line;
            if (i + 1 < lines.size()) unmarked += '\n';
        }
    }
    if (!segment || inside) return invalid;
    // Reuse the writer's visibility/nesting checks and canonical marker layout, for a passage
    // that already is the card's: definitions in it are no obstacle (TexMarkMode).
    auto plan = markTexRanges(path, unmarked, ranges, cardId, nullptr, ExistingPassage | (aroundPendingProposals ? HeldProposals : NewPassage), lineNumbers);
    if (!plan.error.isEmpty()) {
        // The pair itself is complete; say why the passage around it is refused.
        invalid.error = plan.error;
        return invalid;
    }
    if (plan.replacement != source) return invalid;
    return plan;
}

ReviewMarkerStrip stripReviewCardMarkers(const QString &path, const QByteArray &source, const QString &cardId) {
    ReviewMarkerStrip strip;
    const auto lines = source.split('\n');
    if (QUuid(cardId).isNull()) { strip.error = QStringLiteral("The card identity is invalid."); return strip; }
    if (!source.contains(cardId.toUtf8())) {
        strip.text = source;
        for (int i = 0; i < lines.size(); ++i) strip.lineMap.append(i + 1);
        return strip;
    }
    if (!readReviewSourceAnchor(path, source, cardId).error.isEmpty()) {
        strip.error = QStringLiteral("This card's markers in %1 are incomplete, nested, or appear more than once. Remove them from the source, then place the card again.").arg(path);
        return strip;
    }
    const QRegularExpression marker(isMarkdownPath(path)
        ? QStringLiteral(R"(^ {0,3}<!-- thoughts:v1:(start|end) review:%1:answer:([0-9]+) -->\r?$)").arg(QRegularExpression::escape(cardId))
        : QStringLiteral("^\\\\ReviewCardMask(Start|End)\\{%1\\}\\{([0-9]+)\\}%$").arg(QRegularExpression::escape(cardId)));
    QList<QByteArray> kept;
    for (int i = 0; i < lines.size(); ++i) {
        if (marker.match(QString::fromUtf8(lines[i])).hasMatch()) { strip.lineMap.append(0); ++strip.removed; continue; }
        kept.append(lines[i]);
        strip.lineMap.append(kept.size());
    }
    strip.text = kept.join('\n');
    if (strip.text.contains(cardId.toUtf8())) {
        strip.error = QStringLiteral("This card's markers in %1 are incomplete, nested, or appear more than once. Remove them from the source, then place the card again.").arg(path);
        strip.text.clear(); strip.lineMap.clear(); strip.removed = 0;
    }
    return strip;
}

// One marker of @p cardId's passage @p segment (any passage when empty), as written or tidied by hand.
static QRegularExpression reviewCardMarkerPattern(const QString &path, const QString &cardId, const QString &segment)
{
    const QString id = QRegularExpression::escape(cardId);
    const QString part = segment.isEmpty() ? QStringLiteral("[^}\\s:]+") : QRegularExpression::escape(segment);
    return QRegularExpression(path.endsWith(QLatin1String(".tex"))
        ? QStringLiteral(R"(\\ReviewCardMask(?:Start|End)\s*\{%1\}\s*\{%2\})").arg(id, part)
        : QStringLiteral(R"(<!--\s*thoughts:v1:(?:start|end)\s+review:%1:answer:%2\s*-->)").arg(id, part));
}

QByteArray removeReviewCardMarkers(const QString &path, const QByteArray &source, const QString &cardId,
                                   const QSet<QString> &segmentIds, QString *error)
{
    const auto fail = [error](const QString &reason) { if (error) *error = reason; return QByteArray(); };
    if (error) error->clear();
    const QString text = QString::fromUtf8(source);
    if (text.toUtf8() != source) return fail(QStringLiteral("The source is not valid UTF-8."));
    static const QRegularExpression lineRest(QStringLiteral(R"(^[ \t]*%?[ \t]*(?:\r?\n|$))"));
    QList<QPair<qsizetype, qsizetype>> ranges;
    for (const QString &segmentId : segmentIds) {
        auto matches = reviewCardMarkerPattern(path, cardId, segmentId).globalMatch(text);
        int found = 0;
        while (matches.hasNext()) {
            const auto match = matches.next();
            qsizetype start = match.capturedStart(), end = match.capturedEnd();
            // From 0 (the file's first line) lastIndexOf would search back from the end.
            const qsizetype line = start > 0 ? text.lastIndexOf(QLatin1Char('\n'), start - 1) + 1 : 0;
            const auto suffix = lineRest.match(text.mid(end));
            if (text.mid(line, start - line).trimmed().isEmpty() && suffix.hasMatch()) { start = line; end += suffix.capturedLength(); }
            ranges.append({start, end});
            ++found;
        }
        if (found != 2) return fail(QStringLiteral("The source does not contain exactly one start and one end marker for each passage of the card."));
    }
    // Any other marker of the card would be left behind.
    int all = 0;
    for (auto matches = reviewCardMarkerPattern(path, cardId, {}).globalMatch(text); matches.hasNext(); matches.next()) ++all;
    if (all != ranges.size()) return fail(QStringLiteral("The source contains markers of the card that its record does not list."));
    std::sort(ranges.begin(), ranges.end());
    QString edited = text;
    for (auto it = ranges.crbegin(); it != ranges.crend(); ++it) edited.remove(it->first, it->second - it->first);
    return edited.toUtf8();
}

QString reviewCardMarkerProblem(const QString &path, const QByteArray &source, const QString &cardId, const QString &cardName)
{
    const bool markdown = isMarkdownPath(path);
    const QString text = QString::fromUtf8(source);
    const QString card = cardName.isEmpty() ? cardId : QStringLiteral("“%1”").arg(cardName);
    const QString fix = QStringLiteral(" Fix the markers in the source, then merge the cards.");
    const QString id = QRegularExpression::escape(cardId);
    // Any marker of the card, however written; and a marker line as Thoughts writes it.
    const QRegularExpression anyMarker(markdown
        ? QStringLiteral(R"(<!--\s*thoughts:v1:(start|end)\s+review:%1:answer:([^\s>]*)\s*-->)").arg(id)
        : QStringLiteral(R"(\\ReviewCardMask(Start|End)\s*\{\s*%1\s*\}\s*\{\s*([^}]*?)\s*\})").arg(id),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression markerLine(markdown
        ? QStringLiteral(R"(^ {0,3}<!-- thoughts:v1:(start|end) review:%1:answer:([0-9]+) -->\r?$)").arg(id)
        : QStringLiteral(R"(^\\ReviewCardMask(Start|End)\{%1\}\{([0-9]+)\}%$)").arg(id));
    const QRegularExpression fenceStart(QStringLiteral(R"(^ {0,3}(`{3,}|~{3,}))"));
    const AgentProposalParse parsed = parseAgentProposals(path, text);
    const QStringList lines = text.split(QLatin1Char('\n'));
    int offset = 0, segment = 0, openLine = 0, bodyLines = 0;
    QString openSegment, fence;
    bool found = false;
    for (int i = 0; i < lines.size(); offset += int(lines[i].size()) + 1, ++i) {
        const QString &line = lines[i];
        const int number = i + 1;
        if (markdown) {
            const QString trimmed = line.trimmed();
            if (!fence.isEmpty()) {
                int length = 0;
                while (length < trimmed.size() && trimmed.at(length) == fence.at(0)) ++length;
                if (length >= fence.size() && trimmed.mid(length).isEmpty()) fence.clear();
                else if (line.contains(cardId)) return QStringLiteral("Line %1 of %2 holds a marker of card %3 inside a code block.").arg(QString::number(number), path, card) + fix;
                continue;
            }
            if (const auto match = fenceStart.match(line); match.hasMatch()) {
                fence = match.captured(1);
                continue;
            }
        }
        if (!line.contains(cardId, Qt::CaseInsensitive)) {
            if (openLine && !line.trimmed().isEmpty()) ++bodyLines;
            continue;
        }
        found = true;
        const auto marker = anyMarker.match(line);
        if (!marker.hasMatch())
            return QStringLiteral("Line %1 of %2 mentions card %3 outside its markers; a card's ID may appear only in its markers.").arg(QString::number(number), path, card) + fix;
        // A marker inside a pending proposal: the proposal holds part of a passage and runs past it.
        const int at = offset + int(marker.capturedStart());
        for (const auto &proposal : parsed.proposals) {
            if (proposal.offset <= at && at < proposal.offset + proposal.length) {
                const int last = proposal.line + int(QStringView(text).mid(proposal.offset, proposal.length).count(QLatin1Char('\n')));
                return QStringLiteral("The marker of card %1 on line %2 of %3 lies inside the pending proposal %4 (lines %5-%6), which starts inside the "
                                      "card's passage and ends outside it, or holds it whole. Decide or withdraw that proposal first, then merge the cards.")
                    .arg(card, QString::number(number), path, proposal.id, QString::number(proposal.line), QString::number(last));
            }
        }
        for (const auto &issue : parsed.issues)
            if (issue.offset <= at && at < issue.offset + issue.length)
                return QStringLiteral("The marker of card %1 on line %2 of %3 lies inside a broken proposal (%4). Fix or remove that proposal first, then merge the cards.")
                    .arg(card, QString::number(number), path, agentProposalIssueMessage(issue));
        const auto whole = markerLine.match(line);
        if (!whole.hasMatch()) {
            // Text before or after the marker on its line: a pending proposal's brace, a comment, other text.
            const int lineEnd = offset + int(line.size());
            for (const auto &proposal : parsed.proposals) {
                if (proposal.offset < lineEnd && offset < proposal.offset + proposal.length)
                    return QStringLiteral("The marker of card %1 on line %2 of %3 shares its line with the pending proposal %4. Decide or withdraw that "
                                          "proposal first (a proposal made now leaves markers on lines of their own), then merge the cards.")
                        .arg(card, QString::number(number), path, proposal.id);
            }
            if (anyMarker.match(line, marker.capturedEnd()).hasMatch())
                return QStringLiteral("Line %1 of %2 holds more than one marker of card %3; each marker must be on a line of its own.").arg(QString::number(number), path, card) + fix;
            return QStringLiteral("The marker of card %1 on line %2 of %3 shares its line with other text (“%4”); it must be alone on its line, "
                                  "written as Thoughts writes it.").arg(card, QString::number(number), path, line.trimmed().left(80)) + fix;
        }
        const bool start = whole.captured(1).compare(QLatin1String("start"), Qt::CaseInsensitive) == 0;
        const QString number_ = whole.captured(2);
        if (start) {
            if (openLine)
                return QStringLiteral("Line %1 of %2 starts passage %3 of card %4 inside its passage %5, which opens on line %6 and has no end marker "
                                      "before it (nested passages, or a missing end marker).").arg(QString::number(number), path, number_, card, openSegment, QString::number(openLine)) + fix;
            if (number_ != QString::number(segment))
                return QStringLiteral("Line %1 of %2 starts passage %3 of card %4, but the card's passages must be numbered 0, 1, … in file order; "
                                      "passage %5 was expected here.").arg(QString::number(number), path, number_, card, QString::number(segment)) + fix;
            openLine = number;
            openSegment = number_;
            bodyLines = 0;
            continue;
        }
        if (!openLine)
            return QStringLiteral("Line %1 of %2 ends passage %3 of card %4, which has no start marker before it.").arg(QString::number(number), path, number_, card) + fix;
        if (number_ != openSegment)
            return QStringLiteral("Line %1 of %2 ends passage %3 of card %4, but the passage open there is %5 (from line %6).")
                .arg(QString::number(number), path, number_, card, openSegment, QString::number(openLine)) + fix;
        if (!bodyLines)
            return QStringLiteral("Passage %1 of card %2 (lines %3-%4 of %5) is empty.").arg(openSegment, card, QString::number(openLine), QString::number(number), path) + fix;
        openLine = 0;
        ++segment;
    }
    if (openLine)
        return QStringLiteral("Line %1 of %2 starts passage %3 of card %4, which has no end marker after it.").arg(QString::number(openLine), path, openSegment, card) + fix;
    if (!found) return QStringLiteral("The markers of card %1 are not in %2. Sync, then merge the cards again.").arg(card, path);
    return {};
}

ReviewCardMergeSource planReviewCardMergeSource(const QString &path, const QByteArray &source, const QString &keptId,
                                                const QStringList &mergedIds, const QHash<QString, QSet<QString>> &segmentIds,
                                                const QHash<QString, QString> &cardNames)
{
    ReviewCardMergeSource plan;
    const auto fail = [](const QString &error) {
        ReviewCardMergeSource failed;
        failed.error = error;
        return failed;
    };
    const bool markdown = isMarkdownPath(path);
    if (!markdown && !path.endsWith(QLatin1String(".tex"), Qt::CaseInsensitive)) return fail(QStringLiteral("Choose a LaTeX or Markdown source file."));
    const QStringList cards = QStringList{keptId} + mergedIds;
    if (mergedIds.isEmpty() || QSet<QString>(cards.cbegin(), cards.cend()).size() != cards.size())
        return fail(QStringLiteral("Choose two or more different cards."));
    const auto name = [&cardNames](const QString &id) { return cardNames.value(id).isEmpty() ? id : QStringLiteral("“%1”").arg(cardNames.value(id)); };
    if (QString::fromUtf8(source).toUtf8() != source) return fail(QStringLiteral("The source is not valid UTF-8."));
    for (const QString &id : cards) {
        if (QUuid(id).isNull() || segmentIds.value(id).isEmpty()) return fail(QStringLiteral("Every card to merge must be linked to a source passage."));
        // Complete pairs in their canonical place, as Thoughts writes them; say what is wrong.
        if (const QString problem = reviewCardMarkerProblem(path, source, id, cardNames.value(id)); !problem.isEmpty()) return fail(problem);
        // Their passages as a card's passages must be, pending proposals held whole.
        if (const auto anchor = readReviewSourceAnchor(path, source, id, true); !anchor.error.isEmpty())
            return fail(QStringLiteral("The passage of card %1 in %2 cannot be a card's passage: %3 Fix it in the source, then merge the cards.")
                            .arg(name(id), path, anchor.error));
    }
    // Every passage: its card and its marker lines (one-based).
    const QRegularExpression marker(markdown
        ? QStringLiteral(R"(^ {0,3}<!-- thoughts:v1:(start|end) review:([0-9A-Za-z-]+):answer:([0-9]+) -->\r?$)")
        : QStringLiteral(R"(^\\ReviewCardMask(Start|End)\{([^}]+)\}\{([0-9]+)\}%$)"));
    struct Passage { QString card, segment; int start = 0, end = 0; };
    QList<Passage> passages;
    QHash<QString, int> open;
    QHash<QString, QSet<QString>> marked;
    QSet<int> markerLines;
    const auto lines = source.split('\n');
    for (int i = 0; i < lines.size(); ++i) {
        const auto match = marker.match(QString::fromUtf8(lines[i]));
        if (!match.hasMatch() || !cards.contains(match.captured(2))) continue;
        const QString key = match.captured(2) + QLatin1Char(':') + match.captured(3);
        markerLines.insert(i + 1);
        if (match.captured(1).compare(QLatin1String("start"), Qt::CaseInsensitive) == 0) {
            open.insert(key, i + 1);
            continue;
        }
        passages.append({match.captured(2), match.captured(3), open.take(key), i + 1});
        marked[match.captured(2)].insert(match.captured(3));
    }
    for (const QString &id : cards) {
        if (marked.value(id) != segmentIds.value(id))
            return fail(QStringLiteral("Card %1 lists other passages than its markers in %2 hold. Sync, then merge the cards again.").arg(name(id), path));
    }
    // The merged cards' markers go first, one card at a time, as their deletions remove them.
    plan.versions.append(source);
    QString error;
    for (const QString &id : mergedIds) {
        const auto next = removeReviewCardMarkers(path, plan.versions.constLast(), id, segmentIds.value(id), &error);
        if (!error.isEmpty()) return fail(error);
        plan.versions.append(next);
    }
    // The text every passage is marked in again: without the kept card's markers too.
    const QByteArray unmarked = removeReviewCardMarkers(path, plan.versions.constLast(), keptId, segmentIds.value(keptId), &error);
    if (!error.isEmpty()) return fail(error);
    QList<QByteArray> kept;
    QList<int> lineMap(lines.size() + 1, 0);
    for (int i = 0; i < lines.size(); ++i) {
        if (markerLines.contains(i + 1)) continue;
        kept.append(lines[i]);
        lineMap[i + 1] = kept.size();
    }
    if (kept.join('\n') != unmarked)
        return fail(QStringLiteral("The cards' markers in %1 cannot be removed line by line. Fix them in the source, then merge the cards.").arg(path));
    // Each passage's own lines, without the text between passages; passages that overlap become one.
    struct Range { int first = 0, last = 0; QStringList cards; };
    QList<Range> ranges;
    for (const auto &passage : std::as_const(passages)) {
        int first = 0, last = 0;
        for (int line = passage.start + 1; line < passage.end; ++line) {
            if (!lineMap[line]) continue;
            if (!first) first = lineMap[line];
            last = lineMap[line];
        }
        if (!first) return fail(QStringLiteral("A passage of card %1 is empty.").arg(name(passage.card)));
        ranges.append({first, last, {passage.card}});
    }
    std::stable_sort(ranges.begin(), ranges.end(), [](const Range &a, const Range &b) { return a.first < b.first; });
    QList<Range> joined;
    for (const auto &range : std::as_const(ranges)) {
        if (!joined.isEmpty() && range.first <= joined.last().last) {
            joined.last().last = std::max(joined.last().last, range.last);
            for (const auto &card : range.cards) if (!joined.last().cards.contains(card)) joined.last().cards.append(card);
            continue;
        }
        joined.append(range);
    }
    QList<QPair<int, int>> lineRanges;
    for (const auto &range : std::as_const(joined)) lineRanges.append({range.first, range.last});
    // Pending proposals inside passages stay inside, byte for byte: only marker lines move.
    auto marking = markReviewSourceRanges(path, unmarked, lineRanges, keptId, nullptr, true);
    if (!marking.error.isEmpty()) return fail(QStringLiteral("The merged card's passages cannot be marked in %1: %2").arg(path, marking.error));
    const auto segments = marking.anchor.value(QStringLiteral("segments")).toArray();
    if (segments.size() != joined.size()) return fail(QStringLiteral("The merged card's passages cannot be marked in %1.").arg(path));
    // The server's rule for the kept card: the source without its old markers equals the source
    // without its new ones.
    QSet<QString> newSegments;
    for (const auto &value : segments) newSegments.insert(value.toObject().value(QStringLiteral("segment_id")).toString());
    if (removeReviewCardMarkers(path, marking.replacement, keptId, newSegments, &error) != unmarked || !error.isEmpty())
        return fail(QStringLiteral("The merged card's markers would change the text of %1.").arg(path));
    plan.replacement = marking.replacement;
    plan.anchor = marking.anchor;
    for (const auto &range : std::as_const(joined)) plan.origins.append(range.cards);
    return plan;
}

namespace {
QString oneLineText(QString text)
{
    static const QRegularExpression whitespace(QStringLiteral("\\s+"));
    return text.replace(whitespace, QStringLiteral(" ")).trimmed();
}

struct AnswerRegion { int page; QRectF rect; };

// The card's answer regions in reading order: by page, then top to bottom, then left to right.
QList<AnswerRegion> answerRegions(const QJsonArray &values)
{
    QList<AnswerRegion> regions;
    for (const auto &value : values) {
        const auto region = value.toObject();
        const auto rect = region.value(QStringLiteral("rect")).toArray();
        const int page = region.value(QStringLiteral("page")).toInt();
        if (page < 1 || rect.size() != 4) continue;
        regions.append({page, QRectF(QPointF(rect[0].toDouble(), rect[1].toDouble()), QPointF(rect[2].toDouble(), rect[3].toDouble())).normalized()});
    }
    std::stable_sort(regions.begin(), regions.end(), [](const AnswerRegion &a, const AnswerRegion &b) {
        if (a.page != b.page) return a.page < b.page;
        if (a.rect.top() != b.rect.top()) return a.rect.top() < b.rect.top();
        return a.rect.left() < b.rect.left();
    });
    return regions;
}

// The text inside the answer regions, only from the PDF the card was made on: its regions mean
// nothing on another build.
QString pdfAnswerText(Poppler::Document *pdf, const ReviewCardAgentInput &card, const QList<AnswerRegion> &regions)
{
    if (!pdf || card.pdfSha256.isEmpty() || digest(read(card.pdfPath)) != card.pdfSha256.toLower()) return {};
    const int pageCount = pdf->numPages();
    QStringList texts;
    for (const auto &region : regions) {
        if (region.page > pageCount) continue;
        std::unique_ptr<Poppler::Page> page(pdf->page(region.page - 1));
        if (!page) continue;
        const QSizeF size = page->pageSizeF();
        const QRectF area(region.rect.left() * size.width(), region.rect.top() * size.height(),
                          region.rect.width() * size.width(), region.rect.height() * size.height());
        for (const auto &line : page->text(area).split(QLatin1Char('\n'))) {
            const QString collapsed = oneLineText(line);
            if (!collapsed.isEmpty()) texts.append(collapsed);
        }
    }
    return texts.join(QLatin1Char('\n'));
}

std::unique_ptr<Poppler::Document> loadCardPdf(const QString &path)
{
    std::unique_ptr<Poppler::Document> pdf = QFileInfo(path).isFile() ? Poppler::Document::load(path) : nullptr;
    if (pdf && pdf->isLocked()) pdf.reset();
    return pdf;
}
}

QList<ReviewCardSourcePassage> reviewCardSourcePassages(const QString &docsRoot, const QString &cardId, const QStringList &sourceFiles,
                                                        QHash<QString, QString> *contentCache)
{
    const auto trimEnd = [](QString text) {
        while (!text.isEmpty() && text.back().isSpace()) text.chop(1);
        return text;
    };
    // The source text between the card's mask markers (TeX or Markdown), markers excluded, read
    // from the local working copy. A marker may sit mid-line: only the text between the markers
    // counts, and its lines are the lines that text occupies. Trailing whitespace is trimmed on
    // every line, and blank lines directly inside the markers are dropped from range and text.
    QList<ReviewCardSourcePassage> sources;
    QStringList seenFiles;
    for (const QString &file : sourceFiles) {
        const QString path = file.isEmpty() ? QString() : QDir(docsRoot).filePath(file);
        if (path.isEmpty() || cardId.isEmpty() || seenFiles.contains(QDir::cleanPath(path))) continue;
        seenFiles.append(QDir::cleanPath(path));
        QString content;
        if (contentCache && contentCache->contains(path)) {
            content = contentCache->value(path);
        } else {
            QFile source(path);
            if (source.open(QIODevice::ReadOnly)) {
                content = QString::fromUtf8(source.readAll());
                content.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
            }
            if (contentCache) contentCache->insert(path, content);
        }
        if (content.isEmpty() || !content.contains(cardId)) continue;
        const QString id = QRegularExpression::escape(cardId);
        // A TeX marker's trailing % (which keeps it out of the typeset text) belongs to the marker.
        const QRegularExpression marker(QStringLiteral(
            R"(\\ReviewCardMask(Start|End)\s*\{%1\}\s*\{([0-9]+)\}%?|<!--\s*thoughts:v1:(start|end)\s+review:%1:answer:([0-9]+)\s*-->)").arg(id));
        QHash<QString, qsizetype> opened; // Segment -> offset just after its start marker.
        QList<ReviewCardSourceSegment> segments;
        auto matches = marker.globalMatch(content);
        while (matches.hasNext()) {
            const auto match = matches.next();
            const bool tex = !match.captured(1).isEmpty();
            const bool isStart = (tex ? match.captured(1) : match.captured(3)).compare(QLatin1String("start"), Qt::CaseInsensitive) == 0;
            const QString segment = tex ? match.captured(2) : match.captured(4);
            if (isStart) {
                opened.insert(segment, match.capturedEnd());
                continue;
            }
            if (!opened.contains(segment)) continue;
            const qsizetype bodyStart = opened.take(segment);
            const int firstLine = int(QStringView(content).left(bodyStart).count(QLatin1Char('\n'))) + 1;
            QStringList body = content.mid(bodyStart, match.capturedStart() - bodyStart).split(QLatin1Char('\n'));
            for (auto &line : body) line = trimEnd(line);
            int first = 0, last = int(body.size()) - 1;
            while (first <= last && body.at(first).isEmpty()) ++first;
            while (last >= first && body.at(last).isEmpty()) --last;
            if (first > last) continue; // An empty marker pair.
            segments.append({firstLine + first, firstLine + last, body.mid(first, last - first + 1).join(QLatin1Char('\n'))});
        }
        std::sort(segments.begin(), segments.end(), [](const ReviewCardSourceSegment &a, const ReviewCardSourceSegment &b) { return a.first < b.first; });
        if (!segments.isEmpty()) sources.append({path, segments});
    }
    return sources;
}

QString reviewCardPdfAnswerText(const ReviewCardAgentInput &card)
{
    const auto regions = answerRegions(card.regions);
    if (regions.isEmpty() || card.pdfSha256.isEmpty()) return {};
    const auto pdf = loadCardPdf(card.pdfPath);
    return pdfAnswerText(pdf.get(), card, regions);
}

QJsonArray reviewCardAgentFeedback(const QString &docsRoot, const QJsonObject &cardRecord, const ReviewCardAgentInput &answer, const QList<QJsonObject> &records,
                                   const QHash<QString, QJsonObject> &homeCards, bool includeDeleted, const std::function<QString()> &fallbackAnswerText)
{
    const QString cardId = cardRecord.value(QStringLiteral("card_id")).toString();
    QList<QJsonObject> result;
    for (const QJsonObject &record : records) {
        if (!ReviewCardFeedback::isShared(record, cardId)) result.append(record);
    }
    // Shared feedback: the other cards' records of its document inside its answer.
    const QString fileDocument = answer.pdfPath.isEmpty() ? QString()
        : DocumentIdentityStore(docsRoot).byFilename(QFileInfo(answer.pdfPath).fileName()).value(QStringLiteral("document_id")).toString();
    QVariantList regions;
    for (const QJsonValue &region : answer.regions) regions.append(region.toObject().toVariantMap());
    const QHash<QString, QString> states = ReviewCardFeedback::sharedStates(
        cardId, ReviewCardFeedback::documentOf(cardRecord, {fileDocument}), homeCards, answer.pdfSha256, ReviewCardFeedback::regionsFromVariant(regions), records, [&] {
            const QString text = reviewCardPdfAnswerText(answer);
            return text.isEmpty() && fallbackAnswerText ? fallbackAnswerText() : text;
        });
    for (const QJsonObject &record : records) {
        if (states.contains(record.value(QStringLiteral("feedback_id")).toString())) result.append(record);
    }
    return ReviewCardFeedback::agentArray(result, cardRecord, includeDeleted, states);
}

QHash<QString, QString> reviewCardPromptFields(const QString &docsRoot, const QJsonArray &projects, const ReviewCardAgentInput &card,
                                               bool withAnswerText)
{
    const auto pdf = loadCardPdf(card.pdfPath);
    const int pageCount = pdf ? pdf->numPages() : 0;
    const auto regions = answerRegions(card.regions);
    int firstPage = 0, lastPage = 0;
    if (!regions.isEmpty()) {
        firstPage = regions.first().page;
        for (const auto &region : regions) lastPage = std::max(lastPage, region.page);
    } else if (card.wholeDocument && pageCount > 0) {
        firstPage = 1;
        lastPage = pageCount;
    } else if (card.page > 0) {
        firstPage = lastPage = card.page;
    }

    auto fields = agentDocumentFields(docsRoot, projects, card.pdfPath, card.documentName);
    if (pageCount > 0) fields.insert(QStringLiteral("pdf_pages"), QString::number(pageCount));
    fields.insert(QStringLiteral("card_id"), oneLineText(card.cardId));
    fields.insert(QStringLiteral("question"), oneLineText(card.question));
    fields.insert(QStringLiteral("note"), oneLineText(card.note));
    if (!card.answerFeedback.isEmpty()) fields.insert(QStringLiteral("answer_feedback"), card.answerFeedback);
    if (firstPage > 0)
        fields.insert(QStringLiteral("answer_pages"), firstPage == lastPage ? QString::number(firstPage)
                                                                           : QStringLiteral("%1–%2").arg(firstPage).arg(lastPage));

    const auto sources = reviewCardSourcePassages(docsRoot, card.cardId, card.sourceFiles);
    QString answer;
    if (!sources.isEmpty()) {
        QStringList files, texts;
        for (const auto &source : sources) {
            QStringList ranges;
            for (const auto &segment : source.segments) {
                ranges.append(QStringLiteral("%1–%2").arg(segment.first).arg(segment.last));
                texts.append(segment.text);
            }
            files.append(QStringLiteral("%1, lines %2").arg(source.path, ranges.join(QStringLiteral("; "))));
        }
        fields.insert(QStringLiteral("answer_source"), files.join(QStringLiteral("; ")));
        answer = texts.join(QStringLiteral("\n\n"));
    } else if (withAnswerText) {
        answer = pdfAnswerText(pdf.get(), card, regions);
    }
    if (withAnswerText) fields.insert(QStringLiteral("answer_text"), answer);
    return fields;
}

QString reviewCardAgentDescription(const QString &docsRoot, const QJsonArray &projects, const ReviewCardAgentInput &card,
                                   const QString &templateText)
{
    const QString effective = templateText.trimmed().isEmpty() ? AgentPrompts::defaultTemplate(AgentPrompts::Kind::Agent) : templateText;
    return AgentPrompts::render(effective, reviewCardPromptFields(docsRoot, projects, card,
                                                                 AgentPrompts::usesPlaceholder(effective, QStringLiteral("answer_text"))));
}
}

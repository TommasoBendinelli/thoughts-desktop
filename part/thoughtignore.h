// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringDecoder>

#include "fileidentitycache.h"
#include "sourceowner.h"

namespace Thoughts {

class ThoughtIgnore {
    struct Rule {
        QRegularExpression expression;
        bool basename = false;
        bool directoryOnly = false;
        bool excluded = true;
    };
    QList<Rule> m_rules;
    QString m_error;

    static QString regularExpression(const QString &pattern) {
        QString expression;
        for (qsizetype index = 0; index < pattern.size(); ++index) {
            const QChar character = pattern.at(index);
            if (character == QLatin1Char('*')) {
                if (index + 1 < pattern.size() && pattern.at(index + 1) == QLatin1Char('*')) {
                    index += 1;
                    if (index + 1 < pattern.size() && pattern.at(index + 1) == QLatin1Char('/')) {
                        expression += QStringLiteral("(?:.*/)?");
                        index += 1;
                    } else expression += QStringLiteral(".*");
                } else expression += QStringLiteral("[^/]*");
            } else if (character == QLatin1Char('?')) expression += QStringLiteral("[^/]");
            else expression += QRegularExpression::escape(QString(character));
        }
        return QStringLiteral("^(?:") + expression + QStringLiteral(")$");
    }

    struct Policy {
        QList<Rule> rules;
        QString error;
    };

    static Policy parse(const QString &contents) {
        Policy policy;
        for (QString line : contents.split(QLatin1Char('\n'))) {
            line = line.trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
            const bool excluded = !line.startsWith(QLatin1Char('!'));
            if (!excluded) line.remove(0, 1);
            const bool anchored = line.startsWith(QLatin1Char('/'));
            const bool directoryOnly = line.endsWith(QLatin1Char('/'));
            while (line.startsWith(QLatin1Char('/'))) line.remove(0, 1);
            while (line.endsWith(QLatin1Char('/'))) line.chop(1);
            if (line.isEmpty() || line == QLatin1String(".") || line == QLatin1String("..") || line.contains(QLatin1Char('\\'))) {
                policy.error = QStringLiteral(".ThoughtIgnore contains an invalid rule."); return policy;
            }
            const QRegularExpression expression(regularExpression(line));
            if (!expression.isValid()) { policy.error = QStringLiteral(".ThoughtIgnore contains an invalid pattern."); return policy; }
            policy.rules.append({expression, !line.contains(QLatin1Char('/')) && !anchored, directoryOnly, excluded});
        }
        return policy;
    }

    // Every folder's policy file, read and compiled again only when it
    // changed: a Library load and each change check consult it for every
    // source, which read and compiled it hundreds of times each.
    static FileIdentityCache<Policy> &policies() {
        static FileIdentityCache<Policy> cache(4096);
        return cache;
    }

public:
    explicit ThoughtIgnore(const QString &root) {
        const QString path = QDir(root).filePath(QStringLiteral(".ThoughtIgnore"));
        const QFileInfo info(path);
        // Without a policy file, skip Git data and Finder metadata only. A
        // Project's default folder also skips the build leftovers written
        // inside it (spec Rule 4).
        if (info.exists() || info.isSymLink()) {
            if (!info.isFile() || info.isSymLink()) { m_error = QStringLiteral(".ThoughtIgnore is not a regular file."); return; }
            if (info.size() > 4 * 1024 * 1024) { m_error = QStringLiteral(".ThoughtIgnore exceeds the 4 MiB limit."); return; }
            const Policy policy = policies().value(path, [](const QString &file) {
                Policy result;
                QFile input(file);
                if (!input.open(QIODevice::ReadOnly)) { result.error = QStringLiteral(".ThoughtIgnore could not be read."); return result; }
                QStringDecoder decoder(QStringDecoder::Utf8);
                const QString contents = decoder.decode(input.readAll());
                if (decoder.hasError()) { result.error = QStringLiteral(".ThoughtIgnore is not valid UTF-8."); return result; }
                return parse(contents);
            });
            m_rules = policy.rules;
            m_error = policy.error;
            return;
        }
        static const Policy folderDefaults = parse(QStringLiteral(".git/\n.DS_Store\n"));
        static const Policy defaultFolderDefaults = parse(QStringLiteral(".git/\n.DS_Store\nbuild/\npdf/\n"));
        const Policy &policy = isDefaultFolder(root) ? defaultFolderDefaults : folderDefaults;
        m_rules = policy.rules;
        m_error = policy.error;
    }

    // Written for a new project that has no .ThoughtIgnore of its own. Sync
    // and the Sources tree use only these rules, so they leave out build
    // outputs, editor leftovers and folders Thoughts never synced.
    static QByteArray defaultRules() {
        return QByteArrayLiteral(
            ".git/\n"
            "tmp/\n"
            "build/\n"
            ".cache/\n"
            "__pycache__/\n"
            ".pytest_cache/\n"
            ".ruff_cache/\n"
            ".github/\n"
            ".vscode/\n"
            ".DS_Store\n"
            "\n"
            "# LaTeX build outputs\n"
            "*.aux\n"
            "*.bbl\n"
            "*.bcf\n"
            "*.blg\n"
            "*.fdb_latexmk\n"
            "*.fls\n"
            "*.lof\n"
            "*.log\n"
            "*.lot\n"
            "*.loa\n"
            "*.lol\n"
            "*.nav\n"
            "*.out\n"
            "*.run.xml\n"
            "*.snm\n"
            "*.synctex\n"
            "*.synctex.gz\n"
            "*.synctex(busy)\n"
            "*.pdfsync\n"
            "*.toc\n"
            "*.vrb\n"
            "*.dvi\n"
            "*.xdv\n"
            "*.acn\n"
            "*.acr\n"
            "*.alg\n"
            "*.glg\n"
            "*.glo\n"
            "*.gls\n"
            "*.ist\n"
            "*.xdy\n"
            "*.pyg\n"
            "_minted-*/\n"
            "main.pdf\n"
            "\n"
            "# Editor, download and patch leftovers\n"
            "*.swp\n"
            "*.swo\n"
            "*.swn\n"
            "*~\n"
            ".#*\n"
            "~$*\n"
            "*#\n"
            "*.bak\n"
            "*.orig\n"
            "*.rej\n"
            "*.tmp\n"
            "*.temp\n"
            "*.part\n"
            "*.crdownload\n"
            "\n"
            "# Folders Thoughts never synced before\n"
            ".venv/\n"
            "venv/\n"
            "node_modules/\n"
            ".mypy_cache/\n"
            ".tox/\n"
            ".hg/\n"
            ".svn/\n"
            "dist/\n"
            "target/\n"
            "out/\n"
            "output/\n"
            "log/\n"
            "logs/\n"
            "temp/\n"
            "pdf/\n"
            "scripts/\n"
            "submission/\n");
    }

    QString error() const { return m_error; }

    static bool isDefaultFolder(const QString &root) {
        return !SourceOwner::defaultFolderRoot(QDir::cleanPath(QFileInfo(root).absoluteFilePath())).isEmpty();
    }

    bool excludes(const QString &relative, bool isDirectory = false) const {
        const QString clean = QDir::cleanPath(relative);
        if (clean != relative || clean.isEmpty() || clean == QLatin1String(".") || clean == QLatin1String("..")
            || clean.startsWith(QLatin1String("../")) || QDir::isAbsolutePath(clean) || clean.contains(QLatin1Char('\\'))) return true;
        const QStringList parts = clean.split(QLatin1Char('/'));
        for (qsizetype end = 1; end <= parts.size(); ++end) {
            bool ignored = false;
            const bool directory = end < parts.size() || isDirectory;
            for (const Rule &rule : m_rules) {
                if (rule.directoryOnly && !directory) continue;
                const QString candidate = rule.basename ? parts.at(end - 1) : parts.mid(0, end).join(QLatin1Char('/'));
                if (rule.expression.match(candidate).hasMatch()) ignored = rule.excluded;
            }
            if (ignored) return true;
        }
        return false;
    }
};

}

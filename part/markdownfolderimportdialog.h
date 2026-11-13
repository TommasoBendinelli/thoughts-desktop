// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "gitclonedialog.h"
#include "markdownfolderimport.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUuid>
#include <QVBoxLayout>

namespace Thoughts {
inline QStringList selectedMarkdownImportDocuments(QTreeWidget *tree)
{
    QStringList paths;
    for (QTreeWidgetItemIterator it(tree); *it; ++it) {
        const auto path = (*it)->data(0, Qt::UserRole).toString();
        if (!path.isEmpty() && (*it)->checkState(0) == Qt::Checked) paths.append(path);
    }
    return paths;
}

inline void populateMarkdownImportPreview(const MarkdownFolderImport &snapshot, QTreeWidget *tree, QLabel *summary, QPushButton *import)
{
    tree->setHeaderLabels({QObject::tr("Folder / document"), QObject::tr("Title"), QObject::tr("Size"), QObject::tr("Issues")});
    tree->headerItem()->setToolTip(2, QObject::tr("Source files and dependencies, before rendering. Shared files count once in folder and selection totals. The final PDF size may differ."));
    tree->setDragDropMode(QAbstractItemView::InternalMove); tree->setDefaultDropAction(Qt::MoveAction);
    QMap<QString, QTreeWidgetItem *> groups;
    for (const auto &path : snapshot.documents) {
        const auto parts = path.split(QLatin1Char('/')); QString key; QTreeWidgetItem *group = nullptr;
        for (int i = 0; i < parts.size() - 1; ++i) {
            key += (key.isEmpty() ? QString() : QStringLiteral("/")) + parts[i];
            if (!groups.contains(key)) {
                auto *item = group ? new QTreeWidgetItem(group) : new QTreeWidgetItem(tree);
                item->setText(0, parts[i]); item->setFlags(item->flags() | Qt::ItemIsAutoTristate | Qt::ItemIsUserCheckable);
                item->setCheckState(0, Qt::Checked); groups.insert(key, item);
            }
            group = groups.value(key);
        }
        auto *item = group ? new QTreeWidgetItem(group) : new QTreeWidgetItem(tree);
        item->setText(0, parts.last()); item->setText(1, snapshot.title(path)); item->setToolTip(0, path);
        item->setData(0, Qt::UserRole, path); item->setFlags((item->flags() | Qt::ItemIsUserCheckable) & ~Qt::ItemIsDropEnabled);
        item->setCheckState(0, Qt::Checked);
    }
    tree->expandToDepth(1); tree->setColumnWidth(0, 300); tree->setColumnWidth(1, 230); tree->setColumnWidth(2, 100);
    summary->setWordWrap(true);
    const auto refresh = [snapshot, tree, summary, import] {
        const QSignalBlocker block(tree); const auto paths = selectedMarkdownImportDocuments(tree);
        const auto selectionIssues = snapshot.selectionIssues(paths); int issueCount = selectionIssues.size();
        QMap<QTreeWidgetItem *, QStringList> folderDocuments;
        for (QTreeWidgetItemIterator it(tree); *it; ++it) {
            const auto path = (*it)->data(0, Qt::UserRole).toString();
            if (path.isEmpty()) continue;
            for (auto *parent = (*it)->parent(); parent; parent = parent->parent()) folderDocuments[parent].append(path);
            const auto errors = paths.contains(path) ? snapshot.issues(path, paths) : QStringList{};
            (*it)->setText(3, errors.join(QStringLiteral("; "))); (*it)->setToolTip(3, errors.join(QLatin1Char('\n'))); issueCount += errors.size();
            const qint64 bytes = snapshot.selectionBytes({path});
            (*it)->setText(2, QLocale().formattedDataSize(bytes));
            (*it)->setData(2, Qt::UserRole, bytes);
            (*it)->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
            (*it)->setToolTip(2, QObject::tr("Document: %1\nDependencies: %2\nTotal: %3 bytes")
                .arg(QLocale().formattedDataSize(snapshot.sizes.value(path)))
                .arg(QLocale().formattedDataSize(bytes - snapshot.sizes.value(path)))
                .arg(QLocale().toString(bytes)));
        }
        for (auto it = folderDocuments.cbegin(); it != folderDocuments.cend(); ++it) {
            const auto bytes = snapshot.selectionBytes(it.value());
            it.key()->setText(2, QLocale().formattedDataSize(bytes));
            it.key()->setData(2, Qt::UserRole, bytes);
            it.key()->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
            it.key()->setToolTip(2, QObject::tr("All documents in this folder and their dependencies. Shared files count once.\n%1 bytes").arg(QLocale().toString(bytes)));
        }
        const qint64 bytes = snapshot.selectionBytes(paths);
        summary->setProperty("selectedBytes", bytes);
        summary->setText(QObject::tr("%1 documents selected · %2 selected · %3 issues").arg(paths.size()).arg(QLocale().formattedDataSize(bytes)).arg(issueCount)
            + (selectionIssues.isEmpty() ? QString() : QLatin1Char('\n') + selectionIssues.join(QLatin1Char('\n'))));
        summary->setToolTip(QObject::tr("Selected source size: %1 bytes. Shared files count once.\nLimits: 100 MiB and 1000 files per import; 10 MiB per file and 4 MiB per Markdown document.").arg(QLocale().toString(bytes)));
        import->setEnabled(!paths.isEmpty() && issueCount == 0);
    };
    QObject::connect(tree, &QTreeWidget::itemChanged, tree, refresh);
    QObject::connect(tree->model(), &QAbstractItemModel::rowsMoved, tree, refresh, Qt::QueuedConnection);
    refresh();
}

inline QJsonObject prepareMarkdownFolderImport(const QString &docsRoot, QWidget *parent, const QString &repository = {}, const QString &subfolder = {})
{
    const auto directory = repository.isEmpty() ? QFileDialog::getExistingDirectory(parent, QObject::tr("Import sources")) : repository;
    if (directory.isEmpty()) return {};
    QByteArray ignoreContents;
    const QFileInfo ignoreInfo(QDir(directory).filePath(QStringLiteral(".ThoughtIgnore")));
    if (ignoreInfo.exists() || ignoreInfo.isSymLink()) {
        if (!ignoreInfo.isFile() || ignoreInfo.isSymLink() || ignoreInfo.size() > MarkdownFolderImport::DocumentLimit) {
            QMessageBox::warning(parent, QObject::tr("Import sources"), QObject::tr(".ThoughtIgnore must be a regular file of at most 4 MiB.")); return {};
        }
        QFile ignoreFile(ignoreInfo.absoluteFilePath());
        if (!ignoreFile.open(QIODevice::ReadOnly)) { QMessageBox::warning(parent, QObject::tr("Import sources"), QObject::tr(".ThoughtIgnore could not be read.")); return {}; }
        ignoreContents = ignoreFile.readAll();
    }
    auto snapshot = MarkdownFolderImport::scan(directory);
    // Cloning owns the complete checkout; discovery/validation must not discard it.
    // Manage documents and Sync can report unsupported or oversized build inputs.
    if (!repository.isEmpty() && !snapshot.error.isEmpty()) {
        const bool latex = snapshot.latex;
        const bool native = snapshot.repositoryLayout;
        snapshot = MarkdownFolderImport{};
        snapshot.latex = latex;
        snapshot.repositoryLayout = native;
    }
    if (!subfolder.isEmpty()) {
        const auto prefix = (snapshot.latex && !QDir(directory).exists(QStringLiteral("tex/source")) ? QStringLiteral("tex/") : QString()) + subfolder + QLatin1Char('/');
        for (auto it = snapshot.documents.begin(); it != snapshot.documents.end();) {
            if (!it->startsWith(prefix)) it = snapshot.documents.erase(it); else ++it;
        }
    }
    if (!snapshot.error.isEmpty() && repository.isEmpty()) {
        QMessageBox::warning(parent, QObject::tr("Import sources"), snapshot.error); return {};
    }
    if (snapshot.documents.isEmpty() && repository.isEmpty()) { QMessageBox::warning(parent, QObject::tr("Import sources"), QObject::tr("No supported documents were found in the selected source folder.")); return {}; }
    QDialog dialog(parent); dialog.setWindowTitle(QObject::tr("Import sources")); dialog.resize(980, 580);
    auto *layout = new QVBoxLayout(&dialog);
    auto *explanation = new QLabel(snapshot.latex
        ? QObject::tr("Select LaTeX documents and drag them into reading order. The copy includes shared source files, styles, references, and figures.\nThe folder is copied into Sources, where you can edit it. Sync renders PDFs into Documents.")
        : QObject::tr("Select Markdown documents and drag them into reading order. Sizes include linked images; shared images count once in totals.\nThe folder is copied into Sources, where you can edit it. Sync renders PDFs into Documents."), &dialog);
    if (!repository.isEmpty()) explanation->setText(QObject::tr("Select documents and drag them into reading order. The full Git checkout, including .git, is kept in Sources. Sync renders the selected documents into Documents."));
    explanation->setWordWrap(true); layout->addWidget(explanation);
    auto *tree = new QTreeWidget(&dialog); tree->setObjectName(QStringLiteral("markdownFolderImportPreview"));
    layout->addWidget(tree);
    auto *summary = new QLabel(&dialog); layout->addWidget(summary);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    auto *import = buttons->addButton(QObject::tr("Import"), QDialogButtonBox::AcceptRole); layout->addWidget(buttons);
    populateMarkdownImportPreview(snapshot, tree, summary, import);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (repository.isEmpty() && dialog.exec() != QDialog::Accepted) return {};
    auto paths = repository.isEmpty() ? selectedMarkdownImportDocuments(tree) : snapshot.documents;
    QByteArray content = snapshot.bundle(paths, repository.isEmpty());
    if (content.isEmpty() && !repository.isEmpty()) {
        snapshot.files.clear(); snapshot.sizes.clear(); snapshot.documents.clear(); paths.clear();
        content = snapshot.bundle({});
    }
    if (content.isEmpty()) { QMessageBox::warning(parent, QObject::tr("Import sources"), QObject::tr("The selected folder copy could not be prepared. Please reopen the folder and try again.")); return {}; }
    const QString digest = QString::fromLatin1(QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex());
    const QString root = QDir(docsRoot).filePath(QStringLiteral(".thoughts/folder-imports"));
    const QString staged = QDir(root).filePath(digest + QStringLiteral(".json"));
    QSaveFile file(staged);
    if (!QDir().mkpath(root) || !file.open(QIODevice::WriteOnly) || file.write(content) != content.size() || !file.commit()) {
        QMessageBox::warning(parent, QObject::tr("Import sources"), QObject::tr("The folder copy could not be saved.")); return {};
    }
    QJsonArray documents;
    for (const auto &path : paths) documents.append(QJsonObject{{QStringLiteral("name"), snapshot.title(path)}, {QStringLiteral("relative_path"), path},
        {QStringLiteral("size_bytes"), snapshot.selectionBytes({path})},
        {QStringLiteral("modified_at"), snapshot.selectionModified({path}).toUTC().toString(Qt::ISODateWithMs)}});
    return {{QStringLiteral("git_checkout"), repository}, {QStringLiteral("thought_ignore"), QString::fromLatin1(ignoreContents.toBase64())},
            {QStringLiteral("thought_ignore_present"), ignoreInfo.exists()},
            {QStringLiteral("saved_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}, {QStringLiteral("name"), MarkdownFolderImport::folderName(directory)}, {QStringLiteral("local_path"), staged},
            {QStringLiteral("sha256"), digest}, {QStringLiteral("documents"), documents}};
}
// The repository's own name for a URL, scp-style address or local path:
// "…/release-1.5-report/", "git@github.com:owner/notes.git" and "C:\repos\notes" give
// "release-1.5-report", "notes" and "notes".
inline QString gitImportName(const QString &location)
{
    QString name = location.trimmed().replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (name.endsWith(QLatin1Char('/'))) name.chop(1);
    name = name.section(QLatin1Char('/'), -1).section(QLatin1Char(':'), -1);
    if (name.endsWith(QLatin1String(".git"))) name.chop(4);
    return name;
}
// What `git clone` accepts here: an absolute local path, an http(s)://, ssh://,
// git:// or file:// URL, or an scp-style [user@]host:path address.
inline bool gitImportAddressSupported(const QString &source)
{
    const auto address = source.trimmed();
    if (address.isEmpty()) return false;
    if (QDir::isAbsolutePath(address)) return true;
    QString host; int port = 0;
    if (sshEndpoint(address, &host, &port)) return true;
    const auto separator = address.indexOf(QLatin1String("://"));
    if (separator <= 0 || separator + 3 >= address.size()) return false;
    static const QStringList schemes{QStringLiteral("http"), QStringLiteral("https"), QStringLiteral("ssh"), QStringLiteral("ssh+git"),
                                     QStringLiteral("git+ssh"), QStringLiteral("git"), QStringLiteral("file")};
    return schemes.contains(address.left(separator).toLower());
}
inline QJsonObject prepareSourcesImport(const QString &docsRoot, QWidget *parent)
{
    QDialog choice(parent); choice.setWindowTitle(QObject::tr("Import sources…"));
    auto *choices = new QVBoxLayout(&choice);
    auto *folder = new QPushButton(QObject::tr("From folder…"), &choice);
    auto *git = new QPushButton(QObject::tr("From Git repository…"), &choice);
    folder->setObjectName(QStringLiteral("importSourcesFromFolder"));
    git->setObjectName(QStringLiteral("importSourcesFromGit"));
    choices->addWidget(folder); choices->addWidget(git);
    QObject::connect(folder, &QPushButton::clicked, &choice, [&choice] { choice.done(1); });
    QObject::connect(git, &QPushButton::clicked, &choice, [&choice] { choice.done(2); });
    const int source = choice.exec();
    if (source == 1) return prepareMarkdownFolderImport(docsRoot, parent);
    if (source != 2) return {};
    QDialog dialog(parent); dialog.setWindowTitle(QObject::tr("From Git repository…"));
    auto *form = new QFormLayout(&dialog);
    auto *url = new QLineEdit(&dialog); url->setObjectName(QStringLiteral("gitImportUrl"));
    url->setPlaceholderText(QObject::tr("https://… or /path/to/local/repository"));
    auto *location = new QWidget(&dialog);
    auto *locationLayout = new QHBoxLayout(location);
    locationLayout->setContentsMargins(0, 0, 0, 0);
    locationLayout->addWidget(url);
    auto *browse = new QPushButton(QObject::tr("Browse…"), location);
    browse->setObjectName(QStringLiteral("gitImportBrowse"));
    locationLayout->addWidget(browse);
    QObject::connect(browse, &QPushButton::clicked, &dialog, [&dialog, url] {
        const auto path = QFileDialog::getExistingDirectory(&dialog, QObject::tr("Choose local Git repository"), QDir::homePath());
        if (!path.isEmpty()) url->setText(path);
    });
    auto *branch = new QLineEdit(&dialog); branch->setObjectName(QStringLiteral("gitImportBranch"));
    branch->setPlaceholderText(QObject::tr("Default branch"));
    auto *subfolder = new QLineEdit(&dialog); subfolder->setObjectName(QStringLiteral("gitImportSubfolder"));
    subfolder->setPlaceholderText(QObject::tr("Repository root"));
    form->addRow(QObject::tr("Repository URL or local path:"), location);
    form->addRow(QObject::tr("Branch:"), branch);
    form->addRow(QObject::tr("Source subfolder (optional):"), subfolder);
    auto *localHint = new QLabel(QObject::tr("Local repositories are cloned from the selected branch. Uncommitted changes are not included."), &dialog);
    localHint->setWordWrap(true); form->addRow(localHint);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QObject::tr("Clone"));
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false); form->addRow(buttons);
    QObject::connect(url, &QLineEdit::textChanged, &dialog, [buttons](const QString &text) { buttons->button(QDialogButtonBox::Ok)->setEnabled(!text.trimmed().isEmpty()); });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return {};
    const auto relative = QDir::cleanPath(subfolder->text().trimmed());
    if (QDir::isAbsolutePath(relative) || relative == QLatin1String("..") || relative.startsWith(QLatin1String("../")) || relative.contains(QLatin1Char('\\'))) {
        QMessageBox::warning(parent, QObject::tr("Import sources"), QObject::tr("Choose a source subfolder inside the repository.")); return {};
    }
    const auto repositorySource = url->text().trimmed();
    if (QDir::isAbsolutePath(repositorySource)) {
        const QFileInfo local(repositorySource);
        if (!local.exists()) {
            QMessageBox::warning(parent, QObject::tr("Import sources"), QObject::tr("The repository path does not exist:\n%1").arg(repositorySource)); return {};
        }
        if (local.isDir() && !QFileInfo(QDir(repositorySource).filePath(QStringLiteral(".git"))).exists()
            && !(QFileInfo(QDir(repositorySource).filePath(QStringLiteral("HEAD"))).isFile()
                && QFileInfo(QDir(repositorySource).filePath(QStringLiteral("objects"))).isDir())) {
            QMessageBox::warning(parent, QObject::tr("Import sources"),
                QObject::tr("This folder is not a Git repository:\n%1\n\nChoose a Git repository to clone, or use From folder… to import ordinary files.").arg(repositorySource)); return {};
        }
    }
    if (!gitImportAddressSupported(repositorySource)) {
        QMessageBox::warning(parent, QObject::tr("Import sources"),
            QObject::tr("This is not a repository address Thoughts can clone:\n%1\n\nUse an https:// URL, an SSH address such as git@github.com:owner/repository.git, or a local folder.").arg(repositorySource)); return {};
    }
    const auto staging = QDir(docsRoot).filePath(QStringLiteral(".thoughts/git-imports"));
    if (!QDir().mkpath(staging)) return {};
    QTemporaryDir checkout(QDir(staging).filePath(QStringLiteral("checkout-XXXXXX")));
    if (!checkout.isValid()) return {};
    const auto repository = QDir(checkout.path()).filePath(QStringLiteral("repository"));
    // libgit2 clones like `git clone [--branch B] -- SOURCE repository`: every branch
    // and tag, origin set to SOURCE, the branch (or the remote's default) checked out.
    const auto outcome = cloneWithProgress(parent, {repositorySource, branch->text().trimmed(), repository, {}}, checkout.path());
    if (outcome.status != GitCloneOutcome::Cloned) {
        // A clone still waiting on the network removes the checkout itself.
        checkout.setAutoRemove(false);
        if (outcome.status == GitCloneOutcome::Failed)
            QMessageBox::warning(parent, QObject::tr("Import sources"), QObject::tr("Could not clone the repository.") + QLatin1String("\n\n") + outcome.message
                + (outcome.detail.isEmpty() ? QString() : QLatin1String("\n\n") + QObject::tr("Details: %1").arg(outcome.detail.right(2000))));
        return {};
    }
    const auto selected = QFileInfo(QDir(repository).filePath(relative)).canonicalFilePath();
    if (selected.isEmpty() || !QFileInfo(selected).isDir() || (selected != QFileInfo(repository).canonicalFilePath() && !selected.startsWith(QFileInfo(repository).canonicalFilePath() + QLatin1Char('/')))) {
        QMessageBox::warning(parent, QObject::tr("Import sources"), QObject::tr("The source subfolder does not exist inside this repository.")); return {};
    }
    auto result = prepareMarkdownFolderImport(docsRoot, parent, repository, relative == QLatin1String(".") ? QString() : relative);
    if (!result.isEmpty()) {
        const QString name = gitImportName(url->text());
        result.insert(QStringLiteral("name"), name.isEmpty() ? QObject::tr("Git repository") : name);
        checkout.setAutoRemove(false);
    }
    return result;
}

}

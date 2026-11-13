/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef THOUGHTS_COMMENTCODEXLAUNCHER_H
#define THOUGHTS_COMMENTCODEXLAUNCHER_H

#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

namespace Thoughts
{

class CommentCodexLauncher : public QObject
{
public:
    struct Options {
        QString codexExecutable;
        QString codexHome;
        std::function<bool(const QString &, const QStringList &)> startDetached;
        std::function<bool(const QString &)> focusTerminal;
        // Unset: codexBypassesApprovalsAndSandbox() decides.
        std::optional<bool> bypassApprovalsAndSandbox;
    };

    explicit CommentCodexLauncher(const QString &docsRoot, Options options = {}, QObject *parent = nullptr);

    bool openComment(const QString &pdfPath, const QString &annotationId, QString *error = nullptr);

    static QString associationKey(const QString &pdfPath, const QString &annotationId);
    // Whether Codex is started with --yolo, which turns off both its
    // approval prompts and its sandbox: it may then run any command and read
    // or write any file of this account, without asking, on the word of
    // whatever it reads in the Library. Off unless the owner of this computer
    // turns it on: BypassApprovalsAndSandbox=true in the [Codex] group of
    // Thoughts' configuration file (thoughtsrc), or THOUGHTS_CODEX_YOLO=1 in
    // the environment (0 turns it off whatever the file says).
    static bool codexBypassesApprovalsAndSandbox();
    static QString terminalScript(const QString &runtimeDirectory,
                                  const QString &docsRoot,
                                  const QString &codexHome,
                                  const QString &codexExecutable,
                                  const QString &sessionId,
                                  bool unarchiveSession,
                                  bool bypassApprovalsAndSandbox = false);

private:
    struct SessionAssociation {
        QString id;
        QString path;
        bool archived = false;
    };

    QString runtimeDirectory(const QString &pdfPath, const QString &annotationId) const;
    QString resolveCodexExecutable() const;
    QString resolveCodexHome() const;
    SessionAssociation consumeCapturedSession(const QString &runtimeDirectory, const QString &pdfPath, const QString &annotationId) const;
    SessionAssociation loadAssociation(const QString &runtimeDirectory) const;
    SessionAssociation locateAssociation(SessionAssociation association) const;
    bool saveAssociation(const QString &runtimeDirectory, const QString &pdfPath, const QString &annotationId, const SessionAssociation &association) const;
    bool launchTerminal(const QString &scriptPath) const;
    bool focusTerminalTab(const QString &tty) const;

    QString m_docsRoot;
    Options m_options;
};

}

#endif

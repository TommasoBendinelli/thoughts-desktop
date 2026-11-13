// SPDX-License-Identifier: GPL-2.0-or-later
#include "gitclonedialog.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QTimer>
#include <QUrl>

#include <limits>
#include <memory>
#include <mutex>
#include <thread>

namespace Thoughts {
namespace {

struct CloneJob {
    std::atomic_bool cancel{false};
    std::atomic_bool finished{false};
    std::mutex mutex;
    GitCloneProgress progress;
    GitCloneOutcome outcome;
};

// Runs each question on the GUI thread while the cloning thread waits. Once the
// clone is canceled no question is shown, so the dialogs' parent is never used
// after cloneWithProgress() returned.
class WidgetPrompts : public GitClonePrompts
{
public:
    WidgetPrompts(QObject *context, QWidget *parent, std::shared_ptr<CloneJob> job)
        : m_context(context), m_parent(parent), m_job(std::move(job))
    {
    }

    bool httpsCredentials(const QString &url, QString *username, QString *password) override
    {
        return onGui([&] {
            QDialog dialog(m_parent);
            dialog.setObjectName(QStringLiteral("gitSignIn"));
            dialog.setWindowTitle(QObject::tr("Sign in to clone"));
            auto *form = new QFormLayout(&dialog);
            const QUrl address(url);
            const auto host = address.host().isEmpty() ? url : address.host();
            QString text = QObject::tr("%1 asks for a sign-in to clone this repository.").arg(host);
            if (host.compare(QLatin1String("github.com"), Qt::CaseInsensitive) == 0 || host.endsWith(QLatin1String(".github.com"), Qt::CaseInsensitive))
                text += QLatin1String("\n\n") + QObject::tr("GitHub needs a fine-grained personal access token, not your password: on github.com open Settings › Developer settings › "
                                                             "Personal access tokens › Fine-grained tokens, give the token read-only access to Contents of this repository, "
                                                             "and paste it below with your GitHub user name.");
            else
                text += QLatin1String("\n\n") + QObject::tr("Enter your user name and your password or access token.");
            text += QLatin1String("\n\n") + QObject::tr("Thoughts uses them only for this clone and does not store them. If the repository is public, check the URL: "
                                                         "servers also ask for a sign-in when a repository does not exist.");
            auto *label = new QLabel(text, &dialog);
            label->setWordWrap(true);
            label->setMinimumWidth(420);
            form->addRow(label);
            auto *user = new QLineEdit(*username, &dialog);
            user->setObjectName(QStringLiteral("gitSignInUser"));
            auto *secret = new QLineEdit(&dialog);
            secret->setObjectName(QStringLiteral("gitSignInPassword"));
            secret->setEchoMode(QLineEdit::Password);
            form->addRow(QObject::tr("User name:"), user);
            form->addRow(QObject::tr("Password or token:"), secret);
            auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
            auto *ok = buttons->button(QDialogButtonBox::Ok);
            ok->setText(QObject::tr("Sign In"));
            const auto update = [ok, user, secret] { ok->setEnabled(!user->text().isEmpty() && !secret->text().isEmpty()); };
            QObject::connect(user, &QLineEdit::textChanged, &dialog, update);
            QObject::connect(secret, &QLineEdit::textChanged, &dialog, update);
            update();
            QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
            QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            form->addRow(buttons);
            (user->text().isEmpty() ? user : secret)->setFocus();
            if (dialog.exec() != QDialog::Accepted) return false;
            *username = user->text();
            *password = secret->text();
            secret->clear();
            return true;
        });
    }

    bool keyPassphrase(const QString &keyPath, bool retry, QString *passphrase) override
    {
        return onGui([&] {
            QInputDialog dialog(m_parent);
            dialog.setObjectName(QStringLiteral("gitKeyPassphrase"));
            dialog.setWindowTitle(QObject::tr("SSH key passphrase"));
            dialog.setInputMode(QInputDialog::TextInput);
            dialog.setTextEchoMode(QLineEdit::Password);
            dialog.setLabelText((retry ? QObject::tr("That passphrase did not unlock the key.") + QLatin1Char('\n') : QString())
                                + QObject::tr("Enter the passphrase of the SSH key %1.\nCancel tries the next key.").arg(keyPath));
            if (dialog.exec() != QDialog::Accepted) return false;
            *passphrase = dialog.textValue();
            return true;
        });
    }

    bool trustUnknownHost(const QString &host, const QString &keyType, const QString &fingerprint, bool otherKeyTypesKnown) override
    {
        return onGui([&] {
            QMessageBox box(QMessageBox::Warning, QObject::tr("Unknown SSH host"),
                            QObject::tr("The authenticity of host “%1” can’t be established.").arg(host), QMessageBox::NoButton, m_parent);
            box.setObjectName(QStringLiteral("gitTrustHost"));
            box.setInformativeText(QObject::tr("Its %1 key fingerprint is\n%2\n\n").arg(keyType, fingerprint)
                                   + (otherKeyTypesKnown ? QObject::tr("Your known_hosts file has keys of other types for this host, but not this one.\n\n") : QString())
                                   + QObject::tr("Compare the fingerprint with the one your Git host publishes (for GitHub: docs.github.com, "
                                                 "“GitHub's SSH key fingerprints”) before you trust it. Trusting adds the key to ~/.ssh/known_hosts."));
            auto *trust = box.addButton(QObject::tr("Trust and Connect"), QMessageBox::AcceptRole);
            box.setDefaultButton(box.addButton(QMessageBox::Cancel));
            box.exec();
            return box.clickedButton() == trust;
        });
    }

private:
    template<typename Question>
    bool onGui(Question question)
    {
        if (m_job->cancel.load()) return false;
        bool answer = false;
        QMetaObject::invokeMethod(m_context, [&] { answer = !m_job->cancel.load() && question(); }, Qt::BlockingQueuedConnection);
        return answer && !m_job->cancel.load();
    }

    QObject *m_context;
    QWidget *m_parent;
    std::shared_ptr<CloneJob> m_job;
};

QString progressText(const GitCloneProgress &progress)
{
    if (progress.filesToCheckOut)
        return QObject::tr("Checking out files: %1 of %2").arg(progress.checkedOutFiles).arg(progress.filesToCheckOut);
    if (progress.totalDeltas && progress.receivedObjects == progress.totalObjects)
        return QObject::tr("Resolving deltas: %1 of %2").arg(progress.indexedDeltas).arg(progress.totalDeltas);
    if (progress.totalObjects)
        return QObject::tr("Receiving objects: %1 of %2 (%3)")
            .arg(progress.receivedObjects)
            .arg(progress.totalObjects)
            .arg(QLocale().formattedDataSize(qint64(progress.receivedBytes)));
    return QObject::tr("Cloning repository…");
}

}

GitCloneOutcome cloneWithProgress(QWidget *parent, const GitCloneRequest &request, const QString &workDirectory)
{
    auto job = std::make_shared<CloneJob>();
    QProgressDialog progress(QObject::tr("Cloning repository…"), QObject::tr("Cancel"), 0, 0, parent);
    progress.setObjectName(QStringLiteral("gitCloneProgress"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setAutoClose(false);
    progress.setAutoReset(false);
    // Questions run in the GUI thread through this object; the cloning thread deletes it.
    auto *context = new QObject;
    auto prompts = std::make_shared<WidgetPrompts>(context, &progress, job);
    std::thread([job, prompts, context, request, workDirectory] {
        auto outcome = gitClone(request, prompts.get(), &job->cancel, [&job](const GitCloneProgress &now) {
            const std::lock_guard lock(job->mutex);
            job->progress = now;
        });
        if (outcome.status == GitCloneOutcome::Cloned && job->cancel.load()) outcome = {GitCloneOutcome::Canceled, {}, {}};
        if (outcome.status != GitCloneOutcome::Cloned) QDir(workDirectory).removeRecursively();
        {
            const std::lock_guard lock(job->mutex);
            job->outcome = outcome;
        }
        job->finished.store(true);
        context->deleteLater();
    }).detach();

    QEventLoop loop;
    QElapsedTimer sinceCancel;
    QObject::connect(&progress, &QProgressDialog::canceled, &loop, [&] {
        job->cancel.store(true);
        sinceCancel.start();
    });
    QTimer poll;
    poll.setInterval(50);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (progress.wasCanceled() && !sinceCancel.isValid()) {
            job->cancel.store(true);
            sinceCancel.start();
        }
        // A clone waiting for a silent server sees Cancel only when the
        // connection times out; stop waiting for it after two seconds.
        if (job->finished.load() || (sinceCancel.isValid() && sinceCancel.elapsed() > 2000)) {
            loop.quit();
            return;
        }
        GitCloneProgress now;
        {
            const std::lock_guard lock(job->mutex);
            now = job->progress;
        }
        const quint64 total = now.filesToCheckOut ? now.filesToCheckOut : now.totalObjects;
        const quint64 done = now.filesToCheckOut ? now.checkedOutFiles : now.receivedObjects;
        if (!progress.wasCanceled()) {
            progress.setLabelText(progressText(now));
            if (total > 0 && total < quint64(std::numeric_limits<int>::max())) {
                progress.setMaximum(int(total));
                progress.setValue(int(done));
            }
        }
    });
    poll.start();
    progress.show();
    loop.exec();
    poll.stop();
    const bool canceled = progress.wasCanceled() || job->cancel.load();
    progress.close();
    if (!job->finished.load()) return {GitCloneOutcome::Canceled, {}, {}};
    GitCloneOutcome outcome;
    {
        const std::lock_guard lock(job->mutex);
        outcome = job->outcome;
    }
    if (canceled && outcome.status == GitCloneOutcome::Cloned) {
        QDir(workDirectory).removeRecursively();
        return {GitCloneOutcome::Canceled, {}, {}};
    }
    return outcome;
}

}

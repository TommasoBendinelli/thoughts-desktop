// SPDX-License-Identifier: GPL-2.0-or-later
#include "dlgreadaloud.h"

#include "readaloud.h"

#include <KColorScheme>
#include <KLocalizedString>

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QPushButton>
#include <QVBoxLayout>

using namespace Thoughts::ReadAloud;

namespace
{
const QString testSentence = QStringLiteral("This is the voice Thoughts uses to read your selection aloud.");
}

DlgReadAloud::DlgReadAloud(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto *intro = new QLabel(i18n("Read Aloud (select text, then ⇧⌘U or the right-click menu) speaks with a Fish Audio voice through OpenRouter. "
                                  "Thoughts sends the selected text and this key only to openrouter.ai; the Thoughts server is not involved. "
                                  "The key is kept in this Mac's Keychain. Without a key, or when the cloud voice fails, the system voice reads instead."),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *link = new QLabel(i18n("Create a key at <a href=\"https://openrouter.ai/settings/keys\">openrouter.ai/settings/keys</a>."), this);
    link->setTextFormat(Qt::RichText);
    link->setOpenExternalLinks(true);
    layout->addWidget(link);

    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_saved = new QLabel(this);
    m_saved->setObjectName(QStringLiteral("readAloudSavedKey"));
    m_saved->setTextFormat(Qt::PlainText);
    form->addRow(i18n("Saved key:"), m_saved);

    auto *keyRow = new QHBoxLayout;
    m_key = new QLineEdit(this);
    m_key->setObjectName(QStringLiteral("readAloudKey"));
    m_key->setEchoMode(QLineEdit::Password);
    m_key->setPlaceholderText(QStringLiteral("sk-or-v1-…"));
    m_key->setAccessibleName(i18n("OpenRouter key"));
    // Nothing of the key in undo history, drag and drop or the clipboard.
    m_key->setDragEnabled(false);
    m_key->setContextMenuPolicy(Qt::NoContextMenu);
    m_key->setMinimumWidth(320);
    keyRow->addWidget(m_key, 1);
    m_save = new QPushButton(i18n("Save"), this);
    m_save->setObjectName(QStringLiteral("readAloudKeySave"));
    m_save->setAutoDefault(false);
    m_save->setEnabled(false);
    keyRow->addWidget(m_save);
    m_remove = new QPushButton(i18n("Remove"), this);
    m_remove->setObjectName(QStringLiteral("readAloudKeyRemove"));
    m_remove->setAutoDefault(false);
    keyRow->addWidget(m_remove);
    form->addRow(i18n("OpenRouter key:"), keyRow);
    layout->addLayout(form);

    auto *testRow = new QHBoxLayout;
    m_test = new QPushButton(i18n("Test voice"), this);
    m_test->setObjectName(QStringLiteral("readAloudTestVoice"));
    m_test->setAutoDefault(false);
    testRow->addWidget(m_test);
    testRow->addStretch(1);
    layout->addLayout(testRow);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("readAloudStatus"));
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_status);
    layout->addStretch(1);

    connect(m_key, &QLineEdit::textChanged, this, [this](const QString &text) { m_save->setEnabled(!text.trimmed().isEmpty()); });
    connect(m_key, &QLineEdit::returnPressed, this, &DlgReadAloud::saveKey);
    connect(m_save, &QPushButton::clicked, this, &DlgReadAloud::saveKey);
    connect(m_remove, &QPushButton::clicked, this, &DlgReadAloud::removeKey);
    connect(m_test, &QPushButton::clicked, this, &DlgReadAloud::testVoice);

    showSavedKey();
}

DlgReadAloud::~DlgReadAloud()
{
    if (m_client) {
        m_client->cancel();
    }
    if (m_output) {
        m_output->stop();
    }
}

void DlgReadAloud::showSavedKey()
{
    const QByteArray key = keyStore()->read();
    m_saved->setText(key.isEmpty() ? i18n("None — Read Aloud uses the system voice.") : maskedKey(key));
    m_remove->setEnabled(!key.isEmpty());
}

void DlgReadAloud::setStatus(const QString &text, bool problem)
{
    m_status->setText(text);
    QPalette palette = m_status->parentWidget()->palette();
    if (problem) {
        palette.setBrush(QPalette::WindowText, KColorScheme(QPalette::Active, KColorScheme::Window).foreground(KColorScheme::NegativeText));
    }
    m_status->setPalette(palette);
}

void DlgReadAloud::saveKey()
{
    QString error;
    const QByteArray key = normalizedKey(m_key->text(), &error);
    if (key.isEmpty()) {
        setStatus(error, true);
        return;
    }
    if (!keyStore()->write(key, &error)) {
        setStatus(i18n("The Keychain did not save the key: %1", error), true);
        return;
    }
    m_key->clear();
    showSavedKey();
    setStatus(i18n("Saved in the Keychain. Read Aloud uses it from the next reading; Test voice checks it."));
}

void DlgReadAloud::removeKey()
{
    QString error;
    if (!keyStore()->remove(&error)) {
        setStatus(i18n("The Keychain did not remove the key: %1", error), true);
        return;
    }
    showSavedKey();
    setStatus(i18n("Removed. Read Aloud uses the system voice."));
}

void DlgReadAloud::testVoice()
{
    QByteArray key;
    if (!m_key->text().trimmed().isEmpty()) {
        QString error;
        key = normalizedKey(m_key->text(), &error);
        if (key.isEmpty()) {
            setStatus(error, true);
            return;
        }
    } else {
        key = keyStore()->read();
        if (key.isEmpty()) {
            setStatus(i18n("Paste a key first, or save one."), true);
            return;
        }
    }
    if (m_output) {
        m_output->stop();
    }
    m_client = std::make_unique<SpeechClient>();
    m_client->setKey(key);
    m_test->setEnabled(false);
    setStatus(i18n("Asking OpenRouter for the voice…"));
    m_client->synthesize(testSentence, [this](const SpeechClient::Result &result) {
        m_test->setEnabled(true);
        if (!result.error.isEmpty()) {
            QString why;
            if (result.httpStatus == 401 || result.httpStatus == 403) {
                why = i18n("OpenRouter refused the key.");
            } else if (result.httpStatus == 402) {
                why = i18n("The OpenRouter account has no credit for this model.");
            } else if (result.httpStatus == 429) {
                why = i18n("OpenRouter's rate limit was reached; try again in a while.");
            } else if (result.httpStatus == 0) {
                why = i18n("OpenRouter could not be reached.");
            } else {
                why = i18n("The voice could not be made.");
            }
            setStatus(result.message.isEmpty() ? why : i18nc("reason, then details", "%1\n%2", why, result.message), true);
            return;
        }
        if (!m_output) {
            m_output = createAudioOutput();
            if (m_output) {
                connect(m_output.get(), &AudioOutput::failed, this, [this](const QString &reason) {
                    setStatus(i18n("OpenRouter answered, but the audio could not be played (%1).", reason), true);
                });
            }
        }
        if (!m_output) {
            setStatus(i18n("The key works (%1), but this build of Thoughts cannot play audio.", result.model));
            return;
        }
        setStatus(i18n("The key works: playing a sentence from %1.", result.model));
        m_output->enqueue(result.audio);
    });
}

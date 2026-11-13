// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef DLGREADALOUD_H
#define DLGREADALOUD_H

#include <QWidget>

#include <memory>

class QLabel;
class QLineEdit;
class QPushButton;

namespace Thoughts::ReadAloud
{
class AudioOutput;
class SpeechClient;
}

// Settings > Read Aloud: the OpenRouter key the cloud voice uses. Save and
// Remove act at once on the macOS Keychain (not through OK/Apply); the key is
// never shown again, only its first and last characters. Test voice reads a
// sentence with the typed key, or else the saved one.
class DlgReadAloud : public QWidget
{
    Q_OBJECT

public:
    explicit DlgReadAloud(QWidget *parent = nullptr);
    ~DlgReadAloud() override;

private:
    void showSavedKey();
    void saveKey();
    void removeKey();
    void testVoice();
    void setStatus(const QString &text, bool problem = false);

    QLineEdit *m_key;
    QLabel *m_saved;
    QLabel *m_status;
    QPushButton *m_save;
    QPushButton *m_remove;
    QPushButton *m_test;
    std::unique_ptr<Thoughts::ReadAloud::SpeechClient> m_client;
    std::unique_ptr<Thoughts::ReadAloud::AudioOutput> m_output;
};

#endif

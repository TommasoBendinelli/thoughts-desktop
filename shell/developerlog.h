// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <memory>

namespace Thoughts
{
// Accept only developer-defined metadata, never document text, URLs or credentials.
class DeveloperLog : public QObject
{
public:
    struct Options {
        QString directory;
        qint64 fileBytes = 2 * 1024 * 1024;
        int fileCount = 10;
        int heartbeatMs = 250;
        int stallMs = 3000;
#ifdef Q_OS_MACOS
        bool captureStacks = true;
#else
        bool captureStacks = false;
#endif
        QString samplerProgram = QStringLiteral("/usr/bin/sample");
        int captureCooldownMs = 30000;
        int captureTimeoutMs = 15000;
    };
    explicit DeveloperLog(const Options &options, QObject *parent = nullptr);
    ~DeveloperLog() override;
    static Options defaultOptions();
    static void record(const QString &event, const QJsonObject &metadata = {}, bool verboseOnly = false);
    static QString directory();
    static QString currentFile();
    static QString diagnostics();
    static QJsonArray performanceEvents();
    static QString latestFreezeReport();
    static QString diagnosticsWithFreezeReport();
    static bool available();
    static bool verbose();
    static void setVerbose(bool enabled);
    static QString documentId(const QString &identity);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Private;
    std::unique_ptr<Private> d;
};

class DeveloperActivity
{
public:
    explicit DeveloperActivity(const QString &name, const QJsonObject &metadata = {});
    ~DeveloperActivity();
    // Adds a field to the .finished event, e.g. what the activity found.
    void add(const QString &key, const QJsonValue &value) { m_metadata.insert(key, value); }
private:
    QString m_name;
    QJsonObject m_metadata;
    QElapsedTimer m_elapsed;
};
}

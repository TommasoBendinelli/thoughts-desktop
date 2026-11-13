// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QHash>
#include <QJsonObject>
#include <QString>

namespace Thoughts
{

class SyncTransferLedger
{
public:
    enum class Direction {
        Upload,
        Download,
    };

    void reset();
    bool planItem(Direction direction, const QString &itemId, qint64 plannedBytes);
    bool updateItem(Direction direction, const QString &itemId, qint64 completedBytes);
    bool commitItem(Direction direction, const QString &itemId, qint64 completedBytes = -1);
    bool rollbackItem(Direction direction, const QString &itemId);
    bool updateUnplannedItem(Direction direction, const QString &itemId, qint64 completedBytes);
    bool commitUnplannedItem(Direction direction, const QString &itemId, qint64 completedBytes);

    // Replacing a reader plan drops only unfinished work from the previous
    // plan. Verified bytes remain part of the operation's honest transfer
    // total and duplicate item identities remain idempotent.
    bool replaceDownloadPlan(const QString &planId, const QHash<QString, qint64> &plannedItems);

    QJsonObject snapshot(bool flush = false) const;

private:
    struct Item {
        qint64 plannedBytes = 0;
        qint64 inFlightBytes = 0;
        qint64 committedBytes = 0;
        bool committed = false;
        QString planId;
    };

    using Items = QHash<QString, Item>;
    Items &items(Direction direction);
    const Items &items(Direction direction) const;

    Items m_uploads;
    Items m_downloads;
    QString m_downloadPlanId;
};

}

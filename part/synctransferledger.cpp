// SPDX-License-Identifier: GPL-2.0-or-later
#include "synctransferledger.h"

#include <limits>

namespace Thoughts
{

namespace
{

bool validItem(const QString &itemId, qint64 bytes)
{
    return !itemId.trimmed().isEmpty() && bytes >= 0;
}

bool safeAdd(qint64 left, qint64 right, qint64 *result)
{
    if (!result || left < 0 || right < 0 || right > std::numeric_limits<qint64>::max() - left) {
        return false;
    }
    *result = left + right;
    return true;
}

}

void SyncTransferLedger::reset()
{
    m_uploads.clear();
    m_downloads.clear();
    m_downloadPlanId.clear();
}

bool SyncTransferLedger::planItem(Direction direction, const QString &itemId, qint64 plannedBytes)
{
    if (!validItem(itemId, plannedBytes)) {
        return false;
    }
    Items &values = items(direction);
    const auto existing = values.constFind(itemId);
    if (existing != values.cend()) {
        return existing->plannedBytes == plannedBytes;
    }
    Item item;
    item.plannedBytes = plannedBytes;
    if (direction == Direction::Download) {
        item.planId = m_downloadPlanId;
    }
    values.insert(itemId, item);
    return true;
}

bool SyncTransferLedger::updateItem(Direction direction, const QString &itemId, qint64 completedBytes)
{
    Items &values = items(direction);
    auto item = values.find(itemId);
    if (item == values.end() || item->committed || completedBytes < 0 || completedBytes > item->plannedBytes) {
        return false;
    }
    item->inFlightBytes = completedBytes;
    return true;
}

bool SyncTransferLedger::commitItem(Direction direction, const QString &itemId, qint64 completedBytes)
{
    Items &values = items(direction);
    auto item = values.find(itemId);
    if (item == values.end()) {
        return false;
    }
    const qint64 resolved = completedBytes < 0 ? item->plannedBytes : completedBytes;
    if (resolved != item->plannedBytes) {
        return false;
    }
    if (item->committed) {
        return item->committedBytes == resolved;
    }
    item->inFlightBytes = 0;
    item->committedBytes = resolved;
    item->committed = true;
    return true;
}

bool SyncTransferLedger::rollbackItem(Direction direction, const QString &itemId)
{
    Items &values = items(direction);
    auto item = values.find(itemId);
    if (item == values.end() || item->committed) {
        return false;
    }
    item->inFlightBytes = 0;
    return true;
}

bool SyncTransferLedger::updateUnplannedItem(Direction direction, const QString &itemId, qint64 completedBytes)
{
    if (!validItem(itemId, completedBytes)) {
        return false;
    }
    Items &values = items(direction);
    auto item = values.find(itemId);
    if (item == values.end()) {
        Item pending;
        pending.plannedBytes = completedBytes;
        pending.inFlightBytes = completedBytes;
        values.insert(itemId, pending);
        return true;
    }
    if (item->committed) {
        return item->committedBytes == completedBytes;
    }
    if (!item->planId.isEmpty() || completedBytes < item->inFlightBytes) {
        return false;
    }
    item->plannedBytes = completedBytes;
    item->inFlightBytes = completedBytes;
    return true;
}

bool SyncTransferLedger::commitUnplannedItem(Direction direction, const QString &itemId, qint64 completedBytes)
{
    if (!validItem(itemId, completedBytes)) {
        return false;
    }
    Items &values = items(direction);
    const auto existing = values.constFind(itemId);
    if (existing != values.cend() && existing->committed) {
        return existing->committedBytes == completedBytes;
    }
    if (!updateUnplannedItem(direction, itemId, completedBytes)) {
        return false;
    }
    return commitItem(direction, itemId, completedBytes);
}

bool SyncTransferLedger::replaceDownloadPlan(const QString &planId, const QHash<QString, qint64> &plannedItems)
{
    if (planId.trimmed().isEmpty()) {
        return false;
    }
    for (auto iterator = plannedItems.cbegin(); iterator != plannedItems.cend(); ++iterator) {
        if (!validItem(iterator.key(), iterator.value())) {
            return false;
        }
        const auto existing = m_downloads.constFind(iterator.key());
        if (existing != m_downloads.cend()
            && (existing->committed || existing->planId == planId)
            && existing->plannedBytes != iterator.value()) {
            return false;
        }
    }
    for (auto iterator = m_downloads.begin(); iterator != m_downloads.end();) {
        if (!iterator->committed && iterator->planId != planId) {
            iterator = m_downloads.erase(iterator);
        } else {
            ++iterator;
        }
    }
    m_downloadPlanId = planId;
    for (auto iterator = plannedItems.cbegin(); iterator != plannedItems.cend(); ++iterator) {
        auto existing = m_downloads.find(iterator.key());
        if (existing == m_downloads.end()) {
            Item item;
            item.plannedBytes = iterator.value();
            item.planId = planId;
            m_downloads.insert(iterator.key(), item);
        } else if (existing->plannedBytes != iterator.value()) {
            return false;
        } else if (!existing->committed) {
            existing->planId = planId;
            existing->inFlightBytes = 0;
        }
    }
    return true;
}

QJsonObject SyncTransferLedger::snapshot(bool flush) const
{
    const auto totals = [](const Items &values, qint64 *completed, qint64 *planned) {
        *completed = 0;
        *planned = 0;
        for (auto iterator = values.cbegin(); iterator != values.cend(); ++iterator) {
            const qint64 visible = iterator->committed ? iterator->committedBytes : iterator->inFlightBytes;
            if (!safeAdd(*completed, visible, completed) || !safeAdd(*planned, iterator->plannedBytes, planned)) {
                *completed = 0;
                *planned = 0;
                return false;
            }
        }
        return true;
    };
    qint64 uploadedCompleted = 0;
    qint64 uploadedTotal = 0;
    qint64 downloadedCompleted = 0;
    qint64 downloadedTotal = 0;
    if (!totals(m_uploads, &uploadedCompleted, &uploadedTotal)
        || !totals(m_downloads, &downloadedCompleted, &downloadedTotal)) {
        return {};
    }
    return {{QStringLiteral("schema_version"), 1},
            {QStringLiteral("transfer_metrics_recorded"), true},
            {QStringLiteral("uploaded_completed_bytes"), uploadedCompleted},
            {QStringLiteral("uploaded_total_bytes"), uploadedTotal},
            {QStringLiteral("downloaded_completed_bytes"), downloadedCompleted},
            {QStringLiteral("downloaded_total_bytes"), downloadedTotal},
            {QStringLiteral("flush"), flush}};
}

SyncTransferLedger::Items &SyncTransferLedger::items(Direction direction)
{
    return direction == Direction::Upload ? m_uploads : m_downloads;
}

const SyncTransferLedger::Items &SyncTransferLedger::items(Direction direction) const
{
    return direction == Direction::Upload ? m_uploads : m_downloads;
}

}

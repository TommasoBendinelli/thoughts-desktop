/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef THOUGHTS_REVIEWINTERVAL_H
#define THOUGHTS_REVIEWINTERVAL_H

#include <QDateTime>
#include <QTimeZone>
#include <QRandomGenerator>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace Thoughts
{

struct ReviewInterval {
    enum Unit { Minutes, Hours, Days, Weeks, Months };
    double amount = 2;
    Unit unit = Weeks;
    QDate date;

    QDateTime dueAt(const QDateTime &now, const QTimeZone &zone = QTimeZone::systemTimeZone()) const
    {
        if (!now.isValid() || !std::isfinite(amount) || amount <= 0 || amount > 1000000000) return {};
        const QDateTime local = now.toTimeZone(zone);
        QDateTime due;
        if (date.isValid()) {
            due = QDateTime(date, local.time(), zone);
        } else if (unit == Minutes || unit == Hours) {
            due = now.addMSecs(qRound64(amount * (unit == Minutes ? 60000.0 : 3600000.0)));
        } else if (unit == Months) {
            const int whole = int(amount);
            const QDateTime base = local.addMonths(whole);
            const QDateTime next = local.addMonths(whole + 1);
            if (!base.isValid() || !next.isValid()) return {};
            due = base.addMSecs(qRound64((amount - whole) * base.msecsTo(next)));
        } else {
            const double days = amount * (unit == Weeks ? 7 : 1);
            const qint64 whole = qint64(days);
            due = local.addDays(whole).addMSecs(qRound64((days - whole) * 86400000.0));
        }
        // Match Android's supported date controls, and reject overflow/past dates.
        return due.isValid() && due > now && due.date().year() <= 9999 ? due.toUTC() : QDateTime{};
    }

    // Twice the last interval, in the largest unit that holds it exactly; two weeks without one.
    static ReviewInterval suggested(const QDateTime &reviewedAt, const QDateTime &dueAt,
                                    const QTimeZone &zone = QTimeZone::systemTimeZone())
    {
        if (!reviewedAt.isValid() || !dueAt.isValid() || dueAt <= reviewedAt) return {};
        return scaled(reviewedAt, dueAt, 2.0, zone);
    }

    // The last interval itself, as the phone's "Current" line shows it; amount 0 without one.
    static ReviewInterval between(const QDateTime &reviewedAt, const QDateTime &dueAt,
                                  const QTimeZone &zone = QTimeZone::systemTimeZone())
    {
        if (!reviewedAt.isValid() || !dueAt.isValid() || dueAt <= reviewedAt) return {0, Days, {}};
        return scaled(reviewedAt, dueAt, 1.0, zone);
    }

    // A length of time as a person says it: from two days on in whole days, or weeks when it is a
    // whole number of them, so 838 hours is 5 weeks; below that as exact as it is. The phone does
    // the same.
    static ReviewInterval readable(double millis)
    {
        constexpr double day = 86400000.0;
        if (millis >= 2 * day) {
            const double days = std::round(millis / day);
            if (std::fmod(days, 7.0) == 0) return {days / 7, Weeks, {}};
            return {days, Days, {}};
        }
        if (millis == day) return {1, Days, {}};
        for (const auto &candidate : {qMakePair(Hours, 3600000.0), qMakePair(Minutes, 60000.0)}) {
            const double value = millis / candidate.second;
            if (value >= 1 && std::abs(value * 1000 - std::round(value * 1000)) < 0.000001) {
                return {value, candidate.first, {}};
            }
        }
        return {millis / 60000.0, Minutes, {}};
    }

private:
    static ReviewInterval scaled(const QDateTime &reviewedAt, const QDateTime &dueAt, double factor, const QTimeZone &zone)
    {
        const auto start = reviewedAt.toTimeZone(zone);
        const auto end = dueAt.toTimeZone(zone);
        const auto calendarMillis = QDateTime(start.date(), start.time(), QTimeZone::UTC)
                                        .msecsTo(QDateTime(end.date(), end.time(), QTimeZone::UTC));
        return readable(factor * (calendarMillis >= 86400000 ? calendarMillis : reviewedAt.msecsTo(dueAt)));
    }

};

// Skip's Random: 2 days plus an exponential delay whose rate puts 95% of postponements within
// 14 days (P(delay <= 14) = 0.95). Same formula on the phone. @p u is uniform in [0, 1).
inline double randomSkipDelayDays(double u)
{
    if (!(u >= 0 && u < 1)) u = 0;
    const double rate = std::log(20.0) / 12.0;
    return 2.0 + -std::log(1.0 - u) / rate;
}

// The source of u for Skip's Random; tests replace it with a fixed value.
inline std::function<double()> &randomSkipUniformSource()
{
    static std::function<double()> source = [] { return QRandomGenerator::global()->generateDouble(); };
    return source;
}

// The Skip dialog's interval for Random: randomSkipDelayDays(u) rounded to the nearest whole
// day, at least 2. Same rule on the phone.
inline int randomSkipDays(double u)
{
    return std::max(2, int(std::lround(randomSkipDelayDays(u))));
}

}
#endif

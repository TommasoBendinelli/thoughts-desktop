// SPDX-License-Identifier: GPL-2.0-or-later
#include "sha256.h"

#include <QCryptographicHash>

#if defined(Q_OS_DARWIN)
#include <CommonCrypto/CommonDigest.h>
#elif defined(Q_OS_WIN)
#include <windows.h>
#include <bcrypt.h>
#endif

namespace Thoughts::Publisher
{

QString sha256Hex(QByteArrayView data)
{
    QByteArray digest(32, Qt::Uninitialized);
#if defined(Q_OS_DARWIN)
    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
    const char *bytes = data.data();
    qsizetype remaining = data.size();
    // CC_LONG is 32 bits.
    while (remaining > 0) {
        const CC_LONG part = CC_LONG(qMin<qsizetype>(remaining, 1 << 30));
        CC_SHA256_Update(&context, bytes, part);
        bytes += part;
        remaining -= part;
    }
    CC_SHA256_Final(reinterpret_cast<unsigned char *>(digest.data()), &context);
#elif defined(Q_OS_WIN)
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool done = BCRYPT_SUCCESS(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0));
    const char *bytes = data.data();
    qsizetype remaining = data.size();
    while (done && remaining > 0) {
        const ULONG part = ULONG(qMin<qsizetype>(remaining, 1 << 30));
        done = BCRYPT_SUCCESS(BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char *>(bytes)), part, 0));
        bytes += part;
        remaining -= part;
    }
    done = done && BCRYPT_SUCCESS(BCryptFinishHash(hash, reinterpret_cast<PUCHAR>(digest.data()), ULONG(digest.size()), 0));
    if (hash) {
        BCryptDestroyHash(hash);
    }
    if (!done) {
        digest = QCryptographicHash::hash(data, QCryptographicHash::Sha256);
    }
#else
    digest = QCryptographicHash::hash(data, QCryptographicHash::Sha256);
#endif
    return QString::fromLatin1(digest.toHex());
}

}

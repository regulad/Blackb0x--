/*
 * SecTrustStorePrivate.h -- hand-declared prototypes for Security.framework's
 * trust-store API.
 *
 * WHY THIS FILE EXISTS. Every symbol below is REAL and EXPORTED by the
 * Security.framework this project targets -- verified directly, not assumed:
 *
 *     $ nm -gU .../iPhoneOS8.4.sdk/System/Library/Frameworks/Security.framework/Security
 *     ...
 *     T _SecTrustStoreContains
 *     T _SecTrustStoreForDomain
 *     T _SecTrustStoreGetSettingsVersionNumber
 *     T _SecTrustStoreRemoveCertificate
 *     T _SecTrustStoreSetTrustSettings
 *
 * and the same five appear in the real device's own dyld shared cache
 * (dyld_shared_cache_armv7 out of the decrypted AppleTV3,2 12H1006 rootfs).
 * So they link and they exist at runtime.
 *
 * What does NOT exist is a public declaration. None of the 14 headers in that
 * SDK's Security.framework/Headers mentions "TrustStore" at all (checked every
 * one). This is a semi-private API in this era exactly as it is today -- the
 * mechanism a Configuration Profile uses to add a trusted root without
 * touching the compiled-in system trust store -- so the prototypes have to be
 * written out by hand.
 *
 * THE SIGNATURES BELOW ARE APPLE'S OWN, taken verbatim from
 * OSX/sec/Security/SecTrustStore.h in Security-57337.20.44 -- the closest
 * published release to the Security-57317.40.2 this firmware's own
 * /usr/libexec/securityd reports itself as. An earlier version of this file
 * wrote them from memory and got two of them wrong; both errors were caught
 * on real hardware, which is worth recording because it is exactly what the
 * verification in cainjector.c exists for:
 *
 *   1. SecTrustStoreContains was declared as
 *        OSStatus SecTrustStoreContains(ref, cert, Boolean *contains)
 *      It is actually
 *        Boolean  SecTrustStoreContains(ref, cert)
 *      -- two arguments, returning the answer directly. The device reported
 *      "NOT present after install (Contains -> 1)" for all 121 certificates:
 *      the 1 WAS the true return value, and the out-param that was being
 *      checked instead never got written at all. The certificates had
 *      installed correctly the whole time.
 *
 *   2. The domain enum was declared User=1, Admin=2, System=3. There is no
 *      Admin domain in this era at all, and the real values are the other way
 *      round: System=1, User=2. The code was passing 2 and calling it "admin"
 *      while actually selecting the user store. That was harmless -- the user
 *      store is the only WRITABLE one, which is what Apple's own header means
 *      by "Only allowed for writeble trust stores" -- but it was right by
 *      accident, and a comment claiming otherwise is worse than no comment.
 *
 * What both errors have in common: they were invisible to the compiler and to
 * the linker, and would have been invisible at runtime too if this tool
 * trusted its own writes. It does not, and that is why the first real run
 * produced a specific, diagnosable number instead of a cheerful "done".
 */

#ifndef BLACKB0X_SECTRUSTSTORE_PRIVATE_H
#define BLACKB0X_SECTRUSTSTORE_PRIVATE_H

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct __SecTrustStore *SecTrustStoreRef;

/* Exactly the two domains this era defines, with Apple's own values.
 * System (1) is read-only; User (2) is the writable one and therefore the
 * only possible target for SecTrustStoreSetTrustSettings. On a device with a
 * single real user that distinction costs nothing -- securityd keeps this
 * store at /Library/Keychains/TrustStore.sqlite3, not per-home. */
typedef uint32_t SecTrustStoreDomain;
enum {
    kSecTrustStoreDomainSystem = 1,
    kSecTrustStoreDomainUser   = 2
};

SecTrustStoreRef SecTrustStoreForDomain(SecTrustStoreDomain domain);

/* trustSettingsDictOrArray == NULL means "default full trust", the same
 * convention SecTrustSettingsSetTrustSettings uses on the desktop. That is
 * what a root anchor wants; per-policy restrictions would go here instead.
 *
 * Requires the `modify-anchor-certificates` entitlement -- without it every
 * call returns -34018 (errSecMissingEntitlement). See entitlements.plist. */
OSStatus SecTrustStoreSetTrustSettings(SecTrustStoreRef ts,
                                       SecCertificateRef certificate,
                                       CFTypeRef trustSettingsDictOrArray);

/* Returns the answer directly. Needs no entitlement -- reads were working
 * fine on hardware while every write was being refused. */
Boolean SecTrustStoreContains(SecTrustStoreRef ts,
                              SecCertificateRef certificate);

#ifdef __cplusplus
}
#endif

#endif /* BLACKB0X_SECTRUSTSTORE_PRIVATE_H */

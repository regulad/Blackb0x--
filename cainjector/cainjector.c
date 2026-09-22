/*
 * cainjector -- install a modern CA trust store onto an Apple TV whose own
 * one is a decade out of date.
 *
 * THE PROBLEM. `apt-get update` against any https:// source fails on this
 * hardware. The transport itself is fine: apt7-lib's /usr/lib/apt/methods/http
 * (which /usr/lib/apt/methods/https is a symlink to) is built on CFNetwork --
 * CFReadStreamCreateForHTTPRequest, and it imports kCFStreamErrorDomainSSL to
 * report real handshake errors -- so it dispatches on the scheme it is handed
 * at runtime and does a genuine TLS handshake through Apple's own
 * SecureTransport. It does not link libssl at all, so the ancient
 * openssl_0.9.8zg this project also ships is irrelevant to it.
 *
 * What is NOT fine is trust. This OS's built-in root store is compiled into
 * Security.framework itself (certsTable.data + certsIndex.data inside the
 * Apple-signed framework bundle; there is no SystemRootCertificates.keychain
 * on this OS at all -- /System/Library/Keychains is empty). It was frozen when
 * the firmware shipped, so roots that postdate it are simply absent. Measured,
 * for the one repo that actually needs it:
 *
 *     apt.awkwardtv.org
 *       leaf  CN=awkwardtv.org
 *         <-  C=US, O=Let's Encrypt, CN=YR2
 *         <-  C=US, O=ISRG, CN=Root YR
 *         <-  C=US, O=Internet Security Research Group, CN=ISRG Root X1
 *
 * ISRG Root X1 is from 2015. This firmware is from before that. Nothing in
 * that chain can validate.
 *
 * THE FIX, AND WHY IT IS NOT "EDIT THE TRUST STORE". certsTable.data is an
 * undocumented binary format inside a signed bundle, and rewriting it would
 * put every TLS consumer on the device at risk to fix one of them. Apple
 * already ships the supported alternative: SecTrustStoreSetTrustSettings(),
 * the same call a Configuration Profile's certificate payload goes through.
 * It is purely additive, it touches no Apple-signed file, and the five
 * SecTrustStore* entry points are confirmed exported both by the pinned
 * iPhoneOS 8.4 SDK's Security.framework stub and by the real device's own dyld
 * shared cache. See SecTrustStorePrivate.h for why the prototypes are
 * hand-written and what is done about that being a risk.
 *
 * WHAT IT INSTALLS. The whole Debian ca-certificates store (121 Mozilla
 * roots), not just the one chain awkwardtv happens to need -- see
 * cainjector/README.md for provenance. Certificates are staged as raw DER,
 * one file per certificate, so this program never parses PEM: the split and
 * the base64 decode both happen on the build host, where there is a real
 * openssl, rather than here, where there is not.
 *
 * WHAT IT DOES NOT FIX. TLS *protocol* support. Anything that links the
 * userland openssl_0.9.8zg package -- curl, openssh -- tops out at TLS 1.0
 * because the 0.9.8 branch never implemented anything newer, and no amount of
 * trust-store work moves a protocol ceiling. This tool buys trust, and only
 * for the CFNetwork/SecureTransport consumers that were already capable of a
 * modern handshake.
 */

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "SecTrustStorePrivate.h"

#ifndef CAINJECTOR_DEFAULT_CERT_DIR
#define CAINJECTOR_DEFAULT_CERT_DIR "/usr/share/blackb0x/cainjector/certs"
#endif

/* A DER certificate that does not fit in this is not a certificate. Real
 * roots in the Debian store run 700-2200 bytes; the cap is generous and
 * exists only so a stray huge file in the directory cannot be read into
 * memory unbounded. */
#define MAX_CERT_BYTES (64 * 1024)

static int ends_with_der(const char *name) {
    size_t n = strlen(name);
    return n > 4 && strcmp(name + n - 4, ".der") == 0;
}

/* Read a whole file. Returns a malloc'd buffer the caller frees, or NULL. */
static unsigned char *read_file(const char *path, size_t *out_len) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return NULL;
    if (st.st_size <= 0 || (size_t)st.st_size > MAX_CERT_BYTES) return NULL;

    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    size_t len = (size_t)st.st_size;
    unsigned char *buf = malloc(len);
    if (!buf) { fclose(f); return NULL; }

    size_t got = fread(buf, 1, len, f);
    fclose(f);
    if (got != len) { free(buf); return NULL; }

    *out_len = len;
    return buf;
}

/* Install one DER certificate as a trusted anchor, then prove it landed.
 *
 * The SecTrustStoreContains() check is the whole point of doing this per
 * certificate rather than firing 121 calls and printing "done", and it has
 * already earned its keep twice on real hardware: once catching a missing
 * entitlement (every write returning -34018 while the tool would otherwise
 * have reported success), and once catching a wrong prototype in this
 * project's own header. See SecTrustStorePrivate.h for both.
 *
 * Returns 1 if the certificate is verifiably in the store afterwards. */
static int install_one(SecTrustStoreRef ts, const char *path, const char *name) {
    size_t len = 0;
    unsigned char *der = read_file(path, &len);
    if (!der) {
        fprintf(stderr, "cainjector: %s: unreadable or implausible size\n", name);
        return 0;
    }

    int ok = 0;
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, der, (CFIndex)len);
    free(der);
    if (!data) {
        fprintf(stderr, "cainjector: %s: CFDataCreate failed\n", name);
        return 0;
    }

    SecCertificateRef cert = SecCertificateCreateWithData(kCFAllocatorDefault, data);
    CFRelease(data);
    if (!cert) {
        /* Not fatal: a malformed file in the directory should not stop the
         * other 120 from being installed. */
        fprintf(stderr, "cainjector: %s: not a parseable DER certificate\n", name);
        return 0;
    }

    OSStatus st = SecTrustStoreSetTrustSettings(ts, cert, NULL);
    if (st != errSecSuccess) {
        /* Not returned early on: the anchor may already be present from a
         * previous run, in which case the Contains check below is the real
         * answer regardless of what this call thought. -34018 here means the
         * binary lost its `modify-anchor-certificates` entitlement (see
         * entitlements.plist) -- the one failure worth recognising on sight. */
        fprintf(stderr, "cainjector: %s: SecTrustStoreSetTrustSettings -> %d%s\n",
                name, (int)st,
                st == -34018 ? " (errSecMissingEntitlement -- signature lost its entitlement?)" : "");
    }

    /* Returns the answer directly; it is NOT an OSStatus with an out-param.
     * Getting that wrong is what made a fully working install report "NOT
     * present" 121 times -- the 1 being printed was the true return value. */
    if (SecTrustStoreContains(ts, cert)) {
        ok = 1;
    } else {
        fprintf(stderr, "cainjector: %s: NOT present after install\n", name);
    }

    CFRelease(cert);
    return ok;
}

int main(int argc, char **argv) {
    const char *dir_path = (argc > 1) ? argv[1] : CAINJECTOR_DEFAULT_CERT_DIR;

    /* Root only. This writes the device's trust store and needs the
     * `modify-anchor-certificates` entitlement to do it; running as anyone
     * else is a failure worth refusing loudly rather than discovering 121
     * error lines later. */
    if (geteuid() != 0) {
        fprintf(stderr, "cainjector: must run as root (euid %d)\n", (int)geteuid());
        return 2;
    }

    DIR *d = opendir(dir_path);
    if (!d) {
        fprintf(stderr, "cainjector: cannot open %s (errno %d, %s)\n",
                dir_path, errno, strerror(errno));
        return 1;
    }

    SecTrustStoreRef ts = SecTrustStoreForDomain(kSecTrustStoreDomainUser);
    if (!ts) {
        fprintf(stderr, "cainjector: SecTrustStoreForDomain(user) returned NULL\n");
        closedir(d);
        return 1;
    }

    unsigned seen = 0, installed = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (!ends_with_der(ent->d_name)) continue;

        char path[PATH_MAX];
        int n = snprintf(path, sizeof(path), "%s/%s", dir_path, ent->d_name);
        if (n <= 0 || (size_t)n >= sizeof(path)) continue;

        seen++;
        installed += (unsigned)install_one(ts, path, ent->d_name);
    }
    closedir(d);

    printf("cainjector: %u of %u certificates verified present in the writable trust store\n",
           installed, seen);

    if (seen == 0) {
        fprintf(stderr, "cainjector: no .der files in %s -- nothing to do\n", dir_path);
        return 1;
    }
    /* Partial success is still failure for this tool's purposes: the caller
     * (a LaunchDaemon that writes a run-once marker on exit 0) should try
     * again on the next boot rather than record a half-installed store. */
    return (installed == seen) ? 0 : 1;
}

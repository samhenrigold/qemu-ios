/* Per-device guest TLS termination. Upstream verification remains in libcurl.
 * The private CA is local to this device state, never installed on the Mac. */
#include <openssl/ssl.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <openssl/rand.h>
#include <arpa/inet.h>
static SSL *client_tls;
static bool tls_path(char *out, size_t size, const char *config, const char *suffix)
{
    int n = snprintf(out, size, "%s%s", config, suffix);
    return n > 0 && (size_t)n < size;
}
static EVP_PKEY *tls_key(void)
{
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    EVP_PKEY *key = NULL;
    if (ctx && EVP_PKEY_keygen_init(ctx) > 0 && EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) > 0)
        EVP_PKEY_keygen(ctx, &key);
    EVP_PKEY_CTX_free(ctx);
    return key;
}
static bool tls_extension(X509 *cert, X509 *issuer, int nid, const char *value)
{
    X509V3_CTX context;
    X509V3_set_ctx(&context, issuer, cert, NULL, NULL, 0);
    X509_EXTENSION *ext = X509V3_EXT_conf_nid(NULL, &context, nid, value);
    bool ok = ext && X509_add_ext(cert, ext, -1);
    X509_EXTENSION_free(ext);
    return ok;
}
static X509 *tls_certificate(EVP_PKEY *key, X509 *issuer, EVP_PKEY *issuer_key, const char *host)
{
    X509 *cert = X509_new();
    unsigned char serial[16];
    BIGNUM *number = NULL;
    if (!cert || RAND_bytes(serial, sizeof(serial)) != 1) goto bad;
    serial[0] &= 0x7f;
    number = BN_bin2bn(serial, sizeof(serial), NULL);
    if (!number || !BN_to_ASN1_INTEGER(number, X509_get_serialNumber(cert))) goto bad;
    BN_free(number); number = NULL;
    if (!X509_set_version(cert, 2) || !X509_set_pubkey(cert, key) ||
        !X509_gmtime_adj(X509_getm_notBefore(cert), -86400) ||
        !X509_gmtime_adj(X509_getm_notAfter(cert), host ? 86400L * 7 : 86400L * 3650)) goto bad;
    X509_NAME *name = X509_get_subject_name(cert);
    const char *cn = host && strlen(host) <= 64 ? host : "Light Touch Device Proxy";
    if (!X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)cn, -1, -1, 0) ||
        !X509_set_issuer_name(cert, issuer ? X509_get_subject_name(issuer) : name)) goto bad;
    if (!tls_extension(cert, issuer ? issuer : cert, NID_basic_constraints,
                       host ? "critical,CA:FALSE" : "critical,CA:TRUE,pathlen:0") ||
        !tls_extension(cert, issuer ? issuer : cert, NID_key_usage,
                       host ? "critical,digitalSignature,keyEncipherment" : "critical,keyCertSign,cRLSign")) goto bad;
    if (host) {
        unsigned char ip[16];
        char san[300];
        bool numeric = inet_pton(AF_INET, host, ip) == 1 || inet_pton(AF_INET6, host, ip) == 1;
        for (const char *p = host; *p; p++)
            if (!isalnum((unsigned char)*p) && *p != '.' && *p != '-' && *p != ':') goto bad;
        if (strlen(host) > 253) goto bad;
        snprintf(san, sizeof(san), "%s:%s", numeric ? "IP" : "DNS", host);
        if (!tls_extension(cert, issuer, NID_subject_alt_name, san) ||
            !tls_extension(cert, issuer, NID_ext_key_usage, "serverAuth")) goto bad;
    }
    if (!X509_sign(cert, issuer_key ? issuer_key : key, EVP_sha1())) goto bad;
    return cert;
bad:
    BN_free(number); X509_free(cert); return NULL;
}
static bool tls_load_ca(const char *config, EVP_PKEY **key, X509 **cert)
{
    char path[PATH_MAX];
    if (!tls_path(path, sizeof(path), config, ".ca.pem")) return false;
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    struct stat st;
    if (fd < 0) return false;
    if (fstat(fd, &st) || st.st_uid != getuid() || (st.st_mode & 077) || !S_ISREG(st.st_mode)) { close(fd); return false; }
    FILE *file = fdopen(fd, "r");
    if (!file) { close(fd); return false; }
    *key = PEM_read_PrivateKey(file, NULL, NULL, NULL);
    *cert = PEM_read_X509(file, NULL, NULL, NULL);
    fclose(file);
    return *key && *cert && X509_check_private_key(*cert, *key) == 1;
}
static bool tls_save_file(const char *path, EVP_PKEY *key, X509 *cert)
{
    char temporary[PATH_MAX];
    if (snprintf(temporary, sizeof(temporary), "%s.XXXXXX", path) >= (int)sizeof(temporary)) return false;
    int fd = mkstemp(temporary);
    if (fd < 0) return false;
    FILE *file = fdopen(fd, "wb");
    if (!file) { close(fd); unlink(temporary); return false; }
    bool ok = key ? PEM_write_PrivateKey(file, key, NULL, NULL, 0, NULL, NULL) && PEM_write_X509(file, cert)
                  : i2d_X509_fp(file, cert);
    if (fclose(file)) ok = false;
    if (ok && rename(temporary, path) == 0) return true;
    unlink(temporary); return false;
}
static int tls_initialize(const char *config)
{
    char path[PATH_MAX], lockpath[PATH_MAX], derpath[PATH_MAX];
    if (!tls_path(path, sizeof(path), config, ".ca.pem") ||
        !tls_path(lockpath, sizeof(lockpath), config, ".ca.lock") ||
        !tls_path(derpath, sizeof(derpath), config, ".ca.der")) return 1;
    int lock = open(lockpath, O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX)) return 1;
    EVP_PKEY *key = NULL;
    X509 *cert = NULL;
    bool ok = tls_load_ca(config, &key, &cert);
    if (!ok && access(path, F_OK) != 0 && errno == ENOENT) {
        EVP_PKEY_free(key); X509_free(cert);
        key = tls_key(); cert = key ? tls_certificate(key, NULL, NULL, NULL) : NULL;
        ok = cert && tls_save_file(path, key, cert);
    }
    if (ok) ok = tls_save_file(derpath, NULL, cert);
    EVP_PKEY_free(key); X509_free(cert); close(lock);
    return ok ? 0 : 1;
}
static SSL_CTX *tls_server_context(const char *config, const char *host)
{
    EVP_PKEY *ca_key = NULL, *key = NULL;
    X509 *ca = NULL, *cert = NULL;
    SSL_CTX *ctx = NULL;
    if (!tls_load_ca(config, &ca_key, &ca)) goto done;
    key = tls_key(); cert = key ? tls_certificate(key, ca, ca_key, host) : NULL;
    if (!cert) goto done;
    ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) goto done;
    SSL_CTX_set_security_level(ctx, 0); /* Legacy guest side only. */
    if (!SSL_CTX_set_min_proto_version(ctx, TLS1_VERSION) ||
        !SSL_CTX_set_cipher_list(ctx, "AES128-SHA:AES256-SHA") ||
        !SSL_CTX_use_certificate(ctx, cert) || !SSL_CTX_use_PrivateKey(ctx, key)) {
        SSL_CTX_free(ctx); ctx = NULL;
    }
done:
    EVP_PKEY_free(ca_key); EVP_PKEY_free(key); X509_free(ca); X509_free(cert);
    return ctx;
}

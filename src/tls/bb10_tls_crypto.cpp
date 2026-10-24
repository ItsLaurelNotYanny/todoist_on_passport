#include "bb10_tls/bb10_tls.h"

#include <openssl/rand.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <openssl/err.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int bb10_tls_random_bytes(unsigned char* buf, size_t len) {
    if (!buf || len == 0) {
        return BB10_TLS_ERR_INVALID_ARG;
    }
    if (RAND_bytes(buf, (int)len) != 1) {
        return BB10_TLS_ERR_ENTROPY_FAILURE;
    }
    return BB10_TLS_OK;
}

int bb10_tls_sha256(const unsigned char* data, size_t len, unsigned char* out_hash_32) {
    if (!data || !out_hash_32) {
        return BB10_TLS_ERR_INVALID_ARG;
    }
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return BB10_TLS_ERR_OUT_OF_MEMORY;
    }

    int rc = BB10_TLS_OK;
    unsigned int md_len = 0;
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1 ||
        EVP_DigestUpdate(ctx, data, len) != 1 ||
        EVP_DigestFinal_ex(ctx, out_hash_32, &md_len) != 1 ||
        md_len != 32) {
        rc = BB10_TLS_ERR_INTERNAL;
    }

    EVP_MD_CTX_free(ctx);
    return rc;
}

int bb10_tls_sha256_file(const char* filepath, char* out_hex_65) {
    if (!filepath || !out_hex_65) {
        return BB10_TLS_ERR_INVALID_ARG;
    }

    FILE* fp = fopen(filepath, "rb");
    if (!fp) {
        return BB10_TLS_ERR_IO_FAILED;
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        fclose(fp);
        return BB10_TLS_ERR_OUT_OF_MEMORY;
    }

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
        EVP_MD_CTX_free(ctx);
        fclose(fp);
        return BB10_TLS_ERR_INTERNAL;
    }

    unsigned char buffer[65536];
    size_t nread = 0;
    while ((nread = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        if (EVP_DigestUpdate(ctx, buffer, nread) != 1) {
            EVP_MD_CTX_free(ctx);
            fclose(fp);
            return BB10_TLS_ERR_INTERNAL;
        }
    }

    int file_err = ferror(fp);
    fclose(fp);

    if (file_err) {
        EVP_MD_CTX_free(ctx);
        return BB10_TLS_ERR_IO_FAILED;
    }

    unsigned char hash[32];
    unsigned int md_len = 0;
    if (EVP_DigestFinal_ex(ctx, hash, &md_len) != 1 || md_len != 32) {
        EVP_MD_CTX_free(ctx);
        return BB10_TLS_ERR_INTERNAL;
    }
    EVP_MD_CTX_free(ctx);

    for (int i = 0; i < 32; ++i) {
        sprintf(out_hex_65 + (i * 2), "%02x", hash[i]);
    }
    out_hex_65[64] = '\0';

    return BB10_TLS_OK;
}

int bb10_tls_calculate_cert_fingerprint(const char* cert_pem, size_t cert_len, char* out_hex_65) {
    if (!cert_pem || cert_len == 0 || !out_hex_65) {
        return BB10_TLS_ERR_INVALID_ARG;
    }
    BIO* bio = BIO_new_mem_buf(cert_pem, static_cast<int>(cert_len));
    if (!bio) {
        return BB10_TLS_ERR_OUT_OF_MEMORY;
    }
    X509* cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
    BIO_free(bio);
    if (!cert) {
        return BB10_TLS_ERR_INVALID_ARG;
    }
    unsigned char* der = NULL;
    int der_len = i2d_X509(cert, &der);
    X509_free(cert);
    if (der_len <= 0 || !der) {
        return BB10_TLS_ERR_INTERNAL;
    }
    unsigned char hash[32];
    int hash_rc = bb10_tls_sha256(der, static_cast<size_t>(der_len), hash);
    OPENSSL_free(der);
    if (hash_rc != BB10_TLS_OK) {
        return hash_rc;
    }
    for (int i = 0; i < 32; ++i) {
        sprintf(out_hex_65 + (i * 2), "%02x", hash[i]);
    }
    out_hex_65[64] = '\0';
    return BB10_TLS_OK;
}

int bb10_tls_generate_self_signed_cert(char** out_cert_pem,
                                       size_t* out_cert_len,
                                       char** out_key_pem,
                                       size_t* out_key_len) {
    if (!out_cert_pem || !out_cert_len || !out_key_pem || !out_key_len) {
        return BB10_TLS_ERR_INVALID_ARG;
    }
    *out_cert_pem = NULL;
    *out_cert_len = 0;
    *out_key_pem = NULL;
    *out_key_len = 0;

    /* Generate EC P-256 Key using OpenSSL 3 EVP API */
    EVP_PKEY* pkey = EVP_EC_gen("P-256");
    if (!pkey) {
        unsigned char probe[1];
        if (RAND_bytes(probe, sizeof(probe)) != 1) {
            return BB10_TLS_ERR_ENTROPY_FAILURE;
        }
        return BB10_TLS_ERR_INTERNAL;
    }

    X509* cert = X509_new();
    if (!cert) {
        EVP_PKEY_free(pkey);
        return BB10_TLS_ERR_OUT_OF_MEMORY;
    }

    /* Version 3 (represented as 2 in X509) */
    X509_set_version(cert, 2);

    /* Generate random 64-bit serial */
    unsigned char serial_bytes[8];
    if (RAND_bytes(serial_bytes, sizeof(serial_bytes)) != 1) {
        X509_free(cert);
        EVP_PKEY_free(pkey);
        return BB10_TLS_ERR_ENTROPY_FAILURE;
    }
    BIGNUM* bn_serial = BN_bin2bn(serial_bytes, sizeof(serial_bytes), NULL);
    if (bn_serial) {
        BN_to_ASN1_INTEGER(bn_serial, X509_get_serialNumber(cert));
        BN_free(bn_serial);
    } else {
        ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    }

    /* 10-year validity */
    X509_gmtime_adj(X509_getm_notBefore(cert), 0);
    X509_gmtime_adj(X509_getm_notAfter(cert), 315360000L);

    X509_NAME* name = X509_NAME_new();
    if (name) {
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   (const unsigned char*)"LocalSend Peer", -1, -1, 0);
        X509_set_subject_name(cert, name);
        X509_set_issuer_name(cert, name);
        X509_NAME_free(name);
    }

    X509_set_pubkey(cert, pkey);

    /* Self-sign certificate with SHA256 */
    if (X509_sign(cert, pkey, EVP_sha256()) <= 0) {
        X509_free(cert);
        EVP_PKEY_free(pkey);
        return BB10_TLS_ERR_INTERNAL;
    }

    /* Export Certificate to PEM BIO */
    BIO* cert_bio = BIO_new(BIO_s_mem());
    if (!cert_bio || PEM_write_bio_X509(cert_bio, cert) <= 0) {
        if (cert_bio) BIO_free(cert_bio);
        X509_free(cert);
        EVP_PKEY_free(pkey);
        return BB10_TLS_ERR_INTERNAL;
    }

    char* cert_data = NULL;
    long cert_size = BIO_get_mem_data(cert_bio, &cert_data);
    *out_cert_pem = (char*)malloc(cert_size + 1);
    if (!*out_cert_pem) {
        BIO_free(cert_bio);
        X509_free(cert);
        EVP_PKEY_free(pkey);
        return BB10_TLS_ERR_OUT_OF_MEMORY;
    }
    memcpy(*out_cert_pem, cert_data, cert_size);
    (*out_cert_pem)[cert_size] = '\0';
    *out_cert_len = (size_t)cert_size;
    BIO_free(cert_bio);

    /* Export Private Key to PKCS8 PEM BIO */
    BIO* key_bio = BIO_new(BIO_s_mem());
    if (!key_bio || PEM_write_bio_PKCS8PrivateKey(key_bio, pkey, NULL, NULL, 0, NULL, NULL) <= 0) {
        if (key_bio) BIO_free(key_bio);
        free(*out_cert_pem);
        *out_cert_pem = NULL;
        *out_cert_len = 0;
        X509_free(cert);
        EVP_PKEY_free(pkey);
        return BB10_TLS_ERR_INTERNAL;
    }

    char* key_data = NULL;
    long key_size = BIO_get_mem_data(key_bio, &key_data);
    *out_key_pem = (char*)malloc(key_size + 1);
    if (!*out_key_pem) {
        BIO_free(key_bio);
        free(*out_cert_pem);
        *out_cert_pem = NULL;
        *out_cert_len = 0;
        X509_free(cert);
        EVP_PKEY_free(pkey);
        return BB10_TLS_ERR_OUT_OF_MEMORY;
    }
    memcpy(*out_key_pem, key_data, key_size);
    (*out_key_pem)[key_size] = '\0';
    *out_key_len = (size_t)key_size;
    BIO_free(key_bio);

    X509_free(cert);
    EVP_PKEY_free(pkey);
    return BB10_TLS_OK;
}

void bb10_tls_free_string(char* str) {
    if (str) {
        free(str);
    }
}

const char* bb10_tls_strerror(int err) {
    switch (err) {
        case BB10_TLS_OK:
            return "Success";
        case BB10_TLS_ERR_WANT_READ:
            return "Operation wants read (try again when socket is readable)";
        case BB10_TLS_ERR_WANT_WRITE:
            return "Operation wants write (try again when socket is writable)";
        case BB10_TLS_ERR_CLOSED:
            return "Connection closed cleanly by peer (close-notify received)";
        case BB10_TLS_ERR_INVALID_ARG:
            return "Invalid argument supplied";
        case BB10_TLS_ERR_INVALID_PIN:
            return "Invalid expected fingerprint pin format (must be 64 hex characters)";
        case BB10_TLS_ERR_PIN_MISMATCH:
            return "Peer certificate SHA-256 fingerprint does not match expected pin";
        case BB10_TLS_ERR_NO_PEER_CERT:
            return "Peer did not present an X.509 certificate";
        case BB10_TLS_ERR_CERT_VERIFY:
            return "Certificate chain or hostname verification failed";
        case BB10_TLS_ERR_CA_LOAD_FAILED:
            return "Failed to load trusted CA root certificate bundle";
        case BB10_TLS_ERR_HANDSHAKE_FAILED:
            return "TLS handshake negotiation failed";
        case BB10_TLS_ERR_ENTROPY_FAILURE:
            return "Cryptographic CSPRNG entropy acquisition failed";
        case BB10_TLS_ERR_OUT_OF_MEMORY:
            return "Out of memory";
        case BB10_TLS_ERR_IO_FAILED:
            return "Underlying socket or file I/O error";
        case BB10_TLS_ERR_INTERNAL:
            return "Internal cryptographic engine error";
        default:
            return "Unknown TLS error";
    }
}

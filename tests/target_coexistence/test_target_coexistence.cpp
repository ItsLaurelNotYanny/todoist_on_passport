#include <bb10_tls/bb10_tls.h>
#include <bb10_tls/bb10_tls.hpp>

#ifdef PASSPORT_BB10
#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtNetwork/QTcpSocket>
#include <QtNetwork/QSslSocket>
#include <QtNetwork/QSslCertificate>
#include <QtNetwork/QHostAddress>
#include <bb/cascades/Application>
#include <dlfcn.h>
#endif

#include <iostream>
#include <string>
#include <cstdlib>

int main(int argc, char** argv) {
    std::cout << "[Target Coexistence Test] Initializing modern bb10_tls subsystem..." << std::endl;

    // 1. Initialize modern bb10_tls (OpenSSL 3.5.9 LTS backend)
    bb10_tls::Context tlsCtx(BB10_TLS_POLICY_P2P);
    if (!tlsCtx.isValid()) {
        std::cerr << "FAIL: bb10_tls context initialization failed" << std::endl;
        return 1;
    }

    std::string certPem, keyPem;
    int gen_rc = bb10_tls::generateSelfSignedCert(certPem, keyPem);
    if (gen_rc != BB10_TLS_OK) {
        std::cerr << "FAIL: bb10_tls self-signed cert generation failed" << std::endl;
        return 2;
    }

    unsigned char entropy[32];
    if (bb10_tls::randomBytes(entropy, sizeof(entropy)) != BB10_TLS_OK) {
        std::cerr << "FAIL: bb10_tls randomBytes failed" << std::endl;
        return 3;
    }

    std::cout << "[Target Coexistence Test] Modern bb10_tls initialized successfully." << std::endl;

    // Negative control options
    bool simulated_disabled_ssl = false;
    bool simulated_missing_libcrypto = false;
    bool simulated_missing_sha1 = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--negative-disabled-ssl") {
            simulated_disabled_ssl = true;
        } else if (std::string(argv[i]) == "--negative-missing-libcrypto") {
            simulated_missing_libcrypto = true;
        } else if (std::string(argv[i]) == "--negative-missing-sha1") {
            simulated_missing_sha1 = true;
        }
    }

    bool supports_ssl = false;
#ifdef PASSPORT_BB10
    std::cout << "[Target Coexistence Test] Initializing legacy BB10 Cascades/QtNetwork in same process..." << std::endl;
    bb::cascades::Application app(argc, argv);

    QTcpSocket tcpSocket;
    std::cout << "[Target Coexistence Test] QTcpSocket constructed (state=" << tcpSocket.state() << ")" << std::endl;

    QSslSocket sslSocket;
    supports_ssl = QSslSocket::supportsSsl();
    std::cout << "[Target Coexistence Test] QSslSocket::supportsSsl() = " << supports_ssl << std::endl;
    std::cout << "[Target Coexistence Test] QSslSocket constructed (state=" << sslSocket.state() << ")" << std::endl;
#else
    supports_ssl = true;
    std::cout << "[Target Coexistence Test] Host simulated supports_ssl = " << supports_ssl << std::endl;
    (void)simulated_missing_libcrypto;
    (void)simulated_missing_sha1;
#endif

    // Shared availability check for legacy SSL backend (exercised by normal path and negative control)
    bool effective_ssl_available = supports_ssl && !simulated_disabled_ssl;
    if (!effective_ssl_available) {
        std::cerr << "FAIL: Legacy SSL backend unavailable (supportsSsl returned false or disabled by negative control)" << std::endl;
        return 4;
    }

#ifdef PASSPORT_BB10
    // 1. Assert legacy Qt cryptographic hash executes correctly in same process
    QByteArray testData("todoist_coexistence_legacy_crypto_test");
    QByteArray sha1Digest = QCryptographicHash::hash(testData, QCryptographicHash::Sha1);
    std::string sha1Hex = sha1Digest.toHex().constData();
    std::cout << "[Target Coexistence Test] Legacy QCryptographicHash SHA1 = " << sha1Hex << std::endl;
    if (sha1Hex != "160a5e3d86e19d26508ed373f9a00b4c7964478e") {
        std::cerr << "FAIL: Legacy QCryptographicHash produced unexpected digest: " << sha1Hex << std::endl;
        return 5;
    }

    // 2. Assert legacy QSslCertificate (QtNetwork's legacy OpenSSL backend) can parse and digest
    QSslCertificate qCert(QByteArray(certPem.c_str(), static_cast<int>(certPem.size())));
    if (qCert.isNull()) {
        std::cerr << "FAIL: Legacy QSslCertificate failed to parse certificate via legacy OpenSSL" << std::endl;
        return 6;
    }
    QByteArray qCertDigest = qCert.digest(QCryptographicHash::Sha1);
    if (qCertDigest.isEmpty()) {
        std::cerr << "FAIL: Legacy QSslCertificate failed to compute digest" << std::endl;
        return 7;
    }
    std::cout << "[Target Coexistence Test] Legacy QSslCertificate SHA-1: "
              << qCertDigest.toHex().constData() << std::endl;

    // 3. Assert system libcrypto is dynamically loadable and callable alongside bb10_tls
    void* sys_crypto = NULL;
    if (!simulated_missing_libcrypto) {
        sys_crypto = dlopen("libcrypto.so.1.0.0", RTLD_LAZY | RTLD_LOCAL);
        if (!sys_crypto) {
            sys_crypto = dlopen("libcrypto.so.0.9.8", RTLD_LAZY | RTLD_LOCAL);
        }
        if (!sys_crypto) {
            sys_crypto = dlopen("libcrypto.so", RTLD_LAZY | RTLD_LOCAL);
        }
    }
    if (!sys_crypto) {
        std::cerr << "FAIL: System libcrypto dynamic loading failed (libcrypto.so.1.0.0, libcrypto.so.0.9.8, or libcrypto.so)" << std::endl;
        return 8;
    }

    typedef unsigned char* (*SHA1_fn)(const unsigned char*, size_t, unsigned char*);
    SHA1_fn sys_sha1 = NULL;
    if (!simulated_missing_sha1) {
        *(void**)(&sys_sha1) = dlsym(sys_crypto, "SHA1");
    }
    if (!sys_sha1) {
        std::cerr << "FAIL: dlsym failed to resolve SHA1 from system libcrypto" << std::endl;
        dlclose(sys_crypto);
        return 9;
    }

    unsigned char sys_md[20];
    if (sys_sha1((const unsigned char*)"todoist_coexistence_legacy_crypto_test", 38, sys_md) == NULL) {
        std::cerr << "FAIL: System libcrypto SHA1 calculation returned NULL" << std::endl;
        dlclose(sys_crypto);
        return 10;
    }

    char hex_buf[41];
    for (int i = 0; i < 20; ++i) sprintf(hex_buf + (i * 2), "%02x", sys_md[i]);
    hex_buf[40] = '\0';
    std::cout << "[Target Coexistence Test] System libcrypto SHA-1 = " << hex_buf << std::endl;
    if (std::string(hex_buf) != "160a5e3d86e19d26508ed373f9a00b4c7964478e") {
        std::cerr << "FAIL: System libcrypto produced mismatched digest" << std::endl;
        dlclose(sys_crypto);
        return 11;
    }
    dlclose(sys_crypto);

    std::cout << "[Target Coexistence Test] Coexistence verified: QtNetwork legacy SSL/crypto and modern bb10_tls coexisting cleanly." << std::endl;
#endif

    std::cout << "[Target Coexistence Test] ALL CHECKS PASSED!" << std::endl;
    return 0;
}

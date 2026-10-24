#include "bb10_tls/bb10_tls.h"
#include "bb10_tls/bb10_tls.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>

#include <openssl/x509.h>
#include <openssl/pem.h>

static void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " <command> [options]\n\n"
              << "Commands:\n"
              << "  client       Perform TLS handshake probe as client\n"
              << "               --url <url>                 Target HTTPS URL (e.g. https://example.com:8443)\n"
              << "               --ca-bundle <path>          Path to CA certificate bundle\n"
              << "               --expected-pin <pin>        Expected 64-hex SHA-256 fingerprint (P2P mode)\n"
              << "               --min-tls-version <version> TLSv1.2 or TLSv1.3 (default: TLSv1.2)\n"
              << "               --max-tls-version <version> TLSv1.2 or TLSv1.3 (default: TLSv1.3)\n\n"
              << "  server       Run standalone TLS server probe\n"
              << "               --port <port>               Port to listen on (default: 8443)\n"
              << "               --cert <file>               Path to PEM certificate (default: auto-generated)\n"
              << "               --key <file>                Path to PEM private key\n"
              << "               --min-tls-version <version> TLSv1.2 or TLSv1.3\n"
              << "               --max-tls-version <version> TLSv1.2 or TLSv1.3\n"
              << "               --once                      Exit after serving one connection\n\n"
              << "  fingerprint  Calculate SHA-256 fingerprint of a PEM certificate file\n"
              << "               --cert <file>               Certificate file to inspect\n"
              << std::endl;
}

static bool parse_version(const std::string& ver_str, bb10_tls_version_t& out_ver) {
    if (ver_str == "TLSv1.2" || ver_str == "1.2") {
        out_ver = BB10_TLS_VERSION_1_2;
        return true;
    }
    if (ver_str == "TLSv1.3" || ver_str == "1.3") {
        out_ver = BB10_TLS_VERSION_1_3;
        return true;
    }
    return false;
}

static bool match_expected_error(int rc, const std::string& exp_str) {
    if (exp_str.empty()) return false;
    if (exp_str == "BB10_TLS_ERR_WANT_READ" || exp_str == "WANT_READ" || exp_str == "-1") {
        return rc == BB10_TLS_ERR_WANT_READ;
    }
    if (exp_str == "BB10_TLS_ERR_WANT_WRITE" || exp_str == "WANT_WRITE" || exp_str == "-2") {
        return rc == BB10_TLS_ERR_WANT_WRITE;
    }
    if (exp_str == "BB10_TLS_ERR_CLOSED" || exp_str == "CLOSED" || exp_str == "-3") {
        return rc == BB10_TLS_ERR_CLOSED;
    }
    if (exp_str == "BB10_TLS_ERR_INVALID_ARG" || exp_str == "INVALID_ARG" || exp_str == "-4") {
        return rc == BB10_TLS_ERR_INVALID_ARG;
    }
    if (exp_str == "BB10_TLS_ERR_INVALID_PIN" || exp_str == "INVALID_PIN" || exp_str == "-5") {
        return rc == BB10_TLS_ERR_INVALID_PIN;
    }
    if (exp_str == "BB10_TLS_ERR_PIN_MISMATCH" || exp_str == "PIN_MISMATCH" || exp_str == "-6") {
        return rc == BB10_TLS_ERR_PIN_MISMATCH;
    }
    if (exp_str == "BB10_TLS_ERR_NO_PEER_CERT" || exp_str == "NO_PEER_CERT" || exp_str == "-7") {
        return rc == BB10_TLS_ERR_NO_PEER_CERT;
    }
    if (exp_str == "BB10_TLS_ERR_CERT_VERIFY" || exp_str == "CERT_VERIFY" || exp_str == "CERT_VERIFY_FAIL" || exp_str == "CERT_VERIFY_FAILED" || exp_str == "-8") {
        return rc == BB10_TLS_ERR_CERT_VERIFY;
    }
    if (exp_str == "BB10_TLS_ERR_CA_LOAD_FAILED" || exp_str == "CA_LOAD_FAILED" || exp_str == "CA_LOAD_FAIL" || exp_str == "-9") {
        return rc == BB10_TLS_ERR_CA_LOAD_FAILED;
    }
    if (exp_str == "BB10_TLS_ERR_HANDSHAKE_FAILED" || exp_str == "HANDSHAKE_FAILED" || exp_str == "-10") {
        return rc == BB10_TLS_ERR_HANDSHAKE_FAILED;
    }
    if (exp_str == "BB10_TLS_ERR_ENTROPY_FAILURE" || exp_str == "ENTROPY_FAILURE" || exp_str == "-11") {
        return rc == BB10_TLS_ERR_ENTROPY_FAILURE;
    }
    if (exp_str == "BB10_TLS_ERR_OUT_OF_MEMORY" || exp_str == "OUT_OF_MEMORY" || exp_str == "-12") {
        return rc == BB10_TLS_ERR_OUT_OF_MEMORY;
    }
    if (exp_str == "BB10_TLS_ERR_IO_FAILED" || exp_str == "IO_FAILED" || exp_str == "-13") {
        return rc == BB10_TLS_ERR_IO_FAILED;
    }
    if (exp_str == "BB10_TLS_ERR_INTERNAL" || exp_str == "INTERNAL" || exp_str == "-14") {
        return rc == BB10_TLS_ERR_INTERNAL;
    }
    return false;
}

static bool parse_url(const std::string& url, std::string& out_host, int& out_port, std::string& out_path) {
    std::string s = url;
    if (s.compare(0, 8, "https://") == 0) {
        s = s.substr(8);
    } else if (s.compare(0, 7, "http://") == 0) {
        s = s.substr(7);
    }

    size_t slash_pos = s.find('/');
    std::string host_port;
    if (slash_pos != std::string::npos) {
        host_port = s.substr(0, slash_pos);
        out_path = s.substr(slash_pos);
    } else {
        host_port = s;
        out_path = "/";
    }

    size_t colon_pos = host_port.find(':');
    if (colon_pos != std::string::npos) {
        out_host = host_port.substr(0, colon_pos);
        out_port = std::atoi(host_port.substr(colon_pos + 1).c_str());
    } else {
        out_host = host_port;
        out_port = 443;
    }
    return !out_host.empty();
}

static int run_client(int argc, char** argv) {
    std::string url;
    std::string ca_bundle;
    std::string expected_pin;
    std::string expected_error;
    std::string min_ver_str = "TLSv1.2";
    std::string max_ver_str = "TLSv1.3";

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--url" && i + 1 < argc) {
            url = argv[++i];
        } else if (arg == "--ca-bundle" && i + 1 < argc) {
            ca_bundle = argv[++i];
        } else if (arg == "--expected-pin" && i + 1 < argc) {
            expected_pin = argv[++i];
        } else if ((arg == "--expect-error" || arg == "--expected-error") && i + 1 < argc) {
            expected_error = argv[++i];
        } else if (arg == "--min-tls-version" && i + 1 < argc) {
            min_ver_str = argv[++i];
        } else if (arg == "--max-tls-version" && i + 1 < argc) {
            max_ver_str = argv[++i];
        } else {
            std::cerr << "Error: Unknown or incomplete option: " << arg << std::endl;
            return 1;
        }
    }

    if (url.empty()) {
        std::cerr << "Error: --url is required for client probe" << std::endl;
        return 1;
    }

    bb10_tls_version_t min_ver, max_ver;
    if (!parse_version(min_ver_str, min_ver)) {
        std::cerr << "Error: Invalid min TLS version: " << min_ver_str << std::endl;
        return 1;
    }
    if (!parse_version(max_ver_str, max_ver)) {
        std::cerr << "Error: Invalid max TLS version: " << max_ver_str << std::endl;
        return 1;
    }

    std::string host, path;
    int port = 443;
    if (!parse_url(url, host, port, path)) {
        std::cerr << "Error: Failed to parse URL: " << url << std::endl;
        return 1;
    }

    std::cout << "[bb10_tls_probe] Target: " << host << ":" << port << path << std::endl;

    // Resolve hostname
    char port_buf[16];
    sprintf(port_buf, "%d", port);
    struct addrinfo hints, *res = NULL;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int gai_err = getaddrinfo(host.c_str(), port_buf, &hints, &res);
    if (gai_err != 0 || !res) {
        std::cerr << "Error: Hostname resolution failed: " << gai_strerror(gai_err) << std::endl;
        return 2;
    }

    int socket_fd = -1;
    for (struct addrinfo* p = res; p != NULL; p = p->ai_next) {
        socket_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (socket_fd < 0) {
            continue;
        }
        if (connect(socket_fd, p->ai_addr, p->ai_addrlen) == 0) {
            break; // Successfully connected
        }
        close(socket_fd);
        socket_fd = -1;
    }
    freeaddrinfo(res);

    if (socket_fd < 0) {
        std::cerr << "Error: Failed to connect TCP socket to " << host << ":" << port << std::endl;
        return 3;
    }

    // Configure Context
    bb10_tls_policy_t policy = expected_pin.empty() ? BB10_TLS_POLICY_PUBLIC_PKI : BB10_TLS_POLICY_P2P;
    bb10_tls_context_t* ctx = bb10_tls_context_create(policy);
    if (!ctx) {
        std::cerr << "Error: Failed to create bb10_tls context" << std::endl;
        close(socket_fd);
        return 4;
    }

    if (!ca_bundle.empty()) {
        int ca_rc = bb10_tls_context_set_ca_path(ctx, ca_bundle.c_str());
        if (ca_rc != BB10_TLS_OK) {
            std::cerr << "Error: Failed to load CA bundle from " << ca_bundle
                      << ": " << bb10_tls_strerror(ca_rc) << std::endl;
            bb10_tls_context_destroy(ctx);
            close(socket_fd);
            if (!expected_error.empty() && match_expected_error(ca_rc, expected_error)) {
                std::cout << "[bb10_tls_probe] Expected error matched: " << expected_error << std::endl;
                return 0;
            }
            return 5;
        }
    }

    if (!expected_pin.empty()) {
        int pin_rc = bb10_tls_context_set_expected_fingerprint(ctx, expected_pin.c_str());
        if (pin_rc != BB10_TLS_OK) {
            std::cerr << "Error: Invalid expected pin: " << bb10_tls_strerror(pin_rc) << std::endl;
            bb10_tls_context_destroy(ctx);
            close(socket_fd);
            if (!expected_error.empty() && match_expected_error(pin_rc, expected_error)) {
                std::cout << "[bb10_tls_probe] Expected error matched: " << expected_error << std::endl;
                return 0;
            }
            return 6;
        }
    }

    bb10_tls_context_set_min_tls_version(ctx, min_ver);
    bb10_tls_context_set_max_tls_version(ctx, max_ver);

    // Create TLS connection
    bb10_tls_connection_t* conn = bb10_tls_connection_create(ctx, socket_fd);
    if (!conn) {
        std::cerr << "Error: Failed to create bb10_tls connection" << std::endl;
        bb10_tls_context_destroy(ctx);
        close(socket_fd);
        return 7;
    }

    struct in_addr in4;
    struct in6_addr in6;
    bool is_ip = (inet_pton(AF_INET, host.c_str(), &in4) == 1) ||
                 (inet_pton(AF_INET6, host.c_str(), &in6) == 1);
    if (is_ip) {
        std::cout << "[bb10_tls_probe] Initiating TLS handshake with IP literal=" << host << "..." << std::endl;
    } else {
        std::cout << "[bb10_tls_probe] Initiating TLS handshake with SNI=" << host << "..." << std::endl;
    }

    int conn_rc = bb10_tls_connect(conn, host.c_str());
    if (conn_rc != BB10_TLS_OK) {
        std::cerr << "Error: TLS Handshake failed: " << bb10_tls_strerror(conn_rc)
                  << " (" << bb10_tls_get_last_error(conn) << ")" << std::endl;
        bb10_tls_connection_destroy(conn);
        bb10_tls_context_destroy(ctx);
        if (!expected_error.empty() && match_expected_error(conn_rc, expected_error)) {
            std::cout << "[bb10_tls_probe] Expected error matched: " << expected_error << std::endl;
            return 0;
        }
        return 8;
    }

    if (!expected_error.empty()) {
        std::cerr << "Error: Expected error " << expected_error << " but handshake succeeded!" << std::endl;
        bb10_tls_shutdown(conn);
        bb10_tls_connection_destroy(conn);
        bb10_tls_context_destroy(ctx);
        return 9;
    }

    char ver_buf[32];
    char cipher_buf[64];
    char fp_buf[65];
    bb10_tls_connection_get_negotiated_version(conn, ver_buf, sizeof(ver_buf));
    bb10_tls_connection_get_cipher(conn, cipher_buf, sizeof(cipher_buf));
    bb10_tls_connection_get_peer_fingerprint(conn, fp_buf);

    std::cout << "[bb10_tls_probe] Handshake successful!" << std::endl;
    std::cout << "  Negotiated Protocol: " << ver_buf << std::endl;
    std::cout << "  Negotiated Cipher:   " << cipher_buf << std::endl;
    std::cout << "  Peer Fingerprint:    " << fp_buf << std::endl;

    // Send HTTP GET probe
    std::ostringstream req;
    req << "GET " << path << " HTTP/1.1\r\n"
        << "Host: " << host << "\r\n"
        << "User-Agent: bb10_tls_probe/1.0\r\n"
        << "Connection: close\r\n\r\n";
    std::string req_str = req.str();

    size_t written = 0;
    int w_rc = bb10_tls_write(conn, req_str.data(), req_str.size(), &written);
    if (w_rc != BB10_TLS_OK || written == 0) {
        std::cerr << "Error: bb10_tls_write failed: " << bb10_tls_strerror(w_rc) << std::endl;
        bb10_tls_connection_destroy(conn);
        bb10_tls_context_destroy(ctx);
        return 10;
    }

    char resp_buf[512];
    size_t nread = 0;
    int r_rc = bb10_tls_read(conn, resp_buf, sizeof(resp_buf) - 1, &nread);
    if (r_rc != BB10_TLS_OK || nread == 0) {
        std::cerr << "Error: bb10_tls_read failed: " << bb10_tls_strerror(r_rc) << std::endl;
        bb10_tls_connection_destroy(conn);
        bb10_tls_context_destroy(ctx);
        return 11;
    }

    resp_buf[nread] = '\0';
    // Print HTTP response status line
    std::string first_line(resp_buf);
    size_t nl = first_line.find("\r\n");
    if (nl != std::string::npos) {
        first_line = first_line.substr(0, nl);
    }
    std::cout << "  HTTP Response Line:  " << first_line << std::endl;

    bb10_tls_shutdown(conn);
    bb10_tls_connection_destroy(conn);
    bb10_tls_context_destroy(ctx);

    return 0;
}

static int run_server(int argc, char** argv) {
    int port = 8443;
    std::string cert_file;
    std::string key_file;
    std::string min_ver_str = "TLSv1.2";
    std::string max_ver_str = "TLSv1.3";
    bool once = false;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if (arg == "--cert" && i + 1 < argc) {
            cert_file = argv[++i];
        } else if (arg == "--key" && i + 1 < argc) {
            key_file = argv[++i];
        } else if (arg == "--min-tls-version" && i + 1 < argc) {
            min_ver_str = argv[++i];
        } else if (arg == "--max-tls-version" && i + 1 < argc) {
            max_ver_str = argv[++i];
        } else if (arg == "--once") {
            once = true;
        } else {
            std::cerr << "Error: Unknown or incomplete option: " << arg << std::endl;
            return 1;
        }
    }

    bb10_tls_version_t min_ver, max_ver;
    if (!parse_version(min_ver_str, min_ver)) {
        std::cerr << "Error: Invalid min TLS version: " << min_ver_str << std::endl;
        return 1;
    }
    if (!parse_version(max_ver_str, max_ver)) {
        std::cerr << "Error: Invalid max TLS version: " << max_ver_str << std::endl;
        return 1;
    }

    std::string cert_pem, key_pem;
    if (cert_file.empty()) {
        std::cout << "[bb10_tls_probe] Generating transient self-signed EC P-256 certificate..." << std::endl;
        char* c_pem = NULL;
        size_t c_len = 0;
        char* k_pem = NULL;
        size_t k_len = 0;
        if (bb10_tls_generate_self_signed_cert(&c_pem, &c_len, &k_pem, &k_len) != BB10_TLS_OK) {
            std::cerr << "Error: Failed to generate self-signed certificate" << std::endl;
            return 1;
        }
        cert_pem.assign(c_pem, c_len);
        key_pem.assign(k_pem, k_len);
        bb10_tls_free_string(c_pem);
        bb10_tls_free_string(k_pem);
    } else {
        std::ifstream cf(cert_file.c_str());
        std::ifstream kf(key_file.c_str());
        if (!cf.is_open() || !kf.is_open()) {
            std::cerr << "Error: Could not open certificate or key file" << std::endl;
            return 1;
        }
        std::stringstream cb, kb;
        cb << cf.rdbuf();
        kb << kf.rdbuf();
        cert_pem = cb.str();
        key_pem = kb.str();
    }

    bb10_tls_context_t* ctx = bb10_tls_context_create(BB10_TLS_POLICY_P2P);
    if (!ctx) {
        std::cerr << "Error: Failed to create server TLS context" << std::endl;
        return 2;
    }

    if (bb10_tls_context_set_credentials(ctx, cert_pem.data(), cert_pem.size(), key_pem.data(), key_pem.size()) != BB10_TLS_OK) {
        std::cerr << "Error: Failed to set server credentials" << std::endl;
        bb10_tls_context_destroy(ctx);
        return 3;
    }

    bb10_tls_context_set_min_tls_version(ctx, min_ver);
    bb10_tls_context_set_max_tls_version(ctx, max_ver);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::cerr << "Error: Failed to create listening socket" << std::endl;
        bb10_tls_context_destroy(ctx);
        return 4;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in sin;
    std::memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(listen_fd, (struct sockaddr*)&sin, sizeof(sin)) < 0) {
        std::cerr << "Error: Failed to bind to port " << port << std::endl;
        close(listen_fd);
        bb10_tls_context_destroy(ctx);
        return 5;
    }

    if (listen(listen_fd, 5) < 0) {
        std::cerr << "Error: Failed to listen on socket" << std::endl;
        close(listen_fd);
        bb10_tls_context_destroy(ctx);
        return 6;
    }

    std::cout << "[bb10_tls_probe] Server listening on 0.0.0.0:" << port << "..." << std::endl;

    int server_exit_code = 0;
    do {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            break;
        }

        std::cout << "[bb10_tls_probe] Inbound connection from " << inet_ntoa(client_addr.sin_addr)
                  << ":" << ntohs(client_addr.sin_port) << std::endl;

        bb10_tls_connection_t* conn = bb10_tls_connection_create(ctx, client_fd);
        if (!conn) {
            close(client_fd);
            continue;
        }

        int a_rc = bb10_tls_accept(conn);
        if (a_rc == BB10_TLS_OK) {
            char ver[32], cipher[64];
            bb10_tls_connection_get_negotiated_version(conn, ver, sizeof(ver));
            bb10_tls_connection_get_cipher(conn, cipher, sizeof(cipher));
            std::cout << "  Negotiated Protocol: " << ver << std::endl;
            std::cout << "  Negotiated Cipher:   " << cipher << std::endl;

            char buf[512];
            size_t nread = 0;
            int r_rc = bb10_tls_read(conn, buf, sizeof(buf) - 1, &nread);

            const char* http_resp = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 17\r\nConnection: close\r\n\r\nbb10_tls_probe ok\n";
            size_t nwritten = 0;
            int w_rc = bb10_tls_write(conn, http_resp, strlen(http_resp), &nwritten);
            int s_rc = bb10_tls_shutdown(conn);

            if (r_rc != BB10_TLS_OK && r_rc != BB10_TLS_ERR_CLOSED) {
                std::cerr << "  TLS read failed: " << bb10_tls_strerror(r_rc) << std::endl;
                server_exit_code = 13;
            } else if (w_rc != BB10_TLS_OK && w_rc != BB10_TLS_ERR_CLOSED) {
                std::cerr << "  TLS write failed: " << bb10_tls_strerror(w_rc) << std::endl;
                server_exit_code = 14;
            } else if (s_rc != BB10_TLS_OK && s_rc != BB10_TLS_ERR_CLOSED) {
                std::cerr << "  TLS shutdown failed: " << bb10_tls_strerror(s_rc) << std::endl;
                server_exit_code = 15;
            }
        } else {
            std::cerr << "  TLS accept failed: " << bb10_tls_strerror(a_rc) << std::endl;
            server_exit_code = 12;
        }
        bb10_tls_connection_destroy(conn);
    } while (!once);

    close(listen_fd);
    bb10_tls_context_destroy(ctx);
    return server_exit_code;
}

static int run_fingerprint(int argc, char** argv) {
    std::string cert_file;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--cert" && i + 1 < argc) {
            cert_file = argv[++i];
        } else {
            std::cerr << "Error: Unknown or incomplete option: " << arg << std::endl;
            return 1;
        }
    }

    if (cert_file.empty()) {
        std::cerr << "Error: --cert is required for fingerprint command" << std::endl;
        return 1;
    }

    FILE* fp = fopen(cert_file.c_str(), "rb");
    if (!fp) {
        std::cerr << "Error: Failed to open certificate file: " << cert_file << std::endl;
        return 2;
    }

    X509* cert = PEM_read_X509(fp, NULL, NULL, NULL);
    fclose(fp);

    if (!cert) {
        std::cerr << "Error: Failed to parse certificate from " << cert_file << std::endl;
        return 3;
    }

    unsigned char* der = NULL;
    int der_len = i2d_X509(cert, &der);
    if (der_len <= 0 || !der) {
        X509_free(cert);
        std::cerr << "Error: Failed to serialize certificate to DER" << std::endl;
        return 4;
    }

    unsigned char hash[32];
    int rc = bb10_tls_sha256(der, static_cast<size_t>(der_len), hash);
    OPENSSL_free(der);
    X509_free(cert);

    if (rc != BB10_TLS_OK) {
        std::cerr << "Error: SHA-256 calculation failed" << std::endl;
        return 5;
    }

    char hex[65];
    for (int i = 0; i < 32; ++i) {
        sprintf(hex + (i * 2), "%02x", hash[i]);
    }
    hex[64] = '\0';

    std::cout << hex << std::endl;
    return 0;
}

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string cmd = argv[1];
    if (cmd == "client") {
        return run_client(argc, argv);
    } else if (cmd == "server") {
        return run_server(argc, argv);
    } else if (cmd == "fingerprint") {
        return run_fingerprint(argc, argv);
    } else {
        std::cerr << "Unknown command: " << cmd << "\n" << std::endl;
        print_usage(argv[0]);
        return 1;
    }
}

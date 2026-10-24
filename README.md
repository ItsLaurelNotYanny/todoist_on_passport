# Todoist on BlackBerry Passport

An offline-first, native Todoist client engineered exclusively for the **BlackBerry Passport** (BlackBerry 10 OS 10.3.3.3216 / QNX 6.6).

Built strictly in native **C++03 and POSIX C**, with zero Android runtime dependencies, fully custom Cascades QML UI tailored for the 1440×1440 square screen, physical QWERTY keyboard shortcuts, and capacitive trackpad navigation.

---

## Key Features

- **Offline-First Architecture**: High-concurrency local SQLite 3 WAL storage with AES encryption for the OAuth API token at rest (the task database itself is standard SQLite format). Local actions queue safely in `sync_command_queue` and sync seamlessly upon reconnection.
- **Incremental Sync Engine**: Todoist Sync API v1 client (`https://api.todoist.com/api/v1/sync`) with `sync_token` cursor tracking, optimistic UI updates, and atomic reconciliation (`temp_id_mapping`).
- **Modern TLS 1.3 & Symbol Isolation**: Custom statically linked OpenSSL 3.5.9 + libcurl runtime with strict symbol hiding (`-fvisibility=hidden`). Completely eliminates symbol collisions with BlackBerry 10's legacy system SSL libraries.
- **Tailored for BlackBerry Passport (1440×1440, 453 PPI)**:
  - Custom square UI with dark theme and high-contrast 4-tier priority cards.
  - Full Active Frame cover support with live task metrics.
  - Native physical QWERTY keyboard shortcuts and capacitive trackpad scroll gestures.

---

## Physical Keyboard Shortcuts

| Key | Action |
| :--- | :--- |
| `[Space]` | Toggle task completion status |
| `[Enter]` | Open task details sheet / edit task |
| `[Backspace]` / `[Del]` | Delete selected task |
| `[C]` | Quick task creation sheet |
| `[R]` | Trigger immediate synchronization |
| `[J]` / `[K]` | Navigate down / up with programmatic off-screen scrolling |
| `Keyboard Swipe` | Capacitive trackpad vertical list scrolling |

---

## Installation

### Method A: Over-the-Air via LocalSend (Recommended)

1. Ensure [LocalSend](https://localsend.org/) is installed and running on your BlackBerry Passport.
2. Ensure your computer and BlackBerry Passport are on the same local Wi-Fi network.
3. Run the automated dispatch script:
   ```bash
   python3 scripts/send-to-passport.py --target <PASSPORT_IP>
   ```
4. Tap **Accept** on the Passport screen. The `.bar` package transfers directly to your device downloads.
5. On the Passport, open the received file in **File Manager** and install via **BAR Opener**.

### Method B: Sideloading via Sachesi / BB10 Command Line

Use standard BlackBerry 10 deployment tools:
```bash
blackberry-deploy -installApp -device <PASSPORT_IP> -password <DEVICE_PASSWORD> build/bb10/todoist-passport.bar
```

---

## Building from Source

### Prerequisites

#### For Host Quality Gate & Testing
- **Host OS**: Linux (Ubuntu 20.04/22.04 or compatible POSIX environment)
- **Host Dependencies**: `cmake` (>= 3.20, required for `ctest --test-dir`), `make`, `perl` (required for OpenSSL host build), `python3` (>= 3.8), and a C/C++ compiler toolchain (gcc/g++ or clang).
*No BlackBerry NDK or cross-toolchain is required to run the automated host test suite.*

#### For BlackBerry 10 BAR Packaging
- **Target Device**: BlackBerry Passport running BlackBerry 10 (OS 10.3.3.3216 / QNX 6.6).
- **Toolchain**: BlackBerry 10 Native SDK (BB10 NDK 10.3.1 cross-compiler).
- **Container Environment**: Docker with the local `bb10-native-dev:local` image (providing the QNX 6.6 / BB10 NDK 10.3.1 toolchain at `/opt/bb10-toolchain.cmake`).
*Note: Because the proprietary BB10 NDK cannot be redistributed publicly, users building target packages must provide their own NDK installation or local Docker container.*

### Build Instructions

1. **Clone the repository**:
   ```bash
   git clone https://github.com/ItsLaurelNotYanny/todoist_on_passport.git
   cd todoist_on_passport
   ```

2. **Acquire pinned upstream sources**:
   ```bash
   python3 scripts/dependencies.py --fetch
   ```

3. **Run local automated quality gate**:
   ```bash
   ./check.sh
   ```
   *The gate verifies dependency checksums, CMake configuration, unit tests (storage, sync, TLS, network, UI), symbol isolation, consumer packaging, asset specifications, and developer path hygiene (9/9 checks).*

4. **Compile BlackBerry 10 BAR package (requires local BB10 Docker environment)**:
   ```bash
   ./scripts/build-bb10.sh
   ```
   *The generated package is placed at `build/bb10/todoist-passport.bar`.*

---

## Technical Documentation

Detailed architecture specifications and RFCs are available in `docs/`:

- [M2: Sync Engine Architecture RFC](docs/M2_SYNC_ENGINE_DESIGN.md)
- [M3: Isolated TLS 1.3 & Network Architecture RFC](docs/M3_TLS_NETWORK_ARCHITECTURE_RFC.md)
- [M4: Cascades UI & Hardware Interaction RFC](docs/M4_CASCADES_UI_SYSTEM_RFC.md)
- [Todoist Sync API Protocol Reference](docs/TODOIST_SYNC_ARCHITECTURE.md)

---

## Project Roadmap

- [ ] **Standalone `posix-tls` Subsystem**: Extract the symbol-isolated TLS 1.3 socket engine into an independent generic library for legacy POSIX and embedded systems.
- [ ] **Multi-account Support**: Support switching between personal and team Todoist accounts.

---

## License & Third-Party Attributions

This project's original code and Cascades QML UI are licensed under the **MIT License**. See [LICENSE](LICENSE) for details. Upstream third-party components and libraries retain their respective licenses.

### Third-Party Components & Licenses

- **SQLite**: Dedicated to the Public Domain ([sqlite.org](https://www.sqlite.org/))
- **PicoJSON**: 2-Clause BSD License ([github.com/kazuho/picojson](https://github.com/kazuho/picojson))
- **bb10_tls**: MIT License (see `share/bb10_tls/LICENSE`)
- **OpenSSL 3.5.9**: Apache License 2.0 ([openssl.org](https://www.openssl.org/source/license.html))
- **libcurl**: curl License / MIT derivative ([curl.se](https://curl.se/docs/copyright.html))

---

## Legal & Trademark Disclaimer

- **Todoist** is a registered trademark of Doist Inc. This project is an independent, unofficial, open-source community port and is not affiliated with, endorsed by, or sponsored by Doist Inc.
- **BlackBerry** and related trademarks, names, and logos are the property of BlackBerry Limited and are registered and/or used in the U.S. and countries around the world.

# Todoist on BlackBerry Passport

An offline-first, native Todoist client engineered exclusively for the **BlackBerry Passport** (BlackBerry 10 OS 10.3.3.3216 / QNX 6.6).

Built strictly in native **C++03 and POSIX C**, with zero Android runtime dependencies, fully custom Cascades QML UI tailored for the 1440×1440 square screen, physical QWERTY keyboard shortcuts, and capacitive trackpad navigation.

---

## Key Features

- **Offline-First Architecture**: Local SQLite 3 WAL storage with AES encryption for your personal API token at rest (the task database itself is standard SQLite format). Local actions queue in `sync_command_queue` and sync upon reconnection.
- **Incremental Sync Engine**: Todoist Sync API v1 client (`https://api.todoist.com/api/v1/sync`) with `sync_token` cursor tracking, optimistic UI updates, and atomic reconciliation (`temp_id_mapping`).
- **Modern TLS 1.3 & Symbol Isolation**: Custom statically linked OpenSSL 3.5.9 + libcurl runtime with symbol hiding (`-fvisibility=hidden`) to isolate it from BlackBerry 10's legacy system SSL libraries.
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

### Requirements and Download

- **Tested device**: A rooted BlackBerry Passport running OS 10.3.3.3216. Release 1.0.7 is an unsigned development BAR (application version `1.0.7.0`); installation on an unrooted device has not been verified for this release.
- **Todoist account**: Create or sign in to your account using [Todoist on the web](https://app.todoist.com/) on a computer, or an officially supported Todoist app. This Passport client syncs with the same account; account registration and token retrieval take place outside it.

Download `todoist-passport-1.0.7.bar` and `SHA256SUMS` from [GitHub Releases](https://github.com/ItsLaurelNotYanny/todoist_on_passport/releases). A source checkout does not contain a BAR. If no binary release is available, follow [Building from Source](#building-from-source) first.

Verify the download from the directory containing both files:

```bash
# Linux
sha256sum -c SHA256SUMS
# macOS
shasum -a 256 -c SHA256SUMS
```

The release 1.0.7 SHA-256 is:

```text
8a4bebc95bdc2c33b39f51af6eb5ad92872cd9d79ce6c754fc4703458cd9f11f
```

### Install the BAR

Install the downloaded BAR using a BB10 installer or deployment tool compatible with your device, such as BAR Opener, Sachesi, or `blackberry-deploy`. Use whichever transfer method your setup requires, and follow your chosen tool's installation and Development Mode instructions.

### First Run: Personal API Token

1. On a computer, sign in to Todoist on the web and open **Settings -> Integrations -> Developer** to copy your own personal API token. See [Todoist's official token instructions](https://developer.todoist.com/).
2. Open this app on the Passport, tap **Token**, paste the token into **Paste Personal API Token**, then tap **Save**.
3. Tap **Sync [R]** or press **R** to synchronize. You can continue using the same account in the web app and official clients.

This app uses a manually supplied personal token; it has no browser OAuth login flow. The saved token is encrypted at rest. Keep it private; the local task database itself is standard SQLite, not an encrypted database.

---

## Building from Source

### Prerequisites

#### For Host Quality Gate & Testing

- **Host OS**: Linux x86_64. The supplied host OpenSSL recipe selects `linux-x86_64`; other host platforms are not covered by this recipe.
- **Host dependencies**: Bash, Git, `cmake` (>= 3.20), `make`, `perl`, `python3` (>= 3.8), a C/C++ compiler toolchain (GCC/G++ or Clang), GNU binutils/coreutils, `tar`, and XZ support.

No BlackBerry SDK or cross-toolchain is required for the host test suite.

#### For BlackBerry 10 BAR Packaging

The supplied packaging script requires **Docker** and the local image **`bb10-native-dev:local`**. Build that image with the included [Dockerfile](docker/Dockerfile.bb10):

```bash
docker build -t bb10-native-dev:local -f docker/Dockerfile.bb10 docker
```

The recipe extends the community [BlackBerry 10 toolchain](https://github.com/XIAZY/blackberry10-toolchain), pinned by image digest, with Perl and pkg-config. It supplies the Clang-based BB10 ARMv7 compiler drivers, BB10 10.3.1 SDK inputs, Qt 4.8/Cascades, `/opt/bb10-toolchain.cmake`, `/opt/bb10-sysroot`, and the native BAR packager. The host needs no separate NDK installation.

Preparing the image and fetching source dependencies require network access; the packaging script runs the prepared container with networking disabled. These instructions are validated on Linux x86_64. Apple Silicon, Docker emulation, and other host platforms have not been validated for this project. The base image is pinned, but added Debian package versions are not locked; rebuilding does not guarantee a byte-identical image or BAR.

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

4. **Prepare the Docker image** using the command in [BAR packaging prerequisites](#for-blackberry-10-bar-packaging), then **compile and package**:
   ```bash
   ./scripts/build-bb10.sh
   ```
   *The script validates the package and writes `build/bb10/todoist-passport.bar`. Rebuilding overwrites this file. A new build can have a different archive checksum; the published checksum identifies the retained release BAR.*

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

This project's original code, including the **bb10_tls** subsystem and Cascades QML UI, is licensed under the **MIT License**. See [LICENSE](LICENSE) and the [bb10_tls license notice](share/bb10_tls/LICENSE) for details. Upstream third-party components and libraries retain their respective licenses.

### Third-Party Components & Licenses

- **SQLite**: Dedicated to the Public Domain ([sqlite.org](https://www.sqlite.org/))
- **PicoJSON**: 2-Clause BSD License ([github.com/kazuho/picojson](https://github.com/kazuho/picojson))
- **OpenSSL 3.5.9**: Apache License 2.0 ([openssl.org](https://www.openssl.org/source/license.html))
- **libcurl**: curl License / MIT derivative ([curl.se](https://curl.se/docs/copyright.html))

---

## Legal & Trademark Disclaimer

- **Todoist** is a registered trademark of Doist Inc. This project is an independent, unofficial, open-source community port and is not affiliated with, endorsed by, or sponsored by Doist Inc.
- **BlackBerry** and related trademarks, names, and logos are the property of BlackBerry Limited and are registered and/or used in the U.S. and countries around the world.

# MUSHclient

[![License](https://img.shields.io/badge/license-MIT--style-blue.svg)](LICENSE)
[![Releases](https://img.shields.io/github/v/release/wadetregaskis/mushclient)](https://github.com/wadetregaskis/mushclient/releases)

MUSHclient is a fast, feature-rich, and highly customizable MUD (Multi-User Dungeon) client.

This repository is an **unofficial fork** of the original [MUSHclient by Nick Gammon](https://github.com/nickgammon/mushclient). It extends the client with native **Linux** and **macOS** support alongside the existing **Windows** support, adds 64-bit compatibility, modernizes the build system, resolves numerous upstream bugs, and updates multi-language scripting support.

> [!NOTE]
> This fork is independently maintained by Wade Tregaskis and is **not endorsed by or affiliated with Nick Gammon**.
>
> If you are looking for the original, official Windows-only release by Nick Gammon, please visit the [official MUSHclient website](http://www.mushclient.com/) or [Gammon Software Solutions](http://www.gammon.com.au/mushclient/).

---

## How to Install

The easiest way to get and run MUSHclient is to download a pre-compiled release from the **[GitHub Releases page](https://github.com/wadetregaskis/mushclient/releases)**.

Pre-built binaries are available for:

* **macOS**:
  Download the `.dmg` installer. Open it and drag **MUSHclient.app** into your `Applications` folder. Compatible with Apple Silicon (M1/M2/M3/M4) and Intel Macs.

* **Linux (Debian, Ubuntu, Linux Mint, etc.)**:
  Download the `.deb` package and install it with:
  ```bash
  sudo dpkg -i mushclient_*.deb
  # or
  sudo apt install ./mushclient_*.deb
  ```
  This automatically registers the desktop launcher, icons, and MIME associations.

* **Linux (Portable / Standalone)**:
  Download the `.tar.xz` archive, extract it anywhere, and launch the application:
  ```bash
  tar -xf mushclient-*.tar.xz
  cd mushclient
  ./mushclient.sh
  ```
  All required runtime libraries, resources, and Lua modules are self-contained.

* **Discworld MUD Edition**:
  Pre-bundled releases featuring Quow's mapper and plugins configured out-of-the-box are also provided under [Releases](https://github.com/wadetregaskis/mushclient/releases).

---

## Features

* **Cross-Platform Native UI**: Clean, native graphical interface on macOS (Cocoa), Linux (wxWidgets GTK3), and Windows.
* **Extensive Scripting Support**: Built-in Lua 5.1 support out of the box, with runtime engine support for Python 3, Perl, JavaScript, Tcl 8.6, and Ruby 4.0.
* **MUD Protocol Support**: ANSI colors, 256-color xterm, 24-bit true color, MXP (Mud eXtension Protocol), MCCP (compression), Telnet subnegotiation, and IPv6.
* **Secure Connections**: Built-in TLS / SSL connection support powered by OpenSSL.
* **High Performance**: Optimized terminal engine capable of handling high-volume combat and spam scrollback with minimal memory overhead.
* **Bug Fixes & Hardening**: Over 100 upstream bugs fixed, including memory leaks, out-of-bounds reads, use-after-free conditions, timer edge cases, and 64-bit portability issues. Tested clean with AddressSanitizer and UndefinedBehaviorSanitizer.
* **Batteries Included**: All core libraries (PCRE, libpng, SQLite 3, zlib, Lua 5.1, miniaudio, spell checker dictionaries, and FIGlet fonts) are included directly in the source repository.

---

## Building from Source

All third-party libraries needed for core functionality (PCRE, libpng, SQLite, zlib, Lua 5.1, miniaudio) are directly bundled in the repository. You do **not** need to manually download or unpack separate dependency archives.

### Prerequisites

* **CMake** 3.14 or later
* A modern C++ compiler supporting C++14 or C++17 (`clang++` or `g++`)

**On Linux (Debian / Ubuntu):**
```bash
sudo apt update
sudo apt install build-essential cmake ninja-build libwxgtk3.2-dev liblua5.1-0-dev libasound2-dev
```
*(Optional development packages for additional scripting engines: `libperl-dev`, `libjavascriptcoregtk-4.1-dev`, `tcl-dev`, `ruby-dev`)*

**On macOS:**
* Install Xcode Command Line Tools:
  ```bash
  xcode-select --install
  ```
* Install dependencies via [Homebrew](https://brew.sh/):
  ```bash
  brew install cmake wxwidgets libpng lua@5.1 openssl@3
  ```

---

### Automated Build with `packaging/build-app.sh`

The recommended and simplest way to build MUSHclient on both macOS and Linux is using the unified packaging script:

```bash
./packaging/build-app.sh
```

This script automatically:
1. Configures and compiles MUSHclient in Release mode.
2. Relocates and bundles shared libraries and runtime resources (dialog templates, artwork, toolbar icons, dictionaries, fonts, and Lua libraries).
3. Produces ready-to-distribute packages in the `dist/` directory:
   * **macOS**: `dist/MUSHclient.app` and `dist/MUSHclient-*.dmg`
   * **Linux**: `dist/mushclient/` (standalone folder), `dist/mushclient-*.tar.xz`, and `dist/mushclient_*.deb`

#### Common Build Options

* **Bundle Discworld MUD Plugins & Maps**:
  ```bash
  ./packaging/fetch-discworld-plugins.sh
  ./packaging/build-app.sh --with-discworld
  ```
* **Debug Build**:
  ```bash
  ./packaging/build-app.sh --build-type Debug
  ```
* **Clean Build**:
  ```bash
  ./packaging/build-app.sh --clean
  ```

---

### Manual Build with CMake

If you prefer building directly with CMake:

```bash
# Configure
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)
```

#### Running the Built Application

* **Linux**:
  ```bash
  ./build/mushclient
  ```
* **macOS**:
  ```bash
  open build/MUSHclient.app
  ```

#### Running the Unit Tests

The test suite includes over 70 unit and regression tests verifying engine logic, script surfaces, protocols, and upstream bug fixes:

```bash
ctest --test-dir build --output-on-failure
```

---

### Extended Scripting Engines (Optional)

In addition to the built-in Lua engine, MUSHclient supports Python 3, Perl, JavaScript, Tcl 8.6, and Ruby 4.0.

For standalone distribution packages without external runtime dependencies, standalone interpreters can be fetched and built using the scripts in `packaging/`:

```bash
./packaging/fetch-python.sh
./packaging/fetch-tcl.sh
./packaging/fetch-ruby.sh
```

When present, `packaging/build-app.sh` automatically incorporates them into the final distribution bundle.

---

## Authors & Credits

* **Nick Gammon** ([Gammon Software Solutions](http://www.gammon.com.au/)): Original author and creator of MUSHclient.
* **Wade Tregaskis**: Cross-platform port (Linux & macOS), 64-bit modernization, bug fixes, and maintenance.

See [credits.txt](src/platform/dialog_text/credits.txt) for a complete list of contributors and component acknowledgments.

---

## License

MUSHclient is open source software released under the terms in the [LICENSE](LICENSE) file.

Copyright (c) 1995–2026 Nick Gammon & Wade Tregaskis. All rights reserved worldwide.

Additional licenses for bundled third-party libraries (PCRE, libpng, SQLite, zlib, Lua, FIGlet, miniaudio) can be found in the [licences/](licences/) directory.

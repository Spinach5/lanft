# lanft — LAN File Transfer Tool

[![Version](https://img.shields.io/badge/version-v1.0.0-blue.svg)](https://gitee.com/dzh258/lanft/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Gitee stars](https://gitee.com/dzh258/lanft/badge/star.svg?theme=dark)](https://gitee.com/dzh258/lanft)
[![GitHub stars](https://img.shields.io/github/stars/Spinach5/lanft?style=social)](https://github.com/Spinach5/lanft)

English | [中文](./README_CN.md)

A fast, reliable LAN file transfer tool with both GUI (SDL2) and CLI modes. Supports TCP/UDP, directory compression, resume, device scanning, and transfer history.

---

## Features

- **Dual mode**: GUI (SDL2) and CLI (terminal)
- **TCP & UDP**: Reliable streaming or custom ACK-based UDP with retransmission
- **Directory transfer**: Auto-compress with libarchive (tar.gz), auto-extract on receive
- **Resume support**: Detects partial files and continues from breakpoint
- **LAN device scanner**: Multi-threaded TCP connect probe across all network interfaces
- **Transfer history**: Persistent records with speed, progress, status saved to disk
- **nc-style roles**: Receiver = server (listens), Sender = client (connects)
- **Cross-platform**: Linux, Windows, macOS, Termux (Android)

---

## Build Options

| CMake Option | Default | Description |
|-------------|---------|-------------|
| `BUILD_GUI` | `ON` | Build with SDL2 GUI. Set `OFF` for CLI-only (no SDL2 dependency, smaller binary). |

```bash
# Full build (GUI + CLI)
cmake .. -DBUILD_GUI=ON

# CLI-only (no SDL2, ideal for servers/embedded)
cmake .. -DBUILD_GUI=OFF
```

---

## Installation

### Linux (Debian/Ubuntu)

```bash
# Prerequisites
sudo apt install -y build-essential cmake git \
    libsdl2-dev libwebsockets-dev libarchive-dev

# Build (GUI)
git clone https://github.com/Spinach5/lan-file-transfer.git lanft
cd lanft && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# CLI-only
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_GUI=OFF
make -j$(nproc)

# Install (optional)
sudo cp lanft /usr/local/bin/
```

### Linux (Fedora/RHEL)

```bash
sudo dnf install -y gcc cmake git \
    SDL2-devel libwebsockets-devel libarchive-devel
# Same build steps as Debian/Ubuntu
```

### Linux (Arch)

```bash
sudo pacman -S --needed base-devel cmake git \
    sdl2 libwebsockets libarchive
# Same build steps as above
```

### Windows (MinGW-w64 via MSYS2)

```bash
# Prerequisites (MSYS2 UCRT64 terminal)
pacman -S mingw-w64-ucrt-x86_64-{cmake,make,gcc,git} \
          mingw-w64-ucrt-x86_64-{SDL2,libwebsockets,libarchive}

# Build (GUI)
git clone https://github.com/Spinach5/lan-file-transfer.git lanft
cd lanft && mkdir build && cd build
cmake .. -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# CLI-only (no SDL2 needed)
cmake .. -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DBUILD_GUI=OFF
make -j$(nproc)
```

### Windows (MSVC + vcpkg)

```powershell
# Prerequisites
vcpkg install sdl2 libwebsockets libarchive

# Build (GUI)
git clone https://github.com/Spinach5/lan-file-transfer.git lanft
cd lanft && mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake
cmake --build . --config Release

# CLI-only
cmake .. -DBUILD_GUI=OFF -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake
cmake --build . --config Release
```

### macOS (Homebrew)

```bash
# Prerequisites
brew install cmake sdl2 libwebsockets libarchive

# Build
git clone https://github.com/Spinach5/lan-file-transfer.git lanft
cd lanft && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Termux (Android)

```bash
pkg update && pkg upgrade
pkg install cmake make clang git binutils \
    sdl2 libwebsockets libarchive termux-x11

cd lanft && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# Run GUI (requires Termux:X11 app)
termux-x11 :0 &
export DISPLAY=:0
./lanft --gui
```

---

## Project Structure

```
websocket/
├── CMakeLists.txt              # Build system
├── README.md
├── .gitignore
└── src/
    ├── main.c                  # SDL GUI entry point & event loop
    ├── main_cli.c              # CLI-only entry point (BUILD_GUI=OFF)
    ├── cli.c                   # CLI argument parsing & terminal transfer
    ├── compat.h                # Cross-platform compatibility (BSD/Winsock)
    ├── network.h / network.c   # libwebsockets raw TCP + plain BSD UDP
    ├── scanner.h / scanner.c   # Multi-threaded LAN TCP Connect scanner
    ├── transfer.h / transfer.c # File send/recv, meta handshake, resume
    ├── protocol.h              # Shared constants, structs, event types
    └── ui.h / ui.c             # SDL2 GUI: tabs, buttons, progress bars
```

### Module Responsibilities

| Module | Purpose | Dependencies |
|--------|---------|-------------|
| `main.c` | SDL2 window, event loop, tab state, thread dispatch | ui, network, scanner, transfer |
| `main_cli.c` | Minimal main for CLI-only builds (no SDL2) | cli |
| `cli.c` | CLI arg parsing (`getopt_long`), progress bar, sync transfer | network, transfer |
| `compat.h` | Cross-platform abstractions (socket_t, Winsock, sleep, gettimeofday) | — |
| `network.c` | TCP via lws raw socket; UDP via BSD `sendto`/`recvfrom` | libwebsockets, protocol.h |
| `scanner.c` | Collects all non-loopback subnets, 32-thread TCP connect scan | pthreads |
| `transfer.c` | Meta handshake, chunked send/recv, resume offset, libarchive compression | network.h, protocol.h, libarchive |
| `ui.c` | Tabbed pages (Scan/Send/Recv/History), text fields, progress bars, 8×16 bitmap font | SDL2 |
| `protocol.h` | Magic bytes, default port, packet headers, event payload structs | — |

### Files Compiled by Mode

| Source | BUILD_GUI=ON | BUILD_GUI=OFF |
|--------|:---:|:---:|
| `main.c` | ✅ | — |
| `main_cli.c` | — | ✅ |
| `ui.c` | ✅ | — |
| `scanner.c` | ✅ | — |
| `cli.c` | ✅ | ✅ |
| `network.c` | ✅ | ✅ |
| `transfer.c` | ✅ | ✅ |
| **Binary size** | ~60 KB | ~40 KB |
| **SDK2 linked** | Yes | No |

---

## Dependencies

| Library | Required | Version | Purpose |
|---------|:---:|---------|---------|
| SDL2 | GUI only | ≥ 2.0 | GUI rendering & event handling |
| libwebsockets | Yes | ≥ 4.0 | TCP raw socket management |
| libarchive | Yes | ≥ 3.0 | Directory compression/extraction (tar.gz) |
| pthreads | Yes | (system) | Multi-threading |
| CMake | Yes | ≥ 3.10 | Build system |

---

## Usage

### GUI Mode

```bash
./lanft --gui
```

Opens SDL2 window with four tabs:

| Tab | Function |
|-----|----------|
| **Scan Devices** | Scan LAN for other lanft instances. Configurable port. Click a device to auto-fill its IP. |
| **Send File** | Select file/directory, choose Dir/File mode, enter **receiver IP**, Start Send. |
| **Receive File** | Select save path, **Listen IP** defaults to `0.0.0.0`. Listen & Receive. |
| **History** | Table of past transfers: name, time, duration, kind, port, status, progress, speed. |

**Typical workflow:**
1. Receiver: Receive File → Browse save dir → Listen & Receive
2. Sender: Send File → Browse file → enter receiver IP → Start Send
3. Both sides show progress bar; completion logged to History

### CLI Mode (default)

CLI is the default mode — no flags needed.

```bash
# Send
lanft --mode=S --address=192.168.1.100 ./report.pdf
lanft -S -p 1234 ./video.mp4
lanft --protocol=UDP --mode=S --address=10.0.0.5 -p 5555 /home/user/docs/

# Receive
lanft --mode=R ./downloads/
lanft -R -p 5555 ./received/
lanft --mode=R --address=10.84.183.2 -p 5555 ./received/

# GUI
lanft --gui

# Info
lanft --help
lanft --version
lanft --history
```

### Options

| Short | Long | Default | Description |
|-------|------|---------|-------------|
| | `--gui` | — | Launch SDL2 GUI (CLI is default) |
| `-h` | `--help` | — | Print help and exit |
| `-v` | `--version` | — | Print version and exit |
| `-S` | — | — | Shorthand for `--mode=S` (send) |
| `-R` | — | — | Shorthand for `--mode=R` (receive) |
| | `--mode=S\|R` | *(required)* | Transfer mode |
| | `--protocol=TCP\|UDP` | TCP | Transport protocol |
| `-p` | `--port=NUM` | 9876 | Port number |
| | `--address=IP` | `0.0.0.0` | Send: receiver IP. Recv: listen IP |
| | `--history` | — | Print history table |

---

## How It Works

### Protocol

1. **Meta handshake**: magic `FT01`, protocol type, filename, total size, flags
2. **Meta response**: magic + resume offset
3. **Data transfer**: raw bytes (TCP) or chunked packets with ACK (UDP)
4. Directory mode: data is a `.tar.gz` archive auto-extracted on receive

### Role Model (nc-style)

```
Receiver (server):  net_listen → net_accept → receive meta → receive data
Sender   (client):  net_connect → send meta → read response → send data
```

### Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Transfer completed successfully |
| 1 | Error (bad args, connection failed, transfer interrupted) |

---

## License

MIT

## Star History
[![Star History Chart](https://api.star-history.com/chart?repos=Spinach5/lanft&type=date&logscale&legend=top-left)](https://www.star-history.com/?repos=Spinach5%2Flanft&type=date&logscale=&legend=top-left)
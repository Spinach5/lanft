# lanft — LAN File Transfer Tool
[![Version](https://img.shields.io/badge/version-v1.0.0-blue.svg)](https://gitee.com/dzh258/lan-file-transfer/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Gitee stars](https://gitee.com/dzh258/lan-file-transfer/badge/star.svg?theme=dark)](https://gitee.com/dzh258/lan-file-transfer)
[![GitHub stars](https://img.shields.io/github/stars/Spinach5/lan-file-transfer?style=social)](https://github.com/Spinach5/lan-file-transfer)

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
- **Cross-platform**: Linux (x86_64, aarch64), Termux (Android), WSL

---

## Project Structure

```
websocket/
├── CMakeLists.txt              # Build system
├── README.md
├── .gitignore
├── docs/
│   └── superpowers/
│       ├── specs/              # Design specifications
│       └── plans/              # Implementation plans
└── src/
    ├── main.c                  # Entry point, SDL event loop, thread spawning
    ├── cli.c                   # CLI argument parsing & terminal transfer
    ├── network.h / network.c   # libwebsockets raw TCP + plain BSD UDP
    ├── scanner.h / scanner.c   # Multi-threaded LAN TCP Connect scanner
    ├── transfer.h / transfer.c # File send/recv, meta handshake, resume, compression
    ├── protocol.h              # Shared constants, structs, event types
    └── ui.h / ui.c             # SDL2 GUI: tabs, buttons, text fields, progress bars
```

### Module Responsibilities

| Module | Purpose | Dependencies |
|--------|---------|-------------|
| `main.c` | SDL2 window, event loop, tab state, thread dispatch | ui, network, scanner, transfer |
| `cli.c` | CLI arg parsing (`getopt_long`), progress bar, sync transfer | network, transfer |
| `network.c` | TCP via lws raw socket; UDP via BSD `sendto`/`recvfrom` | libwebsockets, protocol.h |
| `scanner.c` | Collects all non-loopback /24 subnets, 32-thread TCP connect scan | pthreads, SDL (events) |
| `transfer.c` | Meta handshake, chunked send/recv, resume offset, libarchive compression | network.h, protocol.h, libarchive |
| `ui.c` | Tabbed pages (Scan/Send/Recv/History), text fields, progress bars, 8x16 bitmap font | SDL2 |
| `protocol.h` | Magic bytes, default port, packet headers, event payload structs | — |

---

## Dependencies

### Required

| Library | Version | Purpose |
|---------|---------|---------|
| SDL2 | ≥ 2.0 | GUI rendering & event handling |
| libwebsockets | ≥ 4.0 | TCP raw socket management |
| libarchive | ≥ 3.0 | Directory compression/extraction (tar.gz) |
| pthreads | (system) | Multi-threading |
| CMake | ≥ 3.10 | Build system |

### Optional (CLI only — no GUI deps needed)

| Tool | Purpose |
|------|---------|
| `getopt_long` | Argument parsing (glibc built-in) |

---

## Installation

### Linux (Debian/Ubuntu)

```bash
# Install build dependencies
sudo apt install -y build-essential cmake git \
    libsdl2-dev libwebsockets-dev libarchive-dev

# Clone and build
git clone <repo-url> lanft
cd lanft
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# Install (optional)
sudo cp lanft /usr/local/bin/
```

### Linux (Fedora/RHEL)

```bash
sudo dnf install -y gcc cmake git \
    SDL2-devel libwebsockets-devel libarchive-devel
# ... same build steps as above
```

### Arch Linux

```bash
sudo pacman -S --needed base-devel cmake git \
    sdl2 libwebsockets libarchive
# ... same build steps as above
```

### Termux (Android)

```bash
pkg update && pkg upgrade
pkg install cmake make clang git binutils \
    sdl2 libwebsockets libarchive termux-x11

# Build
cd lanft && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# Run GUI (requires Termux:X11 app)
termux-x11 :0 &
export DISPLAY=:0
./lanft
```

---

## Usage

### GUI Mode (no arguments)

```bash
./lanft
```

Opens SDL2 window with four tabs:

| Tab | Function |
|-----|----------|
| **Scan Devices** | Scan LAN for other lanft instances. Configurable port. Click a device to auto-fill its IP. |
| **Send File** | Select file/directory, choose Dir/File mode, enter **receiver IP**, Start Send. Stop or Esc to cancel. |
| **Receive File** | Select save path, **Listen IP** defaults to `0.0.0.0`. Listen & Receive. Stop or Esc to cancel. |
| **History** | Table of past transfers with name, time, duration, kind, port, status, progress, speed. |

**Typical workflow:**
1. Receiver: Receive File → Browse save dir → Listen & Receive (waits for sender)
2. Sender: Send File → Browse file → enter receiver IP → Start Send
3. Both sides show progress bar; completion logged to History

### CLI Mode (any arguments)

```bash
# Send a file (TCP, default port 9876)
lanft --mode=S --address=192.168.1.100 ./report.pdf

# Send a directory (UDP, custom port)
lanft --protocol=UDP --mode=S --address=10.0.0.5 -p 5555 /home/user/docs/

# Short options: send
lanft -S -p 1234 ./video.mp4

# Receive (listen on 0.0.0.0:9876, save to ./downloads/)
lanft --mode=R ./downloads/

# Receive on specific interface and port
lanft --mode=R --address=10.84.183.2 -p 5555 ./received/

# Show help
lanft --help

# Show version
lanft --version

# Show transfer history
lanft --history
```

### Options Reference

| Short | Long | Default | Description |
|-------|------|---------|-------------|
| `-h` | `--help` | — | Print help and exit |
| `-v` | `--version` | — | Print version and exit |
| `-S` | `--mode=S` | *(required)* | Shorthand for `--mode=S` (send) |
| `-R` | `--mode=R` | *(required)* | Shorthand for `--mode=R` (receive) |
| | `--mode=S\|R` | *(required)* | Transfer mode |
| | `--protocol=TCP\|UDP` | TCP | Transport protocol |
| `-p` | `--port=NUM` | 9876 | Port number |
| | `--address=IP` | `0.0.0.0` | Send: receiver IP. Recv: listen IP |
| | `--history` | — | Print history table and exit |

---

## How It Works

### Protocol

1. **Meta handshake** (270 bytes): magic `FT01`, protocol type, filename, total size, flags (directory indicator)
2. **Meta response** (16 bytes): magic + resume offset
3. **Data transfer**: raw bytes (TCP) or chunked packets with ACK (UDP)
4. If flags indicate directory, the data is a `.tar.gz` archive auto-extracted on receive

### Role Model (nc-style)

```
Receiver (server):  net_listen → net_accept → receive meta → receive data
Sender   (client):  net_connect → send meta → read response → send data
```

### Scanner

- Reads all non-loopback network interfaces via `getifaddrs()`
- Extracts /24 subnet from each IP
- Spawns 32 threads, each probing IPs via non-blocking TCP connect + 800ms `select()` timeout
- Works across LAN, WiFi, VPN (tun), and virtual bridges

### Resume

- Sender sends file metadata including total size
- Receiver checks local filesystem for existing partial file
- If found and smaller, replies with current byte offset
- Sender seeks to offset and resumes streaming
- Works for both TCP and UDP

---

## Exit Codes (CLI)

| Code | Meaning |
|------|---------|
| 0 | Transfer completed successfully |
| 1 | Error (bad args, connection failed, transfer interrupted) |

---

## License

MIT

## Star History
[![Star History Chart](https://api.star-history.com/chart?repos=Spinach5/lan-file-transfer&type=date&logscale&legend=top-left)](https://www.star-history.com/?repos=Spinach5%2Flan-file-transfer&type=date&logscale=&legend=top-left)
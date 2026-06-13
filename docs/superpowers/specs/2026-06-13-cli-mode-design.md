# CLI Mode — Design Spec

**Date:** 2026-06-13
**Goal:** Add command-line mode to existing LAN file transfer tool. When args passed, run CLI (no SDL). When no args, run GUI (unchanged).

---

## 1. Architecture

```
main.c ── argc>1? ──→ cli_main(argc, argv)    ← CLI path (no SDL init)
        │
        └── argc==1? ──→ existing SDL loop      ← GUI path (unchanged)
```

New file `src/cli.c` handles all CLI logic. Reuses `network.c`, `transfer.c` directly — no changes to network module.

---

## 2. Argument Parsing (`src/cli.c`)

Uses `getopt_long`. Config struct:

```c
struct cli_config {
    int  protocol;      // FT_PROTO_TCP (0) or FT_PROTO_UDP (1), default TCP
    int  mode;          // 0=S (send), 1=R (receive)
    int  port;          // default 9876
    char address[64];   // default "0.0.0.0"
    char path[1024];    // positional: file/dir to send, or save dir
    bool show_history;
    bool show_help;
    bool show_version;
};
```

| Short | Long | Arg | Description |
|-------|------|-----|-------------|
| `-h` | `--help` | — | Print help and exit |
| `-v` | `--version` | — | Print version and exit |
| `-S` | — | — | Shortcut for `--mode=S` |
| `-R` | — | — | Shortcut for `--mode=R` |
| | `--protocol` | TCP/UDP | Default TCP |
| | `--mode` | S/R | Required |
| `-p` | `--port` | num | Default 9876 |
| | `--address` | IP | Send: required; Recv: default 0.0.0.0 |
| | `--history` | — | Print history and exit |

Command format:
```
lanft --mode=S [--address=IP] [-p PORT] FILE_OR_DIR
lanft --mode=R [--address=IP] [-p PORT] SAVE_DIR
```

Validation rules:
- `--mode` required → else error + help
- Send: file/dir must exist
- Recv: save dir must exist
- Send: `--address` required if not provided

---

## 3. Transfer Flow

CLI runs synchronous in main thread (no pthread, no SDL events).

**Send (client):**
```
net_create(TCP) → net_connect(address, port) [retry until connected or timeout]
→ transfer_send(nc, path, protocol) → print summary → exit
```

**Receive (server):**
```
net_create(TCP) → net_listen_ip(address, port) → net_accept(nc) [wait for sender]
→ transfer_recv(nc, path, protocol) → print summary → exit
```

---

## 4. Callback Integration (`transfer.c/h`)

`transfer.c` currently pushes SDL events directly. Added callback mechanism:

```c
typedef void (*transfer_progress_fn)(uint64_t done, uint64_t total);
typedef void (*transfer_error_fn)(const char *msg);
typedef void (*transfer_done_fn)(void);

void transfer_set_callbacks(transfer_progress_fn prog,
                            transfer_error_fn err,
                            transfer_done_fn done);
```

- **GUI path:** callbacks push SDL_UserEvent (default behavior)
- **CLI path:** callbacks print progress bar to stderr

Progress bar format: `[====>     ] 45%  1.2MB / 2.7MB  3.4MB/s`

Summary on completion: prints filename, size, duration, speed. Exit code 0 = success, 1 = error.

---

## 5. Files Changed

| File | Change |
|------|--------|
| `src/cli.c` | **New** — arg parsing, CLI orchestration, progress printing |
| `src/main.c` | Add `cli_main()` call when `argc > 1`; init default SDL callbacks |
| `src/transfer.h` | Add callback typedefs + `transfer_set_callbacks()` |
| `src/transfer.c` | Replace `push_progress/push_error/push_xfer_done` with callback calls |
| `CMakeLists.txt` | Add `src/cli.c` |

---

## 6. Examples

```bash
# Send file via TCP
lanft --mode=S --address=192.168.1.100 ./report.pdf

# Send directory via UDP
lanft --protocol=UDP --mode=S --address=10.0.0.5 -p 5555 /home/user/docs/

# Short options: send
lanft -S -p 1234 ./video.mp4

# Receive via TCP (listen on 0.0.0.0:9876)
lanft --mode=R ./downloads/

# Receive via UDP
lanft --protocol=UDP --mode=R --address=0.0.0.0 -p 5555 ./received/
```

---

## 7. YAGNI

- Interactive CLI prompt
- `--scan` for device scanning
- Multiple file transfer
- Manual resume offset

#include "transfer.h"
#include "network.h"
#include "protocol.h"
#include "compat.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <archive.h>
#include <archive_entry.h>
#ifdef BUILD_GUI
#include <SDL2/SDL.h>
#endif

/* ── Callbacks ─────────────────────────────────────────────── */

static transfer_progress_fn g_prog_cb = NULL;
static transfer_error_fn    g_err_cb  = NULL;
static transfer_done_fn     g_done_cb = NULL;
static transfer_accept_fn   g_accept_cb = NULL;
static bool                 g_auto_accept = true;
volatile int                g_accept_response = 0;
volatile bool               g_accept_pending = false;

void transfer_accept(void)
{
    g_accept_response = 1;
    g_accept_pending = false;
}

void transfer_reject(void)
{
    g_accept_response = -1;
    g_accept_pending = false;
}

/* ── Runtime config ─────────────────────────────────────────── */
#define MAX_CHUNK_SIZE  (1024 * 1024)   /* 1 MiB ceiling */
static uint8_t  g_chunk_buf[MAX_CHUNK_SIZE];
static int      g_buffer_size          = 65536;
static int      g_timeout_seconds      = 30;
static int      g_bandwidth_limit      = 0;    /* 0 = unlimited */
static int      g_max_connections      = 0;    /* 0 = unlimited */
static char     g_overwrite_policy[16] = "rename";

void transfer_set_buffer_size(int size)
{
    if (size > 0 && size <= MAX_CHUNK_SIZE)
        g_buffer_size = size;
}

void transfer_set_timeout(int seconds)
{
    if (seconds >= 0)
        g_timeout_seconds = seconds;
}

void transfer_set_overwrite_policy(const char *policy)
{
    if (policy && policy[0])
        strncpy(g_overwrite_policy, policy, sizeof(g_overwrite_policy) - 1);
}

void transfer_set_bandwidth_limit(int limit)
{
    g_bandwidth_limit = (limit > 0) ? limit : 0;
}

void transfer_set_max_connections(int max_conn)
{
    g_max_connections = (max_conn > 0) ? max_conn : 0;
}

void transfer_set_callbacks(transfer_progress_fn prog,
                            transfer_error_fn err,
                            transfer_done_fn done)
{
    g_prog_cb = prog;
    g_err_cb  = err;
    g_done_cb = done;
}

void transfer_set_auto_accept(bool enabled)
{
    g_auto_accept = enabled;
}

void transfer_set_accept_callback(transfer_accept_fn cb)
{
    g_accept_cb = cb;
}

/* ── Last received filename (for persistent listener history) ── */

static char g_last_recv_name[FT_MAX_FILENAME];

const char *transfer_last_recv_name(void)
{
    return g_last_recv_name[0] ? g_last_recv_name : NULL;
}

/* ── Helpers ───────────────────────────────────────────────── */

#ifdef BUILD_GUI
static void push_event(int code, void *data)
{
    SDL_Event event;
    SDL_memset(&event, 0, sizeof(event));
    event.type = code;
    event.user.data1 = data;
    SDL_PushEvent(&event);
}
#endif

static void push_error(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_err_cb) { g_err_cb(buf); return; }
#ifdef BUILD_GUI
    struct event_error *err = calloc(1, sizeof(*err));
    if (!err) return;
    strncpy(err->message, buf, sizeof(err->message) - 1);
    push_event(SDL_USEREVENT + 5, err);
#else
    log_write("Error: %s\n", buf);
#endif
}

static void push_progress(uint64_t done, uint64_t total)
{
    if (g_prog_cb) { g_prog_cb(done, total); return; }
#ifdef BUILD_GUI
    struct event_progress *p = calloc(1, sizeof(*p));
    if (!p) return;
    p->bytes_done = done;
    p->bytes_total = total;
    push_event(SDL_USEREVENT + 3, p);
#endif
}

static void push_xfer_done(void)
{
    if (g_done_cb) { g_done_cb(); return; }
#ifdef BUILD_GUI
    push_event(SDL_USEREVENT + 4, NULL);
#endif
}

static uint64_t file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (uint64_t)st.st_size;
}

/* ── Helpers ───────────────────────────────────────────────── */

static uint64_t now_ms_transfer(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* ── Meta exchange ─────────────────────────────────────────── */

static int send_meta(struct net_context *nc, const char *filename,
                     uint64_t size, int proto)
{
    struct ft_meta meta;
    memset(&meta, 0, sizeof(meta));
    meta.magic = FT_MAGIC;
    meta.protocol = (uint8_t)proto;
    meta.name_len = (uint8_t)strlen(filename);
    strncpy(meta.filename, filename, sizeof(meta.filename) - 1);
    meta.total_size = size;
    return net_send(nc, &meta, sizeof(meta));
}

/* ── TCP Send ──────────────────────────────────────────────── */

/* ── TCP send with direct socket I/O ──────────────────────── */

static int sock_read_full(socket_t fd, void *buf, size_t len, int timeout_ms)
{
    size_t received = 0;
    uint8_t *p = (uint8_t *)buf;
    while (received < len) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        int ret = select(fd + 1, &fds, NULL, NULL, &tv);
        if (ret < 0) { log_write("[sock_read_full] select error\n"); return -1; }
        if (ret == 0) {
            log_write("[sock_read_full] timeout after %dms, got %zu/%zu bytes\n",
                      timeout_ms, received, len);
            return -1;
        }
        ssize_t n = sock_read(fd, p + received, len - received);
        if (n <= 0) {
            log_write("[sock_read_full] recv EOF/error after %zu bytes (n=%zd)\n",
                      received, n);
            return -1;
        }
        received += n;
    }
    return 0;
}

static int sock_write_full(socket_t fd, const void *buf, size_t len)
{
    size_t sent = 0;
    const uint8_t *p = (const uint8_t *)buf;
    while (sent < len) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        /* Wait for writability (use config timeout) */
        struct timeval tv = {g_timeout_seconds > 0 ? g_timeout_seconds : 5, 0};
        int ret = select(fd + 1, NULL, &fds, NULL, &tv);
        if (ret <= 0) return -1;
        ssize_t n = sock_write(fd, p + sent, len - sent);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

/* ── libarchive directory compression/extraction ───────────── */

static int add_to_archive(struct archive *a, const char *disk_path, const char *arc_path)
{
    struct stat st;
    if (lstat(disk_path, &st) != 0) return -1;

    struct archive_entry *entry = archive_entry_new();
    archive_entry_set_pathname(entry, arc_path);
    archive_entry_set_size(entry, st.st_size);
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, st.st_mode & 0777);
    archive_write_header(a, entry);

    FILE *fp = fopen(disk_path, "rb");
    if (fp) {
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
            archive_write_data(a, buf, n);
        fclose(fp);
    }
    archive_entry_free(entry);
    return 0;
}

static void walk_and_add(struct archive *a, const char *disk_base, const char *arc_base)
{
    DIR *d = opendir(disk_base);
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        char full_disk[1280], full_arc[1280];
        snprintf(full_disk, sizeof(full_disk), "%s/%s", disk_base, de->d_name);
        snprintf(full_arc, sizeof(full_arc), "%s/%s", arc_base, de->d_name);

        struct stat st;
        if (lstat(full_disk, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            walk_and_add(a, full_disk, full_arc);
        } else if (S_ISREG(st.st_mode)) {
            add_to_archive(a, full_disk, full_arc);
        }
    }
    closedir(d);
}

static char *compress_dir_to_tmp(const char *dirpath, uint64_t *out_size)
{
    char tmpfile[256];
#ifdef _WIN32
    snprintf(tmpfile, sizeof(tmpfile), "lanft_%d.tar.gz", (int)GetCurrentProcessId());
#else
    snprintf(tmpfile, sizeof(tmpfile), "/tmp/lanft_%d.tar.gz", (int)getpid());
#endif

    struct archive *a = archive_write_new();
    archive_write_add_filter_gzip(a);
    archive_write_set_format_pax_restricted(a);
    if (archive_write_open_filename(a, tmpfile) != ARCHIVE_OK) {
        archive_write_free(a);
        return NULL;
    }

    const char *dirname = strrchr(dirpath, '/');
    if (dirname) dirname++; else dirname = dirpath;
    walk_and_add(a, dirpath, dirname);

    archive_write_close(a);
    archive_write_free(a);

    struct stat st;
    if (stat(tmpfile, &st) != 0) { unlink(tmpfile); return NULL; }
    *out_size = (uint64_t)st.st_size;
    return strdup(tmpfile);
}

static int extract_archive(const char *arc_path, const char *dest_dir)
{
    struct archive *a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    if (archive_read_open_filename(a, arc_path, 65536) != ARCHIVE_OK) {
        archive_read_free(a);
        return -1;
    }

    char curdir[1024];
    if (!getcwd(curdir, sizeof(curdir))) { curdir[0] = '/'; curdir[1] = '\0'; }
    mkdir(dest_dir, 0755);
    chdir(dest_dir);

    struct archive_entry *entry;
    int ret;
    while ((ret = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        const char *path = archive_entry_pathname(entry);
        /* Create parent directories if needed */
        char *slash = strrchr(path, '/');
        if (slash) { *slash = '\0'; mkdir(path, 0755); /* best effort */ *slash = '/'; }

        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            const void *buf;
            size_t size;
            int64_t offset;
            while ((ret = archive_read_data_block(a, &buf, &size, &offset)) == ARCHIVE_OK) {
                write(fd, buf, size);
            }
            close(fd);
        }
    }
    archive_read_free(a);
    chdir(curdir);
    return 0;
}

static int tcp_send_file(struct net_context *nc, const char *filepath,
                          uint64_t resume_offset, bool is_dir)
{
    socket_t fd = net_get_fd(nc);
    log_write("[SEND] tcp_send_file: fd=%d, file=%s\n", fd, filepath);
    if (fd < 0) { log_write("[SEND] BAD FD!\n"); push_error("No socket"); return -2; }

    /* Get file size (path is already prepared — always a regular file) */
    FILE *fp = fopen(filepath, "rb");
    if (!fp) { push_error("Cannot open: %s", filepath); return -2; }
    fseek(fp, 0, SEEK_END);
    uint64_t total = (uint64_t)ftell(fp);
    fclose(fp);

    const char *fname = strrchr(filepath, '/');
    const char *bs    = strrchr(filepath, '\\');
    if (bs > fname) fname = bs;
    if (fname) fname++; else fname = filepath;

    /* Send meta */
    struct ft_meta meta;
    memset(&meta, 0, sizeof(meta));
    meta.magic = FT_MAGIC;
    meta.protocol = FT_PROTO_TCP;
    meta.flags = is_dir ? 1 : 0;
    strncpy(meta.filename, fname, sizeof(meta.filename) - 1);
    meta.name_len = (uint8_t)strlen(meta.filename);
    meta.total_size = total;

    log_write("[SEND] sending meta (name=%s, size=%lu, dir=%d)...\n",
            meta.filename, (unsigned long)total, is_dir);
    if (sock_write_full(fd, &meta, sizeof(meta)) != 0) {
        push_error("Failed to send file metadata");
        return -1;
    }
    log_write("[SEND] meta sent OK\n");

    /* Read meta response */
    log_write("[SEND] waiting for meta response...\n");
    struct ft_meta_resp resp;
    memset(&resp, 0, sizeof(resp));
    int meta_timeout_ms = (g_timeout_seconds > 0) ? g_timeout_seconds * 1000 : 300000;
    if (sock_read_full(fd, &resp, sizeof(resp), meta_timeout_ms) == 0 &&
        resp.magic == FT_MAGIC) {
        resume_offset = resp.resume_offset;
        log_write("[SEND] got meta response, resume_offset=%lu\n", (unsigned long)resume_offset);
    } else {
        log_write("[SEND] no meta response, likely scanner\n");
        return -1;
    }

    if (resume_offset >= total && total > 0) {
        log_write("[SEND] already complete on receiver, done\n");
        push_xfer_done();
        return 0;
    }

    /* Send data */
    uint64_t sent = resume_offset;
    {
        FILE *fp = fopen(filepath, "rb");
        if (!fp) { push_error("Cannot open: %s", filepath); return -2; }
        fseek(fp, (long)resume_offset, SEEK_SET);
        sent = resume_offset;

        while (sent < total) {
            size_t to_read = (total - sent) > (uint64_t)g_buffer_size
                             ? (size_t)g_buffer_size : (size_t)(total - sent);
            size_t n = fread(g_chunk_buf, 1, to_read, fp);
            if (n == 0) break;
            if (sock_write_full(fd, g_chunk_buf, n) != 0) {
                push_error("Send failed at %lu / %lu bytes",
                           (unsigned long)sent, (unsigned long)total);
                fclose(fp);
                return -2;
            }
            sent += n;
            log_write("[SEND] progress %lu/%lu\n", (unsigned long)sent, (unsigned long)total);
            push_progress(sent, total);

            /* Bandwidth throttling */
            if (g_bandwidth_limit > 0) {
                static uint64_t throttle_start_ms = 0;
                if (sent == n) throttle_start_ms = now_ms_transfer();
                uint64_t elapsed = now_ms_transfer() - throttle_start_ms;
                uint64_t target_ms = (sent * 1000ULL) / (uint64_t)g_bandwidth_limit;
                if (elapsed < target_ms) {
                    usleep((unsigned int)((target_ms - elapsed) * 1000));
                }
            }
        }
        fclose(fp);
    }

    log_write("[SEND] transfer done, sent=%lu/%lu\n", (unsigned long)sent, (unsigned long)total);
    if (sent >= total) push_xfer_done();
    return (sent >= total) ? 0 : -2;
}

/* ── TCP receive with direct socket I/O ───────────────────── */

static void tcp_recv_file(struct net_context *nc, const char *savepath)
{
    socket_t fd = net_get_fd(nc);
    log_write("[RECV] tcp_recv_file: fd=%d, save=%s\n", fd, savepath);
    if (fd < 0) { log_write("[RECV] BAD FD!\n"); push_error("No socket"); return; }

    /* Read meta — use a short initial timeout to filter out scanner probes.
       Scanners connect and immediately close without sending data, which
       causes select() to signal readability (EOF) within milliseconds. */
    log_write("[RECV] waiting for meta...\n");
    struct ft_meta meta;
    /* Use configured timeout. Scanner probes close immediately (EOF in μs).
       Slow senders (e.g. compressing large dirs) get the full timeout. */
    int meta_timeout_ms = (g_timeout_seconds > 0) ? g_timeout_seconds * 1000 : 120000;
    if (sock_read_full(fd, &meta, sizeof(meta), meta_timeout_ms) != 0) {
        log_write("[RECV] no meta in %dms, likely scanner probe — ignoring\n",
                  meta_timeout_ms);
        return;
    }
    log_write("[RECV] got meta: name=%s, size=%lu, flags=%d\n",
            meta.filename, (unsigned long)meta.total_size, meta.flags);

    if (meta.magic != FT_MAGIC) {
        push_error("Protocol mismatch — bad magic bytes");
        return;
    }

    strncpy(g_last_recv_name, meta.filename, sizeof(g_last_recv_name) - 1);

    bool is_dir = (meta.flags & 0x01) != 0;

    /* Determine output path and calculate resume offset */
    char rootpath[1024];
    snprintf(rootpath, sizeof(rootpath), "%s/%s", savepath, meta.filename);
    uint64_t local_size = file_size(rootpath);
    uint64_t resume_offset = 0;
    if (!is_dir && local_size > 0 && local_size < meta.total_size) {
        resume_offset = local_size;
    }

    /* ── Auto-accept check (BEFORE meta response — sender has 5min timeout) */
    if (!g_auto_accept && g_accept_cb) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        char sender_ip[64] = "unknown";
        char sender_host[256] = "";
        if (getpeername(fd, (struct sockaddr *)&peer, &peer_len) == 0) {
            inet_ntop(AF_INET, &peer.sin_addr, sender_ip, sizeof(sender_ip));
            struct sockaddr_in sa;
            memset(&sa, 0, sizeof(sa));
            sa.sin_family = AF_INET;
            sa.sin_addr = peer.sin_addr;
            getnameinfo((const struct sockaddr *)&sa, sizeof(sa),
                        sender_host, sizeof(sender_host), NULL, 0, 0);
        }
        log_write("[RECV] asking user to accept transfer from %s (%s)...\n",
                sender_ip, sender_host);
        if (!g_accept_cb(sender_ip, sender_host, meta.filename, meta.total_size)) {
            log_write("[RECV] transfer rejected by user\n");
            push_error("Transfer rejected by user");
            return;
        }
        log_write("[RECV] transfer accepted by user\n");
    }

    /* Send meta response now that user has accepted.
       Sender waits up to 5 minutes for this. */
    {
        struct ft_meta_resp resp;
        memset(&resp, 0, sizeof(resp));
        resp.magic = FT_MAGIC;
        resp.resume_offset = resume_offset;
        if (sock_write_full(fd, &resp, sizeof(resp)) != 0) {
            push_error("Failed to send meta response");
            return;
        }
        log_write("[RECV] meta response sent, resume_offset=%lu\n",
                (unsigned long)resume_offset);
    }

    log_write("[RECV] ready for data, total=%lu, is_dir=%d\n",
            (unsigned long)meta.total_size, is_dir);

    /* Receive file data (as single file) */
    uint64_t received = resume_offset;

    /* ── Overwrite policy ────────────────────────────────── */
    char final_path[1024];
    strncpy(final_path, rootpath, sizeof(final_path) - 1);

    if (resume_offset == 0) {
        struct stat exist_st;
        if (stat(rootpath, &exist_st) == 0) {
            if (strcmp(g_overwrite_policy, "skip") == 0) {
                log_write("[RECV] file exists, skipping: %s\n", rootpath);
                push_error("File already exists (overwrite_policy=skip)");
                return;
            } else if (strcmp(g_overwrite_policy, "rename") == 0) {
                /* Generate a new name: file (1).ext, file (2).ext, ... */
                char base[900], ext[128];
                const char *dot = strrchr(meta.filename, '.');
                if (dot && dot != meta.filename) {
                    size_t blen = dot - meta.filename;
                    if (blen > sizeof(base) - 1) blen = sizeof(base) - 1;
                    memcpy(base, meta.filename, blen); base[blen] = '\0';
                    strncpy(ext, dot, sizeof(ext) - 1);
                } else {
                    strncpy(base, meta.filename, sizeof(base) - 1);
                    ext[0] = '\0';
                }
                int n = 1;
                while (n < 1000) {
                    snprintf(final_path, sizeof(final_path), "%s/%s (%d)%s",
                             savepath, base, n, ext);
                    if (stat(final_path, &exist_st) != 0) break;
                    n++;
                }
                log_write("[RECV] file exists, renamed to: %s\n", final_path);
            }
            /* "overwrite" → just use rootpath as-is (wb mode truncates) */
        }
    }

    FILE *fp = fopen(final_path, resume_offset > 0 ? "ab" : "wb");
    if (!fp) {
        push_error("Cannot create file: %s", final_path);
        return;
    }
    if (resume_offset > 0) fseek(fp, (long)resume_offset, SEEK_SET);

    while (received < meta.total_size) {
        size_t to_read = (meta.total_size - received) > (uint64_t)g_buffer_size
                         ? (size_t)g_buffer_size : (size_t)(meta.total_size - received);

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        struct timeval tv = {g_timeout_seconds > 0 ? g_timeout_seconds : 30, 0};
        if (select(fd + 1, &fds, NULL, NULL, &tv) <= 0) {
            push_error("Receive timeout");
            fclose(fp);
            return;
        }

        ssize_t n = sock_read(fd, g_chunk_buf, to_read);
        if (n <= 0) {
            push_error("Receive failed");
            fclose(fp);
            return;
        }

        fwrite(g_chunk_buf, 1, n, fp);
        received += n;
        log_write("[RECV] progress %lu/%lu\n", (unsigned long)received, (unsigned long)meta.total_size);
        push_progress(received, meta.total_size);
    }
    fclose(fp);

    /* If directory: extract the .tar.gz archive */
    if (is_dir && received >= meta.total_size) {
        log_write("[RECV] extracting archive %s to %s...\n", final_path, savepath);
        if (extract_archive(final_path, savepath) == 0) {
            log_write("[RECV] extraction complete\n");
            unlink(final_path);  /* remove temp archive */
        } else {
            push_error("Failed to extract archive");
            return;
        }
    }

    log_write("[RECV] transfer done, received=%lu/%lu\n", (unsigned long)received, (unsigned long)meta.total_size);
    push_xfer_done();
}

/* ── UDP Send ──────────────────────────────────────────────── */

static void udp_send_file(struct net_context *nc, const char *filepath)
{
    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        push_error("Cannot open file: %s", filepath);
        return;
    }

    fseek(fp, 0, SEEK_END);
    uint64_t total = (uint64_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);

    const char *fname = strrchr(filepath, '/');
    const char *bs    = strrchr(filepath, '\\');
    if (bs > fname) fname = bs;   /* Windows backslash */
    if (fname) fname++; else fname = filepath;

    /* Send meta 3 times for reliability */
    struct ft_meta meta;
    memset(&meta, 0, sizeof(meta));
    meta.magic = FT_MAGIC;
    meta.protocol = FT_PROTO_UDP;
    meta.name_len = (uint8_t)strlen(fname);
    strncpy(meta.filename, fname, sizeof(meta.filename) - 1);
    meta.total_size = total;

    for (int i = 0; i < 3; i++) {
        net_send(nc, &meta, sizeof(meta));
        usleep(50000);
    }

    /* Wait for meta response */
    struct ft_meta_resp resp;
    memset(&resp, 0, sizeof(resp));
    bool got_resp = false;
    for (int retry = 0; retry < 20; retry++) {
        int n = net_udp_recv(nc, &resp, sizeof(resp), 250);
        if (n == sizeof(resp) && resp.magic == FT_MAGIC) {
            got_resp = true;
            break;
        }
    }
    if (!got_resp) resp.resume_offset = 0;

    /* Calculate packet count */
    uint64_t remaining = total - resp.resume_offset;
    uint32_t total_pkts = (uint32_t)((remaining + FT_CHUNK_SIZE - 1) / FT_CHUNK_SIZE);
    if (remaining == 0) total_pkts = 1;

    fseek(fp, (long)resp.resume_offset, SEEK_SET);

    bool *acked = calloc(total_pkts + 1, sizeof(bool));
    int *retries_arr = calloc(total_pkts + 1, sizeof(int));
    if (!acked || !retries_arr) {
        push_error("Out of memory");
        free(acked); free(retries_arr); fclose(fp);
        return;
    }

    uint32_t seq = 0;
    uint64_t sent_bytes = resp.resume_offset;

    while (seq < total_pkts) {
        struct ft_udp_pkt pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.seq = seq;
        pkt.total = total_pkts;

        size_t to_read = FT_CHUNK_SIZE;
        uint64_t offset = (uint64_t)seq * FT_CHUNK_SIZE;
        if (offset + FT_CHUNK_SIZE > remaining)
            to_read = (size_t)(remaining - offset);
        pkt.data_len = (uint16_t)to_read;
        fread(pkt.data, 1, to_read, fp);

        size_t pkt_size = offsetof(struct ft_udp_pkt, data) + to_read;
        net_send(nc, &pkt, pkt_size);

        /* Check for ACKs (non-blocking) */
        struct ft_udp_ack ack;
        int n = net_udp_recv(nc, &ack, sizeof(ack), 10);
        while (n == sizeof(ack) && ack.magic == FT_MAGIC) {
            if (ack.seq < total_pkts && !acked[ack.seq]) {
                acked[ack.seq] = true;
                uint64_t seg_size = (ack.seq == total_pkts - 1)
                    ? (remaining - (uint64_t)ack.seq * FT_CHUNK_SIZE)
                    : FT_CHUNK_SIZE;
                sent_bytes += seg_size;
                push_progress(sent_bytes, total);
            }
            n = net_udp_recv(nc, &ack, sizeof(ack), 5);
        }

        while (seq < total_pkts && acked[seq]) seq++;
        if (seq < total_pkts && !acked[seq]) {
            retries_arr[seq]++;
            if (retries_arr[seq] > FT_MAX_RETRIES) {
                push_error("UDP transfer failed: max retries for packet %u", seq);
                goto udp_cleanup;
            }
        }
    }

    /* Send EOF markers */
    struct ft_udp_pkt eof;
    memset(&eof, 0, sizeof(eof));
    eof.seq = total_pkts;
    eof.total = total_pkts;
    eof.data_len = 0;
    for (int i = 0; i < 5; i++) {
        net_send(nc, &eof, offsetof(struct ft_udp_pkt, data));
        usleep(100000);
    }

    push_xfer_done();

udp_cleanup:
    free(acked);
    free(retries_arr);
    fclose(fp);
}

/* ── UDP Receive ───────────────────────────────────────────── */

static void udp_recv_file(struct net_context *nc, const char *savepath)
{
    struct ft_meta meta;
    memset(&meta, 0, sizeof(meta));
    bool got_meta = false;

    for (int retry = 0; retry < 30; retry++) {
        int n = net_udp_recv(nc, &meta, sizeof(meta), 500);
        if (n == sizeof(meta) && meta.magic == FT_MAGIC) {
            got_meta = true;
            break;
        }
    }

    if (!got_meta) {
        push_error("Protocol mismatch — no valid meta received");
        return;
    }

    strncpy(g_last_recv_name, meta.filename, sizeof(g_last_recv_name) - 1);

    /* ── Auto-accept check ───────────────────────────────── */
    if (!g_auto_accept && g_accept_cb) {
        char sender_ip[64] = "unknown";
        char sender_host[256] = "";
        const struct sockaddr_in *peer = net_udp_last_sender(nc);
        if (peer && peer->sin_family == AF_INET) {
            inet_ntop(AF_INET, &peer->sin_addr, sender_ip, sizeof(sender_ip));
            getnameinfo((const struct sockaddr *)peer, sizeof(*peer),
                        sender_host, sizeof(sender_host), NULL, 0, 0);
        }
        if (!g_accept_cb(sender_ip, sender_host, meta.filename, meta.total_size)) {
            push_error("Transfer rejected by user");
            return;
        }
    }

    /* ── Output path + overwrite policy ───────────────────── */
    char fullpath[1024];
    snprintf(fullpath, sizeof(fullpath), "%s/%s", savepath, meta.filename);
    char final_path[1024];
    strncpy(final_path, fullpath, sizeof(final_path) - 1);

    uint64_t local_size = file_size(fullpath);
    uint64_t resume_offset = 0;
    const char *mode = "wb";

    if (local_size > 0 && local_size < meta.total_size) {
        resume_offset = local_size;
        mode = "ab";
    } else if (local_size >= meta.total_size) {
        /* Already complete */
        struct ft_meta_resp resp;
        resp.magic = FT_MAGIC;
        resp.resume_offset = meta.total_size;
        for (int i = 0; i < 3; i++) {
            net_send(nc, &resp, sizeof(resp));
            usleep(50000);
        }
        push_xfer_done();
        return;
    } else if (local_size == 0 && resume_offset == 0) {
        /* File exists but has size 0, or file doesn't exist yet.
           Check overwrite policy if file already exists. */
        struct stat exist_st;
        if (stat(fullpath, &exist_st) == 0) {
            if (strcmp(g_overwrite_policy, "skip") == 0) {
                push_error("File already exists (overwrite_policy=skip)");
                return;
            } else if (strcmp(g_overwrite_policy, "rename") == 0) {
                char base[900], ext[128];
                const char *dot = strrchr(meta.filename, '.');
                if (dot && dot != meta.filename) {
                    size_t blen = dot - meta.filename;
                    if (blen > sizeof(base) - 1) blen = sizeof(base) - 1;
                    memcpy(base, meta.filename, blen); base[blen] = '\0';
                    strncpy(ext, dot, sizeof(ext) - 1);
                } else {
                    strncpy(base, meta.filename, sizeof(base) - 1);
                    ext[0] = '\0';
                }
                int n = 1;
                while (n < 1000) {
                    snprintf(final_path, sizeof(final_path), "%s/%s (%d)%s",
                             savepath, base, n, ext);
                    if (stat(final_path, &exist_st) != 0) break;
                    n++;
                }
            }
        }
    }

    /* Send meta response */
    struct ft_meta_resp resp;
    memset(&resp, 0, sizeof(resp));
    resp.magic = FT_MAGIC;
    resp.resume_offset = resume_offset;
    for (int i = 0; i < 3; i++) {
        net_send(nc, &resp, sizeof(resp));
        usleep(50000);
    }

    FILE *fp = fopen(final_path, mode);
    if (!fp) {
        push_error("Cannot create file: %s", final_path);
        return;
    }

    uint64_t received = resume_offset;
    bool *received_pkts = NULL;
    uint32_t total_pkts = 0;
    uint64_t file_data_start = resume_offset;

    struct ft_udp_pkt pkt;
    while (1) {
        int n = net_udp_recv(nc, &pkt, sizeof(pkt), 1000);
        if (n < (int)offsetof(struct ft_udp_pkt, data)) continue;

        if (pkt.data_len == 0) break;  /* EOF */

        if (!received_pkts && pkt.total > 0) {
            total_pkts = pkt.total;
            received_pkts = calloc(total_pkts, sizeof(bool));
            if (!received_pkts) {
                push_error("Out of memory");
                fclose(fp);
                return;
            }
        }

        if (received_pkts && pkt.seq < total_pkts && !received_pkts[pkt.seq]) {
            uint64_t offset = (uint64_t)pkt.seq * FT_CHUNK_SIZE + file_data_start;
            fseek(fp, (long)offset, SEEK_SET);
            fwrite(pkt.data, 1, pkt.data_len, fp);
            received_pkts[pkt.seq] = true;
            received += pkt.data_len;
            push_progress(received, meta.total_size);
        }

        struct ft_udp_ack ack;
        ack.magic = FT_MAGIC;
        ack.seq = pkt.seq;
        net_send(nc, &ack, sizeof(ack));
    }

    fclose(fp);
    free(received_pkts);

    if (received >= meta.total_size) {
        push_xfer_done();
    } else {
        push_error("UDP transfer incomplete: %lu / %lu bytes received",
                   (unsigned long)received, (unsigned long)meta.total_size);
    }
}

/* ── Public API ────────────────────────────────────────────── */

void transfer_send(struct net_context *nc, const char *filepath, int protocol)
{
    /* Detect if path is a compressed archive (was originally a directory) */
    bool is_dir = false;
    size_t plen = strlen(filepath);
    if (plen > 7 && strcmp(filepath + plen - 7, ".tar.gz") == 0)
        is_dir = true;

    log_write("[SEND] transfer_send start, proto=%d, file=%s, dir=%d\n",
              protocol, filepath, is_dir);
    if (protocol == FT_PROTO_TCP) {
        log_write("[SEND] connected, fd=%d, starting transfer...\n", net_get_fd(nc));
        tcp_send_file(nc, filepath, 0, is_dir);
        log_write("[SEND] transfer done\n");
    } else {
        udp_send_file(nc, filepath);
    }
}

void transfer_recv(struct net_context *nc, const char *savepath, int protocol)
{
    log_write("[RECV] transfer_recv start, proto=%d, save=%s\n", protocol, savepath);
    if (protocol == FT_PROTO_TCP) {
        /* Receiver is server — accept sender connection */
        log_write("[RECV] waiting for sender (net_accept)...\n");
        int ar = net_accept(nc);
        if (ar == -2) {
            log_write("[RECV] net_accept cancelled\n");
            push_xfer_done();
            return;
        }
        if (ar != 0) {
            log_write("[RECV] net_accept FAILED\n");
            push_error("Failed to accept sender connection");
            return;
        }
        log_write("[RECV] sender connected, fd=%d, starting receive...\n", net_get_fd(nc));
        tcp_recv_file(nc, savepath);
        log_write("[RECV] tcp_recv_file returned\n");
    } else {
        udp_recv_file(nc, savepath);
    }
}

/* ── Pre-send preparation (compress BEFORE connecting) ────────── */

char *transfer_prepare_send(const char *filepath, uint64_t *out_size)
{
    struct stat st;
    if (stat(filepath, &st) != 0) {
        log_write("[PREPARE] cannot stat: %s\n", filepath);
        return NULL;
    }

    if (!S_ISDIR(st.st_mode)) {
        /* Regular file — no compression needed */
        *out_size = (uint64_t)st.st_size;
        return strdup(filepath);
    }

    /* Directory — compress now, before connecting */
    log_write("[PREPARE] compressing directory: %s\n", filepath);
    push_progress(0, 1);  /* signal "preparing" */

    char tmpfile[256];
#ifdef _WIN32
    snprintf(tmpfile, sizeof(tmpfile), "lanft_%d.tar.gz", (int)GetCurrentProcessId());
#else
    snprintf(tmpfile, sizeof(tmpfile), "/tmp/lanft_%d.tar.gz", (int)getpid());
#endif

    struct archive *a = archive_write_new();
    archive_write_add_filter_gzip(a);
    archive_write_set_format_pax_restricted(a);
    if (archive_write_open_filename(a, tmpfile) != ARCHIVE_OK) {
        archive_write_free(a);
        log_write("[PREPARE] failed to create archive\n");
        return NULL;
    }

    const char *dirname = strrchr(filepath, '/');
    if (dirname) dirname++; else dirname = filepath;
    walk_and_add(a, filepath, dirname);

    archive_write_close(a);
    archive_write_free(a);

    if (stat(tmpfile, &st) != 0) {
        unlink(tmpfile);
        log_write("[PREPARE] archive stat failed\n");
        return NULL;
    }

    *out_size = (uint64_t)st.st_size;
    log_write("[PREPARE] compressed to %s (%lu bytes)\n",
              tmpfile, (unsigned long)*out_size);
    return strdup(tmpfile);
}

void transfer_cleanup_send(char *prepared_path, const char *original_path)
{
    if (!prepared_path || !original_path) return;
    if (strcmp(prepared_path, original_path) != 0) {
        /* It's a temp archive — delete it */
        unlink(prepared_path);
        log_write("[CLEANUP] removed temp archive: %s\n", prepared_path);
    }
    free(prepared_path);
}

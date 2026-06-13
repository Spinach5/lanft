#include "transfer.h"
#include "network.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <SDL2/SDL.h>

/* ── Helpers ───────────────────────────────────────────────── */

static void push_event(int code, void *data)
{
    SDL_Event event;
    SDL_memset(&event, 0, sizeof(event));
    event.type = code;
    event.user.data1 = data;
    SDL_PushEvent(&event);
}

static void push_error(const char *fmt, ...)
{
    struct event_error *err = calloc(1, sizeof(*err));
    if (!err) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err->message, sizeof(err->message), fmt, ap);
    va_end(ap);
    push_event(SDL_USEREVENT + 5, err);
}

static void push_progress(uint64_t done, uint64_t total)
{
    struct event_progress *p = calloc(1, sizeof(*p));
    if (!p) return;
    p->bytes_done = done;
    p->bytes_total = total;
    push_event(SDL_USEREVENT + 3, p);
}

static void push_xfer_done(void)
{
    push_event(SDL_USEREVENT + 4, NULL);
}

static uint64_t file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (uint64_t)st.st_size;
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

static int sock_read_full(int fd, void *buf, size_t len, int timeout_ms)
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
        if (ret <= 0) return -1;
        ssize_t n = read(fd, p + received, len - received);
        if (n <= 0) return -1;
        received += n;
    }
    return 0;
}

static int sock_write_full(int fd, const void *buf, size_t len)
{
    size_t sent = 0;
    const uint8_t *p = (const uint8_t *)buf;
    while (sent < len) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        /* Wait up to 5s for writability */
        struct timeval tv = {5, 0};
        int ret = select(fd + 1, NULL, &fds, NULL, &tv);
        if (ret <= 0) return -1;
        ssize_t n = write(fd, p + sent, len - sent);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

/* ── Directory send helpers ────────────────────────────────── */

#define DIR_MAX_FILES 4096

struct dir_file {
    char path[1024];     /* relative path from dir root */
    uint64_t size;
};

static int dir_walk(const char *base, const char *subdir,
                    struct dir_file *files, int *count)
{
    char full[1280];
    snprintf(full, sizeof(full), "%s/%s", base, subdir ? subdir : "");
    /* Remove trailing slash if present and not root */
    size_t flen = strlen(full);
    if (flen > 1 && full[flen-1] == '/') full[flen-1] = '\0';

    DIR *d = opendir(full);
    if (!d) return -1;

    struct dirent *de;
    while ((de = readdir(d)) != NULL && *count < DIR_MAX_FILES) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        char item_full[1280], rel[1024];
        snprintf(item_full, sizeof(item_full), "%s/%s", full, de->d_name);
        if (subdir)
            snprintf(rel, sizeof(rel), "%s/%s", subdir, de->d_name);
        else
            strncpy(rel, de->d_name, sizeof(rel) - 1);

        struct stat st;
        if (stat(item_full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            /* Add directory entry (size=0 marks dir) */
            strncpy(files[*count].path, rel, sizeof(files[0].path) - 1);
            files[*count].size = 0;
            (*count)++;
            dir_walk(base, rel, files, count);
        } else if (S_ISREG(st.st_mode)) {
            strncpy(files[*count].path, rel, sizeof(files[0].path) - 1);
            files[*count].size = (uint64_t)st.st_size;
            (*count)++;
        }
    }
    closedir(d);
    return 0;
}

static int tcp_send_file(struct net_context *nc, const char *filepath,
                          uint64_t resume_offset)
{
    int fd = net_get_fd(nc);
    fprintf(stderr, "[SEND] tcp_send_file: fd=%d, file=%s\n", fd, filepath);
    if (fd < 0) { fprintf(stderr, "[SEND] BAD FD!\n"); push_error("No socket"); return -2; }

    /* Check if path is a directory */
    struct stat path_st;
    bool is_dir = false;
    if (stat(filepath, &path_st) == 0 && S_ISDIR(path_st.st_mode))
        is_dir = true;

    const char *fname = strrchr(filepath, '/');
    if (fname) fname++; else fname = filepath;

    /* If directory, walk it and collect files */
    struct dir_file *dir_files = NULL;
    int dir_count = 0;
    uint64_t total = 0;

    if (is_dir) {
        dir_files = calloc(DIR_MAX_FILES, sizeof(struct dir_file));
        if (!dir_files) { push_error("Out of memory"); return -2; }
        dir_walk(filepath, NULL, dir_files, &dir_count);
        /* Calculate total size */
        for (int i = 0; i < dir_count; i++)
            total += dir_files[i].size;
        fprintf(stderr, "[SEND] directory mode: %d entries, %lu total bytes\n",
                dir_count, (unsigned long)total);
    } else {
        FILE *fp = fopen(filepath, "rb");
        if (!fp) { fprintf(stderr, "[SEND] CANNOT OPEN!\n"); push_error("Cannot open: %s", filepath); return -2; }
        fseek(fp, 0, SEEK_END);
        total = (uint64_t)ftell(fp);
        fclose(fp);
    }

    /* Send meta */
    struct ft_meta meta;
    memset(&meta, 0, sizeof(meta));
    meta.magic = FT_MAGIC;
    meta.protocol = FT_PROTO_TCP;
    meta.flags = is_dir ? 1 : 0;
    meta.name_len = (uint8_t)strlen(fname);
    strncpy(meta.filename, fname, sizeof(meta.filename) - 1);
    meta.total_size = total;

    fprintf(stderr, "[SEND] sending meta (name=%s, size=%lu, dir=%d)...\n",
            fname, (unsigned long)total, is_dir);
    if (sock_write_full(fd, &meta, sizeof(meta)) != 0) {
        fprintf(stderr, "[SEND] FAILED to send meta!\n");
        push_error("Failed to send file metadata");
        free(dir_files);
        return -1;  /* likely scanner probe */
    }
    fprintf(stderr, "[SEND] meta sent OK\n");

    /* For directories: send directory entries after meta */
    if (is_dir && dir_files) {
        /* Send each directory entry */
        for (int i = 0; i < dir_count; i++) {
            struct ft_dirent de;
            de.path_len = (uint16_t)strlen(dir_files[i].path);
            de.is_dir = (dir_files[i].size == 0) ? 1 : 0;
            de.file_size = dir_files[i].size;
            sock_write_full(fd, &de, sizeof(de));
            sock_write_full(fd, dir_files[i].path, de.path_len);
        }
        /* Send terminator */
        struct ft_dirent term;
        memset(&term, 0, sizeof(term));
        term.path_len = 0;
        sock_write_full(fd, &term, sizeof(term));
        fprintf(stderr, "[SEND] sent %d directory entries\n", dir_count);
    }

    /* Read meta response */
    fprintf(stderr, "[SEND] waiting for meta response...\n");
    struct ft_meta_resp resp;
    if (sock_read_full(fd, &resp, sizeof(resp), 10000) == 0 &&
        resp.magic == FT_MAGIC) {
        resume_offset = resp.resume_offset;
        fprintf(stderr, "[SEND] got meta response, resume_offset=%lu\n", (unsigned long)resume_offset);
    }

    if (resume_offset >= total && total > 0) {
        fprintf(stderr, "[SEND] already complete on receiver, done\n");
        push_xfer_done();
        free(dir_files);
        return 0;
    }

    uint64_t sent = 0;

    if (is_dir && dir_files) {
        /* Send each file in the directory */
        char base_dir[1024];
        strncpy(base_dir, filepath, sizeof(base_dir) - 1);

        for (int i = 0; i < dir_count; i++) {
            if (dir_files[i].size == 0) continue; /* skip dirs */

            char full_path[1280];
            snprintf(full_path, sizeof(full_path), "%s/%s", base_dir, dir_files[i].path);

            FILE *fp = fopen(full_path, "rb");
            if (!fp) {
                push_error("Cannot open: %s", dir_files[i].path);
                free(dir_files);
                return -2;
            }

            uint8_t buf[FT_TCP_CHUNK_SIZE];
            uint64_t file_sent = 0;
            while (file_sent < dir_files[i].size) {
                size_t to_read = (dir_files[i].size - file_sent) > FT_TCP_CHUNK_SIZE
                                 ? FT_TCP_CHUNK_SIZE : (size_t)(dir_files[i].size - file_sent);
                size_t n = fread(buf, 1, to_read, fp);
                if (n == 0) break;
                if (sock_write_full(fd, buf, n) != 0) {
                    push_error("Send failed");
                    fclose(fp);
                    free(dir_files);
                    return -2;
                }
                file_sent += n;
                sent += n;
                fprintf(stderr, "[SEND] dir progress %lu/%lu\n", (unsigned long)sent, (unsigned long)total);
                push_progress(sent, total);
            }
            fclose(fp);
        }
    } else {
        /* Single file transfer */
        FILE *fp = fopen(filepath, "rb");
        if (!fp) { push_error("Cannot open: %s", filepath); free(dir_files); return -2; }
        fseek(fp, (long)resume_offset, SEEK_SET);
        sent = resume_offset;

        uint8_t buf[FT_TCP_CHUNK_SIZE];
        while (sent < total) {
            size_t to_read = (total - sent) > FT_TCP_CHUNK_SIZE
                             ? FT_TCP_CHUNK_SIZE : (size_t)(total - sent);
            size_t n = fread(buf, 1, to_read, fp);
            if (n == 0) break;
            if (sock_write_full(fd, buf, n) != 0) {
                push_error("Send failed at %lu / %lu bytes",
                           (unsigned long)sent, (unsigned long)total);
                fclose(fp);
                free(dir_files);
                return -2;
            }
            sent += n;
            fprintf(stderr, "[SEND] progress %lu/%lu\n", (unsigned long)sent, (unsigned long)total);
            push_progress(sent, total);
        }
        fclose(fp);
    }

    fprintf(stderr, "[SEND] transfer done, sent=%lu/%lu\n", (unsigned long)sent, (unsigned long)total);
    free(dir_files);
    if (sent >= total) push_xfer_done();
    return (sent >= total) ? 0 : -2;
}

/* ── TCP receive with direct socket I/O ───────────────────── */

static void tcp_recv_file(struct net_context *nc, const char *savepath)
{
    int fd = net_get_fd(nc);
    fprintf(stderr, "[RECV] tcp_recv_file: fd=%d, save=%s\n", fd, savepath);
    if (fd < 0) { fprintf(stderr, "[RECV] BAD FD!\n"); push_error("No socket"); return; }

    /* Read meta */
    fprintf(stderr, "[RECV] waiting for meta...\n");
    struct ft_meta meta;
    if (sock_read_full(fd, &meta, sizeof(meta), 30000) != 0) {
        fprintf(stderr, "[RECV] FAILED to receive meta!\n");
        push_error("Failed to receive file metadata (timeout)");
        return;
    }
    fprintf(stderr, "[RECV] got meta: name=%s, size=%lu, flags=%d\n",
            meta.filename, (unsigned long)meta.total_size, meta.flags);

    if (meta.magic != FT_MAGIC) {
        push_error("Protocol mismatch — bad magic bytes");
        return;
    }

    bool is_dir = (meta.flags & 0x01) != 0;

    /* Determine output path */
    char rootpath[1024];
    snprintf(rootpath, sizeof(rootpath), "%s/%s", savepath, meta.filename);

    /* Read directory entries if this is a directory transfer */
    struct dir_file *dir_files = NULL;
    int dir_count = 0;
    bool has_entries = false;

    if (is_dir) {
        dir_files = calloc(DIR_MAX_FILES, sizeof(struct dir_file));
        if (!dir_files) { push_error("Out of memory"); return; }

        /* Receive directory entries until terminator */
        while (dir_count < DIR_MAX_FILES) {
            struct ft_dirent de;
            if (sock_read_full(fd, &de, sizeof(de), 30000) != 0) {
                push_error("Failed to receive directory entry");
                free(dir_files);
                return;
            }
            if (de.path_len == 0) break;  /* terminator */

            char path_buf[1024];
            memset(path_buf, 0, sizeof(path_buf));
            if (sock_read_full(fd, path_buf, de.path_len, 10000) != 0) {
                push_error("Failed to receive directory path");
                free(dir_files);
                return;
            }
            path_buf[de.path_len] = '\0';

            strncpy(dir_files[dir_count].path, path_buf, sizeof(dir_files[0].path) - 1);
            dir_files[dir_count].size = de.is_dir ? 0 : de.file_size;
            dir_count++;
        }

        /* Create the root directory and all subdirectories */
        mkdir(rootpath, 0755);
        for (int i = 0; i < dir_count; i++) {
            if (dir_files[i].size == 0) {
                /* Create subdirectory */
                char sub_path[1280];
                snprintf(sub_path, sizeof(sub_path), "%s/%s", rootpath, dir_files[i].path);
                mkdir(sub_path, 0755);
            }
        }

        has_entries = true;
        fprintf(stderr, "[RECV] received %d directory entries\n", dir_count);
    }

    /* Calculate resume behavior */
    uint64_t local_size = has_entries ? 0 : file_size(rootpath);
    uint64_t resume_offset = 0;

    if (!is_dir && local_size > 0 && local_size < meta.total_size) {
        resume_offset = local_size;
    }

    /* Send meta response */
    struct ft_meta_resp resp;
    memset(&resp, 0, sizeof(resp));
    resp.magic = FT_MAGIC;
    resp.resume_offset = resume_offset;
    if (sock_write_full(fd, &resp, sizeof(resp)) != 0) {
        push_error("Failed to send meta response");
        free(dir_files);
        return;
    }

    fprintf(stderr, "[RECV] ready for data, total=%lu, is_dir=%d\n",
            (unsigned long)meta.total_size, is_dir);
    uint64_t received = 0;
    uint8_t buf[FT_TCP_CHUNK_SIZE];

    if (is_dir && dir_files) {
        /* Receive file data for each file in the directory */
        for (int i = 0; i < dir_count; i++) {
            if (dir_files[i].size == 0) continue; /* skip dir entries */

            char full_path[1280];
            snprintf(full_path, sizeof(full_path), "%s/%s", rootpath, dir_files[i].path);

            FILE *fp = fopen(full_path, "wb");
            if (!fp) {
                push_error("Cannot create file: %s", full_path);
                free(dir_files);
                return;
            }

            uint64_t file_recv = 0;
            while (file_recv < dir_files[i].size) {
                size_t to_read = (dir_files[i].size - file_recv) > FT_TCP_CHUNK_SIZE
                                 ? FT_TCP_CHUNK_SIZE : (size_t)(dir_files[i].size - file_recv);

                fd_set fds;
                FD_ZERO(&fds);
                FD_SET(fd, &fds);
                struct timeval tv = {30, 0};
                if (select(fd + 1, &fds, NULL, NULL, &tv) <= 0) {
                    push_error("Receive timeout");
                    fclose(fp);
                    free(dir_files);
                    return;
                }

                ssize_t n = read(fd, buf, to_read);
                if (n <= 0) {
                    push_error("Receive failed");
                    fclose(fp);
                    free(dir_files);
                    return;
                }

                fwrite(buf, 1, n, fp);
                file_recv += n;
                received += n;
                push_progress(received, meta.total_size);
            }
            fclose(fp);
        }
    } else {
        /* Single file receive */
        FILE *fp = fopen(rootpath, resume_offset > 0 ? "ab" : "wb");
        if (!fp) {
            push_error("Cannot create file: %s", rootpath);
            free(dir_files);
            return;
        }
        if (resume_offset > 0) fseek(fp, (long)resume_offset, SEEK_SET);

        received = resume_offset;
        while (received < meta.total_size) {
            size_t to_read = (meta.total_size - received) > FT_TCP_CHUNK_SIZE
                             ? FT_TCP_CHUNK_SIZE : (size_t)(meta.total_size - received);

            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(fd, &fds);
            struct timeval tv = {30, 0};
            if (select(fd + 1, &fds, NULL, NULL, &tv) <= 0) {
                push_error("Receive timeout");
                fclose(fp);
                free(dir_files);
                return;
            }

            ssize_t n = read(fd, buf, to_read);
            if (n <= 0) {
                push_error("Receive failed");
                fclose(fp);
                free(dir_files);
                return;
            }

            fwrite(buf, 1, n, fp);
            received += n;
            fprintf(stderr, "[RECV] progress %lu/%lu\n", (unsigned long)received, (unsigned long)meta.total_size);
            push_progress(received, meta.total_size);
        }
        fclose(fp);
    }

    fprintf(stderr, "[RECV] transfer done, received=%lu/%lu\n", (unsigned long)received, (unsigned long)meta.total_size);
    free(dir_files);
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

    char fullpath[1024];
    snprintf(fullpath, sizeof(fullpath), "%s/%s", savepath, meta.filename);

    uint64_t local_size = file_size(fullpath);
    uint64_t resume_offset = 0;
    const char *mode = "wb";

    if (local_size > 0 && local_size < meta.total_size) {
        resume_offset = local_size;
        mode = "ab";
    } else if (local_size >= meta.total_size) {
        struct ft_meta_resp resp;
        resp.magic = FT_MAGIC;
        resp.resume_offset = meta.total_size;
        for (int i = 0; i < 3; i++) {
            net_send(nc, &resp, sizeof(resp));
            usleep(50000);
        }
        push_xfer_done();
        return;
    }

    struct ft_meta_resp resp;
    memset(&resp, 0, sizeof(resp));
    resp.magic = FT_MAGIC;
    resp.resume_offset = resume_offset;
    for (int i = 0; i < 3; i++) {
        net_send(nc, &resp, sizeof(resp));
        usleep(50000);
    }

    FILE *fp = fopen(fullpath, mode);
    if (!fp) {
        push_error("Cannot create file: %s", fullpath);
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
    fprintf(stderr, "[SEND] transfer_send start, proto=%d, file=%s\n", protocol, filepath);
    if (protocol == FT_PROTO_TCP) {
        /* Accept loop: scanner probes may trigger accept().
           If handshake fails, re-accept for the real receiver. */
        int attempt = 0;
        while (attempt < 50) {
            attempt++;
            fprintf(stderr, "[SEND] calling net_accept... (attempt %d)\n", attempt);
            if (net_accept(nc) != 0) {
                fprintf(stderr, "[SEND] net_accept FAILED\n");
                push_error("Failed to accept client connection");
                return;
            }
            fprintf(stderr, "[SEND] net_accept OK, fd=%d\n", net_get_fd(nc));
            int result = tcp_send_file(nc, filepath, 0);
            if (result == 0) return;           /* success */
            if (result == -1) continue;         /* scanner — retry */
            return;                             /* real error — stop */
        }
        push_error("Too many scanner probes — no real receiver found");
    } else {
        udp_send_file(nc, filepath);
    }
}

void transfer_recv(struct net_context *nc, const char *savepath, int protocol)
{
    fprintf(stderr, "[RECV] transfer_recv start, proto=%d, save=%s\n", protocol, savepath);
    if (protocol == FT_PROTO_TCP) {
        fprintf(stderr, "[RECV] fd=%d, calling tcp_recv_file...\n", net_get_fd(nc));
        tcp_recv_file(nc, savepath);
        fprintf(stderr, "[RECV] tcp_recv_file returned\n");
    } else {
        udp_recv_file(nc, savepath);
    }
}

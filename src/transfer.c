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

/* ── Meta response receiver (used by sender to get resume_offset) ── */

struct meta_resp_state {
    struct ft_meta_resp resp;
    size_t pos;
    bool done;
};

static void meta_resp_rx_cb(void *user, const void *data, size_t len)
{
    struct meta_resp_state *st = (struct meta_resp_state *)user;
    if (!st || st->done) return;
    size_t to_copy = len;
    size_t remaining = sizeof(struct ft_meta_resp) - st->pos;
    if (to_copy > remaining) to_copy = remaining;
    memcpy((uint8_t *)&st->resp + st->pos, data, to_copy);
    st->pos += to_copy;
    if (st->pos >= sizeof(struct ft_meta_resp))
        st->done = true;
}

static void tcp_send_file(struct net_context *nc, const char *filepath,
                          uint64_t resume_offset)
{
    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        push_error("Cannot open file: %s", filepath);
        return;
    }

    fseek(fp, 0, SEEK_END);
    uint64_t total = (uint64_t)ftell(fp);

    const char *fname = strrchr(filepath, '/');
    if (fname) fname++; else fname = filepath;

    /* Set up RX callback to capture the meta response */
    struct meta_resp_state mrs;
    memset(&mrs, 0, sizeof(mrs));
    net_set_rx_cb(nc, meta_resp_rx_cb, &mrs);

    if (send_meta(nc, fname, total, FT_PROTO_TCP) != 0) {
        push_error("Failed to send file metadata");
        fclose(fp);
        return;
    }

    /* Wait for meta response from receiver (with timeout) */
    {
        int timeout = 0;
        while (!mrs.done && net_is_connected(nc) && timeout < 200) {
            net_service(nc, 50);
            timeout++;
        }

        if (mrs.done && mrs.resp.magic == FT_MAGIC) {
            resume_offset = mrs.resp.resume_offset;
        }
    }

    /* If receiver already has the complete file, skip transfer */
    if (resume_offset >= total) {
        push_xfer_done();
        fclose(fp);
        return;
    }

    fseek(fp, (long)resume_offset, SEEK_SET);
    uint64_t sent = resume_offset;

    /* Restore RX callback to NULL for data phase (sender doesn't need RX during send) */
    net_set_rx_cb(nc, NULL, NULL);

    uint8_t buf[FT_TCP_CHUNK_SIZE];
    while (sent < total && net_is_connected(nc)) {
        size_t to_read = (total - sent) > FT_TCP_CHUNK_SIZE
                         ? FT_TCP_CHUNK_SIZE : (size_t)(total - sent);
        size_t n = fread(buf, 1, to_read, fp);
        if (n == 0) break;

        net_send(nc, buf, n);

        /* Wait for TX to drain */
        for (int w = 0; w < 40 && net_is_connected(nc); w++) {
            net_service(nc, 25);
            if (!net_tx_pending(nc)) break;
        }

        sent += n;
        push_progress(sent, total);
    }

    fclose(fp);

    /* Final flush */
    for (int i = 0; i < 40; i++) net_service(nc, 25);

    if (sent >= total) {
        push_xfer_done();
    } else if (net_is_connected(nc)) {
        push_error("Transfer interrupted at %lu / %lu bytes",
                   (unsigned long)sent, (unsigned long)total);
    }
}

/* ── TCP Receive (state machine via RX callback) ───────────── */

struct tcp_recv_state {
    int phase;              /* 0=meta, 1=meta_done_wait, 2=data */
    struct ft_meta meta;
    size_t meta_pos;
    FILE *fp;
    uint64_t received;
    uint64_t total_size;
    bool done;
};

static void tcp_recv_rx_cb(void *user, const void *data, size_t len)
{
    struct tcp_recv_state *st = (struct tcp_recv_state *)user;
    if (!st) return;

    if (st->phase == 0) {
        /* Collecting meta bytes */
        size_t to_copy = len;
        size_t remaining = sizeof(struct ft_meta) - st->meta_pos;
        if (to_copy > remaining) to_copy = remaining;
        memcpy((uint8_t *)&st->meta + st->meta_pos, data, to_copy);
        st->meta_pos += to_copy;
        if (st->meta_pos >= sizeof(struct ft_meta)) {
            st->phase = 1;  /* meta complete, signal main loop */
        }
    } else if (st->phase == 2 && st->fp) {
        /* Writing file data */
        fwrite(data, 1, len, st->fp);
        st->received += len;
    }
}

static void tcp_close_cb(void *user)
{
    struct tcp_recv_state *st = (struct tcp_recv_state *)user;
    if (st) st->done = true;
}

static void tcp_recv_file(struct net_context *nc, const char *savepath)
{
    struct tcp_recv_state st;
    memset(&st, 0, sizeof(st));
    st.phase = 0;

    net_set_rx_cb(nc, tcp_recv_rx_cb, &st);
    net_set_close_cb(nc, tcp_close_cb, &st);

    /* Phase 1: wait for meta to arrive */
    int timeout = 0;
    while (st.phase == 0 && net_is_connected(nc) && !st.done && timeout < 500) {
        net_service(nc, 50);
        timeout++;
    }

    if (st.phase == 0) {
        push_error("Failed to receive file metadata (timeout)");
        return;
    }

    struct ft_meta *meta = &st.meta;
    if (meta->magic != FT_MAGIC) {
        push_error("Protocol mismatch — bad magic bytes");
        return;
    }

    /* Determine output path */
    char fullpath[1024];
    snprintf(fullpath, sizeof(fullpath), "%s/%s", savepath, meta->filename);

    uint64_t local_size = file_size(fullpath);
    uint64_t resume_offset = 0;
    const char *mode = "wb";

    if (local_size > 0 && local_size < meta->total_size) {
        resume_offset = local_size;
        mode = "ab";
    } else if (local_size >= meta->total_size) {
        struct ft_meta_resp resp;
        memset(&resp, 0, sizeof(resp));
        resp.magic = FT_MAGIC;
        resp.resume_offset = meta->total_size;
        net_send(nc, &resp, sizeof(resp));
        for (int i = 0; i < 10; i++) net_service(nc, 50);
        push_xfer_done();
        return;
    }

    /* Send meta response with resume offset */
    struct ft_meta_resp resp;
    memset(&resp, 0, sizeof(resp));
    resp.magic = FT_MAGIC;
    resp.resume_offset = resume_offset;
    net_send(nc, &resp, sizeof(resp));
    for (int i = 0; i < 10; i++) net_service(nc, 50);

    /* Open file */
    st.fp = fopen(fullpath, mode);
    if (!st.fp) {
        push_error("Cannot create file: %s", fullpath);
        return;
    }
    st.received = resume_offset;
    st.total_size = meta->total_size;
    st.phase = 2;   /* switch to data reception */
    st.done = false;

    /* Phase 2: receive file data */
    while (st.received < st.total_size && net_is_connected(nc) && !st.done) {
        net_service(nc, 100);
        if (st.received > 0 && st.received % (FT_TCP_CHUNK_SIZE / 2) == 0) {
            push_progress(st.received, st.total_size);
        }
    }

    /* Final progress update */
    push_progress(st.received, st.total_size);

    if (st.fp) fclose(st.fp);

    if (st.received >= st.total_size) {
        push_xfer_done();
    } else if (net_is_connected(nc)) {
        push_error("Transfer interrupted at %lu / %lu bytes",
                   (unsigned long)st.received, (unsigned long)st.total_size);
    }
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
    if (protocol == FT_PROTO_TCP) {
        /* Sender is server — wait for receiver to connect */
        if (net_accept(nc) != 0) {
            push_error("Failed to accept client connection");
            return;
        }
        tcp_send_file(nc, filepath, 0);
    } else {
        udp_send_file(nc, filepath);
    }
}

void transfer_recv(struct net_context *nc, const char *savepath, int protocol)
{
    if (protocol == FT_PROTO_TCP) {
        tcp_recv_file(nc, savepath);
    } else {
        udp_recv_file(nc, savepath);
    }
}

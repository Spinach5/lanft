#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

/* Magic bytes "FT01" as big-endian uint32 */
#define FT_MAGIC          0x46543031
#define FT_DEFAULT_PORT   9876
#define FT_CHUNK_SIZE     8192
#define FT_TCP_CHUNK_SIZE 65536
#define FT_UDP_WINDOW     16
#define FT_UDP_TIMEOUT_MS 500
#define FT_MAX_RETRIES    10
#define FT_MAX_FILENAME   256

/* Protocol type */
#ifndef FT_PROTO_TCP
#define FT_PROTO_TCP  0
#define FT_PROTO_UDP  1
#endif

/* Meta handshake: sender → receiver */
struct ft_meta {
    uint32_t magic;          /* FT_MAGIC */
    uint8_t  protocol;       /* FT_PROTO_TCP or FT_PROTO_UDP */
    uint8_t  flags;          /* 0x01 = is directory */
    uint8_t  name_len;
    char     filename[FT_MAX_FILENAME];
    uint64_t total_size;     /* file size, or total bytes for dir */
};

/* Directory entry header (after meta, before file data, for dir transfers) */
struct ft_dirent {
    uint16_t path_len;
    uint8_t  is_dir;
    uint64_t file_size;
    /* followed by path[path_len] bytes */
};

/* Meta response: receiver → sender */
struct ft_meta_resp {
    uint32_t magic;          /* FT_MAGIC */
    uint64_t resume_offset;  /* 0 = start fresh, >0 = resume */
};

/* UDP data packet header followed by data */
struct ft_udp_pkt {
    uint32_t seq;            /* packet sequence number */
    uint32_t total;          /* total number of packets */
    uint16_t data_len;       /* bytes in data[] (0 = EOF marker) */
    uint8_t  data[FT_CHUNK_SIZE];
};

/* UDP ACK: receiver → sender */
struct ft_udp_ack {
    uint32_t magic;          /* FT_MAGIC */
    uint32_t seq;            /* acknowledged seq number */
};

/* SDL user event payloads (used with SDL_UserEvent in ui.h) */

struct event_scan_found {
    char ip[64];
    char hostname[256];
};

struct event_progress {
    uint64_t bytes_done;
    uint64_t bytes_total;
};

struct event_error {
    char message[512];
};

struct event_scan_done {
    int total_found;
};

#endif /* PROTOCOL_H */

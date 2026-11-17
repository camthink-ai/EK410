/**
 * @file ct_prov_frag.c
 * @brief CT-Prov/1 GATT fragment assemble / slice (NeoLink §5).
 */
#include "ct_prov_frag.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void ct_frag_assembler_init(ct_frag_assembler_t *a, int mtu)
{
    memset(a, 0, sizeof(*a));
    a->mtu = mtu > 23 ? mtu : 23;
}

void ct_frag_assembler_reset(ct_frag_assembler_t *a)
{
    free(a->buf);
    a->buf = NULL;
    a->buf_len = 0;
    a->buf_cap = 0;
    a->active = false;
    a->kind = 0;
    a->message_id = 0;
    a->count = 0;
    a->expected_index = 0;
    a->reject_reason[0] = '\0';
    a->packet.bytes = NULL;
    a->packet.len = 0;
}

void ct_frag_assembler_deinit(ct_frag_assembler_t *a)
{
    ct_frag_assembler_reset(a);
}

void ct_frag_assembler_set_mtu(ct_frag_assembler_t *a, int mtu)
{
    a->mtu = mtu > 23 ? mtu : 23;
}

static ct_frag_feed_t reject(ct_frag_assembler_t *a, const char *reason)
{
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%s", reason ? reason : "malformed_frame");
    ct_frag_assembler_reset(a);
    snprintf(a->reject_reason, sizeof(a->reject_reason), "%s", tmp);
    return CT_FRAG_REJECTED;
}

ct_frag_feed_t ct_frag_feed(ct_frag_assembler_t *a, const uint8_t *frame, size_t len)
{
    if (!a) {
        return CT_FRAG_REJECTED;
    }
    if (!frame || len < CT_FRAG_HDR_SIZE) {
        return reject(a, "malformed_frame");
    }
    uint8_t magic = frame[0];
    uint8_t kind = frame[1];
    uint8_t mid = frame[2];
    uint8_t index = frame[3];
    uint8_t count = frame[4];

    if (magic != CT_FRAG_MAGIC) {
        return reject(a, "bad_magic");
    }
    if (kind != CT_FRAG_KIND_REQUEST && kind != CT_FRAG_KIND_RESPONSE &&
        kind != CT_FRAG_KIND_EVENT && kind != CT_FRAG_KIND_PROTO_ERR) {
        return reject(a, "unknown_packet_kind");
    }

    if (!a->active) {
        if (count < 1) {
            return reject(a, "malformed_frame");
        }
        if (index != 0) {
            return reject(a, "out_of_order");
        }
        a->active = true;
        a->kind = kind;
        a->message_id = mid;
        a->count = count;
        a->expected_index = 0;
    } else if (kind != a->kind || mid != a->message_id || count != a->count) {
        return reject(a, "header_changed");
    } else if (index < a->expected_index) {
        return reject(a, "duplicate_fragment");
    } else if (index > a->expected_index) {
        return reject(a, "out_of_order");
    }

    size_t plen = len - CT_FRAG_HDR_SIZE;
    size_t need = a->buf_len + plen;
    if (need > CT_FRAG_MAX_LOGICAL) {
        return reject(a, "oversized");
    }
    if (need > a->buf_cap) {
        size_t ncap = a->buf_cap ? a->buf_cap * 2 : 256;
        while (ncap < need) {
            ncap *= 2;
        }
        if (ncap > CT_FRAG_MAX_LOGICAL) {
            ncap = CT_FRAG_MAX_LOGICAL;
        }
        uint8_t *nb = realloc(a->buf, ncap);
        if (!nb) {
            return reject(a, "oversized");
        }
        a->buf = nb;
        a->buf_cap = ncap;
    }
    if (plen) {
        memcpy(a->buf + a->buf_len, frame + CT_FRAG_HDR_SIZE, plen);
        a->buf_len += plen;
    }

    bool is_final = (index == (uint8_t)(a->count - 1));
    if (!is_final) {
        a->expected_index = (uint8_t)(index + 1);
        return CT_FRAG_ASSEMBLING;
    }

    a->packet.kind = a->kind;
    a->packet.message_id = a->message_id;
    a->packet.bytes = a->buf;
    a->packet.len = a->buf_len;
    /* Detach buffer ownership into packet; assembler stays clean. */
    a->buf = NULL;
    a->buf_len = 0;
    a->buf_cap = 0;
    a->active = false;
    a->expected_index = 0;
    return CT_FRAG_ASSEMBLED;
}

int ct_frag_slice(int mtu, uint8_t kind, uint8_t message_id,
                  const char *json, size_t json_len,
                  uint8_t **out_frames, size_t *out_lens, int max_frames)
{
    if (!json || json_len == 0 || !out_frames || !out_lens || max_frames < 1) {
        return -1;
    }
    if (json_len > CT_FRAG_MAX_LOGICAL) {
        return -1;
    }
    int att_mtu = mtu > 23 ? mtu : 23;
    int payload_max = att_mtu - 3 - CT_FRAG_HDR_SIZE;
    if (payload_max < 1) {
        return -1;
    }
    int count = (int)((json_len + (size_t)payload_max - 1) / (size_t)payload_max);
    if (count > 255 || count > max_frames) {
        return -1;
    }

    size_t off = 0;
    for (int i = 0; i < count; i++) {
        size_t chunk = json_len - off;
        if (chunk > (size_t)payload_max) {
            chunk = (size_t)payload_max;
        }
        size_t flen = CT_FRAG_HDR_SIZE + chunk;
        uint8_t *f = malloc(flen);
        if (!f) {
            for (int j = 0; j < i; j++) {
                free(out_frames[j]);
                out_frames[j] = NULL;
            }
            return -1;
        }
        f[0] = CT_FRAG_MAGIC;
        f[1] = kind;
        f[2] = message_id;
        f[3] = (uint8_t)i;
        f[4] = (uint8_t)count;
        if (chunk) {
            memcpy(f + CT_FRAG_HDR_SIZE, json + off, chunk);
        }
        out_frames[i] = f;
        out_lens[i] = flen;
        off += chunk;
    }
    return count;
}

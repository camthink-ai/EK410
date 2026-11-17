/**
 * @file ct_prov_frag.h
 * @brief CT-Prov/1 GATT fragment layer (§5) — assemble + slice.
 */
#ifndef CT_PROV_FRAG_H_
#define CT_PROV_FRAG_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CT_FRAG_MAGIC           0xC1
#define CT_FRAG_KIND_REQUEST    0x01
#define CT_FRAG_KIND_RESPONSE   0x02
#define CT_FRAG_KIND_EVENT      0x03
#define CT_FRAG_KIND_PROTO_ERR  0x7F
#define CT_FRAG_HDR_SIZE        5
#define CT_FRAG_MAX_LOGICAL     4096

typedef enum {
    CT_FRAG_ASSEMBLING = 0,
    CT_FRAG_ASSEMBLED,
    CT_FRAG_REJECTED,
} ct_frag_feed_t;

typedef struct {
    uint8_t kind;
    uint8_t message_id;
    uint8_t *bytes;   /* owned by assembler until reset / next assemble */
    size_t len;
} ct_frag_packet_t;

typedef struct {
    int mtu; /* ATT_MTU; used for send payload_max only */
    uint8_t *buf;
    size_t buf_len;
    size_t buf_cap;
    bool active;
    uint8_t kind;
    uint8_t message_id;
    uint8_t count;
    uint8_t expected_index;
    char reject_reason[32];
    ct_frag_packet_t packet; /* valid when feed returns ASSEMBLED */
} ct_frag_assembler_t;

void ct_frag_assembler_init(ct_frag_assembler_t *a, int mtu);
void ct_frag_assembler_reset(ct_frag_assembler_t *a);
void ct_frag_assembler_deinit(ct_frag_assembler_t *a);
void ct_frag_assembler_set_mtu(ct_frag_assembler_t *a, int mtu);

/** Feed one ATT frame (header + payload). On ASSEMBLED, a->packet is valid. */
ct_frag_feed_t ct_frag_feed(ct_frag_assembler_t *a, const uint8_t *frame, size_t len);

/**
 * Slice JSON into ATT frames. Each frame is malloc'd; caller frees all.
 * @param out_frames array of pointers (caller provides capacity)
 * @param out_lens   parallel lengths
 * @param max_frames capacity of arrays
 * @return frame count, or -1 on error
 */
int ct_frag_slice(int mtu, uint8_t kind, uint8_t message_id,
                  const char *json, size_t json_len,
                  uint8_t **out_frames, size_t *out_lens, int max_frames);

#ifdef __cplusplus
}
#endif

#endif /* CT_PROV_FRAG_H_ */

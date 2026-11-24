/**
 * @file ct_prov_ble.c
 * @brief CT-Prov/1 NimBLE peripheral for NeoLink (UUIDs 7B1E0001–4).
 */
#include "ct_prov_ble.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "ct_prov.h"
#include "ct_prov_frag.h"
#include "wifi.h"

#define TAG "-->CTBLE"

/* 7B1E0001-9D5B-4B5B-8F6D-2A4C0B7E0100 (LE wire order) */
static const ble_uuid128_t UUID_SVC =
    BLE_UUID128_INIT(0x00, 0x01, 0x7E, 0x0B, 0x4C, 0x2A, 0x6D, 0x8F,
                     0x5B, 0x4B, 0x5B, 0x9D, 0x01, 0x00, 0x1E, 0x7B);
static const ble_uuid128_t UUID_C2D =
    BLE_UUID128_INIT(0x00, 0x01, 0x7E, 0x0B, 0x4C, 0x2A, 0x6D, 0x8F,
                     0x5B, 0x4B, 0x5B, 0x9D, 0x02, 0x00, 0x1E, 0x7B);
static const ble_uuid128_t UUID_D2C =
    BLE_UUID128_INIT(0x00, 0x01, 0x7E, 0x0B, 0x4C, 0x2A, 0x6D, 0x8F,
                     0x5B, 0x4B, 0x5B, 0x9D, 0x03, 0x00, 0x1E, 0x7B);
static const ble_uuid128_t UUID_META =
    BLE_UUID128_INIT(0x00, 0x01, 0x7E, 0x0B, 0x4C, 0x2A, 0x6D, 0x8F,
                     0x5B, 0x4B, 0x5B, 0x9D, 0x04, 0x00, 0x1E, 0x7B);

#define CT_BLE_MAX_FRAMES 64
#define CT_BLE_RX_MAX     (CT_FRAG_MAX_LOGICAL + 8)

typedef struct {
    uint16_t conn;
    uint8_t message_id;
    char *json;
    size_t json_len;
} ct_ble_job_t;

static bool s_want_up;
static bool s_started;
static bool s_synced;
static uint8_t s_own_addr_type;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_mtu = 23;
static uint16_t s_c2d_handle;
static uint16_t s_d2c_handle;
static uint16_t s_meta_handle;
static bool s_d2c_indicate;

static char s_name[20];
static uint8_t s_meta[20];
static uint8_t s_meta_len;

static ct_frag_assembler_t s_rx;
static QueueHandle_t s_job_q;
static SemaphoreHandle_t s_ind_done;
static TaskHandle_t s_worker;

void ble_store_config_init(void);

static void build_identity(void)
{
    ct_prov_ble_name(s_name, sizeof(s_name));
    size_t nlen = strlen(s_name);
    if (nlen > 17) {
        nlen = 17;
        s_name[17] = '\0';
    }
    /* META: protocol_version | flags(provisionable) | name_len | name */
    s_meta[0] = 0x01;
    s_meta[1] = 0x02;
    s_meta[2] = (uint8_t)nlen;
    memcpy(s_meta + 3, s_name, nlen);
    s_meta_len = (uint8_t)(3 + nlen);
}

static int gap_event(struct ble_gap_event *event, void *arg);
/* Defined below; GATT RX may lazy-start the worker. */
esp_err_t ct_prov_ble_start_worker(void);

static void advertise(void)
{
    if (!s_want_up || !s_synced) {
        return;
    }
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        return;
    }

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);

    /* Adv: 02 01 06 | 17 21 | UUID LE | ver model mac3 flags — 27B */
    uint8_t adv[27];
    adv[0] = 0x02;
    adv[1] = 0x01;
    adv[2] = 0x06;
    adv[3] = 0x17;
    adv[4] = 0x21;
    static const uint8_t uuid_le[16] = {
        0x00, 0x01, 0x7E, 0x0B, 0x4C, 0x2A, 0x6D, 0x8F,
        0x5B, 0x4B, 0x5B, 0x9D, 0x01, 0x00, 0x1E, 0x7B};
    memcpy(adv + 5, uuid_le, 16);
    adv[21] = 0x01; /* format_version */
    adv[22] = CT_PROV_MODEL_CODE;
    adv[23] = mac[3];
    adv[24] = mac[4];
    adv[25] = mac[5];
    adv[26] = 0x02; /* provisionable */

    size_t nlen = strlen(s_name);
    uint8_t rsp[2 + 17];
    rsp[0] = (uint8_t)(1 + nlen);
    rsp[1] = 0x09;
    memcpy(rsp + 2, s_name, nlen);

    int rc = ble_gap_adv_set_data(adv, sizeof(adv));
    if (rc != 0) {
        ESP_LOGE(TAG, "adv set_data rc=%d", rc);
        return;
    }
    rc = ble_gap_adv_rsp_set_data(rsp, (int)(2 + nlen));
    if (rc != 0) {
        ESP_LOGE(TAG, "adv rsp rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params ap;
    memset(&ap, 0, sizeof(ap));
    ap.conn_mode = BLE_GAP_CONN_MODE_UND;
    ap.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &ap, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv start rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "advertising as %s", s_name);
}

static int send_indicate(uint16_t conn, const uint8_t *data, size_t len)
{
    if (!s_d2c_indicate || conn == BLE_HS_CONN_HANDLE_NONE) {
        return BLE_HS_ENOTCONN;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) {
        return BLE_HS_ENOMEM;
    }
    if (s_ind_done) {
        xSemaphoreTake(s_ind_done, 0); /* clear stale */
    }
    int rc = ble_gatts_indicate_custom(conn, s_d2c_handle, om);
    if (rc != 0) {
        return rc;
    }
    if (s_ind_done) {
        if (xSemaphoreTake(s_ind_done, pdMS_TO_TICKS(5000)) != pdTRUE) {
            ESP_LOGW(TAG, "indicate confirm timeout");
            return BLE_HS_ETIMEOUT;
        }
    }
    return 0;
}

static void worker_task(void *arg)
{
    (void)arg;
    for (;;) {
        ct_ble_job_t job;
        if (xQueueReceive(s_job_q, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        wifi_ble_prov_touch();

        char *resp = NULL;
        esp_err_t hr = ct_prov_handle_json(job.json, (int)job.json_len, &resp);
        free(job.json);
        job.json = NULL;

        /* Wi-Fi Rescan can drop the phone link; wait briefly for reconnect
         * before discarding the scan/config response. */
        uint16_t conn = job.conn;
        if (conn != BLE_HS_CONN_HANDLE_NONE && s_conn == BLE_HS_CONN_HANDLE_NONE) {
            for (int i = 0; i < 25 && s_conn == BLE_HS_CONN_HANDLE_NONE; i++) {
                vTaskDelay(pdMS_TO_TICKS(40));
            }
            if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
                conn = s_conn;
                ESP_LOGI(TAG, "GATT rebind after radio op — deliver on handle=%u", conn);
            }
        }
        if (conn == BLE_HS_CONN_HANDLE_NONE || conn != s_conn) {
            ESP_LOGW(TAG, "drop response — no GATT peer after radio op");
            free(resp);
            continue;
        }

        const char *out = resp;
        size_t out_len = resp ? strlen(resp) : 0;
        char errbuf[160];
        if (hr == ESP_ERR_INVALID_ARG || !resp) {
            snprintf(errbuf, sizeof(errbuf),
                     "{\"id\":0,\"op\":\"protocol.error.result\",\"ok\":false,"
                     "\"error\":{\"code\":\"INVALID_MESSAGE\",\"message\":\"malformed\"}}");
            out = errbuf;
            out_len = strlen(errbuf);
        }

        uint8_t *frames[CT_BLE_MAX_FRAMES];
        size_t lens[CT_BLE_MAX_FRAMES];
        int n = ct_frag_slice((int)s_mtu, CT_FRAG_KIND_RESPONSE, job.message_id,
                              out, out_len, frames, lens, CT_BLE_MAX_FRAMES);
        free(resp);
        if (n < 0) {
            ESP_LOGE(TAG, "slice failed mtu=%u len=%u", (unsigned)s_mtu, (unsigned)out_len);
            continue;
        }
        for (int i = 0; i < n; i++) {
            if (s_conn != conn) {
                for (int j = i; j < n; j++) {
                    free(frames[j]);
                }
                break;
            }
            int rc = send_indicate(conn, frames[i], lens[i]);
            free(frames[i]);
            if (rc != 0) {
                ESP_LOGW(TAG, "indicate[%d/%d] rc=%d", i, n, rc);
                for (int j = i + 1; j < n; j++) {
                    free(frames[j]);
                }
                break;
            }
        }
    }
}

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        if (attr_handle == s_meta_handle) {
            int rc = os_mbuf_append(ctxt->om, s_meta, s_meta_len);
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (attr_handle != s_c2d_handle) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);
        if (om_len < CT_FRAG_HDR_SIZE || om_len > 512) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        uint8_t tmp[512];
        int rc = ble_hs_mbuf_to_flat(ctxt->om, tmp, sizeof(tmp), NULL);
        if (rc != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        wifi_ble_prov_touch();
        ct_frag_feed_t st = ct_frag_feed(&s_rx, tmp, om_len);
        if (st == CT_FRAG_ASSEMBLING) {
            return 0;
        }
        if (st == CT_FRAG_REJECTED) {
            ESP_LOGW(TAG, "rx reject: %s", s_rx.reject_reason);
            return 0;
        }
        /* ASSEMBLED */
        if (s_rx.packet.kind != CT_FRAG_KIND_REQUEST || !s_rx.packet.bytes) {
            free(s_rx.packet.bytes);
            s_rx.packet.bytes = NULL;
            return 0;
        }
        if (!s_job_q) {
            free(s_rx.packet.bytes);
            s_rx.packet.bytes = NULL;
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        if (!s_worker && ct_prov_ble_start_worker() != ESP_OK) {
            free(s_rx.packet.bytes);
            s_rx.packet.bytes = NULL;
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        ct_ble_job_t job = {
            .conn = conn_handle,
            .message_id = s_rx.packet.message_id,
            .json = (char *)s_rx.packet.bytes,
            .json_len = s_rx.packet.len,
        };
        s_rx.packet.bytes = NULL;
        s_rx.packet.len = 0;
        if (xQueueSend(s_job_q, &job, 0) != pdTRUE) {
            free(job.json);
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_SVC.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &UUID_C2D.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC,
                .val_handle = &s_c2d_handle,
            },
            {
                .uuid = &UUID_D2C.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_INDICATE | BLE_GATT_CHR_F_READ |
                         BLE_GATT_CHR_F_READ_ENC,
                .val_handle = &s_d2c_handle,
            },
            {
                .uuid = &UUID_META.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_READ,
                .val_handle = &s_meta_handle,
            },
            {0},
        },
    },
    {0},
};

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            s_mtu = 23;
            ct_frag_assembler_reset(&s_rx);
            ct_frag_assembler_set_mtu(&s_rx, 23);
            s_d2c_indicate = false;
            wifi_ble_prov_touch();
            ESP_LOGI(TAG, "connected handle=%u", s_conn);
            ble_gap_security_initiate(s_conn);
        } else {
            ESP_LOGW(TAG, "connect fail status=%d", event->connect.status);
            advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnect reason=%d", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_d2c_indicate = false;
        s_mtu = 23;
        ct_frag_assembler_reset(&s_rx);
        if (s_want_up) {
            advertise();
        }
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_d2c_handle) {
            s_d2c_indicate = event->subscribe.cur_indicate;
            ESP_LOGI(TAG, "D2C indicate=%d", (int)s_d2c_indicate);
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        s_mtu = event->mtu.value;
        ct_frag_assembler_set_mtu(&s_rx, (int)s_mtu);
        ESP_LOGI(TAG, "mtu=%u", s_mtu);
        return 0;

    case BLE_GAP_EVENT_NOTIFY_TX:
        if (event->notify_tx.indication &&
            (event->notify_tx.status == 0 || event->notify_tx.status == BLE_HS_EDONE)) {
            /* Wait for peer confirm (EDONE); also accept status 0 as progress. */
            if (event->notify_tx.status == BLE_HS_EDONE && s_ind_done) {
                xSemaphoreGive(s_ind_done);
            }
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "enc_change status=%d", event->enc_change.status);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_want_up && s_conn == BLE_HS_CONN_HANDLE_NONE) {
            advertise();
        }
        return 0;

    default:
        return 0;
    }
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "nimble reset reason=%d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer addr rc=%d", rc);
        return;
    }
    s_synced = true;
    advertise();
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ct_prov_ble_start(void)
{
    if (s_started) {
        s_want_up = true;
        if (s_synced && s_conn == BLE_HS_CONN_HANDLE_NONE) {
            advertise();
        }
        return ESP_OK;
    }

    build_identity();
    ct_frag_assembler_init(&s_rx, 23);

    if (!s_job_q) {
        s_job_q = xQueueCreate(2, sizeof(ct_ble_job_t));
    }
    if (!s_ind_done) {
        s_ind_done = xSemaphoreCreateBinary();
    }
    if (!s_job_q || !s_ind_done) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "nimble init — free %u int %u largest_int %u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    s_want_up = true;
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts count rc=%d", rc);
        nimble_port_deinit();
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts add rc=%d", rc);
        nimble_port_deinit();
        return ESP_FAIL;
    }
    ble_svc_gap_device_name_set(s_name);
    ble_store_config_init();

    nimble_port_freertos_init(host_task);

    /* Worker is started later via ct_prov_ble_start_worker() AFTER BLE-warm
     * Wi-Fi init — C61 forbids PSRAM task stacks, so the 5KB worker must
     * not compete with esp_wifi_init for contiguous internal RAM. */

    s_started = true;
    ESP_LOGI(TAG, "NimBLE CT-Prov started (%s) — free %u int %u largest_int %u",
             s_name,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}

esp_err_t ct_prov_ble_start_worker(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_worker) {
        return ESP_OK;
    }
    /* 4K is enough for CT-Prov JSON; keep lean for post-NimBLE heap. */
    if (xTaskCreate(worker_task, "ct_ble", 4096, NULL, 5, &s_worker) != pdPASS) {
        ESP_LOGE(TAG, "ct_ble worker create failed (int=%u largest=%u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        s_worker = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ct_ble worker up — free %u int %u largest_int %u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}

void ct_prov_ble_stop(void)
{
    s_want_up = false;
    if (!s_started) {
        return;
    }
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    ble_gap_adv_stop();
    s_synced = false;

    /* Full host/controller teardown — required so SoftAP/httpd can reclaim RAM.
     * Silent "adv stop only" left BLE stuck and SoftAP starved. */
    int rc = nimble_port_stop();
    if (rc != 0) {
        ESP_LOGW(TAG, "nimble_port_stop rc=%d — deinit anyway", rc);
    }
    nimble_port_deinit();
    s_started = false;
    /* The host task is gone: release any half-received request buffer, or the
     * next ct_frag_assembler_init() zeroes the pointer and leaks it. */
    ct_frag_assembler_deinit(&s_rx);
    ESP_LOGI(TAG, "NimBLE fully stopped");
}

bool ct_prov_ble_is_up(void)
{
    return s_want_up && s_started;
}

bool ct_prov_ble_client_connected(void)
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE;
}

void ct_prov_ble_rf_quiet(void)
{
    if (!s_started) {
        return;
    }
    ble_gap_adv_stop();
    ESP_LOGI(TAG, "RF quiet — adv stopped for Wi-Fi associate");
}

void ct_prov_ble_rf_restore(void)
{
    if (!s_started || !s_want_up) {
        return;
    }
    if (s_synced && s_conn == BLE_HS_CONN_HANDLE_NONE) {
        advertise();
    } else if (s_synced) {
        /* Keep link; advertising optional while connected. */
        ESP_LOGI(TAG, "RF restore — GATT still up, skip adv");
    }
}

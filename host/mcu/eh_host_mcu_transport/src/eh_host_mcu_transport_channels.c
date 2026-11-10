/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"

#include "eh_common_interface.h"
#include "eh_common_header.h"
#include "eh_frame.h"
#include "eh_host_mcu_transport.h"
#include "eh_host_mcu_transport_channels.h"
#include "eh_host_mcu_transport_priv.h"
#include "eh_host_mcu_transport_state.h"
#include "eh_host_port_dma.h"
#include "eh_frame.h"
#include "esp_netif_types.h"
#include "eh_host_frame_pool.h"

#define TAG "eh_chan"

int eh_host_auto_init_ensure(void);

eh_host_channel_t *chan_arr[ESP_MAX_IF];
static eh_host_channel_t *aux_chan_arr[ESP_AUX_TYPE_MAX];

#if EH_HOST_PORT_WIFI_TX_DATA_THROTTLE_EN
volatile uint32_t wifi_tx_throttling;
#endif

/* What lwIP gets when a Wi-Fi frame cannot be queued now: it backs off. */
#if defined(ESP_ERR_ESP_NETIF_TX_FAILED)
#define EH_TX_BACKOFF_ERR ESP_ERR_ESP_NETIF_TX_FAILED
#else
#define EH_TX_BACKOFF_ERR ESP_ERR_ESP_NETIF_NO_MEM
#endif

#ifdef CONFIG_ESP_HOSTED_HOST_SDIO_FRAME_POOLS
#define EH_TX_POOL_WAIT_MS CONFIG_ESP_HOSTED_HOST_SDIO_TX_POOL_WAIT_MS
#else
#define EH_TX_POOL_WAIT_MS 0
#endif

/* A buffer for one outgoing frame and the free that matches it. Wi-Fi data
 * comes from the TX pool (Lichen #260) and waits a few ms when it is empty;
 * control frames, and every frame when there is no pool, use the heap. */
static uint8_t *tx_frame_alloc(uint8_t if_type, size_t total, void (**free_fn)(void *),
                               esp_err_t *err)
{
    *err = ESP_ERR_NO_MEM;
    if (if_type == ESP_STA_IF || if_type == ESP_AP_IF) {
        eh_fp_get_t why = EH_FP_OFF;
        void *p = eh_host_frame_pool_tx_get(total, EH_TX_POOL_WAIT_MS, &why);
        if (why == EH_FP_GOT) {
            *free_fn = eh_host_frame_pool_tx_put;
            return (uint8_t *)p;
        }
        if (why == EH_FP_EMPTY) {
            *err = EH_TX_BACKOFF_ERR;
            return NULL;
        }
    }
    *free_fn = eh_host_port_dma_free;
    /* PSRAM first only where the bus copies the frame before any DMA (SDIO
     * STREAM, see eh_host_frame_pool.h); otherwise DMA-capable internal RAM,
     * which sdmmc/SPI master need for a buffer they DMA from. */
    return (uint8_t *)eh_host_frame_tx_heap_alloc(total);
}

static esp_err_t transport_tx(uint8_t if_type, uint8_t if_num, uint8_t pkt_type,
                              const uint8_t *payload, uint16_t payload_len,
                              uint8_t flags)
{
    if (!payload || payload_len == 0) return ESP_FAIL;
    /* The first TX before the link is up starts auto-init. The host caps packet
     * sent inside bring-up comes after TX_ACTIVE, so it skips this. */
    if (!eh_host_mcu_transport_state_is_tx_ready()) {
        (void)eh_host_auto_init_ensure();
    }

    if (eh_host_wifi_tx_is_throttled() &&
        (if_type == ESP_STA_IF || if_type == ESP_AP_IF)) {
        return EH_TX_BACKOFF_ERR;
    }

    size_t total = eh_frame_hdr_size() + payload_len;
    void (*free_fn)(void *) = NULL;
    esp_err_t err = ESP_ERR_NO_MEM;
    uint8_t *buf = tx_frame_alloc(if_type, total, &free_fn, &err);
    if (!buf) return err;

    memcpy(buf + eh_frame_hdr_size(), payload, payload_len);

    /* Bus owns lifetime of `buf`. */
    interface_buffer_handle_t bh = {
        .priv_buffer_handle = buf,
        .if_type            = if_type,
        .if_num             = if_num,
        .flags              = flags,
        .pkt_type           = pkt_type,
        .payload            = buf + eh_frame_hdr_size(),
        .payload_len        = payload_len,
        .payload_zcopy      = 0,
        .free_buf_handle    = free_fn,
    };
    eh_frame_encode(buf, &bh, payload_len);
    int rc = eh_host_mcu_transport_tx(&bh);
    if (rc >= 0) {
        return ESP_OK;
    }
    if (if_type == ESP_STA_IF || if_type == ESP_AP_IF) {
        return EH_TX_BACKOFF_ERR;
    }
    return ESP_FAIL;
}

esp_err_t eh_host_transport_tx(uint8_t if_type, uint8_t if_num,
                                const uint8_t *payload, uint16_t payload_len,
                                uint8_t flags)
{
    return transport_tx(if_type, if_num, 0, payload, payload_len, flags);
}

esp_err_t eh_host_transport_aux_tx(uint8_t aux_type,
                                    const uint8_t *payload, uint16_t len,
                                    uint8_t flags)
{
    if (aux_type >= ESP_AUX_TYPE_MAX)
        return ESP_ERR_INVALID_ARG;
    return transport_tx(ESP_AUX_IF, 0, aux_type, payload, len, flags);
}

static esp_err_t sta_tx(void *h, void *buf, size_t len)
{
    (void)h;
    if (!buf || len == 0 || len > UINT16_MAX) return ESP_FAIL;
    return eh_host_transport_tx(ESP_STA_IF, 0, buf, (uint16_t)len, 0);
}
static esp_err_t ap_tx(void *h, void *buf, size_t len)
{
    (void)h;
    if (!buf || len == 0 || len > UINT16_MAX) return ESP_FAIL;
    return eh_host_transport_tx(ESP_AP_IF, 0, buf, (uint16_t)len, 0);
}
static esp_err_t serial_tx(void *h, void *buf, size_t len)
{
    (void)h;
    if (!buf || len == 0 || len > UINT16_MAX) return ESP_FAIL;
    return eh_host_transport_tx(ESP_SERIAL_IF, 0, buf, (uint16_t)len, 0);
}

eh_host_channel_t *eh_host_transport_get_channel(uint8_t if_type)
{
    return (if_type < ESP_MAX_IF) ? chan_arr[if_type] : NULL;
}

/* NULL when out of range. */
static eh_host_channel_t **channel_slot(uint8_t if_type, uint8_t aux_type)
{
    if (if_type == ESP_AUX_IF)
        return (aux_type < ESP_AUX_TYPE_MAX) ? &aux_chan_arr[aux_type] : NULL;
    return (if_type < ESP_MAX_IF) ? &chan_arr[if_type] : NULL;
}

static eh_host_channel_t *get_aux_channel(uint8_t aux_type)
{
    eh_host_channel_t **slot = channel_slot(ESP_AUX_IF, aux_type);
    return slot ? *slot : NULL;
}

eh_host_channel_t *eh_host_transport_add_aux_channel(uint8_t aux_type,
                                                      eh_host_channel_rx_fn_t rx)
{
    eh_host_channel_t **slot = channel_slot(ESP_AUX_IF, aux_type);
    if (!slot || !rx) return NULL;

    free(*slot);
    *slot = NULL;

    eh_host_channel_t *chan = (eh_host_channel_t *)calloc(1, sizeof(*chan));
    if (!chan) return NULL;

    chan->if_type  = ESP_AUX_IF;
    chan->aux_type = aux_type;
    chan->rx       = rx;
    chan->api_chan = chan;

    *slot = chan;
    return chan;
}

void eh_host_transport_aux_rx(uint8_t aux_type, const uint8_t *payload, uint16_t len)
{
    eh_host_channel_t *chan = get_aux_channel(aux_type);

    if (!chan || !chan->rx) {
        ESP_LOGW(TAG, "rx-drop: no channel for aux_type=%u len=%u",
                 (unsigned int)aux_type, (unsigned int)len);
        return;
    }
    /* Copy: channel rx may retain past return; bus frees DMA sync. */
    uint8_t *copy = (uint8_t *)malloc(len);
    if (!copy) {
        ESP_LOGE(TAG, "rx-drop: aux_type=%u copy alloc failed len=%u",
                 (unsigned int)aux_type, (unsigned int)len);
        return;
    }
    memcpy(copy, payload, len);
    (void)chan->rx(chan->api_chan, copy, copy, len);
}

eh_host_channel_t *eh_host_transport_add_channel(void *api_chan,
                                                  uint8_t if_type,
                                                  uint8_t secure,
                                                  eh_host_channel_tx_fn_t *tx_out,
                                                  eh_host_channel_rx_fn_t rx)
{
    if (if_type >= ESP_MAX_IF || !tx_out || !rx) return NULL;

    if (chan_arr[if_type]) {
        free(chan_arr[if_type]);
        chan_arr[if_type] = NULL;
    }

    eh_host_channel_t *chan = (eh_host_channel_t *)calloc(1, sizeof(*chan));
    if (!chan) return NULL;

    chan->if_type  = if_type;
    chan->secure   = secure;
    chan->rx       = rx;
    chan->api_chan = api_chan ? api_chan : chan;

    switch (if_type) {
    case ESP_STA_IF:    chan->tx = sta_tx;    break;
    case ESP_AP_IF:     chan->tx = ap_tx;     break;
    case ESP_SERIAL_IF: chan->tx = serial_tx; break;
    default:
        free(chan);
        return NULL;
    }

    *tx_out = chan->tx;
    chan_arr[if_type] = chan;
    return chan;
}

int eh_host_transport_remove_channel(eh_host_channel_t *channel)
{
    eh_host_channel_t **slot =
        channel ? channel_slot(channel->if_type, channel->aux_type) : NULL;

    if (!slot || *slot != channel) return -1;
    *slot = NULL;
    free(channel);
    return 0;
}

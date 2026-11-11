/* Shared Broadcom full-MAC firmware protocol (CDC control, BDC data).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_NET_BCM_FULLMAC_H
#define HW_NET_BCM_FULLMAC_H
#include "hw/core/qdev.h"
#include "qemu/timer.h"
#include "net/net.h"

#define BCM_FULLMAC_CONTROL 0
#define BCM_FULLMAC_EVENT 1
#define BCM_FULLMAC_DATA 2

/*
 * The CDC control header that rides on channel 0, in two sizes.
 *
 * 2.1.1's AppleBCM4325 uses the three word form - command, length, flags -
 * with no status word: a WLC_UP request arrives as a 24 byte frame, twelve
 * bytes of SDPCM and twelve of CDC with no payload at all.
 *
 * 3.1.3's AppleBCMWLAN uses the four word cdc_ioctl_t that brcmfmac
 * documents, with a trailing status word, so the same WLC_UP request is a 28
 * byte frame. A reply built with the short header makes the driver log
 * "AppleBCMWLANCmdManager::processResponse(): No space for cdc_ioctl_t header
 * in response. Dropping." and every command then times out.
 *
 * Which one is in use is read off the wire rather than configured: the SDPCM
 * software header says where the CDC starts and the CDC header says how long
 * its payload is, so the difference is the header size. See cdc_hdrlen().
 */
#define CDC_HDRLEN 12
#define CDC_HDRLEN_STATUS 16
#define CDC_OFF_STATUS 12
#define CDC_DCMD_ERROR 0x01
#define CDC_DCMD_SET 0x02

#define WLC_GET_VAR 262
#define WLC_SET_VAR 263

/*
 * Events do not arrive on the event channel. This driver dispatches receive by
 * channel and answers channel 1 with "WTF?? Got an event packet!!!" before
 * dropping it - events come up the data channel, BDC encapsulated, as an 802.3
 * frame that handleDataPacket recognizes by its ethertype.
 */
/*
 * handleDataPacket logs byte 0 as bdc->flags and byte 1 as bdc->priority, then
 * advances the packet by six - not the four a bdc_header occupies - before
 * treating the rest as 802.3. The two extra bytes are padding that lands the
 * IP header on a four-byte boundary.
 */
#define BDC_HDRLEN 6
/*
 * 3.1.3's AppleBCMWLAN skips the four a bdc_header really occupies. Its
 * handleDataPacket does "movs r3, #4; add r3, fp" (VA 0xc0707f32) and its
 * handleEventPacket (VA 0xc070773c) memcmps the OUI at packet offset 0x17 and
 * reads usr_subtype at 0x1a - which are 0x13 and 0x16 from the ethernet header
 * once four bytes of BDC are subtracted, i.e. exactly 2.1.1's offsets. So only
 * the header length changed; deliver a six byte one to this driver and every
 * frame is shifted by two, the OUI check fails, and it is dropped in silence.
 */
#define BDC_HDRLEN_STD 4
#define BDC_MAX_HDRLEN 6
#define BDC_PROTO_VER 2
/* wlioctl.h: toe_ol bit, the dongle checksums transmitted TCP/UDP. */
#define TOE_TX_CSUM_OL 0x00000001

#define ETHER_TYPE_BRCM 0x886c
#define BCMETH_SUBTYPE 0x8001
#define BCMETH_USR_SUBTYPE 0x0001
/* handleEventPacket memcmps three bytes at packet offset 0x13 against this. */
#define BCMETH_OUI_0 0x00
#define BCMETH_OUI_1 0x10
#define BCMETH_OUI_2 0x18

/* Event numbers, read out of the driver's own dispatch table. */
#define WLC_E_SET_SSID 0
#define WLC_E_JOIN 1
#define WLC_E_AUTH 3
#define WLC_E_ASSOC 7
#define WLC_E_LINK 16
#define WLC_E_SCAN_COMPLETE 26
#define WLC_E_ESCAN_RESULT 69
#define ESCAN_RESULTS_FIXED 12
#define ESCAN_STATUS_PARTIAL 8
#define ESCAN_STATUS_ABORT 4

/* wl_event_msg_t flags. The driver reads the link bit out of a LINK event to
 * tell "link came up" from "link went down". */
#define WLC_EVENT_MSG_LINK 0x01

/* ioctls the model answers with something other than zeros. */
#define WLC_UP 2
#define WLC_GET_RATE 12
#define WLC_GET_BSSID 23
#define WLC_GET_SSID 25
#define WLC_SET_SSID 26
#define WLC_GET_RSSI 127
#define WLC_GET_BSS_INFO 136

/* The largest reply the model will build for a get. */
#define CDC_MAX_PAYLOAD 8192

/*
 * wl_bss_info_t, as the driver's beacon factory reads it. The reply to
 * WLC_GET_BSS_INFO is a four byte buffer length followed by the structure -
 * the driver passes buffer+4 to the factory.
 */
#define BSS_INFO_VERSION 108
#define BSS_INFO_OFF_VERSION 0x00
#define BSS_INFO_OFF_LENGTH 0x04
#define BSS_INFO_OFF_BSSID 0x08
#define BSS_INFO_OFF_BEACON 0x0e
#define BSS_INFO_OFF_CAPABILITY 0x10
#define BSS_INFO_OFF_SSID_LEN 0x12
#define BSS_INFO_OFF_SSID 0x13
#define BSS_INFO_OFF_RATE_COUNT 0x34
#define BSS_INFO_OFF_RATES 0x38
#define BSS_INFO_OFF_CHANNEL 0x48
#define BSS_INFO_OFF_RSSI 0x4e
#define BSS_INFO_OFF_PHY_NOISE 0x50
#define BSS_INFO_OFF_IE_OFFSET 0x74
#define BSS_INFO_OFF_IE_LENGTH 0x78
#define BSS_INFO_LEN 0x7c
#define BSS_CAP_ESS 0x0001
/* SSID, supported rates, DS parameter set and country, appended as beacon IEs. */
#define BSS_INFO_IE_LEN (2 + 8 + 2 + 4 + 2 + 1 + 2 + 6)
#define BSS_INFO_TOTAL (BSS_INFO_LEN + BSS_INFO_IE_LEN)

/*
 * wl_iscan_results_t, the reply to get_var iscanresults: a status word followed
 * by a wl_scan_results_t (buflen, version, count) and the BSS list. buflen
 * counts from the version field to the end of the last entry.
 */
#define ISCAN_OFF_STATUS 0x00
#define ISCAN_OFF_BUFLEN 0x04
#define ISCAN_OFF_VERSION 0x08
#define ISCAN_OFF_COUNT 0x0c
#define ISCAN_OFF_BSS 0x10
#define ISCAN_RESULTS_FIXED 0x0c /* wl_scan_results_t, from buflen on */
#define ISCAN_TOTAL (ISCAN_OFF_BSS + BSS_INFO_TOTAL)

/* get_var "counters": wl_cnt_t, version and length (16 bits each) then 32-bit
 * counters; rxbeaconmbss is the 86th (the 4329's 4.218 layout). */
#define WL_CNT_OFF_RXBEACONMBSS 0x158

/* How long a scan is made to appear to take before it reports complete. */
#define SCAN_COMPLETE_DELAY_NS (1500 * 1000 * 1000LL)

/* wl_iscan_results_t status. The driver keeps polling on PARTIAL. */
#define WL_SCAN_RESULTS_SUCCESS 0
#define WL_SCAN_RESULTS_PARTIAL 1

/* wlc_ssid_t, the payload of WLC_SET_SSID: a length word then up to 32 bytes. */
#define WLC_SSID_MAX 32

/* wl_event_msg: 46 bytes through 6.x; later firmware appends ifidx and bsscfgidx (0 here). 7.x's
 * handleEventPacket drops a frame that does not run past offset 76, the 28 bytes of BDC/ether/bcmeth
 * headers plus that 48-byte form ("DeviceBuffer length 76 shorter than wl_event_msg_t start at 76"), so
 * the message carries four more zero bytes. Earlier drivers read the fields they know (datalen is 0) and
 * ignore the tail. */
#define WL_EVENT_MSG_LEN 52

typedef void (*BCMFullmacSend)(void *, unsigned, const uint8_t *, uint32_t);
typedef bool (*BCMFullmacCanReceive)(void *);
typedef struct BCMFullmacState {
    BCMFullmacSend send;
    BCMFullmacCanReceive can_receive;
    void *opaque;
    uint8_t mac[6];
    const char *fw_version;
    bool dongle_started, iscan_reported, associated, host_netif;
    bool escan_pending;
    uint16_t escan_sync_id;
    uint32_t escan_status;
    unsigned cdc_hdrlen, bdc_hdrlen;
    /* Enabled by transports whose firmware supports BDC signalling. */
    bool fws_supported;
    uint32_t fws_tlv;
    NICState *nic;
    NICConf conf;
    QEMUTimer *scan_timer, *join_timer;
    uint8_t bssid[6];
    char country[3];
    uint32_t toe, toe_ol;
    unsigned tx_log, host_rx_log;
} BCMFullmacState;
extern const uint8_t bcm_fullmac_default_bssid[6];
void bcm_fullmac_init(BCMFullmacState *, BCMFullmacSend, BCMFullmacCanReceive,
                      void *);
void bcm_fullmac_reset(BCMFullmacState *);
void bcm_fullmac_cleanup(BCMFullmacState *);
void bcm_fullmac_setup_net(BCMFullmacState *, DeviceState *, const char *,
                           const char *);
/* known_hdrlen is 0 for legacy SDIO inference, 16 for USB's fixed form. */
void bcm_fullmac_cdc(BCMFullmacState *, const uint8_t *, uint32_t, unsigned);
void bcm_fullmac_tx_data(BCMFullmacState *, const uint8_t *, uint32_t);
#endif

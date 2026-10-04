/*
 * Fake cellular baseband for the emulated iPhones: transport-independent core.
 *
 * Bytes in from the AP's UART (or HSIC later), bytes out through a callback. The core stacks what
 * iPhone OS 1.0's CommCenter speaks (docs/baseband/commcenter-1.0.md): an optional H5 (3-wire UART)
 * link, a 27.010 basic-mode multiplexer, and an AT engine per DLCI over a small network/SIM/call model.
 * No QEMU objects in here, so the unit test drives it directly; ios_baseband.c is the QEMU side.
 */
#ifndef HW_MISC_IOS_BASEBAND_CORE_H
#define HW_MISC_IOS_BASEBAND_CORE_H

#define IOS_BB_MAX_CH    8     /* AT channels: 0 = pre-mux line, 1..7 = DLCIs */
#define IOS_BB_MAX_CALLS 4
#define IOS_BB_H5_WINDOW 7

typedef void (*IosBbOutFn)(void *opaque, const uint8_t *buf, size_t len);

/* +XCALLSTAT states (Infineon). */
enum {
    IOS_BB_CALL_ACTIVE = 0,
    IOS_BB_CALL_HELD = 1,
    IOS_BB_CALL_DIALING = 2,
    IOS_BB_CALL_ALERTING = 3,
    IOS_BB_CALL_INCOMING = 4,
    IOS_BB_CALL_WAITING = 5,
    IOS_BB_CALL_RELEASED = 6,
    IOS_BB_CALL_CONNECTED = 7,
};

typedef struct IosBbCall {
    bool used;
    bool mt;
    int id;
    int stat;
    int64_t due_ms;            /* next scripted step (MO progress, MT ring repeat) */
    char number[32];
} IosBbCall;

typedef struct IosBbAtChan {
    char line[600];
    unsigned len;
    bool echo;
    bool open;                 /* DLCI established (channel 0: always) */
} IosBbAtChan;

typedef struct IosBbH5Pkt {
    uint8_t seq;
    uint16_t len;
    uint8_t data[512];
} IosBbH5Pkt;

typedef struct IosBbCore {
    IosBbOutFn out;
    void *opaque;
    int64_t now_ms;

    /* Network and SIM, set by the device's properties. */
    char operator_long[33];
    char operator_short[17];
    char plmn[7];              /* MCC+MNC digits */
    int signal_dbm;
    int battery;               /* percent, from the board's charger/PMU model */
    bool registered;           /* attached to the home network when the radio is on */
    bool sim_present;
    unsigned lac, ci;
    char imei[16], imsi[16], iccid[21];
    int answer_delay_ms;       /* MO calls: remote picks up after this (<0: never) */

    /* Link: raw bytes until the host starts H5. */
    bool h5;
    bool h5_active;            /* CONFIG RESP sent */
    bool h5_crc;               /* negotiated data integrity check */
    bool h5_in_frame, h5_esc;
    uint8_t h5_rx[2048];
    unsigned h5_rxlen;
    uint8_t h5_tx_seq, h5_rx_next;
    bool h5_need_ack;
    IosBbH5Pkt h5_unacked[IOS_BB_H5_WINDOW];
    unsigned h5_nunacked;
    int64_t h5_last_tx_ms;
    uint8_t h5_txq[8192];
    unsigned h5_txq_len;

    /* 27.010 basic mode. */
    bool mux;
    bool mux_sleep;
    bool mux_leave;            /* CLD acknowledged: back to AT after this frame */
    uint8_t mf[2048];
    unsigned mflen;

    IosBbAtChan ch[IOS_BB_MAX_CH];

    /* AT/network state. */
    bool hex_cs;               /* +CSCS="HEX" */
    int cfun;
    int cops_format;
    bool cops_detached;        /* +COPS=2 */
    int creg_n, creg_ch;
    int cgreg_n, cgreg_ch;
    int xciev_ch, xsim_ch, call_ch;
    int last_rssi, last_creg;  /* what the host was last told, to send URCs on change only */
    int next_call_id;
    IosBbCall calls[IOS_BB_MAX_CALLS];

    /* Most recent outgoing call, for the device's read-only property. */
    char last_dialed[32];
} IosBbCore;

void ios_bb_init(IosBbCore *bb, IosBbOutFn out, void *opaque);
/* Baseband power cycle (reset GPIO): back to raw AT, no mux, no calls. Keeps the controls. */
void ios_bb_reset(IosBbCore *bb);
void ios_bb_input(IosBbCore *bb, const uint8_t *buf, size_t len);
/* Advance time: H5 retransmit, ring repeat, scripted call progress. */
void ios_bb_tick(IosBbCore *bb, int64_t now_ms);
/* A control (signal, registration, operator, SIM) changed: tell the host what it would see. */
void ios_bb_changed(IosBbCore *bb);

int ios_bb_rssi(const IosBbCore *bb);
/* Network-side call events. Return false if refused (no service, line busy). */
bool ios_bb_incoming_call(IosBbCore *bb, const char *number);
void ios_bb_remote_hangup(IosBbCore *bb);
void ios_bb_remote_answer(IosBbCore *bb);
/* "idle", "dialing", "alerting", "incoming", "active", "held" of the first live call. */
const char *ios_bb_call_state(const IosBbCore *bb);

#endif

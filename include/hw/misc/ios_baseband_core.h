/*
 * Fake cellular baseband for the emulated iPhones: transport-independent core.
 *
 * Bytes in from the AP's UART (or HSIC later), bytes out through a callback. The core stacks what
 * iPhone OS 1.0's CommCenter speaks (docs/baseband/commcenter-1.0.md and
 * commcenter-1.0-calls-sms.md): an H5 (3-wire UART) link, a 27.010 basic-mode multiplexer, and an AT
 * engine per DLCI over a small network/SIM/call/SMS model. No QEMU objects in here, so the unit test
 * drives it directly; ios_baseband.c is the QEMU side.
 *
 * Channel map (CommCenter 1.0): 1 = call, 2 = reg (also battery), 3 = sms/SIM, 4 = low/settings,
 * 5 = pdp_ctl. Channel 0 is the pre-mux "default" dispatcher. The model sends unsolicited codes on
 * the channel that owns them; the exact line formats are the ones the 1.0 parsers accept.
 */
#ifndef HW_MISC_IOS_BASEBAND_CORE_H
#define HW_MISC_IOS_BASEBAND_CORE_H

#define IOS_BB_MAX_CH    16    /* AT channels: 0 = pre-mux line, 1..15 = DLCIs (4.x opens 1..13) */
#define IOS_BB_MAX_CALLS 4
#define IOS_BB_H5_WINDOW 7
#define IOS_BB_SMS_STORE 4     /* +CMGR backfill slots */

typedef void (*IosBbOutFn)(void *opaque, const uint8_t *buf, size_t len);

/* +XCALLSTAT states (Infineon; 7 is never sent - 1.0 crashes on it). */
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
    bool mt;                  /* mobile-terminated (network side originated it) */
    int id;
    int stat;
    int next_stat;            /* scripted step to emit at due_ms, -1 = none */
    int64_t due_ms;           /* next scripted step (MO progress, MT ring repeat) */
    int rings;                /* MT: RING URCs emitted so far (s0 auto-answer) */
    char number[32];
} IosBbCall;

typedef struct IosBbAtChan {
    char line[600];
    unsigned len;
    bool echo;
    bool open;                /* DLCI established (channel 0: always) */
    bool sms_prompt;          /* "> " sent, collecting a +CMGS PDU until ^Z/ESC */
    int data_cid;             /* >0: +CGDATA switched this DLCI to raw IP for that context */
} IosBbAtChan;

typedef struct IosBbH5Pkt {
    uint8_t seq;
    uint16_t len;
    uint8_t data[1024];
} IosBbH5Pkt;

/* 27.010 basic-mode receive state machine. */
enum {
    IOS_BB_MX_SEARCH = 0,     /* outside a frame */
    IOS_BB_MX_ADDR,
    IOS_BB_MX_CTRL,
    IOS_BB_MX_LEN0,
    IOS_BB_MX_LEN1,
    IOS_BB_MX_DATA,
    IOS_BB_MX_FCS,
};

typedef struct IosBbMuxRx {
    int state;
    unsigned flags_run;       /* consecutive 0xF9 outside frames (wake-up answer) */
    uint8_t addr, ctrl;
    unsigned len, cnt;
    uint8_t buf[1600];
    uint8_t fcs;
} IosBbMuxRx;

typedef struct IosBbSms {
    bool used;
    char num[32];
    char pdu[400];            /* full 23.040 hex PDU as sent in +CMT */
} IosBbSms;

typedef struct IosBbCore {
    IosBbOutFn out;
    void *opaque;
    int64_t now_ms;
    int64_t wall_offset_ms;    /* now_ms + this = Unix ms (SMS timestamps); set by the device */

    /* Network and SIM, set by the device's properties. */
    char operator_long[33];
    char operator_short[17];
    char plmn[7];              /* MCC+MNC digits */
    char sca[24];              /* service centre number, "" = none */
    char voicemail[24];
    int signal_dbm;
    int battery;               /* percent, from the board's charger/PMU model */
    bool registered;           /* attached to the home network when the radio is on */
    bool nitz;                 /* the network sends its time zone (+CTZV) once the host asks (+CTZR) */
    bool ctzr_on;              /* +CTZR=1: time zone reports wanted, on ctzr_ch */
    int ctzr_ch;
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
    bool mux_leave;            /* CLD acknowledged: back to AT after this frame */
    IosBbMuxRx muxrx;

    IosBbAtChan ch[IOS_BB_MAX_CH];

    /* AT/network state. */
    bool hex_cs;               /* +CSCS="HEX" */
    bool colp_off;             /* +COLP=0; on by default (1.0 never sends +COLP=1) */
    int cfun;
    int cops_format;
    bool cops_detached;        /* +COPS=2 */
    int creg_n, creg_ch;
    int xreg_n;                /* +XREG (4.x data bearer) URCs enabled; they ride creg_ch */
    bool xsigstr_on;           /* +xsigstr=1 (iOS 6): +XSIGSTR signal reports instead of +xcgedpage polls */
    int xsigstr_ch;
    int cgreg_n, cgreg_ch;
    int xciev_ch, xsim_ch, call_ch, sms_ch;   /* learnt from where each is enabled */
    int s0;                    /* auto-answer register (at s0=n) */
    int last_rssi, last_batt;  /* what the host was last told, to send +XCIEV on change only */
    bool sim_last;            /* last SIM presence pushed, to send +XSIM on change only */
    int last_creg;             /* last +CREG stat emitted */
    int reg_step;              /* 0 idle, 1 search scheduled, 2 registered scheduled */
    int64_t reg_due_ms;
    int64_t xsim_due_ms;       /* +XSIM: n to push (SIM re-detection), 0 = none */
    int temp_period_ms;        /* +xdrv=5,16,<s>: periodic +XDRVI: 5,17 temperature reports */
    int temp_ch;
    int64_t temp_due_ms;
    bool xsim_pushed;          /* the host has been told about the current SIM */
    int next_call_id;
    IosBbCall calls[IOS_BB_MAX_CALLS];
    int ceer_cause;               /* what +CEER reports after a release (16 = normal) */

    /* SMS. */
    int sms_mr;                /* last reference given out by +CMGS */
    char last_mo_num[32];      /* destination of the last guest send */
    char last_mo_text[512];    /* decoded text of it */
    unsigned mo_count;         /* guest sends so far (mo-sms-count: a new one even with the same text) */
    IosBbSms store[IOS_BB_SMS_STORE];
    unsigned store_next;

    /* Most recent outgoing call, for the device's read-only property. */
    char last_dialed[32];

    /* Packet data: raw IPv4 on a DLCI after +CGDATA="M-RAW_IP". NULL = no network. */
    IosBbOutFn data_out;
    void *data_opaque;
    bool pdp_active;
    uint8_t ip_rx[2048];       /* guest -> network packet being reassembled */
    unsigned ip_rxlen;
} IosBbCore;

void ios_bb_init(IosBbCore *bb, IosBbOutFn out, void *opaque);
/* Baseband power cycle (reset GPIO): back to raw AT, no mux, no calls. Keeps the controls. */
void ios_bb_reset(IosBbCore *bb);
void ios_bb_input(IosBbCore *bb, const uint8_t *buf, size_t len);
/* Advance time: H5 retransmit, ring repeat, scripted call progress. */
void ios_bb_tick(IosBbCore *bb, int64_t now_ms);
/* Earliest due_ms the tick needs to run at again, 0 if none. */
int64_t ios_bb_next_due(const IosBbCore *bb);
/* A control (signal, registration, operator, SIM) changed: tell the host what it would see. */
void ios_bb_changed(IosBbCore *bb);
/* The network's name or PLMN changed: a searching/registered cycle makes the host re-read it. */
void ios_bb_operator_changed(IosBbCore *bb);

int ios_bb_rssi(const IosBbCore *bb);
/* Network-side call events. Return false if refused (no service, line busy). */
bool ios_bb_incoming_call(IosBbCore *bb, const char *number);
void ios_bb_remote_hangup(IosBbCore *bb);
void ios_bb_remote_answer(IosBbCore *bb);
/* "idle", "dialing", "alerting", "incoming", "active", "held" of the first live call. */
const char *ios_bb_call_state(const IosBbCore *bb);

/* A sender incoming-sms takes: 1-20 digits, optionally after a "+" (always sent as international). */
bool ios_bb_sms_sender_ok(const char *number);
/* mcc-mnc: 5 or 6 digits (MCC + 2- or 3-digit MNC). carrier: 1-32 printable bytes, no '"'. */
bool ios_bb_plmn_ok(const char *plmn);
bool ios_bb_carrier_ok(const char *name);

/* Network-side SMS: deliver a 23.040 SMS-DELIVER as +CMT on DLCI 3. */
bool ios_bb_incoming_sms(IosBbCore *bb, const char *number, const char *text);
const char *ios_bb_last_mo_sms_number(const IosBbCore *bb);
const char *ios_bb_last_mo_sms_text(const IosBbCore *bb);

/*
 * Network -> guest IPv4 packet on the data DLCI. The guest's address and DNS are
 * slirp's defaults (IOS_BB_PDP_IP / IOS_BB_PDP_DNS). False if no data DLCI is up.
 */
#define IOS_BB_PDP_IP  "10.0.2.15"
#define IOS_BB_PDP_DNS "10.0.2.3"
bool ios_bb_data_input(IosBbCore *bb, const uint8_t *pkt, size_t len);

/* H5 packet CRC as the Apple kext computes it (golden vector "123456789" -> 0xf689). */
uint16_t ios_bb_h5_crc(const uint8_t *p, size_t n);

/*
 * Infineon SPI framing (3GS and iPhone 4: BasebandSPI's IFX protocol, see
 * docs/baseband/commcenter-4.2.1-3gs.md). Every SPI transfer is full duplex:
 * each side sends a 4-byte header then its payload. Header byte 0 + low nibble
 * of byte 1 = this frame's payload length, byte 1 bit 4 = more to follow.
 * v1: byte 3 bit 6 = CTS. v2: bytes 2-3 (12 bits) grant the peer that many more
 * transmit credits, byte 1 bit 5 = rx error, byte 1 bit 6 = "I need credits"
 * (the N90 kernel sets it until granted). The modem side here sits behind the
 * core's out callback: what the core says queues up, SRDY asks the AP to clock it.
 */
#define IOS_BB_IFX_HDR 4

typedef struct IosBbIfx {
    int version;               /* 1 (3GS) or 2 (iPhone 4) */
    unsigned max_data;         /* DT max-data-size: payload bytes per frame */
    int credits_out;           /* v2: credits the AP still holds (our model of it) */
    int credits_in;            /* v2: credits the AP granted us: data frames we may send */
    uint8_t txq[16384];        /* modem -> AP bytes not yet clocked out */
    unsigned txq_len;
} IosBbIfx;

void ios_bb_ifx_modem_reset(IosBbIfx *x);
void ios_bb_ifx_init(IosBbIfx *x, int version, unsigned max_data);
/* The core's out callback (opaque = the IosBbIfx). */
void ios_bb_ifx_queue(void *opaque, const uint8_t *buf, size_t len);
/* Data waiting: the modem wants SRDY asserted. */
bool ios_bb_ifx_pending(const IosBbIfx *x);
/*
 * One transfer of n bytes: fills miso, and points *rx and *rxlen at the AP's payload
 * inside mosi (0 bytes on an empty or malformed frame) for the caller to hand to
 * ios_bb_input. Either side may be NULL: a controller that produces MISO before the
 * AP's MOSI has arrived calls it twice (the modem's reply never depends on it).
 */
void ios_bb_ifx_xfer(IosBbIfx *x, const uint8_t *mosi, uint8_t *miso, size_t n,
                     const uint8_t **rx, size_t *rxlen);
/* A MISO frame the AP set up but never clocked out: its payload goes back to the head of the queue. */
void ios_bb_ifx_unsent(IosBbIfx *x, const uint8_t *miso);

/* The radio nvram image iBoot reads (+xdrv=9,1,<block>): 0x600 bytes into nv. */
void ios_bb_radio_nvram(uint8_t *nv);

#endif

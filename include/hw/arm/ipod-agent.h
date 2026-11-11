#ifndef HW_ARM_IPOD_AGENT_H
#define HW_ARM_IPOD_AGENT_H

/* Local host/guest RPC. Tokens identify a daemon session, not a security boundary. */
#define IT_AGENT_REQUEST_MAX (256 * 1024)
#define IT_AGENT_RESPONSE_MAX (1024 * 1024)
#define IT_AGENT_CHUNK_MAX 1024
#define IT_AGENT_QUEUE_MAX 16
#define IT_AGENT_LEASE_MS 10000
/* A request is routed to an app's it_typein only where one has polled this recently (each polls 20 times a second). */
#define IT_AGENT_UI_ALIVE_MS 2000
/* The agent's state report: one guest-memory window, sent with every poll. */
#define IT_AGENT_REPORT_MAX IT_AGENT_CHUNK_MAX

typedef struct IPodAgent IPodAgent;
typedef int (*IPodAgentCopy)(void *, uint32_t, uint8_t *, size_t, bool);
/* acquire returns a reference released with free, or NULL after teardown. */
void ipod_agent_publish(IPodAgent *a);
IPodAgent *ipod_agent_acquire(void);
IPodAgent *ipod_agent_new(void);
void ipod_agent_free(IPodAgent *a);
void ipod_agent_reset(IPodAgent *a);
/* QEMU's RTC (-rtc base=, clock=), in Unix seconds: the host's UTC by default. */
int64_t ipod_agent_rtc_now(void);
/* The wall clock the agent keeps the guest on (op 0x165): QEMU's RTC plus offset seconds (a pinned machine clock). */
void ipod_agent_set_clock_offset(IPodAgent *a, int64_t offset);
/* Request is an ASCII id/op header, newline, and base64 body. 0, or -EINVAL
 * (malformed), -EFBIG (over IT_AGENT_REQUEST_MAX: v3 agents take large files
 * as `putpart` chunks), -EBUSY (duplicate id or full queue). */
int ipod_agent_submit(IPodAgent *a, const char *request);
const char *ipod_agent_submit_error(int error);
bool ipod_agent_cancel(IPodAgent *a, const char *id);
/* Owned result string; empty when none. */
char *ipod_agent_take_result(IPodAgent *a);
const char *ipod_agent_status(IPodAgent *a, int64_t now_ms);
/* The agent's state report (contrib/it-agent/README.md, "State report") when it changed since *serial (0 at first),
 * which this updates: a malloc'd string, empty once the session that sent it ended (a reset, a new or cancelled
 * session, a lease's silence). NULL when nothing changed. The guest wrote it. */
char *ipod_agent_report(IPodAgent *a, int64_t now_ms, uint64_t *serial);
int64_t ipod_agent_call(IPodAgent *a, unsigned op, uint64_t token,
                       uint32_t address, uint32_t offset, uint32_t length,
                       int64_t now_ms, uint64_t candidate_token,
                       IPodAgentCopy copy, void *opaque);
#endif

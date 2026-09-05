#ifndef HW_ARM_IPOD_AGENT_H
#define HW_ARM_IPOD_AGENT_H

/* Local host/guest RPC. Tokens identify a daemon session, not a security boundary. */
#define IT_AGENT_REQUEST_MAX (256 * 1024)
#define IT_AGENT_RESPONSE_MAX (1024 * 1024)
#define IT_AGENT_CHUNK_MAX 1024
#define IT_AGENT_QUEUE_MAX 16
#define IT_AGENT_LEASE_MS 10000

typedef struct IPodAgent IPodAgent;
typedef int (*IPodAgentCopy)(void *, uint32_t, uint8_t *, size_t, bool);
IPodAgent *ipod_agent_new(void);
void ipod_agent_free(IPodAgent *a);
void ipod_agent_reset(IPodAgent *a);
/* Request is an ASCII id/op header, newline, and base64 body. */
bool ipod_agent_submit(IPodAgent *a, const char *request);
/* Owned result string; empty when none. */
char *ipod_agent_take_result(IPodAgent *a);
const char *ipod_agent_status(IPodAgent *a, int64_t now_ms);
int64_t ipod_agent_call(IPodAgent *a, unsigned op, uint64_t token,
                       uint32_t address, uint32_t offset, uint32_t length,
                       int64_t now_ms, uint64_t candidate_token,
                       IPodAgentCopy copy, void *opaque);
#endif

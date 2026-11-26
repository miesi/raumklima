// KNXnet/IP routing and DPT 9 - plain C without ESP-IDF, so that a host test can check it
// (components/knx/test). Reference for all byte layouts: calimero-core 2.6 (VERIFIED against
// its output, see test/test_knx_proto.c).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KNX_MCAST "224.0.23.12"
#define KNX_PORT 3671

#define KNX_SVC_SEARCH_REQ 0x0201
#define KNX_SVC_SEARCH_RES 0x0202
#define KNX_SVC_ROUTING_IND 0x0530
#define KNX_SVC_ROUTING_LOST 0x0531
#define KNX_SVC_ROUTING_BUSY 0x0532

#define KNX_APCI_READ 0x000
#define KNX_APCI_RESPONSE 0x040
#define KNX_APCI_WRITE 0x080

#define DPT9_MAX 670433.28    // 0x7fff is "invalid data"
#define DPT9_MIN -671088.64

// DPT 9: 2 byte float. False if the value is out of range.
bool dpt9_encode(double v, uint8_t out[2]);
// False for 0x7fff (invalid data).
bool dpt9_decode(const uint8_t in[2], double *v);

// "main/middle/sub" (3 levels, 0-31/0-7/0-255) and "area.line.device" (0-15.0-15.0-255).
bool knx_ga_parse(const char *s, uint16_t *ga);
void knx_ga_format(uint16_t ga, char *buf, size_t len);
bool knx_ia_parse(const char *s, uint16_t *ia);
void knx_ia_format(uint16_t ia, char *buf, size_t len);

// ROUTING_INDICATION with a cEMI L_Data.ind to a group address, priority low. data/dlen:
// the value after the APCI (2 bytes for DPT 9, 0 for a read). Returns the length, 0 if buf
// is too small.
size_t knx_build_group(uint8_t *buf, size_t len, uint16_t src, uint16_t ga, uint16_t apci, const uint8_t *data,
                       size_t dlen);

typedef struct {
    uint16_t service;      // KNX_SVC_*
    // routing indication with an L_Data.ind
    bool ldata;
    bool group;            // destination is a group address
    uint16_t src, dst;
    uint16_t apci;         // KNX_APCI_*
    uint8_t data[14];
    uint8_t dlen;          // bytes after the APCI; 0 for a value in the APCI byte (1 bit types)
    // routing busy
    uint16_t busy_wait_ms;
    // search response (DIB DEVICE_INFO)
    uint16_t dev_ia;
    uint8_t dev_mcast[4];
    char dev_name[31];
} knx_msg_t;

// SEARCH_REQUEST; the answer goes to ip (network byte order) and port. IP side only: a
// router answers itself and puts nothing on the TP line.
size_t knx_build_search(uint8_t *buf, size_t len, const uint8_t ip[4], uint16_t port);

// False if the packet is not KNXnet/IP.
bool knx_parse(const uint8_t *pkt, size_t len, knx_msg_t *m);

#ifdef __cplusplus
}
#endif

#include "knx_proto.h"

#include <stdio.h>
#include <string.h>

bool dpt9_encode(double v, uint8_t out[2])
{
    if (!(v >= DPT9_MIN && v <= DPT9_MAX)) {   // also catches NaN
        return false;
    }
    // as calimero 2.6: divide first, round at the end (gives the same bytes, see the host test)
    double x = v * 100.0;
    int e = 0;
    while (x < -2048.0 || x > 2047.0) {
        x /= 2.0;
        e++;
    }
    int m = x >= 0 ? (int)(x + 0.5) : -(int)(-x + 0.5);
    if (m > 2047) {   // 2047.5 .. 2047.99 rounds up out of the mantissa
        m /= 2;
        e++;
    }
    if (e > 15) {
        return false;
    }
    uint16_t raw = (uint16_t)(((m & 0x800) << 4) | (e << 11) | (m & 0x7FF));
    out[0] = (uint8_t)(raw >> 8);
    out[1] = (uint8_t)raw;
    return true;
}

bool dpt9_decode(const uint8_t in[2], double *v)
{
    uint16_t raw = (uint16_t)((in[0] << 8) | in[1]);
    if (raw == 0x7FFF) {
        return false;
    }
    int m = raw & 0x7FF;
    if (raw & 0x8000) {
        m -= 2048;
    }
    int e = (raw >> 11) & 0xF;
    *v = 0.01 * m * (double)(1 << e);
    return true;
}

bool knx_ga_parse(const char *s, uint16_t *ga)
{
    unsigned a, b, c;
    char end;
    if (sscanf(s, "%u/%u/%u%c", &a, &b, &c, &end) != 3 || a > 31 || b > 7 || c > 255) {
        return false;
    }
    *ga = (uint16_t)((a << 11) | (b << 8) | c);
    return true;
}

void knx_ga_format(uint16_t ga, char *buf, size_t len)
{
    snprintf(buf, len, "%u/%u/%u", ga >> 11, (ga >> 8) & 7, ga & 0xFF);
}

bool knx_ia_parse(const char *s, uint16_t *ia)
{
    unsigned a, b, c;
    char end;
    if (sscanf(s, "%u.%u.%u%c", &a, &b, &c, &end) != 3 || a > 15 || b > 15 || c > 255) {
        return false;
    }
    *ia = (uint16_t)((a << 12) | (b << 8) | c);
    return true;
}

void knx_ia_format(uint16_t ia, char *buf, size_t len)
{
    snprintf(buf, len, "%u.%u.%u", ia >> 12, (ia >> 8) & 0xF, ia & 0xFF);
}

size_t knx_build_group(uint8_t *buf, size_t len, uint16_t src, uint16_t ga, uint16_t apci, const uint8_t *data,
                       size_t dlen)
{
    size_t n = 6 + 9 + 2 + dlen;   // header, cEMI up to the length octet, TPCI/APCI, data
    if (len < n || dlen > 14) {
        return 0;
    }
    uint8_t *p = buf;
    *p++ = 0x06;   // header length
    *p++ = 0x10;   // KNXnet/IP version 1.0
    *p++ = KNX_SVC_ROUTING_IND >> 8;
    *p++ = KNX_SVC_ROUTING_IND & 0xFF;
    *p++ = (uint8_t)(n >> 8);
    *p++ = (uint8_t)n;
    *p++ = 0x29;   // cEMI L_Data.ind
    *p++ = 0x00;   // no additional info
    *p++ = 0x9C;   // standard frame, repeat, broadcast, priority low (as calimero)
    *p++ = 0xE0;   // destination is a group address, hop count 6
    *p++ = (uint8_t)(src >> 8);
    *p++ = (uint8_t)src;
    *p++ = (uint8_t)(ga >> 8);
    *p++ = (uint8_t)ga;
    *p++ = (uint8_t)(1 + dlen);           // octets after the TPCI byte
    *p++ = (uint8_t)((apci >> 8) & 0x03); // TPCI: unnumbered data, APCI bits 9-8
    *p++ = (uint8_t)(apci & 0xC0);        // APCI bits 7-6
    if (dlen) {
        memcpy(p, data, dlen);
    }
    return n;
}

size_t knx_build_search(uint8_t *buf, size_t len, const uint8_t ip[4], uint16_t port)
{
    if (len < 14) {
        return 0;
    }
    const uint8_t b[14] = {0x06, 0x10, KNX_SVC_SEARCH_REQ >> 8, KNX_SVC_SEARCH_REQ & 0xFF, 0x00, 14,
                           0x08, 0x01, ip[0], ip[1], ip[2], ip[3], (uint8_t)(port >> 8), (uint8_t)port};
    memcpy(buf, b, sizeof(b));   // HPAI: length 8, IPV4_UDP
    return sizeof(b);
}

bool knx_parse(const uint8_t *pkt, size_t len, knx_msg_t *m)
{
    memset(m, 0, sizeof(*m));
    if (len < 6 || pkt[0] != 0x06 || pkt[1] != 0x10) {
        return false;
    }
    m->service = (uint16_t)((pkt[2] << 8) | pkt[3]);
    size_t total = (size_t)((pkt[4] << 8) | pkt[5]);
    if (total > len) {
        return false;
    }
    const uint8_t *b = pkt + 6;
    size_t bl = total - 6;
    if (m->service == KNX_SVC_ROUTING_BUSY) {
        // DIB: length, device state, wait time (ms), control field
        if (bl >= 4) {
            m->busy_wait_ms = (uint16_t)((b[2] << 8) | b[3]);
        }
        return true;
    }
    if (m->service == KNX_SVC_SEARCH_RES) {
        // HPAI (8), DIB DEVICE_INFO (54): IA at 4, multicast at 14, MAC at 18, name at 24
        // (VERIFIED with calimero 2.6 DeviceDIB)
        const uint8_t *d = b + 8;
        if (bl >= 8 + 54 && d[1] == 0x01) {
            m->dev_ia = (uint16_t)((d[4] << 8) | d[5]);
            memcpy(m->dev_mcast, d + 14, 4);
            memcpy(m->dev_name, d + 24, 30);
            m->dev_name[30] = '\0';
        }
        return true;
    }
    if (m->service != KNX_SVC_ROUTING_IND || bl < 2 || b[0] != 0x29) {
        return true;   // KNXnet/IP, but nothing we decode
    }
    size_t o = 2 + b[1];   // message code, additional info length, additional info
    if (bl < o + 7) {
        return true;
    }
    const uint8_t *c = b + o;
    m->group = (c[1] & 0x80) != 0;
    m->src = (uint16_t)((c[2] << 8) | c[3]);
    m->dst = (uint16_t)((c[4] << 8) | c[5]);
    size_t l = c[6];   // octets after the TPCI byte
    if (l < 1 || bl < o + 7 + 1 + l) {
        return true;
    }
    const uint8_t *t = c + 7;
    m->apci = (uint16_t)(((t[0] & 0x03) << 8) | (t[1] & 0xC0));
    size_t dlen = l - 1;
    if (dlen > sizeof(m->data)) {
        return true;
    }
    memcpy(m->data, t + 2, dlen);
    m->dlen = (uint8_t)dlen;
    m->ldata = true;
    return true;
}

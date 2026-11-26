// Host test for knx_proto.c. The expected bytes come from calimero-core 2.6
// (DPTXlator2ByteFloat "9.002", CEMILData + RoutingIndication + PacketHelper.toPacket),
// generated on 2026-11-26. Run: hub/tools/test_host.sh
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "knx_proto.h"

static int fails;

static void hex(const uint8_t *b, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) {
        sprintf(out + 2 * i, "%02x", b[i]);
    }
    out[2 * n] = '\0';
}

static void check_dpt9(double v, const char *want, double back)
{
    uint8_t d[2];
    char h[8];
    if (!dpt9_encode(v, d)) {
        printf("FAIL encode %g: out of range\n", v);
        fails++;
        return;
    }
    hex(d, 2, h);
    double got;
    dpt9_decode(d, &got);
    if (strcmp(h, want) != 0 || fabs(got - back) > 1e-6) {
        printf("FAIL %g: %s (want %s), back %g (want %g)\n", v, h, want, got, back);
        fails++;
    }
}

static void check_frame(const char *what, uint16_t src, uint16_t ga, uint16_t apci, const uint8_t *d, size_t dl,
                        const char *want)
{
    uint8_t buf[32];
    char h[80];
    size_t n = knx_build_group(buf, sizeof(buf), src, ga, apci, d, dl);
    hex(buf, n, h);
    if (strcmp(h, want) != 0) {
        printf("FAIL frame %s: %s (want %s)\n", what, h, want);
        fails++;
    }
    knx_msg_t m;
    if (!knx_parse(buf, n, &m) || !m.ldata || !m.group || m.src != src || m.dst != ga || m.apci != apci ||
        m.dlen != dl || memcmp(m.data, d, dl) != 0) {
        printf("FAIL parse %s\n", what);
        fails++;
    }
}

int main(void)
{
    // value, calimero bytes, calimero value back
    check_dpt9(0, "0000", 0);
    check_dpt9(0.01, "0001", 0.01);
    check_dpt9(-0.01, "87ff", -0.01);
    check_dpt9(21.5, "0c33", 21.5);
    check_dpt9(-21.5, "8bcd", -21.5);
    check_dpt9(20.48, "0c00", 20.48);
    check_dpt9(20.47, "07ff", 20.47);
    check_dpt9(40.96, "1400", 40.96);
    check_dpt9(100, "1ce2", 100);
    check_dpt9(1013.25, "362f", 1013.12);
    check_dpt9(101325, "6cd5", 101335.04);
    check_dpt9(450, "2d7e", 449.92);
    check_dpt9(1234.5, "3789", 1234.56);
    check_dpt9(-30, "8a24", -30);
    check_dpt9(99.99, "1ce2", 100);
    check_dpt9(0.005, "0001", 0.01);
    check_dpt9(-671088.64, "f800", -671088.64);
    check_dpt9(-20.48, "8000", -20.48);
    check_dpt9(5000, "47a1", 4999.68);
    check_dpt9(99999, "6cc5", 100024.32);
    uint8_t d[2];
    if (dpt9_encode(670760.96, d) || dpt9_encode(NAN, d)) {   // calimero: out of range
        printf("FAIL range\n");
        fails++;
    }
    const uint8_t inv[2] = {0x7f, 0xff};
    double v;
    if (dpt9_decode(inv, &v)) {
        printf("FAIL 0x7fff must be invalid\n");
        fails++;
    }

    uint16_t a, g;
    knx_ia_parse("15.15.250", &a);
    knx_ga_parse("31/7/250", &g);
    const uint8_t t215[2] = {0x0c, 0x33};
    check_frame("write", a, g, KNX_APCI_WRITE, t215, 2, "06100530001329009ce0fffafffa0300800c33");
    knx_ga_parse("1/2/3", &g);
    check_frame("read", a, g, KNX_APCI_READ, NULL, 0, "06100530001129009ce0fffa0a03010000");
    knx_ia_parse("1.1.250", &a);
    check_frame("response", a, g, KNX_APCI_RESPONSE, t215, 2, "06100530001329009ce011fa0a030300400c33");

    char s[16];
    knx_ga_format(0xFFFA, s, sizeof(s));
    if (strcmp(s, "31/7/250") != 0 || knx_ga_parse("32/0/0", &g) || knx_ga_parse("1/8/0", &g) ||
        knx_ga_parse("1/2/3x", &g) || knx_ia_parse("16.0.1", &a)) {
        printf("FAIL address parse/format\n");
        fails++;
    }
    const uint8_t busy[] = {0x06, 0x10, 0x05, 0x32, 0x00, 0x0c, 0x06, 0x00, 0x00, 0x64, 0x00, 0x00};
    knx_msg_t m;
    if (!knx_parse(busy, sizeof(busy), &m) || m.service != KNX_SVC_ROUTING_BUSY || m.busy_wait_ms != 100) {
        printf("FAIL routing busy\n");
        fails++;
    }
    // search response: header, HPAI, DIB as built for the calimero DeviceDIB check
    uint8_t sr[6 + 8 + 54] = {0x06, 0x10, 0x02, 0x02, 0x00, sizeof(sr), 0x08, 0x01, 192, 168, 178, 2, 0x0e, 0x57};
    uint8_t *dib = sr + 14;
    dib[0] = 54;
    dib[1] = 1;
    dib[2] = 0x20;
    dib[4] = 0x11;
    dib[5] = 0x01;
    dib[14] = 224;
    dib[16] = 23;
    dib[17] = 12;
    dib[18] = 0xaa;
    memcpy(dib + 24, "TestRouter", 10);
    if (!knx_parse(sr, sizeof(sr), &m) || m.service != KNX_SVC_SEARCH_RES || m.dev_ia != 0x1101 ||
        m.dev_mcast[0] != 224 || m.dev_mcast[3] != 12 || strcmp(m.dev_name, "TestRouter") != 0) {
        printf("FAIL search response\n");
        fails++;
    }
    const uint8_t ip[4] = {192, 168, 178, 41};
    uint8_t q[16];
    char qh[40];
    hex(q, knx_build_search(q, sizeof(q), ip, 3671), qh);
    if (strcmp(qh, "06100201000e0801c0a8b2290e57") != 0) {
        printf("FAIL search request %s\n", qh);
        fails++;
    }
    printf("%s (%d failures)\n", fails ? "FAILED" : "all tests passed", fails);
    return fails ? 1 : 0;
}

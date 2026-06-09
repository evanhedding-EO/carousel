/* carousel - CL57EC EtherCAT test tool (milestones 0-1b: SDO only, no motion).
 *
 *   carousel <ifname> scan                       enumerate the bus
 *   carousel <ifname> params                      read key config objects (read-only)
 *   carousel <ifname> sdo get <idx> <sub> <type>  read one object
 *   carousel <ifname> sdo set <idx> <sub> <type> <val>   write one object
 *   carousel <ifname> inputs                      live digital-input bits (60FD)
 *
 * <idx> is hex (e.g. 2401), <sub> is decimal, <val> is decimal or 0x-hex.
 * <type> is one of: u8 u16 u32 i8 i16 i32
 *
 * Object widths come from the ESI dictionary, e.g. 2206h/2400h/2401h/2407h = u16,
 * 6060h/6098h = i8, 60FD/60FF/607C/6081 = u32/i32.  Slave is assumed to be #1.
 */
#include "bus.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <stdint.h>

#define SLAVE 1

static volatile sig_atomic_t g_run = 1;
static void on_sigint(int sig) { (void)sig; g_run = 0; }

/* ---- value-type helpers ------------------------------------------------- */
static int type_size(const char *t)
{
    if (!strcmp(t, "u8")  || !strcmp(t, "i8"))  return 1;
    if (!strcmp(t, "u16") || !strcmp(t, "i16")) return 2;
    if (!strcmp(t, "u32") || !strcmp(t, "i32")) return 4;
    return 0;
}
static int type_signed(const char *t) { return t[0] == 'i'; }

/* little-endian buffer -> integer */
static int64_t le_to_int(const uint8_t *b, int sz, int is_signed)
{
    uint64_t u = 0;
    for (int i = sz - 1; i >= 0; i--) u = (u << 8) | b[i];
    if (is_signed && (b[sz - 1] & 0x80)) {           /* sign-extend */
        uint64_t mask = ~0ULL << (sz * 8);
        u |= mask;
    }
    return (int64_t)u;
}
static void int_to_le(int64_t v, uint8_t *b, int sz)
{
    for (int i = 0; i < sz; i++) b[i] = (uint8_t)(v >> (8 * i));
}

/* ---- subcommands -------------------------------------------------------- */
static int cmd_scan(void)
{
    bus_print_slaves();
    return 0;
}

static int cmd_sdo_get(uint16_t idx, uint8_t sub, const char *type)
{
    int sz = type_size(type);
    if (!sz) { fprintf(stderr, "bad type '%s'\n", type); return 2; }
    uint8_t buf[8] = {0};
    if (!sdo_read(SLAVE, idx, sub, buf, sz)) {
        fprintf(stderr, "SDO read 0x%04X:%u failed\n", idx, sub);
        return 1;
    }
    int64_t v = le_to_int(buf, sz, type_signed(type));
    printf("0x%04X:%u = %lld (0x%llX)\n", idx, sub, (long long)v, (unsigned long long)v);
    return 0;
}

static int cmd_sdo_set(uint16_t idx, uint8_t sub, const char *type, const char *valstr)
{
    int sz = type_size(type);
    if (!sz) { fprintf(stderr, "bad type '%s'\n", type); return 2; }
    int64_t v = strtoll(valstr, NULL, 0);
    uint8_t buf[8] = {0};
    int_to_le(v, buf, sz);
    if (!sdo_write(SLAVE, idx, sub, buf, sz)) {
        fprintf(stderr, "SDO write 0x%04X:%u failed\n", idx, sub);
        return 1;
    }
    printf("wrote 0x%04X:%u = %lld; reading back...\n", idx, sub, (long long)v);
    return cmd_sdo_get(idx, sub, type);              /* confirm */
}

static int cmd_params(void)
{
    struct { uint16_t idx; uint8_t sub; int sz; int sgn; const char *name; } items[] = {
        { 0x2206, 0, 2, 0, "DriverMode (1=open, 2-4=closed)" },
        { 0x2205, 0, 2, 0, "DefaultDir (0=fwd, 1=rev)" },
        { 0x2400, 0, 2, 0, "Microstep / div" },
        { 0x2401, 0, 2, 0, "Max current (mA)" },
        { 0x2407, 0, 2, 0, "Encoder resolution" },
        { 0x6041, 0, 2, 0, "Status word" },
        { 0x6061, 0, 1, 1, "Mode of operation display" },
    };
    int n = (int)(sizeof items / sizeof items[0]);
    for (int i = 0; i < n; i++) {
        uint8_t buf[8] = {0};
        if (sdo_read(SLAVE, items[i].idx, items[i].sub, buf, items[i].sz)) {
            int64_t v = le_to_int(buf, items[i].sz, items[i].sgn);
            printf("  0x%04X  %-32s = %lld (0x%llX)\n",
                   items[i].idx, items[i].name, (long long)v, (unsigned long long)v);
        } else {
            printf("  0x%04X  %-32s = <read failed>\n", items[i].idx, items[i].name);
        }
    }
    return 0;
}

static int cmd_inputs(void)
{
    signal(SIGINT, on_sigint);
    printf("Live digital inputs (60FD). Hand-spin the carousel; HOME should toggle. Ctrl-C to stop.\n");
    while (g_run) {
        uint32_t din = 0;
        if (sdo_read(SLAVE, 0x60FD, 0, &din, 4)) {
            printf("\r  60FD=0x%08X  -LIM=%u  +LIM=%u  HOME=%u  PROBE1=%u  PROBE2=%u   ",
                   din, din & 1u, (din >> 1) & 1u, (din >> 2) & 1u,
                   (din >> 16) & 1u, (din >> 17) & 1u);
        } else {
            printf("\r  <SDO read failed>                                            ");
        }
        fflush(stdout);
        usleep(50000);                               /* ~20 Hz */
    }
    printf("\n");
    return 0;
}

/* ---- dispatch ----------------------------------------------------------- */
static void usage(const char *p)
{
    fprintf(stderr,
        "usage:\n"
        "  %s <ifname> scan\n"
        "  %s <ifname> params\n"
        "  %s <ifname> sdo get <idxHex> <sub> <type>\n"
        "  %s <ifname> sdo set <idxHex> <sub> <type> <val>\n"
        "  %s <ifname> inputs\n"
        "  type = u8|u16|u32|i8|i16|i32\n", p, p, p, p, p);
}

int main(int argc, char **argv)
{
    if (argc < 3) { usage(argv[0]); return 2; }
    const char *ifname = argv[1];
    const char *cmd    = argv[2];

    int n = bus_open(ifname);
    if (n <= 0) return 1;

    int rc = 0;
    if (!strcmp(cmd, "scan")) {
        rc = cmd_scan();
    } else if (!strcmp(cmd, "params")) {
        rc = cmd_params();
    } else if (!strcmp(cmd, "inputs")) {
        rc = cmd_inputs();
    } else if (!strcmp(cmd, "sdo") && argc >= 4) {
        const char *op = argv[3];
        if (!strcmp(op, "get") && argc == 7) {
            rc = cmd_sdo_get((uint16_t)strtol(argv[4], NULL, 16),
                             (uint8_t)strtol(argv[5], NULL, 0), argv[6]);
        } else if (!strcmp(op, "set") && argc == 8) {
            rc = cmd_sdo_set((uint16_t)strtol(argv[4], NULL, 16),
                             (uint8_t)strtol(argv[5], NULL, 0), argv[6], argv[7]);
        } else {
            usage(argv[0]); rc = 2;
        }
    } else {
        usage(argv[0]); rc = 2;
    }

    bus_close();
    return rc;
}

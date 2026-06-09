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

/* ---- milestone 2: enable at zero velocity (no motion) ------------------- */
static const char *cia402_state(uint16_t sw)
{
    switch (sw & 0x6F) {
    case 0x00: case 0x20: return "NotReadyToSwitchOn";
    case 0x40: case 0x60: return "SwitchOnDisabled";
    case 0x21:            return "ReadyToSwitchOn";
    case 0x23:            return "SwitchedOn";
    case 0x27:            return "OperationEnabled";
    case 0x07:            return "QuickStopActive";
    case 0x0F: case 0x2F: return "FaultReactionActive";
    case 0x08: case 0x28: return "Fault";
    default:              return "?";
    }
}

static int cmd_enable(void)
{
    signal(SIGINT, on_sigint);

    int8_t mode = 3;                                 /* PV mode; target velocity stays 0 */
    sdo_write(SLAVE, 0x6060, 0, &mode, 1);

    /* The drive defaults to DC-SYNC0, which our non-RT loop can't provide, so it
     * drops out of OP after a few seconds. Force SM-synchronous mode (no DC);
     * the ESI lists SM-Synchron with AssignActivate=0, so it's supported. */
    uint16_t sync_type = 1;                          /* 0=free-run, 1=SM-synchronous, 2=DC */
    sdo_write(SLAVE, 0x1C32, 1, &sync_type, 2);      /* outputs SM */
    sdo_write(SLAVE, 0x1C33, 1, &sync_type, 2);      /* inputs SM */

    if (!bus_enter_op()) return 1;
    printf("OP reached. Enabling (PV mode, target velocity = 0 -> no motion)...\n");

    /* CiA402 bring-up: derive next controlword from the current statusword. */
    int enabled = 0;
    for (int i = 0; i < 1000 && g_run && !enabled; i++) {
        uint16_t sw = bus_statusword(), cw;
        if (sw & 0x0008)                cw = 0x0080;            /* Fault -> reset */
        else switch (sw & 0x6F) {
            case 0x40: case 0x60:       cw = 0x0006; break;     /* SwitchOnDisabled -> Shutdown */
            case 0x21:                  cw = 0x0007; break;     /* Ready -> SwitchOn */
            case 0x23:                  cw = 0x000F; break;     /* SwitchedOn -> EnableOp */
            case 0x27:                  cw = 0x000F; enabled = 1; break;  /* OperationEnabled */
            default:                    cw = 0x0006; break;
        }
        bus_set_controlword(cw);
        bus_cycle();
        usleep(2000);
    }

    uint16_t sw = bus_statusword();
    if (enabled) printf("ENABLED: status=0x%04X (%s). Holding (motor energized). Ctrl-C to stop.\n",
                        sw, cia402_state(sw));
    else fprintf(stderr, "did NOT reach OperationEnabled: status=0x%04X (%s)\n",
                 sw, cia402_state(sw));

    int bad = 0;
    while (g_run && enabled) {                        /* hold enabled; watch for a drop */
        bus_set_controlword(0x000F);
        bus_cycle();
        uint16_t hs = bus_statusword();
        if ((hs & 0x6F) == 0x27) {
            bad = 0;
        } else if (++bad >= 10) {                     /* ~20 ms of non-OP -> report and stop */
            fprintf(stderr, "\ndrive left OperationEnabled: status=0x%04X (%s)\n",
                    hs, cia402_state(hs));
            enabled = 0;
        }
        usleep(2000);
    }

    printf("\ndisabling (quick stop -> disable voltage)...\n");
    for (int i = 0; i < 50; i++) { bus_set_controlword(0x0002); bus_cycle(); usleep(2000); }
    for (int i = 0; i < 10; i++) { bus_set_controlword(0x0000); bus_cycle(); usleep(2000); }
    return enabled ? 0 : 1;
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
        "  %s <ifname> enable                 (energize at zero velocity; no motion)\n"
        "  type = u8|u16|u32|i8|i16|i32\n", p, p, p, p, p, p);
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
    } else if (!strcmp(cmd, "enable")) {
        rc = cmd_enable();
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

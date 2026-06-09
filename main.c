/* carousel - CL57EC EtherCAT test tool.
 *
 *   carousel <ifname> scan                       enumerate the bus
 *   carousel <ifname> params                      read key config objects (read-only)
 *   carousel <ifname> sdo get <idx> <sub> <type>  read one object
 *   carousel <ifname> sdo set <idx> <sub> <type> <val>   write one object
 *   carousel <ifname> inputs                      live digital-input bits (60FD)
 *   carousel <ifname> pdomap                       print the drive's real PDO mapping
 *   carousel <ifname> enable                       energize at zero velocity (no motion)
 *   carousel <ifname> spin <counts/s>              PV-mode rotation (10000 = 1 rev/s)
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

/* CiA 402 control word (6040h) commands */
#define CW_SHUTDOWN           0x0006
#define CW_SWITCH_ON          0x0007
#define CW_ENABLE_OP          0x000F
#define CW_QUICK_STOP         0x0002
#define CW_DISABLE_VOLTAGE    0x0000
#define CW_FAULT_RESET        0x0080

/* CiA 402 status word (6041h): the state lives in bits 0-3,5,6 -> mask 0x6F */
#define SW_MASK               0x006F
#define SW_FAULT_BIT          0x0008
#define SW_SWITCH_ON_DISABLED 0x0040
#define SW_READY_TO_SWITCH_ON 0x0021
#define SW_SWITCHED_ON        0x0023
#define SW_OPERATION_ENABLED  0x0027
#define SW_QUICK_STOP_ACTIVE  0x0007

/* Modes of operation (6060h) */
#define MODE_PV               3        /* Profile Velocity */

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

/* ---- read the drive's ACTUAL PDO mapping (the ESI XML did not match) ----- */
static void dump_pdo_assign(uint16_t sm_assign, const char *label)
{
    uint8_t npdo = 0;
    if (!sdo_read(SLAVE, sm_assign, 0, &npdo, 1)) {
        printf("  %s (0x%04X): read failed\n", label, sm_assign);
        return;
    }
    printf("  %s (0x%04X): %u PDO(s)\n", label, sm_assign, npdo);
    int byteoff = 0;
    for (uint8_t p = 1; p <= npdo; p++) {
        uint16_t pdo = 0;
        sdo_read(SLAVE, sm_assign, p, &pdo, 2);
        uint8_t nent = 0;
        sdo_read(SLAVE, pdo, 0, &nent, 1);
        printf("    PDO 0x%04X: %u entries\n", pdo, nent);
        for (uint8_t e = 1; e <= nent; e++) {
            uint32_t m = 0;                          /* packed: index<<16 | sub<<8 | bitlen */
            sdo_read(SLAVE, pdo, e, &m, 4);
            printf("      byte %2d : 0x%04X:%u  %2u bits\n",
                   byteoff, (uint16_t)(m >> 16), (uint8_t)(m >> 8), (uint8_t)m);
            byteoff += (uint8_t)m / 8;
        }
    }
}

static int cmd_pdomap(void)
{
    dump_pdo_assign(0x1C12, "RxPDO outputs (master->drive)");
    dump_pdo_assign(0x1C13, "TxPDO inputs  (drive->master)");
    return 0;
}

/* ---- milestones 2-3: CiA402 enable / motion ----------------------------- */
static const char *cia402_state(uint16_t sw)
{
    switch (sw & SW_MASK) {
    case 0x00: case 0x20:        return "NotReadyToSwitchOn";
    case SW_SWITCH_ON_DISABLED:
    case 0x60:                   return "SwitchOnDisabled";
    case SW_READY_TO_SWITCH_ON:  return "ReadyToSwitchOn";
    case SW_SWITCHED_ON:         return "SwitchedOn";
    case SW_OPERATION_ENABLED:   return "OperationEnabled";
    case SW_QUICK_STOP_ACTIVE:   return "QuickStopActive";
    case 0x0F: case 0x2F:        return "FaultReactionActive";
    case SW_FAULT_BIT: case 0x28:return "Fault";
    default:                     return "?";
    }
}

/* Walk the state machine to OperationEnabled, holding `mode` in the cyclic PDO.
 * Returns 1 if enabled, 0 on timeout/abort. */
static int cia402_enable(int8_t mode)
{
    int enabled = 0;
    for (int i = 0; i < 1000 && g_run && !enabled; i++) {
        uint16_t sw = bus_statusword(), cw;
        if (sw & SW_FAULT_BIT)              cw = CW_FAULT_RESET;
        else switch (sw & SW_MASK) {
            case SW_READY_TO_SWITCH_ON:     cw = CW_SWITCH_ON; break;
            case SW_SWITCHED_ON:            cw = CW_ENABLE_OP; break;
            case SW_OPERATION_ENABLED:      cw = CW_ENABLE_OP; enabled = 1; break;
            default:                        cw = CW_SHUTDOWN;  break;  /* incl. SwitchOnDisabled */
        }
        bus_set_controlword(cw);
        bus_set_mode(mode);
        bus_cycle();
        usleep(2000);
    }
    return enabled;
}

/* Ramp target velocity to 0, let the drive decelerate, then de-energize. */
static void cia402_disable(void)
{
    printf("\nstopping (ramp to 0) and disabling...\n");
    bus_set_target_velocity(0);
    for (int i = 0; i < 1500; i++) {                 /* up to ~3 s to coast down */
        bus_set_controlword(CW_ENABLE_OP);
        bus_set_target_velocity(0);
        bus_cycle();
        if (labs(bus_velocity_actual()) < 50) break;
        usleep(2000);
    }
    for (int i = 0; i < 10; i++) { bus_set_controlword(CW_DISABLE_VOLTAGE); bus_cycle(); usleep(2000); }
}

static int cmd_enable(void)
{
    signal(SIGINT, on_sigint);
    if (!bus_enter_op()) return 1;
    printf("OP reached. Enabling (PV mode, target velocity = 0 -> no motion)...\n");
    bus_debug();

    bus_set_target_velocity(0);
    int enabled = cia402_enable(MODE_PV);
    uint16_t sw = bus_statusword();
    if (!enabled) {
        fprintf(stderr, "did NOT reach OperationEnabled: status=0x%04X (%s)\n", sw, cia402_state(sw));
        bus_report_state("enable failed");
        return 1;
    }
    printf("ENABLED: status=0x%04X (%s). Holding (motor energized). Ctrl-C to stop.\n",
           sw, cia402_state(sw));

    int bad = 0;
    while (g_run) {                                   /* hold; watch for a drop */
        bus_set_controlword(CW_ENABLE_OP);
        bus_set_mode(MODE_PV);
        bus_cycle();
        uint16_t hs = bus_statusword();
        if ((hs & SW_MASK) == SW_OPERATION_ENABLED) bad = 0;
        else if (++bad >= 10) {
            fprintf(stderr, "\ndrive left OperationEnabled: status=0x%04X (%s)\n", hs, cia402_state(hs));
            break;
        }
        usleep(2000);
    }
    cia402_disable();
    return 0;
}

static int cmd_spin(int32_t vel)
{
    signal(SIGINT, on_sigint);
    if (!bus_enter_op()) return 1;
    printf("OP reached. Enabling for PV spin...\n");

    bus_set_target_velocity(0);                       /* enable at 0, then ramp up */
    if (!cia402_enable(MODE_PV)) {
        uint16_t sw = bus_statusword();
        fprintf(stderr, "enable failed: status=0x%04X (%s)\n", sw, cia402_state(sw));
        bus_report_state("spin enable failed");
        return 1;
    }
    printf("SPINNING at %d counts/s (~%.2f rev/s, 10000 cnt/rev). Ctrl-C to stop.\n",
           vel, vel / 10000.0);

    int tick = 0;
    while (g_run) {
        bus_set_controlword(CW_ENABLE_OP);
        bus_set_mode(MODE_PV);
        bus_set_target_velocity(vel);
        bus_cycle();
        if (++tick % 250 == 0)                        /* ~every 0.5 s */
            printf("\r  target=%d  actual=%d counts/s   ", vel, bus_velocity_actual());
        fflush(stdout);
        usleep(2000);
    }
    cia402_disable();
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
        "  %s <ifname> pdomap                 (print the drive's actual PDO mapping)\n"
        "  %s <ifname> enable                 (energize at zero velocity; no motion)\n"
        "  %s <ifname> spin <counts/s>        (PV mode; 10000 = 1 rev/s)\n"
        "  type = u8|u16|u32|i8|i16|i32\n", p, p, p, p, p, p, p, p);
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
    } else if (!strcmp(cmd, "pdomap")) {
        rc = cmd_pdomap();
    } else if (!strcmp(cmd, "enable")) {
        rc = cmd_enable();
    } else if (!strcmp(cmd, "spin") && argc == 4) {
        rc = cmd_spin((int32_t)strtol(argv[3], NULL, 0));
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

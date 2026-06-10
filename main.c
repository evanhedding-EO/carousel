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
 *   carousel <ifname> move <counts>                PP relative move (10000 = 1 rev)
 *   carousel <ifname> home [method]                homing to the sensor flag
 *   carousel <ifname> session                      interactive PP session (holds between cmds)
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
#include <sys/select.h>

#define SLAVE 1

/* Mechanics: 1:1 direct drive, 2400h microstep = 10000 -> counts per carousel rev. */
#define COUNTS_PER_REV        10000
#define NUM_STATIONS          24       /* vial positions; 10000/24 = 416.67 counts apart */

/* CiA 402 control word (6040h) commands */
#define CW_SHUTDOWN           0x0006
#define CW_SWITCH_ON          0x0007
#define CW_ENABLE_OP          0x000F
#define CW_QUICK_STOP         0x0002
#define CW_DISABLE_VOLTAGE    0x0000
#define CW_FAULT_RESET        0x0080

/* control word bits used in Profile Position mode */
#define CW_NEW_SETPOINT       0x0010   /* bit4 */
#define CW_RELATIVE           0x0040   /* bit6: target is relative */

/* CiA 402 status word (6041h): the state lives in bits 0-3,5,6 -> mask 0x6F */
#define SW_MASK               0x006F
#define SW_FAULT_BIT          0x0008
#define SW_SWITCH_ON_DISABLED 0x0040
#define SW_READY_TO_SWITCH_ON 0x0021
#define SW_SWITCHED_ON        0x0023
#define SW_OPERATION_ENABLED  0x0027
#define SW_QUICK_STOP_ACTIVE  0x0007
#define SW_TARGET_REACHED     0x0400   /* bit10 */
#define SW_SETPOINT_ACK       0x1000   /* bit12, Profile Position */
#define SW_HOMING_ATTAINED    0x1000   /* bit12, Homing mode (same bit, mode-specific) */
#define SW_HOMING_ERROR       0x2000   /* bit13, Homing mode */

/* Modes of operation (6060h) */
#define MODE_PP               1        /* Profile Position */
#define MODE_PV               3        /* Profile Velocity */
#define MODE_HOME             6        /* Homing */

/* Homing defaults (tune during bring-up; method also overridable on the CLI) */
#define HOME_METHOD           21       /* home-switch only, no index (mid-travel flag) */
#define HOME_SPEED_FAST       10000    /* 6099:1 search-for-switch, counts/s (1 rev/s) */
#define HOME_SPEED_SLOW       2000     /* 6099:2 latch speed, counts/s (slow = repeatable) */
#define HOME_ACCEL            50000    /* 609Ah */
#define HOME_OFFSET           0        /* 607Ch: counts from latched edge to station 0 */

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

/* Profile-Position move with the proper new-set-point handshake.
 * Returns 1 if target-reached, 0 on timeout/abort. Must already be OperationEnabled. */
static int pp_move(int32_t target, int relative)
{
    uint16_t trig = CW_ENABLE_OP | CW_NEW_SETPOINT | (relative ? CW_RELATIVE : 0);

    bus_set_controlword(CW_ENABLE_OP);                /* ensure new-set-point low */
    bus_set_mode(MODE_PP);
    bus_set_target_position(target);
    bus_cycle();

    int acked = 0;                                    /* rising edge -> wait set-point ack (bit12) */
    for (int i = 0; i < 1000 && g_run; i++) {
        bus_set_controlword(trig);
        bus_set_mode(MODE_PP);
        bus_set_target_position(target);
        bus_cycle();
        if (bus_statusword() & SW_SETPOINT_ACK) { acked = 1; break; }
        usleep(2000);
    }
    bus_set_controlword(CW_ENABLE_OP);                /* drop new-set-point (handshake) */
    bus_set_mode(MODE_PP);
    bus_cycle();
    if (!acked) return 0;

    for (int i = 0; i < 5000 && g_run; i++) {         /* wait target reached (bit10), up to ~10 s */
        bus_set_controlword(CW_ENABLE_OP);
        bus_set_mode(MODE_PP);
        bus_cycle();
        if (bus_statusword() & SW_TARGET_REACHED) return 1;
        usleep(2000);
    }
    return 0;
}

static int cmd_move(int32_t delta)
{
    signal(SIGINT, on_sigint);

    /* Profile dynamics are left to the drive so they can be tuned live with
     * `sdo set` (6081 velocity, 6083/6084 accel/decel). Report what's in effect. */
    uint32_t pv = 0, acc = 0, dec = 0;
    sdo_read(SLAVE, 0x6081, 0, &pv, 4);
    sdo_read(SLAVE, 0x6083, 0, &acc, 4);
    sdo_read(SLAVE, 0x6084, 0, &dec, 4);
    printf("profile: velocity=%u accel=%u decel=%u (tune via 'sdo set')\n", pv, acc, dec);

    if (!bus_enter_op()) return 1;
    if (!cia402_enable(MODE_PP)) {
        uint16_t sw = bus_statusword();
        fprintf(stderr, "enable failed: status=0x%04X (%s)\n", sw, cia402_state(sw));
        bus_report_state("move enable failed");
        return 1;
    }

    int32_t start = bus_position_actual();
    printf("PP relative move %+d counts (%.2f rev) from %d ...\n", delta, delta / 10000.0, start);
    int reached = pp_move(delta, 1);
    int32_t end = bus_position_actual();
    printf("%s: %d -> %d (moved %+d, wanted %+d)\n",
           reached ? "target reached" : "NOT reached (timeout/abort)", start, end, end - start, delta);

    cia402_disable();
    return 0;
}

/* Run the drive's built-in homing. Assumes already OperationEnabled (any mode);
 * switches the cyclic mode to MODE_HOME and leaves the caller to restore its own
 * mode afterwards. Returns 1 homed, 0 on error/timeout/abort. */
static int do_home(int8_t method)
{
    /* homing parameters (SDO; none of these are in the PDO) */
    uint32_t vfast = HOME_SPEED_FAST, vslow = HOME_SPEED_SLOW, acc = HOME_ACCEL;
    int32_t off = HOME_OFFSET;
    sdo_write(SLAVE, 0x6098, 0, &method, 1);          /* homing method */
    sdo_write(SLAVE, 0x6099, 1, &vfast, 4);           /* speed: search for switch */
    sdo_write(SLAVE, 0x6099, 2, &vslow, 4);           /* speed: search for zero (slow latch) */
    sdo_write(SLAVE, 0x609A, 0, &acc, 4);             /* homing acceleration */
    sdo_write(SLAVE, 0x607C, 0, &off, 4);             /* home offset */

    printf("Homing (method %d). Searching for the sensor flag... Ctrl-C to abort.\n", method);

    for (int i = 0; i < 10; i++) {                    /* bit4 low in MODE_HOME first, so the */
        bus_set_controlword(CW_ENABLE_OP);            /* drive sees a clean rising edge after */
        bus_set_mode(MODE_HOME);                      /* any mode switch settles */
        bus_cycle();
        usleep(2000);
    }

    int done = 0, err = 0, tick = 0;
    for (int i = 0; i < 15000 && g_run && !done && !err; i++) {   /* up to ~30 s */
        bus_set_controlword(CW_ENABLE_OP | CW_NEW_SETPOINT);      /* bit4 = "homing start" */
        bus_set_mode(MODE_HOME);
        bus_cycle();
        uint16_t sw = bus_statusword();
        if (sw & SW_HOMING_ERROR) err = 1;
        else if ((sw & SW_HOMING_ATTAINED) && (sw & SW_TARGET_REACHED)) done = 1;
        if (++tick % 250 == 0) { printf("\r  searching... pos=%d   ", bus_position_actual()); fflush(stdout); }
        usleep(2000);
    }
    bus_set_controlword(CW_ENABLE_OP);                /* drop homing-start */
    bus_set_mode(MODE_HOME);
    bus_cycle();

    if (done)
        printf("\nHOMED. position now %d (= home offset %d).\n", bus_position_actual(), HOME_OFFSET);
    else if (err) {
        fprintf(stderr, "\nHOMING ERROR: status=0x%04X\n", bus_statusword());
        bus_report_state("homing error");
    } else
        fprintf(stderr, "\nhoming did not complete (timeout/abort)\n");

    return done;
}

static int cmd_home(int8_t method)
{
    signal(SIGINT, on_sigint);

    if (!bus_enter_op()) return 1;
    if (!cia402_enable(MODE_HOME)) {
        uint16_t sw = bus_statusword();
        fprintf(stderr, "enable failed: status=0x%04X (%s)\n", sw, cia402_state(sw));
        bus_report_state("home enable failed");
        return 1;
    }

    int done = do_home(method);

    cia402_disable();
    return done ? 0 : 1;
}

/* ---- milestone 4: interactive PP session --------------------------------
 * One process enables ONCE and stays enabled, so the drive servo-holds the
 * carousel between commands (a bare `move` de-energizes on exit and the disc
 * free-spins -- no good with vials on it). The loop keeps the 2 ms PDO cycle
 * alive (OP/DC die without it) and polls stdin non-blocking. */

/* Absolute target for station n via the shortest path. Always recomputed from
 * n (rounding never accumulates: max error 0.5 count) and commanded absolute,
 * so software can't drift. The position register stays continuous (cur+delta,
 * never jumps by whole revs). */
static int32_t station_target(int n)
{
    int s = ((n % NUM_STATIONS) + NUM_STATIONS) % NUM_STATIONS;
    int32_t target_angle = (int32_t)(((int64_t)s * COUNTS_PER_REV + NUM_STATIONS / 2)
                                     / NUM_STATIONS);
    int32_t cur = bus_position_actual();
    int32_t cur_angle = ((cur % COUNTS_PER_REV) + COUNTS_PER_REV) % COUNTS_PER_REV;
    int32_t delta = target_angle - cur_angle;
    if      (delta >  COUNTS_PER_REV / 2) delta -= COUNTS_PER_REV;
    else if (delta < -COUNTS_PER_REV / 2) delta += COUNTS_PER_REV;
    return cur + delta;
}

static void session_help(void)
{
    printf("commands:\n"
           "  goto <counts>      absolute move\n"
           "  move <delta>       relative move\n"
           "  station <n>        shortest-path move to station n (0..%d)\n"
           "  home [method]      run homing (default method %d), then hold\n"
           "  pos                position, angle, nearest station\n"
           "  sdo get <idxHex> <sub> <type>\n"
           "  sdo set <idxHex> <sub> <type> <val>   (live tuning: 2213/6081/6083/6084...)\n"
           "  help               this text\n"
           "  q                  stop holding, de-energize, exit\n",
           NUM_STATIONS - 1, HOME_METHOD);
}

static void session_move(int32_t target, int relative)
{
    int32_t start = bus_position_actual();
    int reached = pp_move(target, relative);
    int32_t end = bus_position_actual();
    printf("%s: %d -> %d (moved %+d)\n",
           reached ? "reached" : "NOT reached (timeout/abort)", start, end, end - start);
}

/* Handle one input line. Returns 1 to quit the session, 0 otherwise. */
static int session_dispatch(char *line)
{
    char *tok[8];
    int nt = 0;
    for (char *t = strtok(line, " \t\r"); t && nt < 8; t = strtok(NULL, " \t\r"))
        tok[nt++] = t;
    if (nt == 0) return 0;

    if (!strcmp(tok[0], "q") || !strcmp(tok[0], "quit")) return 1;

    if (!strcmp(tok[0], "help") || !strcmp(tok[0], "?")) {
        session_help();
    } else if (!strcmp(tok[0], "pos")) {
        int32_t p = bus_position_actual();
        int32_t ang = ((p % COUNTS_PER_REV) + COUNTS_PER_REV) % COUNTS_PER_REV;
        int near = (int)(((int64_t)ang * NUM_STATIONS + COUNTS_PER_REV / 2)
                         / COUNTS_PER_REV) % NUM_STATIONS;
        printf("pos=%d  angle=%d/%d counts  nearest station=%d (at %d)\n",
               p, ang, COUNTS_PER_REV, near,
               (int)(((int64_t)near * COUNTS_PER_REV + NUM_STATIONS / 2) / NUM_STATIONS));
    } else if (!strcmp(tok[0], "goto") && nt == 2) {
        session_move((int32_t)strtol(tok[1], NULL, 0), 0);
    } else if (!strcmp(tok[0], "move") && nt == 2) {
        session_move((int32_t)strtol(tok[1], NULL, 0), 1);
    } else if (!strcmp(tok[0], "station") && nt == 2) {
        int n = (int)strtol(tok[1], NULL, 0);
        int32_t target = station_target(n);
        printf("station %d -> absolute %d\n", n, target);
        session_move(target, 0);
    } else if (!strcmp(tok[0], "home")) {
        int8_t method = (nt == 2) ? (int8_t)strtol(tok[1], NULL, 0) : HOME_METHOD;
        do_home(method);                              /* loop restores MODE_PP next cycle */
    } else if (!strcmp(tok[0], "sdo") && nt >= 2 && !strcmp(tok[1], "get") && nt == 5) {
        cmd_sdo_get((uint16_t)strtol(tok[2], NULL, 16), (uint8_t)strtol(tok[3], NULL, 0), tok[4]);
    } else if (!strcmp(tok[0], "sdo") && nt >= 2 && !strcmp(tok[1], "set") && nt == 6) {
        cmd_sdo_set((uint16_t)strtol(tok[2], NULL, 16), (uint8_t)strtol(tok[3], NULL, 0), tok[4], tok[5]);
    } else {
        printf("? unrecognized (try 'help')\n");
    }
    return 0;
}

static int cmd_session(void)
{
    signal(SIGINT, on_sigint);

    /* Profile dynamics are the drive's current values -- tune via in-session
     * `sdo set` (a second process can't open the NIC while this one holds it). */
    uint32_t pv = 0, acc = 0, dec = 0;
    sdo_read(SLAVE, 0x6081, 0, &pv, 4);
    sdo_read(SLAVE, 0x6083, 0, &acc, 4);
    sdo_read(SLAVE, 0x6084, 0, &dec, 4);
    printf("profile: velocity=%u accel=%u decel=%u (tune via 'sdo set')\n", pv, acc, dec);

    if (!bus_enter_op()) return 1;
    if (!cia402_enable(MODE_PP)) {
        uint16_t sw = bus_statusword();
        fprintf(stderr, "enable failed: status=0x%04X (%s)\n", sw, cia402_state(sw));
        bus_report_state("session enable failed");
        return 1;
    }
    printf("SESSION: enabled in PP, servo-holding at pos=%d. 'help' for commands, 'q' to exit.\n",
           bus_position_actual());
    printf("carousel> ");
    fflush(stdout);

    char line[256];
    size_t len = 0;
    int bad = 0, quit = 0;
    while (g_run && !quit) {
        bus_set_controlword(CW_ENABLE_OP);            /* hold; keeps OP/DC alive */
        bus_set_mode(MODE_PP);
        bus_cycle();

        uint16_t sw = bus_statusword();
        if ((sw & SW_MASK) == SW_OPERATION_ENABLED) bad = 0;
        else if (++bad >= 10) {
            fprintf(stderr, "\ndrive left OperationEnabled: status=0x%04X (%s)\n",
                    sw, cia402_state(sw));
            break;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        struct timeval tv = { 0, 0 };                 /* poll only: never stall the PDO */
        if (select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv) > 0) {
            char buf[64];
            ssize_t r = read(STDIN_FILENO, buf, sizeof buf);
            if (r <= 0) break;                        /* EOF/error: exit cleanly */
            for (ssize_t i = 0; i < r && !quit; i++) {
                if (buf[i] == '\n') {
                    line[len] = '\0';
                    len = 0;
                    quit = session_dispatch(line);
                    if (!quit) { printf("carousel> "); fflush(stdout); }
                } else if (len < sizeof line - 1) {
                    line[len++] = buf[i];
                }
            }
        }
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
        "  %s <ifname> move <counts>          (PP relative move; 10000 = 1 rev)\n"
        "  %s <ifname> home [method]          (homing; default method %d)\n"
        "  %s <ifname> session                (interactive PP session; holds between commands)\n"
        "  type = u8|u16|u32|i8|i16|i32\n", p, p, p, p, p, p, p, p, p, p, HOME_METHOD, p);
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
    } else if (!strcmp(cmd, "move") && argc == 4) {
        rc = cmd_move((int32_t)strtol(argv[3], NULL, 0));
    } else if (!strcmp(cmd, "home")) {
        int8_t method = (argc == 4) ? (int8_t)strtol(argv[3], NULL, 0) : HOME_METHOD;
        rc = cmd_home(method);
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

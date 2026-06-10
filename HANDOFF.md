# Carousel / CL57EC EtherCAT — Session Handoff

Full log of what we built, the issues we hit, and what we learned about the
options. Read this top-to-bottom to resume.

---

## 1. The system

- **Drive:** STEPPERONLINE **CL57EC** EtherCAT closed-loop stepper. Internally it
  enumerates as **YAKO ESD2505PE** — vendor `0x00000994`, product `0x00001B00`,
  rev `0x1`. (ESI file = `docs/Bohign_MS_ECAT_V2.5.xml`, a multi-device YAKO file.)
- **Motor:** ~5 A NEMA-23-class closed-loop stepper, **1:1 direct drive** (no reducer)
  to a **~2-foot metal carousel disc** (significant rotational inertia, all on the motor).
- **Home sensor:** Panasonic **PM-L25-C3** — optical U-slot photomicrosensor, **NPN
  open-collector**, 6 mm slot. Wired common-anode (`XCOM→+24V`, output→**X0**).
  X0 default function = Origin (`2310h=1`). A flag/vane on the carousel passes the slot.
- **Bench:** OnLogic **Karbon** (host `karbon2`, user `eo`). Dual Intel **I210** NICs
  (driver `igb`) = `enp6s0`/`enp7s0`. **EtherCAT cable is on `enp7s0`.**
- **Dev:** Mac. Repo `carousel/` → GitHub `evanhedding-EO/carousel` (branch `main`).
  **User handles all git**; the Karbon mirrors the repo and `git pull`s.

## 2. Software stack

- **Master:** SOEM **2.0.0** (context-based `ecx_*` API, NOT classic `ec_*`). Checked
  out as a **sibling**: `carousel_testing/SOEM/`.
- **Build:** plain **`make`, no CMake**. The Makefile compiles SOEM's `src/*.c` +
  `osal/linux` + `oshw/linux` directly and ships SOEM's options header at
  `soem_config/soem/ec_options.h` (static, SOEM stock defaults — the file CMake
  normally generates).
- **`net-up.sh`** — brings the NIC up after a power-cycle (the interface defaults to
  admin-DOWN each boot; EtherCAT needs link, no IP). Usage: `./net-up.sh` (auto-sudo).
- **Binary:** `sudo ./build/carousel <ifname> <command>`. Files: `main.c`, `bus.{c,h}`.

### Build + run on the Karbon
```sh
./net-up.sh
cd carousel && git pull && make
sudo ./build/carousel enp7s0 <command>
```

## 3. Commands built (all in the single `carousel` binary)

| Command | What | Status |
|---|---|---|
| `scan` | enumerate bus, print vendor/product/rev/state | ✅ verified |
| `params` | read key config objects (read-only) | ✅ verified |
| `sdo get/set <idxHex> <sub> <type> [val]` | generic SDO; type=u8/u16/u32/i8/i16/i32 | ✅ verified |
| `inputs` | live digital inputs `60FD` (HOME=bit2) | ✅ verified |
| `pdomap` | reads the drive's REAL PDO map from `0x1C12/0x1C13` | ✅ verified |
| `enable` | energize at zero velocity (PV), hold, Ctrl-C disables | ✅ verified |
| `spin <counts/s>` | PV-mode continuous rotation (10000=1 rev/s) | ✅ verified both dirs |
| `move <counts>` | PP relative move, new-setpoint handshake | ✅ verified (~3–26 cnt error) |
| `home [method]` | CiA402 homing (default method 21) | ⚠️ homes, but control is bad (see §6) |
| `session` | interactive PP session: enable once, hold, `goto/move/station/home/pos/sdo/q` | 🆕 built, needs hardware test |

## 4. KEY LEARNINGS (the important stuff)

### 4a. The ESI XML does NOT match the firmware PDO mapping — trust `pdomap`
The XML claimed a 39-byte TxPDO with a leading ErrorCode (status at byte 2). The drive's
**actual** mapping (read live via `pdomap`) is **12 bytes each way**:

```
RxPDO 0x1600 (out): 6040 ControlWord @0 | 607A TargetPos @2 | 60FF TargetVel @6 | 6060 Mode @10 | pad @11
TxPDO 0x1A00 (in):  6041 StatusWord  @0 | 6064 PosActual @2 | 606C VelActual @6 | 6061 ModeDisp @10 | pad @11
```
Mode of operation (6060) **is cyclic** (out byte 10) — set it in the PDO, not by SDO.
`bus.c` reads/writes these exact offsets. Cost us a long debug (status read as `0xFFFD`,
which was actually PosActual = −3).

### 4b. Reaching OP and holding it
- `ecx_config_init` only *requests* PRE-OP; `bus_open()` explicitly forces PRE-OP +
  `statecheck` (mailbox/SDO needs PRE-OP or higher).
- **The drive needs Distributed Clocks (DC SYNC0) to HOLD OP.** Without it, OP drops
  after a few seconds. `bus_enter_op()` does `ecx_configdc()` + `ecx_dcsync0(slave,
  TRUE, 2 ms, 0)`. The ESC generates SYNC0 in hardware, so our loose ~2 ms software
  loop is fine.
- Sync-type objects `0x1C32:01` / `0x1C33:01` are **READ-ONLY** — you can't pick the
  sync mode by SDO; DC is activated via the DC registers (what `ecx_dcsync0` does).

### 4c. Object widths (from the manual dictionary)
`2206/2400/2401/2407` = **U16**; `6060/6098` = **I8**; `60FD/60FF/607C/6081/6083/6084/
6064/606C` = **U/I32**.

### 4d. Current config (read via `params`)
- `2206h` closed-loop mode = **3** (but we've been changing it — see §6)
- `2400h` microstep = **10000** → **10000 counts/rev** (= counts/carousel-rev at 1:1)
- `2401h` max current = **5000 mA** (matches the 5 A motor — correct)
- `2407h` encoder resolution = **4000** (1000-line × 4)

### 4e. Speed limits / safety objects
- **No global max-speed object** (no CiA402 `6080h`). Speed is capped per mode by the
  command object itself: **`6081h` = PP max speed**, **`60FFh` = PV speed**.
- User set **`6081h = 800`** for very slow/safe moves during tuning.
- Position **soft limits**: `607Dh:01` (neg) / `607Dh:02` (pos), default ±2e8.
- `6085h` emergency-stop decel default = **0** (abrupt) — set it if quick-stops feel violent.
- Accel/decel `6083h`/`6084h` factory default = **500000** (aggressive).

### 4f. CiA402 control in our code (named constants in `main.c`)
Control word: `CW_SHUTDOWN 0x06`, `CW_SWITCH_ON 0x07`, `CW_ENABLE_OP 0x0F`,
`CW_QUICK_STOP 0x02`, `CW_DISABLE_VOLTAGE 0x00`, `CW_FAULT_RESET 0x80`,
`CW_NEW_SETPOINT 0x10` (bit4; also "homing start"), `CW_RELATIVE 0x40` (bit6).
Status (mask `0x6F`): `0x40` SwitchOnDisabled, `0x21` Ready, `0x23` SwitchedOn,
`0x27` OperationEnabled; `0x08` fault bit; `0x0400` target-reached (bit10);
`0x1000` setpoint-ack / homing-attained (bit12); `0x2000` homing-error (bit13).
Shared helpers: `cia402_enable(mode)`, `cia402_disable()`, `pp_move(target, relative)`.

## 5. THE CORE PROBLEM: control quality (unsolved — main thing to fix)

**Symptom:** after homing, the carousel **overshoots → jerks back → settles offset**, and
in one attempt fell into a **runaway oscillation (limit cycle) around home**.

**Root cause:** an **underdamped closed-loop hold loop aggravated by high inertia**
(2-ft metal disc, 1:1 direct drive). Confirmed with `enable`: it holds fine undisturbed,
but a **hand torque bump → endless wobble** that won't damp out. The homing deceleration
transient is the "bump" that triggers it.

This is a **hybrid closed-loop stepper** — the dictionary exposes **NO position/velocity
PID gains**, only *filters*, a *mode* selector, and *current* settings. So tuning is
empirical (use the hand-bump-on-`enable` as the pass/fail test), not servo-PID.

## 6. TUNING FINDINGS — what we learned about the options

### `2206h` closed-loop mode — a real trade-off (tested on hardware)
| Mode | Hold | Moves |
|---|---|---|
| **3** (overdrive angle 2, default) | wobbles on disturbance | **smooth** (overshoot, no oscillation) |
| **2** (overdrive angle 1) | **solid even when bumped** | wild / jerky / hunts |
| **4** | solid | **hunts uncontrollably** |

**Conclusion:** modes 2/4 buy hold-stiffness but wreck moves. **Moves are
non-negotiable for a carousel → stay on mode 3** and fix the hold via *filtering +
gentle dynamics*, NOT by switching mode.

### Tuning knobs (all live-settable via `sdo set`, session-only)
- **`2213h` feedback speed filter** (1–64, default 32) — main damping knob for the
  hold wobble. Sweep both ways (too much adds lag). **Not yet swept — start here.**
- `2208h`/`2209h` pulse command filter, `240Dh` mean filter — smooth the command.
- `2405h`/`2406h` standstill lock current/time — at-rest hold behavior.
- `6083h`/`6084h` accel/decel ↓ — reduce overshoot/excitation.
- `2500h–2503h` current-loop gains (Kp/Ki/Kc) — last resort.
- **Open-loop mode (`2206h=1`)** = *cannot hunt* (no feedback loop). Fallback if
  closed-loop can't be tamed; still homes on the sensor. Loses stall/position-verify.

### Important: `move` now leaves dynamics to the drive
`cmd_move` no longer writes `6081/6083/6084` — it **reads and reports** them so they're
live-tunable via `sdo set`. ⚠️ Set `6083`/`6084` explicitly before moving, or an abrupt
default applies.

### ⚠️ None of the tuning is persisted
All `sdo set` changes are **session-only** and reset on power-cycle. To save to the
drive's EEPROM: **`sdo set 2102 0 u16 2`** then restart. Don't save until values are good.

## 7. `move` does not hold — by design
Every command is self-contained: **enable → act → `cia402_disable()` (de-energize)**.
That's why the shaft **free-spins after a move**. The drive *would* servo-hold with full
torque if left enabled in PP. The fix is **architectural** (stay enabled), not a setting.

## 8. Control architecture (decisions for next session)

The drive runs the real-time loop; the master commands high-level. Modes: **PP** (send a
target, drive holds it), **CSP** (stream a position every cycle), **PV** (speed). "Hold"
= staying enabled in a position mode (no setting). Homing mode 6 = canned convenience;
custom homing is possible (PV + watch `60FD` + touch-probe `60B8/60B9/60BA` hardware latch).

**User decisions (this session):**
1. **Control model = interactive PP session** — one process enables ONCE, stays enabled
   (servo-holds between commands), and accepts `goto`/`move`/`home`/`pos`/`quit`.
2. **Homing = tune the drive's built-in homing** (mode 6) to be gentle so its finish
   doesn't excite the wobble (slow latch `6099:2`, low accel `609A`, maybe set `6085`).

## 9. NEXT STEPS

1. ✅ **DONE — interactive PP session** (`session` subcommand, `main.c` only):
   - `bus_enter_op()` → `cia402_enable(MODE_PP)` once; ~2 ms loop holds
     `CW_ENABLE_OP`/`MODE_PP` (keeps OP/DC alive) and polls stdin non-blocking
     (`select()`, zero timeout, line buffer).
   - Commands: `goto` (absolute), `move` (relative), `station <n>` (shortest path),
     `home [method]`, `pos`, `sdo get/set` (live tuning — a 2nd process can't open
     the NIC while the session holds it), `q`. `cia402_disable()` on exit/Ctrl-C/EOF.
   - `do_home(method)` extracted from `cmd_home` (assumes enabled; session's loop
     restores MODE_PP the next cycle, so it holds at 0 after homing).
   - **Station indexing built in**: `NUM_STATIONS = 24` (user-confirmed),
     `COUNTS_PER_REV = 10000` → 416.67 counts/station. `station_target()` always
     recomputes from n (integer-rounded, error ≤0.5 count, never accumulates),
     normalizes delta to ±half-rev (shortest path, user-confirmed over
     unidirectional), commands absolute = cur+delta (position register continuous).
   - ⚠️ Not hardware-tested yet. Test: holds against hand torque; `home` → holds
     at 0; `station 6` then `station 18` (should reverse); `q` ramps + de-energizes.
2. **Tune the hold** (bench): on mode 3, sweep `2213h` (try 40/48/56/64), lower
   `6083/6084`, use the hand-bump test until a bump settles quickly. Use the
   session's `sdo set` + hold for this — bump while it holds, tune, bump again.
3. **Tune homing gentle**: slow `6099:2` latch + low `609A` accel so the finish doesn't
   kick the limit cycle; re-`home` and check landing repeatability over several runs.
4. **Home-sensor drift check** (deferred by user): when the flag passes the sensor,
   latch actual position and compare to expected (~0 mod 10000) — health check only;
   closed-loop already prevents lost steps. Software: poll HOME bit in the session
   loop. Better: hardware touch probe `60B8/60B9/60BA`.
5. When values are dialed in, persist with `2102h=2`. Calibrate `607C` home offset
   (jog to perfect station-0 alignment, measure delta from sensor edge) once the
   mechanical fixture is final.

### Operating model (user-confirmed this session)
Home **once per power-up** (slow), then navigate by **absolute station moves**;
"go home" = `goto 0` / `station 0`, NOT re-homing (homing's decel transient is
what excites the wobble). Stay enabled the whole session so vials survive bumps.
Sensor-on-pass is a drift *check*, not navigation. All speeds slow (vials).

## 10. Gotchas to remember
- NIC is admin-DOWN after every power-cycle → run `./net-up.sh` first.
- `scan` shows state `0x02` = PRE-OP (correct); SDO needs ≥ PRE-OP.
- If OP won't hold → DC (`ecx_configdc`/`ecx_dcsync0`) is the reason; already handled.
- Trust `pdomap`, never the ESI XML, for byte offsets.
- Tuning is session-only until `2102h=2`.

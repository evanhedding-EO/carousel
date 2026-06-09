# carousel — CL57EC EtherCAT test tool

Minimal C / SOEM tool for bringing up a STEPPERONLINE CL57EC (YAKO OEM) over
EtherCAT. Built in milestones; this stage is **SDO only — no motion**.

## Build (Linux)

```sh
git clone https://github.com/OpenEtherCATsociety/SOEM   # into this folder -> ./SOEM
cmake -B build && cmake --build build
```

Binary lands at `build/carousel`.

## Permissions

Raw-socket access needs privilege. Either run with `sudo`, or grant the binary
the capability once:

```sh
sudo setcap cap_net_raw,cap_net_admin+ep build/carousel
```

Find your NIC name with `ip link` (e.g. `eth0`, `enp3s0`).

## Commands (milestones 0–1b)

| Command | What it does |
|---|---|
| `carousel <if> scan` | enumerate the bus; print name/vendor/product/rev/state |
| `carousel <if> params` | read key config objects (read-only confirmation) |
| `carousel <if> sdo get <idxHex> <sub> <type>` | read one object |
| `carousel <if> sdo set <idxHex> <sub> <type> <val>` | write one object, then read back |
| `carousel <if> inputs` | live digital-input bits (`60FD`); HOME = bit 2 |

`<idxHex>` is hex (`2401`), `<sub>` decimal, `<val>` decimal or `0x…`.
`<type>` = `u8 u16 u32 i8 i16 i32`. Object widths from the ESI: `2206h/2400h/2401h/2407h` = `u16`.

### Suggested first run

```sh
sudo ./build/carousel eth0 scan        # confirm the CL57EC enumerates
sudo ./build/carousel eth0 params      # read current config
sudo ./build/carousel eth0 inputs      # hand-spin carousel, watch HOME toggle
# set motor current (use YOUR motor's rating in mA), confirms with read-back:
sudo ./build/carousel eth0 sdo set 2401 0 u16 2000
```

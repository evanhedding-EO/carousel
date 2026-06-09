#!/bin/sh
# net-up.sh - bring up the EtherCAT NIC (link only, no IP; SOEM uses raw frames).
# Run after a boot/power-cycle, then: sudo ./build/carousel <ifname> scan
#
# Usage: ./net-up.sh [ifname]      default ifname: enp7s0
#        (auto-elevates with sudo; pass enp6s0 if the cable's in the other port)

IF="${1:-enp7s0}"

# need root to set link state - re-exec under sudo if we aren't
[ "$(id -u)" -eq 0 ] || exec sudo "$0" "$@"

ip link set "$IF" up
echo "== $IF =="
ip -br link show "$IF"
ethtool "$IF" 2>/dev/null | grep -E 'Link detected|Speed' || echo "(ethtool not installed)"

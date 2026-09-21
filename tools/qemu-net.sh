#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# qemu-net.sh - put the machine on the real network, if this host can.
#
# Run as:  qemu-net.sh <qemu-binary> [args...]
#
# It works out how this particular host is connected, picks the best
# way to give the guest a presence on that network, adds the right -nic
# argument, runs QEMU, and takes the plumbing down again afterwards.
#
# WHY THIS IS DECIDED AT RUNTIME rather than written into a Makefile:
# the answer genuinely differs per machine and per day. A wired desktop
# can put the guest straight onto the LAN; the same tree on a laptop on
# Wi-Fi cannot, for reasons no configuration can work around; and a
# machine that already has a bridge wants to use it rather than have a
# second one built alongside. Hard-coding any one of those makes the
# other two wrong.
#
# WHAT IT WILL NOT DO: reconfigure an interface that is already carrying
# traffic. Everything here is additive -- a new macvtap or a new tap --
# so the worst case is that it fails to create something and falls back.
# Turning a host's primary interface into a bridge is the one approach
# that could take the host off the network, and it is deliberately not
# attempted automatically. Nothing that runs unattended should be able
# to do that to the machine it is running on.
#
# Override with SAGE_NET:
#
#   auto      work it out (the default)
#   macvtap   insist on a macvtap; fail rather than fall back
#   bridge    insist on joining an existing bridge
#   lan       insist on real LAN access; fail rather than fall back
#   slirp     QEMU's user-mode NAT -- no privileges, no LAN presence
#   none      no network at all
#
# The test suites deliberately do NOT use this. They run on slirp, so
# that they need no privileges, touch nothing outside the emulator, and
# give the same answer on every machine.

set -u

MODE=${SAGE_NET:-auto}
TAPIF=${SAGE_TAP:-sage0}
VERBOSE=${SAGE_NET_VERBOSE:-1}

say() {
    [ "$VERBOSE" = "0" ] || echo "net: $*" >&2
}

# --- what is this host connected by? --------------------------------

primary_iface() {
    ip route show default 2>/dev/null | awk '{print $5; exit}'
}

is_wireless() {      # is_wireless <iface>
    [ -d "/sys/class/net/$1/wireless" ] || [ -e "/sys/class/net/$1/phy80211" ]
}

# The bridge an interface is enslaved to, if any.
bridge_of() {        # bridge_of <iface>
    local m
    m=$(readlink -f "/sys/class/net/$1/master" 2>/dev/null) || return 1
    [ -n "$m" ] || return 1
    m=$(basename "$m")
    [ -d "/sys/class/net/$m/bridge" ] || return 1
    echo "$m"
}

have_sudo() {
    sudo -n true 2>/dev/null
}

#
# A stable MAC for this host's guest.
#
# Locally administered (the 02 bit in the first octet is what 52 carries
# here, the same prefix QEMU uses) and derived from the hostname, so that
# the guest keeps the same address across runs and therefore keeps the
# same DHCP lease. Two different hosts running this tree get two
# different guests rather than a duplicate address on the LAN, which is
# the failure this is actually avoiding.
#
guest_mac() {
    local h
    h=$(hostname | cksum | cut -d' ' -f1)
    printf '52:54:00:%02x:%02x:%02x\n' \
        $(( (h >> 16) & 0xff )) $(( (h >> 8) & 0xff )) $(( h & 0xff ))
}

# --- the three ways of attaching ------------------------------------

cleanup_cmd=""
qemu_pid=""

cleanup() {
    if [ -n "$qemu_pid" ]; then
        kill "$qemu_pid" 2>/dev/null
        wait "$qemu_pid" 2>/dev/null
        qemu_pid=""
    fi
    if [ -n "$cleanup_cmd" ]; then
        eval "$cleanup_cmd" >/dev/null 2>&1
        cleanup_cmd=""
    fi
}
trap cleanup EXIT INT TERM

#
# Attach a tap to a bridge that already exists.
#
# The best case, and the only one where the HOST can also talk to the
# guest: a bridge forwards between all of its ports including the one
# the host's own address sits on. If the machine already has a bridge
# carrying its primary interface -- a VM host usually does -- this costs
# nothing and gives full L2 connectivity.
#
try_bridge() {       # try_bridge <bridge>
    local br=$1 mac
    mac=$(guest_mac)

    have_sudo || return 1
    sudo ip link del "$TAPIF" >/dev/null 2>&1
    sudo ip tuntap add dev "$TAPIF" mode tap user "$(id -un)" || return 1
    sudo ip link set "$TAPIF" master "$br" || { sudo ip link del "$TAPIF"; return 1; }
    sudo ip link set "$TAPIF" up || { sudo ip link del "$TAPIF"; return 1; }

    cleanup_cmd="sudo ip link del $TAPIF"
    NIC_ARG="tap,ifname=$TAPIF,script=no,downscript=no,mac=$mac"
    say "bridged onto $br via $TAPIF, mac $mac"
    say "the guest is a real host on that LAN -- DHCP will reach it"
    return 0
}

#
# macvtap: give the guest its own MAC on the parent interface.
#
# Real L2 presence on the LAN with no reconfiguration of anything -- the
# parent keeps its address and its connection throughout, and removing
# the macvtap is one command. That makes it the right default on a wired
# host with no bridge.
#
# ONE LIMITATION, and it is inherent rather than a bug: in macvlan the
# parent interface and its macvtap children cannot talk to each other.
# Every other machine on the LAN can reach the guest; the host running
# QEMU cannot. Test from a different machine, or use a bridge.
#
try_macvtap() {      # try_macvtap <parent>
    local parent=$1 mac idx node
    mac=$(guest_mac)

    have_sudo || return 1
    sudo ip link del "$TAPIF" >/dev/null 2>&1
    sudo ip link add link "$parent" name "$TAPIF" type macvtap mode bridge \
        2>/dev/null || return 1
    sudo ip link set "$TAPIF" address "$mac" up || {
        sudo ip link del "$TAPIF"; return 1; }

    idx=$(cat "/sys/class/net/$TAPIF/ifindex" 2>/dev/null) || {
        sudo ip link del "$TAPIF"; return 1; }
    node=/dev/tap$idx

    # The device node is created by udev and does not always exist the
    # instant the link does.
    for _ in $(seq 1 20); do
        [ -c "$node" ] && break
        sleep 0.1
    done
    [ -c "$node" ] || { sudo ip link del "$TAPIF"; return 1; }

    sudo chown "$(id -u)" "$node" || { sudo ip link del "$TAPIF"; return 1; }

    cleanup_cmd="sudo ip link del $TAPIF"
    MACVTAP_NODE=$node
    NIC_ARG="tap,fd=3,mac=$mac"
    say "macvtap on $parent via $TAPIF, mac $mac"
    say "the guest is a real host on that LAN -- DHCP will reach it"
    say "NOTE: $(hostname) itself cannot ping the guest; any other host can"
    return 0
}

# --- choose ----------------------------------------------------------

NIC_ARG=""
MACVTAP_NODE=""

choose() {
    local iface br

    case "$MODE" in
    macvtap)
        iface=$(primary_iface)
        try_macvtap "$iface" && return 0
        echo "net: macvtap on $iface was asked for and failed" >&2
        exit 1
        ;;
    bridge)
        iface=$(primary_iface)
        br=$(bridge_of "$iface") && try_bridge "$br" && return 0
        echo "net: no bridge on $iface to join" >&2
        exit 1
        ;;
    none)
        NIC_ARG="none"
        say "no network, by request"
        return 0
        ;;
    slirp)
        NIC_ARG="user"
        say "user-mode NAT, by request"
        return 0
        ;;
    esac

    iface=$(primary_iface)
    if [ -z "$iface" ]; then
        say "no default route on this host; falling back to user-mode NAT"
        NIC_ARG="user"
        return 0
    fi

    if br=$(bridge_of "$iface"); then
        say "$iface is already on bridge $br"
        try_bridge "$br" && return 0
        say "could not add a port to $br"
    fi

    if is_wireless "$iface"; then
        #
        # Not a limitation of this script. An 802.11 station may only
        # use its own MAC as the source of the frames it sends, so it
        # cannot carry a second machine's traffic -- bridging and
        # macvtap are both impossible on a Wi-Fi link unless the AP
        # speaks 4-address (WDS) mode, which almost none do.
        #
        say "$iface is wireless -- Wi-Fi cannot bridge a second MAC"
        say "falling back to user-mode NAT; use a wired host for real LAN access"
        NIC_ARG="user"
        return 0
    fi

    if ! have_sudo; then
        say "$iface is wired, but this needs sudo to attach to it"
        say "falling back to user-mode NAT"
        NIC_ARG="user"
        return 0
    fi

    try_macvtap "$iface" && return 0

    say "could not attach to $iface; falling back to user-mode NAT"
    NIC_ARG="user"
    return 0
}

choose

if [ "$MODE" = "lan" ] && [ "$NIC_ARG" = "user" ]; then
    echo "net: SAGE_NET=lan was asked for and real LAN access is not" >&2
    echo "net: available on this host. Refusing to fall back silently." >&2
    exit 1
fi

[ $# -gt 0 ] || { echo "usage: $0 <qemu> [args...]" >&2; exit 2; }

#
# QEMU's on-board NIC takes its backend from -nic: the machine creates
# the LAN91C111 itself and calls qemu_configure_nic_device(), so there is
# no -device to attach a -netdev to.
#
#
# QEMU runs in the background and this waits for it, rather than running
# it in the foreground. That is not a style choice: bash defers a trap
# until the current FOREGROUND command finishes, so a script that runs
# QEMU in the foreground and is then killed never runs its cleanup --
# the emulator keeps going and the interface it was using is left behind
# on the host. Waiting on a background child makes the signal arrive
# while this shell is the one running.
#
if [ -n "$MACVTAP_NODE" ]; then
    # The fd has to be opened by the shell that runs QEMU.
    exec 3<>"$MACVTAP_NODE"
    "$@" -nic "$NIC_ARG" &
else
    "$@" -nic "$NIC_ARG" &
fi
qemu_pid=$!

wait "$qemu_pid"
rc=$?
qemu_pid=""

[ -n "$MACVTAP_NODE" ] && exec 3>&-

cleanup
exit $rc

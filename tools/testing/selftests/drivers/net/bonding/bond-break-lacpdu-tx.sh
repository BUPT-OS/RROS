#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

# Regression Test:
#   Verify LACPDUs get transmitted after setting the MAC address of
#   the bond.
#
# https://bugzilla.redhat.com/show_bug.cgi?id=2020773
#
#       +---------+
#       | fab-br0 |
#       +---------+
#            |
#       +---------+
#       |  fbond  |
#       +---------+
#        |       |
#    +------+ +------+
#    |veth1 | |veth2 |
#    +------+ +------+
#
# We use veths instead of physical interfaces

set -e
tmp=$(mktemp -q dump.XXXXXX)
cleanup() {
	ip link del fab-br0 >/dev/null 2>&1 || :
	ip link del fbond  >/dev/null 2>&1 || :
	ip link del veth1-bond  >/dev/null 2>&1 || :
	ip link del veth2-bond  >/dev/null 2>&1 || :
	modprobe -r bonding  >/dev/null 2>&1 || :
	rm -f -- ${tmp}
}

trap cleanup 0 1 2
cleanup
sleep 1

# create the bridge
ip link add fab-br0 address 52:54:00:3B:7C:A6 mtu 1500 type bridge \
	forward_delay 15

# create the bond
ip link add fbond type bond mode 4 miimon 200 xmit_hash_policy 1 \
	ad_actor_sys_prio 65535 lacp_rate fast

# set bond address
ip link set fbond address 52:54:00:3B:7C:A6
ip link set fbond up

# set again bond sysfs parameters
ip link set fbond type bond ad_actor_sys_prio 65535

# create veths
ip link add name veth1-bond type veth peer name veth1-end
ip link add name veth2-bond type veth peer name veth2-end

# add ports
ip link set fbond master fab-br0
ip link set veth1-bond master fbond
ip link set veth2-bond master fbond

# bring up
ip link set veth1-end up
ip link set veth2-end up
ip link set fab-br0 up
ip link set fbond up
ip addr add dev fab-br0 10.0.0.3

tcpdump -n -i veth1-end -e ether proto 0x8809 >${tmp} 2>&1 &
sleep 15
pkill tcpdump >/dev/null 2>&1
rc=0
num=$(grep "packets captured" ${tmp} | awk '{print $1}')
if test "$num" -gt 0; then
	echo "PASS, captured ${num}"
else
	echo "FAIL"
	rc=1
fi
exit $rc

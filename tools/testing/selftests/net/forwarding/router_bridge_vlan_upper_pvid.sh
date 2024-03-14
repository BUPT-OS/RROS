#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

# +----------------------------+
# |                   H1 (vrf) |
# |   + $h1.10                 |                      +----------------------+
# |   | 192.0.2.1/28           |                      |             H2 (vrf) |
# |   | 2001:db8:1::1/64       |                      |  + $h2               |
# |   |                        |                      |  | 192.0.2.130/28    |
# |   + $h1                    |                      |  | 2001:db8:2::2/64  |
# +---|------------------------+                      +--|-------------------+
#     |                                                  |
# +---|--------------------------------------------------|-------------------+
# |   |                            router (main VRF)     |                   |
# | +-|--------------------------+                       + $swp2             |
# | | + $swp1      BR1 (802.1q)  |                         192.0.2.129/28    |
# | +-----+----------------------+                         2001:db8:2::1/64  |
# |       |                                                                  |
# |       + br1.10                                                           |
# |         192.0.2.2/28                                                     |
# |         2001:db8:1::2/64                                                 |
# +--------------------------------------------------------------------------+

ALL_TESTS="
	ping_ipv4
	ping_ipv6
	pvid_set_unset
	ping_ipv4
	ping_ipv6
	pvid_set_move
	ping_ipv4
	ping_ipv6
"
NUM_NETIFS=4
source lib.sh

h1_create()
{
	simple_if_init $h1
	vlan_create $h1 10 v$h1 192.0.2.1/28 2001:db8:1::1/64
	ip -4 route add 192.0.2.128/28 vrf v$h1 nexthop via 192.0.2.2
	ip -6 route add 2001:db8:2::/64 vrf v$h1 nexthop via 2001:db8:1::2
}

h1_destroy()
{
	ip -6 route del 2001:db8:2::/64 vrf v$h1
	ip -4 route del 192.0.2.128/28 vrf v$h1
	vlan_destroy $h1 10
	simple_if_fini $h1
}

h2_create()
{
	simple_if_init $h2 192.0.2.130/28 2001:db8:2::2/64
	ip -4 route add 192.0.2.0/28 vrf v$h2 nexthop via 192.0.2.129
	ip -6 route add 2001:db8:1::/64 vrf v$h2 nexthop via 2001:db8:2::1
}

h2_destroy()
{
	ip -6 route del 2001:db8:1::/64 vrf v$h2
	ip -4 route del 192.0.2.0/28 vrf v$h2
	simple_if_fini $h2 192.0.2.130/28 2001:db8:2::2/64
}

router_create()
{
	ip link add name br1 address $(mac_get $swp1) \
		type bridge vlan_filtering 1 vlan_default_pvid 0
	ip link set dev br1 up

	ip link set dev $swp1 master br1
	ip link set dev $swp1 up

	ip link set dev $swp2 up
	__addr_add_del $swp2 add 192.0.2.129/28 2001:db8:2::1/64

	bridge vlan add dev br1 vid 10 self
	bridge vlan add dev $swp1 vid 10
	vlan_create br1 10 "" 192.0.2.2/28 2001:db8:1::2/64
}

router_destroy()
{
	vlan_destroy br1 10
	bridge vlan del dev $swp1 vid 10
	bridge vlan del dev br1 vid 10 self

	__addr_add_del $swp2 del 192.0.2.129/28 2001:db8:2::1/64
	ip link set dev $swp2 down

	ip link set dev $swp1 down
	ip link set dev $swp1 nomaster

	ip link del dev br1
}

setup_prepare()
{
	h1=${NETIFS[p1]}
	swp1=${NETIFS[p2]}

	swp2=${NETIFS[p3]}
	h2=${NETIFS[p4]}

	vrf_prepare

	h1_create
	h2_create

	router_create

	forwarding_enable
}

pvid_set_unset()
{
	log_info "Set and unset PVID on VLAN 10"

	bridge vlan add dev br1 vid 10 pvid self
	sleep 1
	bridge vlan add dev br1 vid 10 self
}

pvid_set_move()
{
	log_info "Set PVID on VLAN 10, then move it to VLAN 20"

	bridge vlan add dev br1 vid 10 pvid self
	sleep 1
	bridge vlan add dev br1 vid 20 pvid self
}

shuffle_vlan()
{
	log_info ""
}

cleanup()
{
	pre_cleanup

	forwarding_restore

	router_destroy

	h2_destroy
	h1_destroy

	vrf_cleanup
}

ping_ipv4()
{
	ping_test $h1 192.0.2.130
}

ping_ipv6()
{
	ping6_test $h1 2001:db8:2::2
}

trap cleanup EXIT

setup_prepare
setup_wait

tests_run

exit $EXIT_STATUS

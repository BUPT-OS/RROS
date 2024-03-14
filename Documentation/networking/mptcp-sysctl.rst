.. SPDX-License-Identifier: GPL-2.0

=====================
MPTCP Sysfs variables
=====================

/proc/sys/net/mptcp/* Variables
===============================

enabled - BOOLEAN
	Control whether MPTCP sockets can be created.

	MPTCP sockets can be created if the value is 1. This is a
	per-namespace sysctl.

	Default: 1 (enabled)

add_addr_timeout - INTEGER (seconds)
	Set the timeout after which an ADD_ADDR control message will be
	resent to an MPTCP peer that has not acknowledged a previous
	ADD_ADDR message.

	The default value matches TCP_RTO_MAX. This is a per-namespace
	sysctl.

	Default: 120

checksum_enabled - BOOLEAN
	Control whether DSS checksum can be enabled.

	DSS checksum can be enabled if the value is nonzero. This is a
	per-namespace sysctl.

	Default: 0

allow_join_initial_addr_port - BOOLEAN
	Allow peers to send join requests to the IP address and port number used
	by the initial subflow if the value is 1. This controls a flag that is
	sent to the peer at connection time, and whether such join requests are
	accepted or denied.

	Joins to addresses advertised with ADD_ADDR are not affected by this
	value.

	This is a per-namespace sysctl.

	Default: 1

pm_type - INTEGER
	Set the default path manager type to use for each new MPTCP
	socket. In-kernel path management will control subflow
	connections and address advertisements according to
	per-namespace values configured over the MPTCP netlink
	API. Userspace path management puts per-MPTCP-connection subflow
	connection decisions and address advertisements under control of
	a privileged userspace program, at the cost of more netlink
	traffic to propagate all of the related events and commands.

	This is a per-namespace sysctl.

	* 0 - In-kernel path manager
	* 1 - Userspace path manager

	Default: 0

stale_loss_cnt - INTEGER
	The number of MPTCP-level retransmission intervals with no traffic and
	pending outstanding data on a given subflow required to declare it stale.
	The packet scheduler ignores stale subflows.
	A low stale_loss_cnt  value allows for fast active-backup switch-over,
	an high value maximize links utilization on edge scenarios e.g. lossy
	link with high BER or peer pausing the data processing.

	This is a per-namespace sysctl.

	Default: 4

scheduler - STRING
	Select the scheduler of your choice.

	Support for selection of different schedulers. This is a per-namespace
	sysctl.

	Default: "default"

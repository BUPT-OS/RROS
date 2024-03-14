/* SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause) */
#ifndef __BPF_TRACING_NET_H__
#define __BPF_TRACING_NET_H__

#define AF_INET			2
#define AF_INET6		10

#define SOL_SOCKET		1
#define SO_REUSEADDR		2
#define SO_SNDBUF		7
#define SO_RCVBUF		8
#define SO_KEEPALIVE		9
#define SO_PRIORITY		12
#define SO_REUSEPORT		15
#define SO_RCVLOWAT		18
#define SO_BINDTODEVICE		25
#define SO_MARK			36
#define SO_MAX_PACING_RATE	47
#define SO_BINDTOIFINDEX	62
#define SO_TXREHASH		74
#define __SO_ACCEPTCON		(1 << 16)

#define IP_TOS			1

#define IPV6_TCLASS		67
#define IPV6_AUTOFLOWLABEL	70

#define TC_ACT_UNSPEC		(-1)
#define TC_ACT_SHOT		2

#define SOL_TCP			6
#define TCP_NODELAY		1
#define TCP_MAXSEG		2
#define TCP_KEEPIDLE		4
#define TCP_KEEPINTVL		5
#define TCP_KEEPCNT		6
#define TCP_SYNCNT		7
#define TCP_WINDOW_CLAMP	10
#define TCP_CONGESTION		13
#define TCP_THIN_LINEAR_TIMEOUTS	16
#define TCP_USER_TIMEOUT	18
#define TCP_NOTSENT_LOWAT	25
#define TCP_SAVE_SYN		27
#define TCP_SAVED_SYN		28
#define TCP_CA_NAME_MAX		16
#define TCP_NAGLE_OFF		1

#define ICSK_TIME_RETRANS	1
#define ICSK_TIME_PROBE0	3
#define ICSK_TIME_LOSS_PROBE	5
#define ICSK_TIME_REO_TIMEOUT	6

#define ETH_HLEN		14
#define ETH_P_IPV6		0x86DD

#define CHECKSUM_NONE		0
#define CHECKSUM_PARTIAL	3

#define IFNAMSIZ		16

#define RTF_GATEWAY		0x0002

#define TCP_INFINITE_SSTHRESH	0x7fffffff
#define TCP_PINGPONG_THRESH	3

#define fib_nh_dev		nh_common.nhc_dev
#define fib_nh_gw_family	nh_common.nhc_gw_family
#define fib_nh_gw6		nh_common.nhc_gw.ipv6

#define inet_daddr		sk.__sk_common.skc_daddr
#define inet_rcv_saddr		sk.__sk_common.skc_rcv_saddr
#define inet_dport		sk.__sk_common.skc_dport

#define ir_loc_addr		req.__req_common.skc_rcv_saddr
#define ir_num			req.__req_common.skc_num
#define ir_rmt_addr		req.__req_common.skc_daddr
#define ir_rmt_port		req.__req_common.skc_dport
#define ir_v6_rmt_addr		req.__req_common.skc_v6_daddr
#define ir_v6_loc_addr		req.__req_common.skc_v6_rcv_saddr

#define sk_num			__sk_common.skc_num
#define sk_dport		__sk_common.skc_dport
#define sk_family		__sk_common.skc_family
#define sk_rmem_alloc		sk_backlog.rmem_alloc
#define sk_refcnt		__sk_common.skc_refcnt
#define sk_state		__sk_common.skc_state
#define sk_v6_daddr		__sk_common.skc_v6_daddr
#define sk_v6_rcv_saddr		__sk_common.skc_v6_rcv_saddr
#define sk_flags		__sk_common.skc_flags
#define sk_reuse		__sk_common.skc_reuse
#define sk_cookie		__sk_common.skc_cookie

#define s6_addr32		in6_u.u6_addr32

#define tw_daddr		__tw_common.skc_daddr
#define tw_rcv_saddr		__tw_common.skc_rcv_saddr
#define tw_dport		__tw_common.skc_dport
#define tw_refcnt		__tw_common.skc_refcnt
#define tw_v6_daddr		__tw_common.skc_v6_daddr
#define tw_v6_rcv_saddr		__tw_common.skc_v6_rcv_saddr

#endif

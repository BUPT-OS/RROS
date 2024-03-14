#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

. "$(dirname "${0}")/mptcp_lib.sh"

sec=$(date +%s)
rndh=$(printf %x $sec)-$(mktemp -u XXXXXX)
ns="ns1-$rndh"
ksft_skip=4
test_cnt=1
timeout_poll=100
timeout_test=$((timeout_poll * 2 + 1))
ret=0

flush_pids()
{
	# mptcp_connect in join mode will sleep a bit before completing,
	# give it some time
	sleep 1.1

	ip netns pids "${ns}" | xargs --no-run-if-empty kill -SIGUSR1 &>/dev/null

	for _ in $(seq 10); do
		[ -z "$(ip netns pids "${ns}")" ] && break
		sleep 0.1
	done
}

cleanup()
{
	ip netns pids "${ns}" | xargs --no-run-if-empty kill -SIGKILL &>/dev/null

	ip netns del $ns
}

mptcp_lib_check_mptcp

ip -Version > /dev/null 2>&1
if [ $? -ne 0 ];then
	echo "SKIP: Could not run test without ip tool"
	exit $ksft_skip
fi
ss -h | grep -q MPTCP
if [ $? -ne 0 ];then
	echo "SKIP: ss tool does not support MPTCP"
	exit $ksft_skip
fi

get_msk_inuse()
{
	ip netns exec $ns cat /proc/net/protocols | awk '$1~/^MPTCP$/{print $3}'
}

__chk_nr()
{
	local command="$1"
	local expected=$2
	local msg="$3"
	local skip="${4:-SKIP}"
	local nr

	nr=$(eval $command)

	printf "%-50s" "$msg"
	if [ $nr != $expected ]; then
		if [ $nr = "$skip" ] && ! mptcp_lib_expect_all_features; then
			echo "[ skip ] Feature probably not supported"
			mptcp_lib_result_skip "${msg}"
		else
			echo "[ fail ] expected $expected found $nr"
			mptcp_lib_result_fail "${msg}"
			ret=$test_cnt
		fi
	else
		echo "[  ok  ]"
		mptcp_lib_result_pass "${msg}"
	fi
	test_cnt=$((test_cnt+1))
}

__chk_msk_nr()
{
	local condition=$1
	shift 1

	__chk_nr "ss -inmHMN $ns | $condition" "$@"
}

chk_msk_nr()
{
	__chk_msk_nr "grep -c token:" "$@"
}

wait_msk_nr()
{
	local condition="grep -c token:"
	local expected=$1
	local timeout=20
	local msg nr
	local max=0
	local i=0

	shift 1
	msg=$*

	while [ $i -lt $timeout ]; do
		nr=$(ss -inmHMN $ns | $condition)
		[ $nr == $expected ] && break;
		[ $nr -gt $max ] && max=$nr
		i=$((i + 1))
		sleep 1
	done

	printf "%-50s" "$msg"
	if [ $i -ge $timeout ]; then
		echo "[ fail ] timeout while expecting $expected max $max last $nr"
		mptcp_lib_result_fail "${msg} # timeout"
		ret=$test_cnt
	elif [ $nr != $expected ]; then
		echo "[ fail ] expected $expected found $nr"
		mptcp_lib_result_fail "${msg} # unexpected result"
		ret=$test_cnt
	else
		echo "[  ok  ]"
		mptcp_lib_result_pass "${msg}"
	fi
	test_cnt=$((test_cnt+1))
}

chk_msk_fallback_nr()
{
	__chk_msk_nr "grep -c fallback" "$@"
}

chk_msk_remote_key_nr()
{
	__chk_msk_nr "grep -c remote_key" "$@"
}

__chk_listen()
{
	local filter="$1"
	local expected=$2
	local msg="$3"

	__chk_nr "ss -N $ns -Ml '$filter' | grep -c LISTEN" "$expected" "$msg" 0
}

chk_msk_listen()
{
	lport=$1

	# destination port search should always return empty list
	__chk_listen "dport $lport" 0 "listen match for dport $lport"

	# should return 'our' mptcp listen socket
	__chk_listen "sport $lport" 1 "listen match for sport $lport"

	__chk_listen "src inet:0.0.0.0:$lport" 1 "listen match for saddr and sport"

	__chk_listen "" 1 "all listen sockets"

	nr=$(ss -Ml $filter | wc -l)
}

chk_msk_inuse()
{
	local expected=$1
	local msg="$2"
	local listen_nr

	listen_nr=$(ss -N "${ns}" -Ml | grep -c LISTEN)
	expected=$((expected + listen_nr))

	for _ in $(seq 10); do
		if [ $(get_msk_inuse) -eq $expected ];then
			break
		fi
		sleep 0.1
	done

	__chk_nr get_msk_inuse $expected "$msg" 0
}

# $1: ns, $2: port
wait_local_port_listen()
{
	local listener_ns="${1}"
	local port="${2}"

	local port_hex i

	port_hex="$(printf "%04X" "${port}")"
	for i in $(seq 10); do
		ip netns exec "${listener_ns}" cat /proc/net/tcp | \
			awk "BEGIN {rc=1} {if (\$2 ~ /:${port_hex}\$/ && \$4 ~ /0A/) {rc=0; exit}} END {exit rc}" &&
			break
		sleep 0.1
	done
}

wait_connected()
{
	local listener_ns="${1}"
	local port="${2}"

	local port_hex i

	port_hex="$(printf "%04X" "${port}")"
	for i in $(seq 10); do
		ip netns exec ${listener_ns} grep -q " 0100007F:${port_hex} " /proc/net/tcp && break
		sleep 0.1
	done
}

trap cleanup EXIT
ip netns add $ns
ip -n $ns link set dev lo up

echo "a" | \
	timeout ${timeout_test} \
		ip netns exec $ns \
			./mptcp_connect -p 10000 -l -t ${timeout_poll} -w 20 \
				0.0.0.0 >/dev/null &
wait_local_port_listen $ns 10000
chk_msk_nr 0 "no msk on netns creation"
chk_msk_listen 10000

echo "b" | \
	timeout ${timeout_test} \
		ip netns exec $ns \
			./mptcp_connect -p 10000 -r 0 -t ${timeout_poll} -w 20 \
				127.0.0.1 >/dev/null &
wait_connected $ns 10000
chk_msk_nr 2 "after MPC handshake "
chk_msk_remote_key_nr 2 "....chk remote_key"
chk_msk_fallback_nr 0 "....chk no fallback"
chk_msk_inuse 2 "....chk 2 msk in use"
flush_pids

chk_msk_inuse 0 "....chk 0 msk in use after flush"

echo "a" | \
	timeout ${timeout_test} \
		ip netns exec $ns \
			./mptcp_connect -p 10001 -l -s TCP -t ${timeout_poll} -w 20 \
				0.0.0.0 >/dev/null &
wait_local_port_listen $ns 10001
echo "b" | \
	timeout ${timeout_test} \
		ip netns exec $ns \
			./mptcp_connect -p 10001 -r 0 -t ${timeout_poll} -w 20 \
				127.0.0.1 >/dev/null &
wait_connected $ns 10001
chk_msk_fallback_nr 1 "check fallback"
chk_msk_inuse 1 "....chk 1 msk in use"
flush_pids

chk_msk_inuse 0 "....chk 0 msk in use after flush"

NR_CLIENTS=100
for I in `seq 1 $NR_CLIENTS`; do
	echo "a" | \
		timeout ${timeout_test} \
			ip netns exec $ns \
				./mptcp_connect -p $((I+10001)) -l -w 20 \
					-t ${timeout_poll} 0.0.0.0 >/dev/null &
done
wait_local_port_listen $ns $((NR_CLIENTS + 10001))

for I in `seq 1 $NR_CLIENTS`; do
	echo "b" | \
		timeout ${timeout_test} \
			ip netns exec $ns \
				./mptcp_connect -p $((I+10001)) -w 20 \
					-t ${timeout_poll} 127.0.0.1 >/dev/null &
done

wait_msk_nr $((NR_CLIENTS*2)) "many msk socket present"
chk_msk_inuse $((NR_CLIENTS*2)) "....chk many msk in use"
flush_pids

chk_msk_inuse 0 "....chk 0 msk in use after flush"

mptcp_lib_result_print_all_tap
exit $ret

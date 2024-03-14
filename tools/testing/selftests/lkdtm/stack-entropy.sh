#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Measure kernel stack entropy by sampling via LKDTM's REPORT_STACK test.
set -e
samples="${1:-1000}"
TRIGGER=/sys/kernel/debug/provoke-crash/DIRECT
KSELFTEST_SKIP_TEST=4

# Verify we have LKDTM available in the kernel.
if [ ! -r $TRIGGER ] ; then
	/sbin/modprobe -q lkdtm || true
	if [ ! -r $TRIGGER ] ; then
		echo "Cannot find $TRIGGER (missing CONFIG_LKDTM?)"
	else
		echo "Cannot write $TRIGGER (need to run as root?)"
	fi
	# Skip this test
	exit $KSELFTEST_SKIP_TEST
fi

# Capture dmesg continuously since it may fill up depending on sample size.
log=$(mktemp -t stack-entropy-XXXXXX)
dmesg --follow >"$log" & pid=$!
report=-1
for i in $(seq 1 $samples); do
        echo "REPORT_STACK" > $TRIGGER
	if [ -t 1 ]; then
		percent=$(( 100 * $i / $samples ))
		if [ "$percent" -ne "$report" ]; then
			/bin/echo -en "$percent%\r"
			report="$percent"
		fi
	fi
done
kill "$pid"

# Count unique offsets since last run.
seen=$(tac "$log" | grep -m1 -B"$samples"0 'Starting stack offset' | \
	grep 'Stack offset' | awk '{print $NF}' | sort | uniq -c | wc -l)
bits=$(echo "obase=2; $seen" | bc | wc -L)
echo "Bits of stack entropy: $bits"
rm -f "$log"

# We would expect any functional stack randomization to be at least 5 bits.
if [ "$bits" -lt 5 ]; then
	echo "Stack entropy is low! Booted without 'randomize_kstack_offset=y'?"
	exit 1
else
	exit 0
fi

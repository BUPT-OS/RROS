#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Runs tests for the HID subsystem

if ! command -v python3 > /dev/null 2>&1; then
	echo "hid-tools: [SKIP] python3 not installed"
	exit 77
fi

if ! python3 -c "import pytest" > /dev/null 2>&1; then
	echo "hid: [SKIP/ pytest module not installed"
	exit 77
fi

if ! python3 -c "import pytest_tap" > /dev/null 2>&1; then
	echo "hid: [SKIP/ pytest_tap module not installed"
	exit 77
fi

if ! python3 -c "import hidtools" > /dev/null 2>&1; then
	echo "hid: [SKIP/ hid-tools module not installed"
	exit 77
fi

TARGET=${TARGET:=.}

echo TAP version 13
python3 -u -m pytest $PYTEST_XDIST ./tests/$TARGET --tap-stream --udevd

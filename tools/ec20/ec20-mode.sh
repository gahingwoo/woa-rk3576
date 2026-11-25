#!/bin/bash
# ec20-mode.sh - read or set the Quectel EC20's USB network mode
# (AT+QCFG="usbnet") from Linux: MBIM (2) so Windows' in-box mobile
# broadband driver can use it, or QMI (0), the factory default. The Linux
# counterpart of ec20-mode.ps1; see docs/dev/ec20.md.
#
#   ec20-mode.sh status        identity, firmware, mode, USB interfaces, drivers
#   ec20-mode.sh mbim          set usbnet 2, read back, reset, check
#   ec20-mode.sh qmi           set usbnet 0 (revert), read back, reset, check
#   options: -p /dev/ttyUSBn   the AT port instead of looking it up
#            -y                do not ask
#            -n                do not reset (takes effect at the next power cycle)
#
# The mode is stored in the module and survives power cycles. The AT port
# used by default is interface 03 (ttyUSB3 on the TaishanPi 3M), which
# ModemManager is told to ignore there, so this does not race MM on
# interface 02. Needs root (or the dialout group) for the tty.
#
# Exit codes: 0 done and checked; 1 refused, cancelled or an AT command
# failed; 2 read-back mismatch; 3 no AT port; 4 the module did not come back
# in time; 5 the mode is set but the expected Linux driver is not bound.
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

set -u
VID=2c7c PID=0125
PORT="" YES=0 NORESET=0 WAIT=120

usage() { sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-1}"; }

# the USB device directory of the module in sysfs, or nothing
usbdev() {
	local d
	for d in /sys/bus/usb/devices/*; do
		[ -f "$d/idVendor" ] || continue
		if [ "$(cat "$d/idVendor")" = "$VID" ] && [ "$(cat "$d/idProduct")" = "$PID" ]; then
			echo "$d"; return 0
		fi
	done
	return 1
}

# the tty of interface $1 (two hex digits) of the module
tty_of() {
	local dev t
	dev=$(usbdev) || return 1
	for t in "$dev":1."$((10#$1))"/ttyUSB*; do
		[ -e "$t" ] && { echo "/dev/$(basename "$t")"; return 0; }
	done
	return 1
}

find_port() { tty_of 03 || tty_of 02; }

# at CMD [TIMEOUT_S]: send one command, print the reply up to the final result
at() {
	local cmd=$1 to=${2:-5} line out="" end
	exec 3<>"$PORT" || return 1
	stty -F "$PORT" 115200 raw -echo -echoe -echok -crtscts clocal 2>/dev/null
	# drop whatever is waiting (URCs)
	while IFS= read -r -t 0.2 line <&3; do :; done
	printf '%s\r' "$cmd" >&3
	end=$((SECONDS + to))
	while [ $SECONDS -lt $end ]; do
		IFS= read -r -t 1 line <&3 || continue
		line=${line%$'\r'}
		[ -z "$line" ] || [ "$line" = "$cmd" ] && continue
		out+="$line"$'\n'
		case $line in OK|ERROR|"+CME ERROR"*|"+CMS ERROR"*) break ;; esac
	done
	exec 3>&-
	printf '%s' "$out"
}

read_usbnet() {
	at 'AT+QCFG="usbnet"' | sed -n 's/.*"usbnet",[[:space:]]*\([0-9]\).*/\1/p' | head -n1
}

show_ifaces() {
	local dev i cls drv
	dev=$(usbdev) || { echo "no USB device $VID:$PID"; return 1; }
	printf '%-4s %-9s %s\n' IF CLASS DRIVER
	for i in "$dev":1.*; do
		[ -f "$i/bInterfaceClass" ] || continue
		cls="$(cat "$i/bInterfaceClass")/$(cat "$i/bInterfaceSubClass")/$(cat "$i/bInterfaceProtocol")"
		drv=-; [ -L "$i/driver" ] && drv=$(basename "$(readlink "$i/driver")")
		printf '%-4s %-9s %s\n' "${i##*.}" "$cls" "$drv"
	done
}

bound() { # bound DRIVER: is any interface of the module bound to it?
	local dev i
	dev=$(usbdev) || return 1
	for i in "$dev":1.*; do
		[ -L "$i/driver" ] && [ "$(basename "$(readlink "$i/driver")")" = "$1" ] && return 0
	done
	return 1
}

mode=""
while [ $# -gt 0 ]; do
	case $1 in
	status|mbim|qmi) mode=$1 ;;
	-p) PORT=${2:-}; shift ;;
	-y) YES=1 ;;
	-n) NORESET=1 ;;
	-h|--help) usage 0 ;;
	*) usage ;;
	esac
	shift
done
[ -n "$mode" ] || usage

[ -n "$PORT" ] || PORT=$(find_port) || PORT=""
if [ -z "$PORT" ] || [ ! -c "$PORT" ]; then
	echo "no AT port of $VID:$PID found (is the option driver loaded, is the modem powered?)"
	show_ifaces
	exit 3
fi
echo "AT port: $PORT"
cur=$(read_usbnet)
[ -n "$cur" ] || { echo "the module did not report its usbnet mode"; exit 1; }
echo "firmware: $(at AT+QGMR | grep -v '^OK$' | head -n1)"
echo "usbnet: $cur (0 QMI, 2 MBIM)"
if [ "$mode" = status ]; then
	show_ifaces
	exit 0
fi

want=2 drv=cdc_mbim
[ "$mode" = qmi ] && want=0 drv=qmi_wwan

if [ "$cur" = "$want" ]; then
	echo "already usbnet $want; nothing to set"
else
	if [ $YES -eq 0 ]; then
		printf 'Set usbnet %s (stored in the module, Windows and Linux both see it) and reset the module? [y/N] ' "$want"
		read -r a; [ "$a" = y ] || [ "$a" = Y ] || exit 1
	fi
	at "AT+QCFG=\"usbnet\",$want" | grep -qx OK || { echo "the module refused the mode"; exit 1; }
	rb=$(read_usbnet)
	[ "$rb" = "$want" ] || { echo "read-back before the reset: $rb, expected $want"; exit 2; }
	echo "set and read back: $rb"
	if [ $NORESET -eq 1 ]; then echo "not reset (-n): takes effect at the next power cycle"; exit 0; fi
	echo "resetting the module (AT+CFUN=1,1); it leaves USB for 20 to 40 s"
	{ exec 3<>"$PORT" && printf 'AT+CFUN=1,1\r' >&3; exec 3>&-; } 2>/dev/null
	sleep 10
	end=$((SECONDS + WAIT)) back=0
	while [ $SECONDS -lt $end ]; do
		sleep 3
		PORT=$(find_port) || continue
		at AT 2 | grep -qx OK && { back=1; break; }
	done
	[ $back -eq 1 ] || { echo "the AT port did not answer within $WAIT s"; show_ifaces; exit 4; }
	after=$(read_usbnet)
	echo "after the reset: usbnet $after"
	[ "$after" = "$want" ] || exit 2
	sleep 5
fi

show_ifaces
if bound "$drv"; then
	echo "network interface bound to $drv"
	command -v mmcli >/dev/null && echo "check: mmcli -L ; mmcli -m any"
	exit 0
fi
echo "no interface is bound to $drv (kernel without CONFIG_USB_NET_${drv^^}?)"
exit 5

#!/bin/sh
#
# ussd-query.sh (or send-ussd)
#
# Robust USSD query tool for Qualcomm MSM8916 modems on OpenWrt.
# Attempts native ModemManager USSD first; if rejected (e.g. on LTE
# without IMS USSD), transparently performs 3G UMTS fallback to
# execute AT+CUSD directly and restores LTE.
#

USSD_CODE="${1:-*101#}"
AT_PORT="/dev/wwan0at1"
[ -c "$AT_PORT" ] || AT_PORT="/dev/wwan0at0"
QMI_DEV="/dev/wwan0qmi0"

if [ -z "$USSD_CODE" ]; then
	echo "Usage: $0 <ussd_code>"
	echo "Example: $0 '*101#'"
	exit 1
fi

# Function to send an AT command and read response
send_at_cmd() {
	local cmd="$1"
	local timeout_sec="${2:-5}"
	local port="$AT_PORT"

	if [ ! -c "$port" ]; then
		return 1
	fi

	# Clear any pending input
	head -n 0 "$port" 2>/dev/null || true

	# Send command and read in background with timeout
	(
		exec 3<>"$port"
		printf "%s\r\n" "$cmd" >&3
		end_time=$(( $(date +%s) + timeout_sec ))
		buf=""
		while [ $(date +%s) -lt "$end_time" ]; do
			if read -t 1 -u 3 line; then
				buf="${buf}${line}"$'\n'
				case "$line" in
					*OK*|*ERROR*|*+CME*|*+CUSD:*)
						if echo "$line" | grep -q "+CUSD:"; then
							echo "$buf"
							exec 3>&-
							return 0
						elif echo "$line" | grep -q -E "ERROR|\+CME"; then
							echo "$buf"
							exec 3>&-
							return 1
						fi
						;;
				esac
			fi
		done
		echo "$buf"
		exec 3>&-
		return 2
	)
}

# 1. First Attempt: Native ModemManager USSD
MM_MODEM=$(mmcli -L 2>/dev/null | grep -o '/org/freedesktop/ModemManager1/Modem/[0-9]*' | head -n 1)
if [ -n "$MM_MODEM" ]; then
	MODEM_NUM=$(echo "$MM_MODEM" | grep -o '[0-9]*$')
	mm_out=$(mmcli -m "$MODEM_NUM" --3gpp-ussd-initiate="$USSD_CODE" 2>&1)
	if [ $? -eq 0 ] && ! echo "$mm_out" | grep -qi "error"; then
		resp=$(echo "$mm_out" | grep -i "network request:" | sed "s/.*network request: '//" | sed "s/'$//")
		if [ -n "$resp" ]; then
			echo "$resp"
			exit 0
		fi
	fi
fi

# 2. Fallback Attempt: Direct 3G CS-Domain AT+CUSD Fallback
if [ ! -c "$AT_PORT" ] || [ ! -e "$QMI_DEV" ]; then
	echo "Error: Required modem devices ($AT_PORT, $QMI_DEV) not available."
	exit 1
fi

# Temporarily pause ModemManager to prevent AT port concurrency collisions
mm_was_running=0
if /etc/init.d/modemmanager status 2>/dev/null | grep -q "running"; then
	/etc/init.d/modemmanager stop >/dev/null 2>&1
	mm_was_running=1
	sleep 1
fi

# Force UMTS mode to attach CS domain for Supplementary Services
qmicli -d "$QMI_DEV" -p --nas-set-system-selection-preference="umts,automatic" >/dev/null 2>&1
sleep 3

# Wait up to 10s for cell registration
reg_ok=0
for i in $(seq 1 10); do
	reg_stat=$(send_at_cmd "AT+CREG?" 2)
	if echo "$reg_stat" | grep -q -E "\+CREG: [0-9],(1|5)"; then
		reg_ok=1
		break
	fi
	sleep 1
done

# Dispatch USSD command
raw_resp=$(send_at_cmd "AT+CUSD=1,\"$USSD_CODE\",15" 8)

# Parse response from +CUSD: <m>,"<text>",<dcs>
parsed_resp=$(echo "$raw_resp" | grep "+CUSD:" | sed -E 's/.*\+CUSD: [0-9]+,"([^"]+)".*/\1/')

# Restore original LTE mode
qmicli -d "$QMI_DEV" -p --nas-set-system-selection-preference="lte,automatic" >/dev/null 2>&1

# Restore ModemManager if it was stopped
if [ "$mm_was_running" -eq 1 ]; then
	/etc/init.d/modemmanager start >/dev/null 2>&1
fi

if [ -n "$parsed_resp" ]; then
	# Output the parsed text
	printf "%b\n" "$parsed_resp"
	exit 0
else
	echo "Failed to retrieve USSD response."
	[ -n "$raw_resp" ] && echo "Raw output: $raw_resp"
	exit 1
fi

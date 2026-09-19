#!/bin/sh
# Change the UF896's IMEI via NV item 550 (NV_IMEI_I), using diag_nv_write's
# setbyte mode (the only write primitive it exposes -- no full-block write).
#
# Background: AT+EGMR (the usual vendor AT command for IMEI writes on many
# Qualcomm-based modems) returns ERROR on this firmware, so this goes straight
# at the DIAG NV item instead. Verified end-to-end on real hardware: written,
# rebooted, and re-read via `qmicli -d /dev/wwan0qmi0 --dms-get-ids` (a path
# independent of the diag tool) to confirm the change actually persisted to
# flash and took effect in the running QMI stack, not just in the NV blob.
#
# NV_IMEI_I encoding (9 meaningful bytes, rest of the 128-byte item is zero
# padding): byte[0] = 0x08 fixed type/length indicator (left untouched).
# byte[1..8] = 8 bytes of BCD digits, low nibble first, decoded as one 16-nibble
# stream: nibble[0] is a 0xA filler (pads the 15-digit IMEI to an even 16
# nibbles), nibbles[1..15] are the IMEI's 15 digits in order. Confirmed by
# round-tripping the *current* IMEI out of a real read before deriving this.
#
# Usage: ./change_imei.sh <ctrl-device> <channel-name> <new-15-digit-imei>
# Example: ./change_imei.sh /dev/rpmsg_ctrl2 DIAG 861716070514434
#
# Run on-device over SSH, with the diag_nv_write binary (built by this repo's
# build-diag-tools.yml CI workflow) already copied to e.g. /tmp/diag_nv_write.

set -e

CTRL="$1"
CHAN="$2"
IMEI="$3"
TOOL="${DIAG_NV_WRITE:-/tmp/diag_nv_write}"

if [ -z "$CTRL" ] || [ -z "$CHAN" ] || [ -z "$IMEI" ]; then
	echo "usage: $0 <ctrl-device> <channel-name> <new-15-digit-imei>" >&2
	exit 2
fi

if [ "$(echo -n "$IMEI" | wc -c)" -ne 15 ]; then
	echo "error: IMEI must be exactly 15 digits, got '$IMEI'" >&2
	exit 2
fi

case "$IMEI" in
	*[!0-9]*)
		echo "error: IMEI must be numeric, got '$IMEI'" >&2
		exit 2
		;;
esac

# Build the 16-nibble stream: filler 0xA, then the 15 IMEI digits.
nibbles="a$IMEI"

# Pack pairs of nibbles (low nibble = earlier digit, matching this firmware's
# decode order) into bytes at NV item 550 offsets 1..8.
i=1
off=1
while [ "$i" -le 16 ]; do
	lo=$(echo -n "$nibbles" | cut -c"$i")
	hi=$(echo -n "$nibbles" | cut -c"$((i + 1))")
	byte=$(( (0x$hi << 4) | 0x$lo ))
	echo "=== offset $off -> $byte (0x$(printf '%02x' "$byte")) ==="
	"$TOOL" "$CTRL" "$CHAN" setbyte 550 "$off" "$byte"
	i=$((i + 2))
	off=$((off + 1))
done

echo ""
echo "Done. Reboot the device for the new IMEI to take effect in the QMI stack,"
echo "then verify with: qmicli -d /dev/wwan0qmi0 --dms-get-ids"

#!/bin/sh
#
# Send one AT command directly to the modem's AT port and print whatever
# comes back within the given window. Needed because this board's busybox
# has no `stty`/`timeout`/`microcom`/`socat` -- this works with nothing but
# ash builtins plus `usleep`.
#
# ModemManager MUST be fully stopped first (`/etc/init.d/modemmanager stop`
# + `pkill -9 -f ModemManager`, verify with `ps`) before using this.
# Opening /dev/wwan0at1 while ModemManager also has it open has been
# observed to silently knock the modem back into DMS operating mode
# "factory-test" -- a real, reproducible bug (see the investigation report,
# section on modem-led-monitor's factory-test relapse watchdog).
#
# Usage: at_cmd.sh "<AT command>" [wait_seconds]
DEV=/dev/wwan0at1
CMD="$1"
WAIT="${2:-3}"
OUT=/tmp/at_out.$$

exec 3<>"$DEV"
cat <&3 >"$OUT" &
CATPID=$!
usleep 200000
printf '%s\r' "$CMD" >&3
sleep "$WAIT"
kill "$CATPID" 2>/dev/null
exec 3<&-
exec 3>&-
cat "$OUT"
rm -f "$OUT"

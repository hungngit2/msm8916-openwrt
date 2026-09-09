#!/bin/sh
#
# qcom-efs-provision.sh
#
# Automated EFS2 configuration for Qualcomm MSM8916 modems.
# Provisions Combined EPS/IMSI Attach, SMS over SGs, and IMS EFS files.
#

TAG="qcom-efs-provision"
MARKER="/etc/qcom-efs-provision.done"
DIAG_BIN="/usr/sbin/diag_efs"

log() {
	logger -t "$TAG" "$*"
	echo "[$TAG] $*"
}

# Allow forced re-provisioning
FORCE=0
[ "$1" = "--force" ] || [ "$1" = "-f" ] && FORCE=1

if [ "$FORCE" -eq 0 ] && [ -f "$MARKER" ]; then
	log "Modem EFS already provisioned ($MARKER exists). Use --force to re-check."
	exit 0
fi

if [ ! -x "$DIAG_BIN" ]; then
	log "Error: $DIAG_BIN not found or not executable."
	exit 1
fi

# Find the modem's rpmsg ctrl device (typically /dev/rpmsg_ctrl2 for remoteproc0)
CTRL_DEV=""
for c in /dev/rpmsg_ctrl*; do
	[ -e "$c" ] || continue
	# Check sysfs char dev to confirm it maps to remoteproc0 (modem)
	c_num=$(stat -c "%t:%T" "$c" 2>/dev/null || stat -f "%Hr:%Lr" "$c" 2>/dev/null || echo "")
	if [ -n "$c_num" ] && [ -d "/sys/dev/char/$c_num" ]; then
		link_target=$(readlink -f "/sys/dev/char/$c_num" 2>/dev/null || echo "")
		case "$link_target" in
			*4080000.remoteproc*|*remoteproc0*)
				CTRL_DEV="$c"
				break
				;;
		esac
	fi
done

# Fallback to /dev/rpmsg_ctrl2 if discovery loop failed but device exists
if [ -z "$CTRL_DEV" ] && [ -e "/dev/rpmsg_ctrl2" ]; then
	CTRL_DEV="/dev/rpmsg_ctrl2"
fi

if [ -z "$CTRL_DEV" ]; then
	log "Waiting for modem rpmsg ctrl device..."
	i=0
	while [ "$i" -lt 15 ]; do
		if [ -e "/dev/rpmsg_ctrl2" ]; then
			CTRL_DEV="/dev/rpmsg_ctrl2"
			break
		fi
		sleep 1
		i=$((i + 1))
	done
fi

if [ -z "$CTRL_DEV" ] || [ ! -e "$CTRL_DEV" ]; then
	log "Error: Modem rpmsg control device not found. Cannot access DIAG EFS."
	exit 1
fi

log "Using rpmsg ctrl device: $CTRL_DEV"

# Helper to verify and provision one EFS item
# Args: path expected_hex
provision_item() {
	local path="$1"
	local val="$2"

	log "Checking EFS item: $path"
	local out
	out=$("$DIAG_BIN" "$CTRL_DEV" DIAG read "$path" 2>&1)
	local rc=$?

	# Check if read succeeded and matches
	if [ "$rc" -eq 0 ] && echo "$out" | grep -qi "$val"; then
		log "  -> $path already configured ($val)."
		return 0
	fi

	log "  -> $path needs update (current output: $out). Provisioning '$val'..."
	# Try create first (for non-existent/lazy items)
	out=$("$DIAG_BIN" "$CTRL_DEV" DIAG create "$path" "$val" 2>&1)
	if [ $? -ne 0 ]; then
		# If create failed because file already exists, use write
		out=$("$DIAG_BIN" "$CTRL_DEV" DIAG write "$path" "$val" 2>&1)
	fi

	# Read-back verification
	out=$("$DIAG_BIN" "$CTRL_DEV" DIAG read "$path" 2>&1)
	if echo "$out" | grep -qi "$val"; then
		log "  -> Successfully verified $path = $val"
		return 0
	else
		log "  -> Warning: Read-back failed for $path ($out)"
		return 1
	fi
}

CHANGES_MADE=0

# 1. Multi-Mode (MMode) & NAS settings for Combined Attach & SMS over SGs
log "--- Phase 1: Configuring MMode Combined Attach & SMS over SGs ---"
provision_item "/nv/item_files/modem/mmode/sms_over_sgs" "01" && CHANGES_MADE=1
provision_item "/nv/item_files/modem/mmode/voice_domain_pref" "01" && CHANGES_MADE=1
provision_item "/nv/item_files/modem/mmode/ue_usage_setting" "01" && CHANGES_MADE=1

# 2. IMS / VoLTE settings
log "--- Phase 2: Configuring IMS Engine EFS items ---"
provision_item "/nv/item_files/ims/IMS_enable" "01" && CHANGES_MADE=1
provision_item "/nv/item_files/ims/ims_operation_mode" "01" && CHANGES_MADE=1
provision_item "/nv/item_files/ims/ims_hybrid_enable" "01" && CHANGES_MADE=1
provision_item "/nv/item_files/ims/qp_ims_reg_config" "01" && CHANGES_MADE=1
provision_item "/nv/item_files/ims/qp_ims_ussd_config" "01" && CHANGES_MADE=1
provision_item "/nv/item_files/ims/qp_ims_sms_config" "01" && CHANGES_MADE=1

touch "$MARKER"
log "Provisioning completed. Marker written to $MARKER."

if [ "$CHANGES_MADE" -eq 1 ]; then
	log "EFS items updated. Refreshing modem radio operating state..."
	# Send AT+CFUN=0 / CFUN=1 if AT port is idle
	for p in /dev/wwan0at1 /dev/wwan0at0; do
		if [ -c "$p" ]; then
			timeout 2 sh -c "printf 'AT+CFUN=0\r\n' > $p" 2>/dev/null || true
			sleep 1
			timeout 2 sh -c "printf 'AT+CFUN=1\r\n' > $p" 2>/dev/null || true
			break
		fi
	done
fi

log "Done."
exit 0

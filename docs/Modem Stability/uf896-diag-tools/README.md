# UF896 DIAG/rpmsg tooling

See [`../UF896_VOLTE_USSD_RCS_INVESTIGATION_REPORT.md`](../UF896_VOLTE_USSD_RCS_INVESTIGATION_REPORT.md) for the full
investigation this tooling was built for. This directory just holds the source.

`at_cmd.sh` is a separate, standalone helper -- not a DIAG tool, just a way to send
one AT command directly to the modem's AT port (`/dev/wwan0at1`) and print whatever
comes back, since this board's busybox has no `stty`/`timeout`/`microcom`/`socat`.
Needed repeatedly throughout this investigation for direct `AT+CUSD`/`AT+CMGL`
testing, bypassing ModemManager entirely. **ModemManager must be fully stopped
first** (see the comment header in the script) -- opening the AT port from two
processes at once has caused a real, reproducible modem relapse into DMS operating
mode `factory-test` (see the investigation report).

## Build

Cross-compile with this repo's own OpenWrt toolchain (after `./build.sh prepare`):

```bash
GCC=openwrt/staging_dir/toolchain-aarch64_generic_gcc-14.3.0_musl/bin/aarch64-openwrt-linux-musl-gcc
$GCC -static -Wall -O2 -I include -o diag_verno_test3 diag_verno_test3.c
$GCC -static -Wall -O2 -I include -o diag_nv_read     diag_nv_read.c
$GCC -static -Wall -O2 -I include -o diag_nv_sweep    diag_nv_sweep.c
$GCC -static -Wall -O2 -I include -o diag_nv_write    diag_nv_write.c
$GCC -static -Wall -O2 -I include -o diag_efs         diag_efs.c
```

CI also cross-compiles all tools in this directory automatically via
`.github/workflows/build-diag-tools.yml` (a lightweight musl-cross toolchain,
not the full OpenWrt build) -- trigger it with `workflow_dispatch` or by
pushing a change under this directory, then grab the built binaries from
the workflow run's artifacts.

`include/linux/rpmsg.h` is the real upstream kernel UAPI header (copied verbatim, not
hand-transcribed), needed because this toolchain's sysroot doesn't ship kernel headers.

## Use (on-device, over SSH)

All tools take the modem's DIAG ctrl device and channel name as the first two args.
On UF896, that's `/dev/rpmsg_ctrl2 DIAG` (confirmed via `/sys/dev/char/252:2` →
`.../4080000.remoteproc/.../remoteproc0/...` — the modem's remoteproc edge).

```bash
# Verify the pipeline still works (compares against known firmware build date):
./diag_verno_test3 /dev/rpmsg_ctrl2 DIAG

# Read one NV item (safe, read-only):
./diag_nv_read /dev/rpmsg_ctrl2 DIAG 550   # NV_IMEI_I, sanity check

# Sweep a range (safe, read-only):
./diag_nv_sweep /dev/rpmsg_ctrl2 DIAG 4200 4201 4202 ...   # explicit list, not a range syntax

# Read-only check of a specific item before considering a write:
./diag_nv_write /dev/rpmsg_ctrl2 DIAG read 4201

# Write ONE byte (reads first, prints original bytes, writes, reads back to verify):
./diag_nv_write /dev/rpmsg_ctrl2 DIAG setbyte 4201 0 1
```

`diag_nv_write`'s `setbyte` mode always prints the exact original 128 bytes before
writing anything, so a revert is just re-running `setbyte` with the original value.

## Changing IMEI (`change_imei.sh`)

`AT+EGMR` (the usual vendor AT command for IMEI writes on many Qualcomm-based
modems) returns `ERROR` on this firmware -- verified by capturing the actual
response, not just assuming. IMEI has to be changed the same way as any other
numbered NV item: through `diag_nv_write`'s `setbyte` primitive against
`NV_IMEI_I` (item 550), since that tool has no full-block write mode.

`change_imei.sh` wraps the 8 `setbyte` calls this requires (item 550's 9
meaningful bytes are `[type/length byte][8 BCD-packed bytes]`; byte 0 is left
untouched). It derives this encoding from decoding a real read of the item, not
from a spec:

```bash
# Copy the script and the diag_nv_write binary to the device first, then:
./change_imei.sh /dev/rpmsg_ctrl2 DIAG 861716070514434

# Reboot for the change to take effect in the running QMI stack, then verify
# via a path independent of the diag tool:
reboot
qmicli -d /dev/wwan0qmi0 --dms-get-ids
```

Verified end-to-end on real hardware this way: written, rebooted, and confirmed
the new IMEI both persisted to flash and is live in `qmicli`'s output.

**Portability caveat:** the BCD encoding above was reverse-engineered from *this
specific firmware build's* real NV data, not from Qualcomm documentation. It
matches the classic Qualcomm `NV_IMEI_I` layout used broadly across MSM/Snapdragon
basebands, so it's likely to carry over to other UF896 units on the same
firmware, but hasn't been verified against a different build -- read item 550
and sanity-check the decode against the device's *known* current IMEI before
trusting a write on unfamiliar firmware.

**Legal/ethical note:** only change the IMEI on hardware you own, for legitimate
testing (e.g. probing carrier provisioning behavior, as this tooling was built
for). Changing IMEI to evade a blacklist, impersonate another device, or on
hardware you don't own is illegal in many jurisdictions and a carrier ToS
violation regardless of jurisdiction.

## EFS file-based NV items (`diag_efs`)

Modern Qualcomm platforms gate a lot of config -- notably IMS/VoLTE -- through a
*separate* file-based NV subsystem (`DIAG_SUBSYS_CMD_F` / subsystem `Efs`, protocol
id 19), not the classic numbered items above. Firmware string analysis of this
board's `modem.bin` dump confirmed paths like `/nv/item_files/ims/IMS_enable` and
~70 other `/nv/item_files/ims/*` entries exist in this build. `diag_nv_read`/
`diag_nv_write` cannot touch these; `diag_efs` speaks the separate EFS2 diag
protocol instead.

```bash
./diag_efs /dev/rpmsg_ctrl2 DIAG read    <efs-path>
./diag_efs /dev/rpmsg_ctrl2 DIAG write   <efs-path> <hex-bytes>   # existing file only
./diag_efs /dev/rpmsg_ctrl2 DIAG create  <efs-path> <hex-bytes>   # EFS_O_CREAT|O_RDWR, for a not-yet-provisioned item
./diag_efs /dev/rpmsg_ctrl2 DIAG unlink  <efs-path>                # revert path for anything created
./diag_efs /dev/rpmsg_ctrl2 DIAG listdir <efs-path>                # EfsOpenDir/ReadDir/CloseDir
```

Same safety discipline as `diag_nv_write`: `write` always reads the file first and
prints the original bytes before writing anything.

### Protocol notes (verified against a real device, not just derived from docs)

Byte layout was initially derived from the open-source JohnBel/EfsTools C# client
(not from Qualcomm documentation). Two things in that reference turned out to be
**incomplete** for this firmware, found by testing against the real device and now
handled by `diag_efs` automatically -- worth knowing if you're extending this tool
or porting the protocol elsewhere:

1. **A single `read()` on the rpmsg char device can return more than one complete
   HDLC-framed DIAG packet concatenated together** (and in principle a frame split
   across two reads). `diag_frame_decode()` only understands "one frame, already
   isolated" -- treating each `read()` as exactly one frame silently corrupted
   multi-frame reads (`EFS_READDIR`/`EFS_CLOSEDIR` responses came back with a stray
   leading byte and a stray trailing CRC+trailer glued on). Fixed with a persistent
   byte accumulator (`read_one_frame()` in `diag_efs.c`) that extracts exactly one
   trailer-delimited frame at a time, escape-aware (a `0x7d`-escaped byte pair must
   not be mistaken for the real trailer).
2. **Two different response header shapes coexist on this firmware.**
   `EFS_OPEN`/`EFS_OPENDIR` responses use a plain 4-byte header (the request's
   `cmd`/`subsys`/`subcmd` echoed back verbatim). `EFS_HELLO`/`EFS_READDIR`/
   `EFS_CLOSEDIR` responses are preceded by one extra byte before that same 4-byte
   echo. `efs_xfer()` recognizes both shapes and normalizes to the same
   `resp+4`-relative offsets either way -- if you see `efs_xfer: gave up waiting
   for matching response`, a third variant may exist that isn't handled yet.

### What's been verified end-to-end on the real device

- `EFS_HELLO` handshake succeeds.
- `/nv/item_files/ims` opens successfully as a directory (confirms the EFS layer
  itself is live and reachable).
- `/nv/item_files/ims/IMS_enable` returned `ENOENT` (err=2) on read/open -- it's a
  **lazily-created item that was never provisioned**, not an existing-but-disabled
  one. This is the correct reading of the firmware log string
  `"...isVolteEnabled not able to fetch RCS nv"` -- the *read itself* fails, not
  just the interpreted value.
- Used `create` to provision `/nv/item_files/ims/IMS_enable` = `01`. Verified
  present via a fresh `open`+`read` (fd allocation and a literal `0x01` byte both
  matched expectations), and confirmed it **survives a full device reboot**
  (EFS2 persists to flash, as expected).
- Net result: creating this one item alone did **not** make the `ims` QMI service
  appear, and did not change USSD/SMS behavior. See the main investigation report
  for the full before/after comparison -- this rules out `IMS_enable` as the sole
  gate, but the tooling and methodology are confirmed sound.

# UF896 DIAG/rpmsg tooling

See [`../UF896_VOLTE_USSD_RCS_INVESTIGATION_REPORT.md`](../UF896_VOLTE_USSD_RCS_INVESTIGATION_REPORT.md) for the full
investigation this tooling was built for. This directory just holds the source.

## Build

Cross-compile with this repo's own OpenWrt toolchain (after `./build.sh prepare`):

```bash
GCC=openwrt/staging_dir/toolchain-aarch64_generic_gcc-14.3.0_musl/bin/aarch64-openwrt-linux-musl-gcc
$GCC -static -Wall -O2 -I include -o diag_verno_test3 diag_verno_test3.c
$GCC -static -Wall -O2 -I include -o diag_nv_read     diag_nv_read.c
$GCC -static -Wall -O2 -I include -o diag_nv_sweep    diag_nv_sweep.c
$GCC -static -Wall -O2 -I include -o diag_nv_write    diag_nv_write.c
```

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

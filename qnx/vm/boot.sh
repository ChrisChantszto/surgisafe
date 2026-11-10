#!/bin/bash
# Boots the QNX 8.0 aarch64le QEMU image (built by `make qnx-image`) on a
# Mac, with the console in this terminal.
#
#   qnx/vm/boot.sh               # QEMU software emulation (TCG)
#
# Apple's hvf acceleration does not work yet: hvf only offers GICv3, and QNX's
# startup-qemu-virt asserts on QEMU's hvf GICv3 ("gic_v3.c:966 ... failed").
# ACCEL=hvf is kept so this can be retried with future QEMU/QNX versions.
# Emulation is fine for functional testing, but its timing is NOT
# representative of real hardware.
#
# QNX's own runimage script disables networking on macOS, so this launcher
# uses QEMU user-mode networking with port forwards instead:
#   localhost:2222 -> QNX ssh (22)
#   localhost:8088 -> QNX port 8080 (the SurgiSafe gateway, later)
#
# Quit QEMU with Ctrl-A then X.

set -euo pipefail

VM_DIR="${QNX_VM_DIR:-$HOME/qnx-tools/vm/image}"
OUT="$VM_DIR/output"
SSH_PORT="${SSH_PORT:-2222}"
WEB_PORT="${WEB_PORT:-8088}"
ACCEL="${ACCEL:-tcg}"

for f in "$OUT/ifs.bin" "$OUT/disk-qemu.vmdk"; do
    if [ ! -f "$f" ]; then
        echo "Missing $f - build the image first (make qnx-image)." >&2
        exit 1
    fi
done

# (Tried: -icount shift=auto,sleep=on. It forces single-threaded TCG and made
# wake-up spikes worse, up to ~36 ms, so it is not offered.)
if [ "$ACCEL" = hvf ]; then
    machine=(-machine virt,accel=hvf -cpu host)
else
    # QNX's runimage uses virt-4.2 + cortex-a57 (GICv2); QEMU 11 no longer
    # has the versioned virt-4.2 machine, so pin the GIC version instead.
    machine=(-machine virt,gic-version=2 -cpu cortex-a57)
fi

cd "$OUT"
exec qemu-system-aarch64 \
    "${machine[@]}" -smp 2 -m 1G \
    -drive file=disk-qemu.vmdk,if=none,id=drv0 \
    -device virtio-blk-device,drive=drv0 \
    -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:${SSH_PORT}-:22,hostfwd=tcp:127.0.0.1:${WEB_PORT}-:8080" \
    -device virtio-net-device,netdev=net0 \
    -object rng-random,filename=/dev/urandom,id=rng0 \
    -device virtio-rng-device,rng=rng0 \
    -kernel ifs.bin \
    -nographic

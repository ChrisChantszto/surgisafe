#!/bin/bash
# Builds the QNX 8.0 aarch64le QEMU image with mkqnximage, inside the
# qnx-build container (see qnx/docker/setup.sh).
#
#   qnx/vm/image.sh
#
# Output: ~/qnx-tools/vm/image/output/{ifs.bin,disk-qemu.vmdk}. A dedicated
# SSH key for root login is created in ~/qnx-tools/vm/keys.
#
# Workaround included: the SDP's (auto-patched) OpenSSH needs sshd-auth, but
# the stock mkqnximage template does not ship it, so sshd rejects every
# connection. A local snippet adds it.

set -euo pipefail

VM_ROOT="${QNX_VM_ROOT:-$HOME/qnx-tools/vm}"
IMG="$VM_ROOT/image"
KEYS="$VM_ROOT/keys"
CONTAINER=qnx-build

mkdir -p "$IMG" "$KEYS"
if [ ! -f "$KEYS/vm_key" ]; then
    ssh-keygen -q -t ed25519 -N "" -C "surgisafe-qnx-vm" -f "$KEYS/vm_key"
fi

# The container sees the Mac home directory at /home/<user>.
to_container() { echo "/home/$(id -un)${1#$HOME}"; }

run_mkqnximage() {
    docker exec "$CONTAINER" bash -c \
        "source ~/qnx800/qnxsdp-env.sh > /dev/null && cd '$(to_container "$IMG")' && mkqnximage $*"
}

# First build creates local/snippets; then add the sshd-auth workaround.
if [ ! -d "$IMG/local" ]; then
    run_mkqnximage --type=qemu --arch=aarch64le --hostname=surgisafe \
        "--ssh-ident=$(to_container "$KEYS/vm_key.pub")" --noprompt || true
fi
if ! grep -q sshd-auth "$IMG/local/snippets/system_files.custom"; then
    printf '%s\n' '' '# SurgiSafe-RT: SDP OpenSSH needs sshd-auth (not in the stock template).' \
        'bin/sshd-auth=usr/libexec/sshd-auth' \
        >> "$IMG/local/snippets/system_files.custom"
    printf '%s\n' '' '# SurgiSafe-RT: sshd looks for its auth helper here.' \
        '[type=link] /usr/libexec/sshd-auth=/system/bin/sshd-auth' \
        >> "$IMG/local/snippets/ifs_files.custom"
fi

run_mkqnximage --type=qemu --arch=aarch64le --hostname=surgisafe \
    "--ssh-ident=$(to_container "$KEYS/vm_key.pub")" --build --noprompt
ls -l "$IMG/output/ifs.bin" "$IMG/output/disk-qemu.vmdk"

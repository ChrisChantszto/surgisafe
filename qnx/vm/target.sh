#!/bin/bash
# Helpers for talking to the QNX VM booted by qnx/vm/boot.sh.
#
#   qnx/vm/target.sh ssh [command...]   # shell or one command on QNX
#   qnx/vm/target.sh deploy             # copy build/qnx/* and the scenario
#                                       # runner to /data/surgisafe
#
# Logs in as root with the VM's dedicated key (created by
# qnx/vm/image.sh), via the localhost:2222 port forward.

set -euo pipefail

KEY="${QNX_VM_KEY:-$HOME/qnx-tools/vm/keys/vm_key}"
PORT="${SSH_PORT:-2222}"
DEST=/data/surgisafe
OPTS=(-i "$KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
      -o LogLevel=ERROR -o ConnectTimeout=10)
REPO="$(cd "$(dirname "$0")/../.." && pwd)"

case "${1:-}" in
ssh)
    shift
    exec ssh "${OPTS[@]}" -p "$PORT" root@127.0.0.1 "$@"
    ;;
deploy)
    files=("$REPO"/build/qnx/safety_monitor "$REPO"/build/qnx/controller
           "$REPO"/build/qnx/safety_core_test
           "$REPO"/qnx/vm/scenarios_on_target.sh)
    for f in "${files[@]}"; do
        [ -f "$f" ] || { echo "Missing $f (run make qnx-build)" >&2; exit 1; }
    done
    ssh "${OPTS[@]}" -p "$PORT" root@127.0.0.1 "mkdir -p $DEST"
    scp -q "${OPTS[@]}" -P "$PORT" "${files[@]}" "root@127.0.0.1:$DEST/"
    ssh "${OPTS[@]}" -p "$PORT" root@127.0.0.1 \
        "chmod +x $DEST/*.sh && ls -l $DEST"
    ;;
*)
    echo "Usage: $0 ssh [command...] | deploy" >&2
    exit 2
    ;;
esac

#!/bin/bash
set -e -u

# Run tmux and start QEMU and GDB
HERE="$(dirname "$0")"
tmux \
    new-session  "$HERE/qemu.sh" \; \
    split-window "$HERE/gdb.sh" \; \

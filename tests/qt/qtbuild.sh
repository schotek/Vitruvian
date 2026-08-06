#!/bin/bash
#
# Author: Vláďa Janeček <vlada@janecek.cloud>
# Sync the local qt6-haikuplugins tree into the VM and build src/platform.
# Usage: qtbuild.sh [tail-lines]
set -e
SP="$(cd "$(dirname "$0")" && pwd)"
TAIL="${1:-25}"
# Checkout of the QPA plugin fork (schotek/qt6-haikuplugins, branch vitruvian).
cd "${QHP_SRC:-$HOME/Coding/GitHub/qt6-haikuplugins}"
tar czf "$SP/qhp.tgz" src 3rdparty
python3 "$SP/vm.py" put "$SP/qhp.tgz" /tmp/qhp.tgz
VM_TIMEOUT=300 python3 "$SP/vm.py" run 'rm -rf /root/qhp && mkdir /root/qhp && tar xzf /tmp/qhp.tgz -C /root/qhp && mkdir -p /root/qhp/build && cd /root/qhp/build && qmake6 ../src/platform/platform.pro 2>&1 | tail -3 && make -j4 2>&1 | tail -'"$TAIL"

#!/bin/bash
# Boot two QNX 8.0 x86_64 guests in QEMU on this host and join them with a direct link.
#
#   nodex: SSH on 127.0.0.1:2222, link address 10.9.0.1
#   nodey: SSH on 127.0.0.1:2223, link address 10.9.0.2
# The link is a second virtio NIC on each guest whose frames travel as UDP datagrams between the
# two QEMU processes (netdev dgram). Nothing else is on it, so killing one QEMU is a silent peer,
# the same as pulling a node's power.
#
# Usage: test/twonode-up.sh IMAGES_DIR      (IMAGES_DIR/nodex and IMAGES_DIR/nodey from mkqnximage)
#        test/twonode-up.sh IMAGES_DIR nodey  (boot only that node)
set -eu
IMG=${1:?usage: twonode-up.sh IMAGES_DIR [node]}
ONLY=${2:-}
KEY=${CN_SSH_KEY:?set CN_SSH_KEY to the private key whose public half the images were built with (mkqnximage --ssh-ident)}

boot() {   # boot NAME SSH_PORT MAC_LAN MAC_LINK LINK_LOCAL_PORT LINK_REMOTE_PORT
    local d=$IMG/$1/output
    if [ -f "$d/qemu.pid" ] && kill -0 "$(cat "$d/qemu.pid")" 2>/dev/null; then
        echo "$1 already running (pid $(cat "$d/qemu.pid"))"; return
    fi
    rm -f "$d/serial.log"
    ( cd "$d/.." && qemu-system-x86_64 -cpu max -m 1G -smp 2 -display none -daemonize \
        -serial file:output/serial.log -pidfile output/qemu.pid \
        -kernel output/ifs.bin -drive file=output/disk-qemu.vmdk,if=ide,id=drv0 \
        -netdev user,id=lan,hostfwd=tcp:127.0.0.1:$2-:22 \
        -device virtio-net-pci,netdev=lan,mac=$3 \
        -netdev dgram,id=link,local.type=inet,local.host=127.0.0.1,local.port=$5,remote.type=inet,remote.host=127.0.0.1,remote.port=$6 \
        -device virtio-net-pci,netdev=link,mac=$4 \
        -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0 )
    echo "$1 started (pid $(cat "$d/qemu.pid"))"
}

ready() {  # ready NAME SSH_PORT LINK_ADDRESS
    local d=$IMG/$1/output i
    for i in $(seq 1 120); do grep -q "Startup complete" "$d/serial.log" 2>/dev/null && break; sleep 1; done
    ssh -i "$KEY" -p "$2" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
        -o BatchMode=yes -o ConnectTimeout=20 root@127.0.0.1 \
        "if_up -p -r 20 vtnet1 >/dev/null 2>&1; ifconfig vtnet1 $3/24 up && ifconfig vtnet1 | grep -E 'inet |status'"
}

[ -z "$ONLY" ] || [ "$ONLY" = nodex ] && boot nodex 2222 52:54:00:12:34:56 52:54:00:00:09:01 12001 12002
[ -z "$ONLY" ] || [ "$ONLY" = nodey ] && boot nodey 2223 52:54:00:12:34:57 52:54:00:00:09:02 12002 12001
[ -z "$ONLY" ] || [ "$ONLY" = nodex ] && { echo "nodex:"; ready nodex 2222 10.9.0.1; }
[ -z "$ONLY" ] || [ "$ONLY" = nodey ] && { echo "nodey:"; ready nodey 2223 10.9.0.2; }
exit 0

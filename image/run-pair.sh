#!/bin/bash
# Boot the two Crossnode demo images (built by image/build-image.sh) in QEMU on a direct link.
#   front:   SSH on 127.0.0.1:2224   central: SSH on 127.0.0.1:2225
# Nothing is configured after boot: the images start Crossnode themselves.
# Usage: image/run-pair.sh PROJECTS_DIR            (PROJECTS_DIR/front and PROJECTS_DIR/central)
#        image/run-pair.sh PROJECTS_DIR stop       (stop both, through their pid files)
set -eu
D=${1:?usage: run-pair.sh PROJECTS_DIR [stop]}
if [ "${2:-}" = stop ]; then
    for n in front central; do
        f=$D/$n/output/qemu.pid
        [ -f "$f" ] && kill "$(cat "$f")" 2>/dev/null && echo "$n stopped"
    done
    exit 0
fi
boot() {   # boot NAME SSH_PORT MAC_LAN MAC_LINK LOCAL_PORT REMOTE_PORT
    local o=$D/$1/output
    if [ -f "$o/qemu.pid" ] && kill -0 "$(cat "$o/qemu.pid")" 2>/dev/null; then echo "$1 already running"; return; fi
    rm -f "$o/serial.log"
    ( cd "$o/.." && qemu-system-x86_64 -cpu max -m 1G -smp 2 -display none -daemonize \
        -serial file:output/serial.log -pidfile output/qemu.pid \
        -kernel output/ifs.bin -drive file=output/disk-qemu.vmdk,if=ide,id=drv0 \
        -netdev user,id=lan,hostfwd=tcp:127.0.0.1:$2-:22 -device virtio-net-pci,netdev=lan,mac=$3 \
        -netdev dgram,id=link,local.type=inet,local.host=127.0.0.1,local.port=$5,remote.type=inet,remote.host=127.0.0.1,remote.port=$6 \
        -device virtio-net-pci,netdev=link,mac=$4 \
        -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0 )
    echo "$1 started"
}
boot front   2224 52:54:00:d4:27:a2 52:54:00:00:09:12 12012 12011
boot central 2225 52:54:00:d4:27:a1 52:54:00:00:09:11 12011 12012
for n in front central; do
    for i in $(seq 1 150); do grep -q "Startup complete" "$D/$n/output/serial.log" 2>/dev/null && break; sleep 1; done
    echo "$n: $(grep -c . "$D/$n/output/serial.log" 2>/dev/null) lines of console, $(grep -q 'Startup complete' "$D/$n/output/serial.log" && echo 'startup complete' || echo 'NO startup complete')"
done

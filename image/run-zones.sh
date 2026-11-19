#!/bin/bash
# Boot the three zonal demo images (built by image/build-image.sh with CN_LAYOUT=zones) in QEMU.
#   front:   SSH on 127.0.0.1:2224, one direct link to central (10.9.0.0/24)
#   central: SSH on 127.0.0.1:2225, two direct links: to front (vtnet1) and to rear (vtnet2, 10.10.0.0/24)
#   rear:    SSH on 127.0.0.1:2226, one direct link to central
# Each link is a pair of QEMU NICs whose frames travel as UDP datagrams between two QEMU processes (netdev
# dgram), so killing one QEMU is a silent peer, the same as cutting that zone's power. Nothing is configured
# after boot: the images start Crossnode themselves.
# Usage: image/run-zones.sh PROJECTS_DIR            (PROJECTS_DIR/front, /rear and /central)
#        image/run-zones.sh PROJECTS_DIR NODE       (boot only that node, for example after a power cut)
#        image/run-zones.sh PROJECTS_DIR stop       (stop all three, through their pid files)
set -eu
D=${1:?usage: run-zones.sh PROJECTS_DIR [NODE|stop]}
ONLY=${2:-}
if [ "$ONLY" = stop ]; then
    for n in front rear central; do
        f=$D/$n/output/qemu.pid
        [ -f "$f" ] && kill "$(cat "$f")" 2>/dev/null && echo "$n stopped"
    done
    exit 0
fi
link() {   # link ID MAC LOCAL_PORT REMOTE_PORT
    echo "-netdev dgram,id=$1,local.type=inet,local.host=127.0.0.1,local.port=$3,remote.type=inet,remote.host=127.0.0.1,remote.port=$4 -device virtio-net-pci,netdev=$1,mac=$2"
}
boot() {   # boot NAME SSH_PORT MAC_LAN LINKS...
    local name=$1 port=$2 mac=$3; shift 3
    local o=$D/$name/output
    if [ -f "$o/qemu.pid" ] && kill -0 "$(cat "$o/qemu.pid")" 2>/dev/null; then echo "$name already running"; return; fi
    rm -f "$o/serial.log"
    # shellcheck disable=SC2068
    ( cd "$o/.." && qemu-system-x86_64 -cpu max -m 1G -smp 2 -display none -daemonize \
        -serial file:output/serial.log -pidfile output/qemu.pid \
        -kernel output/ifs.bin -drive file=output/disk-qemu.vmdk,if=ide,id=drv0 \
        -netdev user,id=lan,hostfwd=tcp:127.0.0.1:$port-:22 -device virtio-net-pci,netdev=lan,mac=$mac \
        $@ \
        -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0 )
    echo "$name started"
}
want() { [ -z "$ONLY" ] || [ "$ONLY" = "$1" ]; }
want front   && boot front   2224 52:54:00:d4:27:a2 $(link link 52:54:00:00:09:12 12012 12011)
want rear    && boot rear    2226 52:54:00:d4:27:a3 $(link link 52:54:00:00:09:14 12014 12013)
want central && boot central 2225 52:54:00:d4:27:a1 $(link link1 52:54:00:00:09:11 12011 12012) $(link link2 52:54:00:00:09:13 12013 12014)
for n in front rear central; do
    want $n || continue
    for i in $(seq 1 150); do grep -q "Startup complete" "$D/$n/output/serial.log" 2>/dev/null && break; sleep 1; done
    echo "$n: $(grep -q 'Startup complete' "$D/$n/output/serial.log" && echo 'startup complete' || echo 'NO startup complete')"
done

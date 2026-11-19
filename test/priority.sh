#!/bin/bash
# Crossnode: the caller's priority travels with the request (two QNX 8.0 guests, see twonode-up.sh).
#
# nodey shares /dev/hello. Two CPU hogs at priority 12 then run on nodey for 8 s, one per CPU, so nothing below
# priority 12 runs there until they stop. On nodex, a caller at priority 30 asks for /net/nodey/dev/hello: with the
# priority carried, nodey's worker runs at 30 and answers while the hogs run. A caller at priority 5 asks the same
# thing and must wait for the hogs, which shows the hogs really do hold the CPUs. With CN_MUTATE=1, nodex runs
# crossnode-net-noprio, which does not carry the priority; then check 2 must FAIL.
#
# Usage: test/priority.sh IMAGES_DIR
set -u
IMG=${1:?usage: priority.sh IMAGES_DIR}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$ROOT/build/x86_64
KEY=${CN_SSH_KEY:?set CN_SSH_KEY to the private key whose public half the images were built with (mkqnximage --ssh-ident)}
MUTATE=${CN_MUTATE:-0}
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
NAME=priority; [ "$MUTATE" = 1 ] && NAME=priority-mutation
OUT=$ROOT/evidence/$NAME-$STAMP.txt
mkdir -p "$ROOT/evidence"
exec > >(tee "$OUT") 2>&1

HOG_S=8           # how long the hogs hold nodey's CPUs
FAST_MS=3000      # bound for the whole priority 30 call (open and GET, timed on the host, SSH included)
SLOW_MS=5000      # the priority 5 call must take at least this long: it waits for the hogs

qx() { ssh -i "$KEY" -p 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o BatchMode=yes root@127.0.0.1 "$@"; }
qy() { ssh -i "$KEY" -p 2223 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o BatchMode=yes -o ConnectTimeout=10 root@127.0.0.1 "$@"; }
host_ms() { perl -MTime::HiRes=time -e 'printf "%d\n", time*1000'; }
timed() {   # timed PRIO: one open plus devctl GET from a caller at PRIO on nodex; prints the result and the whole call's ms
    local t0 t1 o; t0=$(host_ms); o=$(qx "on -p $1 /tmp/cn/cnprobe get /net/nodey/dev/hello"); t1=$(host_ms)
    echo "$o total_ms=$((t1 - t0))"
}
NET=crossnode-net; [ "$MUTATE" = 1 ] && NET=crossnode-net-noprio
for f in crossnode-net crossnode-net-noprio crossnode-agent hellodev cnprobe; do
    [ -x "$BIN/$f" ] || { echo "missing $BIN/$f: run make first"; exit 1; }
done
echo "commit: $(git -C "$ROOT" rev-parse --short HEAD)$(git -C "$ROOT" diff --quiet HEAD -- src tools test Makefile || echo ' (with uncommitted changes)')"
echo "nodex runs $NET"
HOSTKEY=$(mktemp); head -c 32 /dev/urandom > "$HOSTKEY"; trap 'rm -f "$HOSTKEY"' EXIT
PASS=0; FAIL=0
check() {
    if [ "$2" -eq 0 ]; then echo "PASS  $1  [$3]"; PASS=$((PASS + 1));
    else echo "FAIL  $1  [$3]"; FAIL=$((FAIL + 1)); fi
}
push() {
    $1 'slay -f -s KILL hellodev crossnode-agent crossnode-net crossnode-net-noprio >/dev/null 2>&1; sleep 1; mkdir -p /tmp/cn; rm -f /tmp/cn/*.log; true'
    for f in hellodev crossnode-agent crossnode-net crossnode-net-noprio cnprobe; do
        $1 "cat > /tmp/cn/$f && chmod +x /tmp/cn/$f" < "$BIN/$f"
    done
    $1 'umask 077; cat > /tmp/cn/key' < "$HOSTKEY"
}
push qy
push qx
# The agent reads each link at priority 30 (-r), so a request is always received; the work then runs at the
# caller's priority (-P, here up to 63).
qy 'cd /tmp/cn && (./hellodev > hellodev.log 2>&1 &) && (./crossnode-agent -v -r 30 -P 63 -k /tmp/cn/key -e /dev/hello -l 10.9.0.2 -p 7777 > agent.log 2>&1 &)'
sleep 1
qx "cd /tmp/cn && (./$NET -k /tmp/cn/key -n nodey -c 10.9.0.2:7777 > net.log 2>&1 &)"
for i in $(seq 1 20); do qx '/tmp/cn/cnprobe get /net/nodey/dev/hello' 2>/dev/null | grep -q rc=EOK && break; sleep 0.5; done

O=$(qx 'on -p 30 /tmp/cn/cnprobe get /net/nodey/dev/hello')
echo "$O" | grep -q 'rc=EOK'
check "1 with nodey idle, a priority 30 call is answered" $? "$(echo "$O" | grep op=get | sed 's/ ms=[0-9]*//')"

hogs() {   # start the hogs on nodey one second from now, so this SSH session ends first
    qy "(sleep 1; for c in 1 2; do on -p 12 sh -c 'e=\$((SECONDS+$HOG_S)); while [ \$SECONDS -lt \$e ]; do :; done' & done) > /dev/null 2>&1 &"
    sleep 2
}
hogs
O=$(timed 30)
MS=$(echo "$O" | sed -n 's/.*total_ms=\([0-9]*\).*/\1/p')
echo "$O" | grep -q 'rc=EOK' && [ -n "$MS" ] && [ "$MS" -le "$FAST_MS" ]
check "2 while priority 12 hogs hold nodey's CPUs, a priority 30 call is answered within ${FAST_MS} ms" $? "rc=$(echo "$O" | sed -n 's/.*rc=\([A-Z]*\).*/\1/p' | head -1), $( [ -n "$MS" ] && [ "$MS" -le "$FAST_MS" ] && echo inside || echo outside) the bound"
sleep $((HOG_S + 2))

hogs
O=$(timed 5)
MS=$(echo "$O" | sed -n 's/.*total_ms=\([0-9]*\).*/\1/p')
echo "$O" | grep -q 'rc=EOK' && [ -n "$MS" ] && [ "$MS" -ge "$SLOW_MS" ]
check "3 control: a priority 5 call waits until the hogs stop (at least ${SLOW_MS} ms), so the hogs do hold the CPUs" $? "rc=$(echo "$O" | sed -n 's/.*rc=\([A-Z]*\).*/\1/p' | head -1), $( [ -n "$MS" ] && [ "$MS" -ge "$SLOW_MS" ] && echo after || echo before) the hogs stopped"
sleep $((HOG_S + 2))

qx 'slay -f -s KILL crossnode-net crossnode-net-noprio >/dev/null 2>&1; true'
qy 'slay -f -s KILL hellodev crossnode-agent >/dev/null 2>&1; true'
echo "== RESULT: $PASS passed, $FAIL failed"

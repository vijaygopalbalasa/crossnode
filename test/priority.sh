#!/bin/bash
# Crossnode: the caller's priority travels with the request (two QNX 8.0 guests, see twonode-up.sh).
#
# nodey shares /dev/hello. Two CPU hogs at priority 12 then run on nodey for 8 s, one per CPU, so nothing below
# priority 12 runs there until they stop. On nodex, a caller at priority 30 asks for /net/nodey/dev/hello: with the
# priority carried, nodey's worker runs at 30 and answers while the hogs run. A caller at priority 5 asks the same
# thing and must wait for the hogs, which shows the hogs really do hold the CPUs. Check 4 puts the hogs on nodex
# instead: crossnode-net there runs its I/O thread at 30 (-r), so the priority 30 caller still gets its answer.
#
# Mutations: CN_MUTATE=1 runs crossnode-net-noprio on nodex, which does not carry the priority: check 2 must FAIL.
# CN_MUTATE=central starts crossnode-net on nodex without -r: check 4 must FAIL.
#
# Ceiling: the network stack (io-sock) runs at priority 21 in these images, so work above 21 on either machine can
# still hold up the link; the hogs here are at 12.
#
# Usage: test/priority.sh IMAGES_DIR
set -u
IMG=${1:?usage: priority.sh IMAGES_DIR}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$ROOT/build/x86_64
KEY=${CN_SSH_KEY:?set CN_SSH_KEY to the private key whose public half the images were built with (mkqnximage --ssh-ident)}
MUTATE=${CN_MUTATE:-0}
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
NAME=priority; [ "$MUTATE" = 1 ] && NAME=priority-mutation; [ "$MUTATE" = central ] && NAME=priority-mutation-central
OUT=$ROOT/evidence/$NAME-$STAMP.txt
mkdir -p "$ROOT/evidence"
exec > >(tee "$OUT") 2>&1

HOG_S=8           # how long the hogs hold nodey's CPUs
FAST_MS=1000      # bound for the whole priority 30 call (open and GET), timed inside the caller's machine
SLOW_MS=3000      # the priority 5 call must take at least this long (three times FAST_MS): it waits for the hogs,
                  # which hold both CPUs for about 6.5 s (the shell counts whole seconds), starting 2 s before the call

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
NETPRIO="-r 30"; [ "$MUTATE" = central ] && NETPRIO=""
echo "nodex starts crossnode-net ${NETPRIO:-without -r}"
qx "cd /tmp/cn && (./$NET $NETPRIO -k /tmp/cn/key -n nodey -c 10.9.0.2:7777 > net.log 2>&1 &)"
for i in $(seq 1 20); do qx '/tmp/cn/cnprobe get /net/nodey/dev/hello' 2>/dev/null | grep -q rc=EOK && break; sleep 0.5; done

O=$(qx 'on -p 30 /tmp/cn/cnprobe get /net/nodey/dev/hello')
echo "$O" | grep -q 'rc=EOK'
check "1 with nodey idle, a priority 30 call is answered" $? "$(echo "$O" | grep op=get | sed 's/ ms=[0-9]*//')"

hogs() {   # start the hogs on nodey one second from now, so this SSH session ends first; a priority 40 shell counts
           # them half a second after they start (SSH cannot run on nodey while they hold both CPUs). Everything is
           # detached from the SSH session, which must end at once: when it ends it takes its children with it.
    qy "rm -f /tmp/cn/hogs.txt; (on -p 40 sh -c 'sleep 1.5; pidin | grep -c \"/sh  *12r\" > /tmp/cn/hogs.txt' > /dev/null 2>&1 < /dev/null &); (sleep 1; for c in 1 2; do on -p 12 sh -c 'e=\$((SECONDS+$HOG_S)); while [ \$SECONDS -lt \$e ]; do :; done' & done) > /dev/null 2>&1 &"
    sleep 2
}
# The network stack's priority on both machines (the ceiling the README states).
echo "io-sock on nodex: $(qx 'pidin -p io-sock' | awk 'NR==2 {print $4}'); on nodey: $(qy 'pidin -p io-sock' | awk 'NR==2 {print $4}')"
in_guest() {   # in_guest PRIO FILE: one open plus GET from a shell at PRIO on nodex, timed there, starting now
    qx "cd /tmp/cn; rm -f $2; (on -p $1 sh -c 'a=\$(./cnprobe now); r=\$(./cnprobe get /net/nodey/dev/hello); b=\$(./cnprobe now); echo \"\$r total_ms=\$((b - a))\" > $2' > /dev/null 2>&1 < /dev/null &)"
}
hogcount() {   # hogcount: the hogs the priority 40 shell saw running (read after they stop)
    qy 'cat /tmp/cn/hogs.txt 2>/dev/null' | tr -d '\r\n '
}
# Checks 2 and 3 start the hogs first (hogs() returns once they run), count them on nodey, then place the call, so
# every call is made while both CPUs are held.
hogs
in_guest 30 c2.txt
sleep 3
O=$(qx 'cat /tmp/cn/c2.txt 2>/dev/null')
sleep $((HOG_S - 3))
H=$(hogcount)
MS=$(echo "$O" | sed -n 's/.*total_ms=\([0-9]*\).*/\1/p')
echo "  hogs running on nodey while the priority 30 call was made: ${H:-none}"
echo "$O" | grep -q 'rc=EOK' && [ -n "$MS" ] && [ "$MS" -le "$FAST_MS" ] && [ "${H:-0}" -ge 2 ]
check "2 while priority 12 hogs hold nodey's CPUs, a priority 30 call is answered within ${FAST_MS} ms" $? "rc=$(echo "$O" | sed -n 's/.*rc=\([A-Z]*\).*/\1/p' | head -1), $( [ -n "$MS" ] && [ "$MS" -le "$FAST_MS" ] && echo inside || echo outside) the bound, ${H:-no} hogs running"
sleep 2

hogs
in_guest 5 c3.txt
sleep $((HOG_S + 1))
O=$(qx 'cat /tmp/cn/c3.txt 2>/dev/null')
H=$(hogcount)
MS=$(echo "$O" | sed -n 's/.*total_ms=\([0-9]*\).*/\1/p')
echo "  hogs running on nodey while the priority 5 call was made: ${H:-none}"
echo "$O" | grep -q 'rc=EOK' && [ -n "$MS" ] && [ "$MS" -ge "$SLOW_MS" ] && [ "${H:-0}" -ge 2 ]
check "3 control: a priority 5 call waits until the hogs stop (at least ${SLOW_MS} ms), so the hogs do hold the CPUs" $? "rc=$(echo "$O" | sed -n 's/.*rc=\([A-Z]*\).*/\1/p' | head -1), $( [ -n "$MS" ] && [ "$MS" -ge "$SLOW_MS" ] && echo after || echo before) the hogs stopped, ${H:-no} hogs running"
sleep 2

# 4: the hogs on nodex, where the caller is. sshd on nodex runs below the hogs, so the whole measurement runs on
# nodex inside one shell started at priority 30, and the host only reads the result afterwards.
qx "cd /tmp/cn; rm -f c4.txt; (on -p 30 sh -c 'sleep 2; a=\$(./cnprobe now); r=\$(./cnprobe get /net/nodey/dev/hello); b=\$(./cnprobe now); echo \"\$r total_ms=\$((b - a))\" > c4.txt' > /dev/null 2>&1 &); (sleep 1; for c in 1 2; do on -p 12 sh -c 'e=\$((SECONDS+$HOG_S)); while [ \$SECONDS -lt \$e ]; do :; done' & done) > /dev/null 2>&1 &"
sleep $((HOG_S + 3))
O=$(qx 'cat /tmp/cn/c4.txt 2>/dev/null')
MS=$(echo "$O" | sed -n 's/.*total_ms=\([0-9]*\).*/\1/p')
echo "$O" | grep -q 'rc=EOK' && [ -n "$MS" ] && [ "$MS" -le "$FAST_MS" ]
check "4 while priority 12 hogs hold nodex's CPUs (the caller's own machine), a priority 30 call is answered within ${FAST_MS} ms" $? "rc=$(echo "$O" | sed -n 's/.*rc=\([A-Z]*\).*/\1/p' | head -1), $( [ -n "$MS" ] && [ "$MS" -le "$FAST_MS" ] && echo inside || echo outside) the bound"
sleep 2

qx 'slay -f -s KILL crossnode-net crossnode-net-noprio >/dev/null 2>&1; true'
qy 'slay -f -s KILL hellodev crossnode-agent >/dev/null 2>&1; true'
echo "== RESULT: $PASS passed, $FAIL failed"

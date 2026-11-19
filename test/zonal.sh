#!/bin/bash
# Zonal demo check: three QNX 8.0 machines (front zone, rear zone, central), booted with no command typed.
# Central reads both zones' simulated sensors by path; the rear zone loses power while central is using it;
# the waiting call fails cleanly within the stated limit, front keeps answering, and rear comes back with no
# restart on central. Images: image/build-image.sh with CN_LAYOUT=zones (optionally confined with CN_SECPOL).
# The sensors are simulations (tools/zonesensor.c), not vehicle interfaces.
# Usage: test/zonal.sh PROJECTS_DIR        (PROJECTS_DIR/front, /rear and /central)
#        CN_MUTATE=1 test/zonal.sh PROJECTS_DIR   a deliberately broken run: front loses power instead of rear, rear
#        is cut later and not booted again, and central's crossnode-net processes are killed before the last check.
#        Checks 9 to 13 must then fail; run it on a policy-recording (open) set, where root may kill the services.
set -u
D=${1:?usage: zonal.sh PROJECTS_DIR}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
KEY=${CN_SSH_KEY:?set CN_SSH_KEY to the private key whose public half the images were built with (mkqnximage --ssh-ident)}
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
MUTATE=${CN_MUTATE:-0}
OUT=$ROOT/evidence/zonal-$([ "$MUTATE" = 1 ] && echo mutation-)$STAMP.txt
exec > >(tee "$OUT") 2>&1
DEAD_MS=2000; SLACK_MS=200          # crossnode-net liveness limit, and the stated allowance on an emulated guest
FRONT=2224; CENTRAL=2225; REAR=2226
ssh_to() { local p=$1; shift; ssh -i "$KEY" -p "$p" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o BatchMode=yes -o ConnectTimeout=20 root@127.0.0.1 "$@"; }
field() { echo "$2" | tr ' ' '\n' | sed -n "s/^$1=//p" | head -1; }
PASS=0; FAIL=0
check() { if [ "$2" -eq 0 ]; then echo "PASS  $1  [$3]"; PASS=$((PASS + 1)); else echo "FAIL  $1  [$3]"; FAIL=$((FAIL + 1)); fi; }
trap '"$ROOT/image/run-zones.sh" "$D" stop >/dev/null 2>&1' EXIT

echo "== Crossnode zonal demo check, $STAMP, build $(cd "$ROOT" && git describe --always --dirty)"
[ "$MUTATE" = 1 ] && echo "== MUTATION RUN (CN_MUTATE=1): front loses power instead of rear, rear is cut later and not booted again, central's crossnode-net is killed. Checks 9 to 13 must FAIL."
"$ROOT/image/run-zones.sh" "$D"
sleep 8
for p in $FRONT $REAR $CENTRAL; do echo "  $(ssh_to $p 'uname -n; uname -r' | tr '\n' ' ')"; done
CONF=$(ssh_to $CENTRAL 'grep -c "^crossnode:" /etc/passwd' 2>/dev/null); CONF=${CONF:-0}
SECURE=$(ssh_to $CENTRAL 'secpol -Q 2>/dev/null | grep -c immutable' 2>/dev/null); SECURE=${SECURE:-0}
echo "  confined services: $([ "$CONF" = 1 ] && echo yes || echo no); security policy enforced: $([ "$SECURE" = 1 ] && echo yes || echo no)"

echo "== 1. Power on, nothing typed: central sees both zones by path"
for i in $(seq 1 10); do ssh_to $CENTRAL 'ls /net/front/dev /net/rear/dev' >/dev/null 2>&1 && break; sleep 2; done
O=$(ssh_to $CENTRAL 'ls /net/front/dev; ls /net/rear/dev' | tr '\n' ' ')
echo "  central# ls /net/front/dev; ls /net/rear/dev -> $O"
[ "$O" = "hello temp radar " ]
check "1 central lists each zone's shared devices, and only those" $? "$O"
F=$(ssh_to $CENTRAL 'cat /net/front/dev/temp'); R1=$(ssh_to $CENTRAL 'cat /net/rear/dev/radar')
echo "  central# cat /net/front/dev/temp -> $F"
echo "  central# cat /net/rear/dev/radar -> $R1"
echo "$F" | grep -q -E '^cabin-temp value=[0-9]+ C t=[0-9]+$' && echo "$R1" | grep -q -E '^rear-radar value=[0-9]+ cm t=[0-9]+$'
check "2 central reads both zones' sensors with cat" $? "$F | $R1"
sleep 2
R2=$(ssh_to $CENTRAL 'cat /net/rear/dev/radar')
echo "  central# cat /net/rear/dev/radar (2 s later) -> $R2"
[ "$(field value "$R1")" != "$(field value "$R2")" ]
check "3 the rear radar's reading moves as central watches it" $? "$(field value "$R1") cm -> $(field value "$R2") cm"

if [ "$CONF" = 1 ]; then
    O=$(ssh_to $CENTRAL 'pidin -f an_U' | awk '/crossnode-net/ {print $3, $4}' | sort | uniq -c | tr -s ' ')
    [ "$(echo "$O" | tr -d '\n')" = " 2 crossnode_net_t 70" ]
    check "4 central's two crossnode-net processes run as uid 70 under crossnode_net_t" $? "$O"
    O="front: $(ssh_to $FRONT 'pidin -f an_U' | awk '/crossnode-agent|hellodev|zonesensor/ {print $3, $4}' | tr '\n' ' '); rear: $(ssh_to $REAR 'pidin -f an_U' | awk '/crossnode-agent|zonesensor/ {print $3, $4}' | tr '\n' ' ')"
    [ "$(echo "$O" | grep -o 'crossnode_agent_[a-z]* 70' | grep -c .)" = 2 ] && [ "$(echo "$O" | grep -o -E '(hellodev_t|zonesensor_t) +71' | grep -c .)" = 3 ]
    check "5 each zone's agent runs as uid 70 and each sensor as uid 71, each under its own security type" $? "$O"
fi
if [ "$SECURE" = 1 ]; then
    O="$(for p in $FRONT $REAR $CENTRAL; do ssh_to $p 'secpol -Q'; done | tr '\n' ' ')"
    [ "$(echo "$O" | grep -o immutable | grep -c .)" = 3 ]
    check "6 all three machines run under a security policy that cannot be changed while they run" $? "$O"
    O=$(ssh_to $CENTRAL 'secpol -c' | grep "^allow_attach crossnode_net_t " | sort | tr '\n' ' ')
    [ "$O" = "allow_attach crossnode_net_t /net/front; allow_attach crossnode_net_t /net/rear; " ]
    check "7 on central, crossnode-net's type may attach exactly /net/front and /net/rear" $? "$O"
fi

K="front $(ssh_to $FRONT 'cksum < /data/etc/crossnode.key') | rear $(ssh_to $REAR 'cksum < /data/etc/crossnode.key') | central-front $(ssh_to $CENTRAL 'cksum < /data/etc/crossnode-front.key') | central-rear $(ssh_to $CENTRAL 'cksum < /data/etc/crossnode-rear.key')"
KF=$(echo "$K" | sed -n 's/^front \([0-9]*\).*/\1/p'); KR=$(echo "$K" | sed -n 's/.*| rear \([0-9]*\) .*/\1/p')
KCF=$(echo "$K" | sed -n 's/.*central-front \([0-9]*\) .*/\1/p'); KCR=$(echo "$K" | sed -n 's/.*central-rear \([0-9]*\) .*/\1/p')
[ -n "$KF" ] && [ -n "$KR" ] && [ "$KF" != "$KR" ] && [ "$KCF" = "$KF" ] && [ "$KCR" = "$KR" ]
check "8 each link has its own key: front's and rear's differ, and central holds each only for its own link" $? "checksums front $KF, rear $KR, central $KCF and $KCR"

echo "== 2. The rear zone loses power while central is watching it"
NETPIDS=$(ssh_to $CENTRAL 'pidin arg' | awk '/crossnode-net/ && !/awk/ {print $1}' | sort | tr '\n' ' ')
ssh_to $CENTRAL 'rm -f /tmp/watch.log; nohup cnprobe watch /net/rear/dev/radar 400 250 > /tmp/watch.log 2>&1 &'
sleep 3
if [ "$MUTATE" = 1 ]; then kill "$(cat "$D/front/output/qemu.pid")"; else kill "$(cat "$D/rear/output/qemu.pid")"; fi
echo "  power cut on $([ "$MUTATE" = 1 ] && echo front || echo rear) at $(date -u +%H:%M:%S) UTC"
sleep 5
W=$(ssh_to $CENTRAL 'cat /tmp/watch.log')
echo "$W" | tail -4 | sed 's/^/  central# cnprobe watch \/net\/rear\/dev\/radar: /'
OK=$(echo "$W" | grep -c "rc=EOK")
LAST=$(echo "$W" | grep "^result=stopped" | tail -1)
MS=$(field call_ms "$LAST")
[ "$OK" -ge 3 ] && [ "$(field rc "$LAST")" = EHOSTDOWN ] && [ "${MS:-99999}" -le $((DEAD_MS + SLACK_MS)) ]
check "9 the live readings stop with EHOSTDOWN within the liveness limit when rear loses power" $? "$OK readings, then $LAST, bound $DEAD_MS+$SLACK_MS ms"
O=$(ssh_to $CENTRAL 'cnprobe watch /net/front/dev/temp 3 100; cat /net/front/dev/hello')
echo "$O" | sed 's/^/  central# (front, rear down) /'
[ "$(echo "$O" | grep -c "op=watch.*rc=EOK")" = 3 ] && echo "$O" | grep -q "hello from front"
check "10 the front zone keeps answering while rear is down" $? "$(echo "$O" | grep -c "rc=EOK") readings and hello"
O=$(ssh_to $CENTRAL 'cnprobe open /net/rear/dev/radar')
[ "$(field rc "$O")" = EHOSTDOWN ]
check "11 a new open on the dead zone fails cleanly instead of hanging" $? "$O"

echo "== 3. The rear zone boots again"
if [ "$MUTATE" = 1 ]; then kill "$(cat "$D/rear/output/qemu.pid")"; else "$ROOT/image/run-zones.sh" "$D" rear; fi
O=""
for i in $(seq 1 $([ "$MUTATE" = 1 ] && echo 3 || echo 30)); do O=$(ssh_to $CENTRAL 'cnprobe watch /net/rear/dev/radar 1 0' 2>/dev/null); echo "$O" | grep -q "rc=EOK" && break; sleep 2; done
R3=$(ssh_to $CENTRAL 'cat /net/rear/dev/radar')
echo "  central# cnprobe watch /net/rear/dev/radar 1 0 -> $O"
echo "  central# cat /net/rear/dev/radar -> $R3"
echo "$O" | grep -q "rc=EOK" && echo "$R3" | grep -q "^rear-radar value="
check "12 central reads the rear radar again after rear reboots" $? "$R3"
[ "$MUTATE" = 1 ] && ssh_to $CENTRAL 'slay -f crossnode-net; sleep 1; true'
NOW=$(ssh_to $CENTRAL 'pidin arg' | awk '/crossnode-net/ && !/awk/ {print $1}' | sort | tr '\n' ' ')
[ -n "$NETPIDS" ] && [ "$NOW" = "$NETPIDS" ]
check "13 nothing was restarted on central: the same crossnode-net processes serve both zones" $? "pids before: $NETPIDS after: $NOW"
echo
echo "== RESULT: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]

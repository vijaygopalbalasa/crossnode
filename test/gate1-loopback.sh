#!/bin/bash
# Crossnode gate 1, part 1: one QNX 8.0 guest, its peer reached over loopback.
#
# Re-runnable evidence. Pushes the build to a booted guest, starts the test device, the agent and
# /net/self, runs every check, and saves the transcript under evidence/.
#
# Usage: test/gate1-loopback.sh [SSH_PORT]     (guest reachable as root on 127.0.0.1:SSH_PORT)
# Env:   CN_SSH_KEY  private key for the guest (default ~/qnx-work/home/.ssh/qnx_gate1)
set -u
PORT=${1:-2222}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$ROOT/build/x86_64
KEY=${CN_SSH_KEY:-$HOME/qnx-work/home/.ssh/qnx_gate1}
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
OUT=$ROOT/evidence/gate1-loopback-$STAMP.txt
mkdir -p "$ROOT/evidence"
exec > >(tee "$OUT") 2>&1

DEAD_MS=2000      # crossnode-net liveness limit (CN_DEAD_MS)
SLACK_MS=1000     # allowance for an emulated guest

q() { ssh -i "$KEY" -p "$PORT" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
          -o LogLevel=ERROR -o BatchMode=yes root@127.0.0.1 "$@"; }
field() { echo "$2" | tr ' ' '\n' | sed -n "s/^$1=//p" | head -1; }
PASS=0
FAIL=0
check() {                       # check NAME STATUS DETAIL
    if [ "$2" -eq 0 ]; then echo "PASS  $1  [$3]"; PASS=$((PASS + 1));
    else echo "FAIL  $1  [$3]"; FAIL=$((FAIL + 1)); fi
}
le() { [ -n "$1" ] && [ "$1" -le "$2" ]; }    # le VALUE LIMIT, false if VALUE is empty

AGENT='cd /tmp/cn && (./crossnode-agent -v -e /dev/hello -e /dev/ser1 -e /tmp/cnx -l 127.0.0.1 -p 7777 >> agent.log 2>&1 &)'
P=/net/self/dev/hello

echo "== Crossnode gate 1, loopback, $STAMP"
echo "guest: $(q 'uname -a')"
echo "build: $(cd "$ROOT" && git describe --always --dirty 2>/dev/null || echo uncommitted)"
for f in hellodev crossnode-agent crossnode-net cnprobe; do
    echo "  $f sha256 $(shasum -a 256 "$BIN/$f" | cut -c1-16)"
done
q 'slay -f -s KILL hellodev crossnode-agent crossnode-net >/dev/null 2>&1; mkdir -p /tmp/cn; rm -f /tmp/cn/*.log /tmp/cn/*.txt; true'
for f in hellodev crossnode-agent crossnode-net cnprobe; do
    q "cat > /tmp/cn/$f && chmod +x /tmp/cn/$f" < "$BIN/$f"
done
q 'rm -rf /tmp/cnx && mkdir /tmp/cnx && echo "plain file inside the share" > /tmp/cnx/ok && ln -s /tmp/cnx/ok /tmp/cnx/inside && ln -s /etc/passwd /tmp/cnx/escape && ln -s /etc/no-such-file /tmp/cnx/probe'
# Root, plus group 1000 (qnxuser), may use /net/self.
q 'cd /tmp/cn && (./hellodev > hellodev.log 2>&1 &) && (./crossnode-net -v -g 1000 -n self -c 127.0.0.1:7777 > net.log 2>&1 &)'
q "$AGENT"
sleep 1

echo
echo "== 1. Same answers locally and through /net/self"
L=$(q 'cd /tmp/cn; cat /dev/hello; for a in "set 41" "add 1" "get"; do ./cnprobe ${a%% *} /dev/hello ${a#* } | cut -d" " -f1-4; done')
R=$(q "cd /tmp/cn; cat $P; for a in \"set 41\" \"add 1\" \"get\"; do ./cnprobe \${a%% *} $P \${a#* } | cut -d' ' -f1-4; done")
echo "local:";  echo "$L" | sed 's/^/  /'
echo "remote:"; echo "$R" | sed 's/^/  /'
[ -n "$L" ] && [ "$L" = "$R" ]
check "1 cat and devctl set/add/get give identical results, info value included" $? "compared $(echo "$L" | wc -l | tr -d ' ') lines"

echo
echo "== 2. A stock QNX tool (stty) through /net/self"
q 'cd /tmp/cn; stty -a < /dev/ser1 > stty-local.txt 2>&1; stty -a < /net/self/dev/ser1 > stty-net.txt 2>&1'
S=$(q 'cd /tmp/cn; cmp stty-local.txt stty-net.txt >/dev/null && echo same; wc -l < stty-net.txt')
echo "$S" | sed 's/^/  /'
echo "$S" | grep -q '^same$'
check "2 stty -a on /net/self/dev/ser1 matches /dev/ser1 byte for byte" $? "$(echo "$S" | tail -1 | tr -d ' ') lines"

echo
echo "== 3. A path outside the share list is refused"
E=$(q "cat /net/self/etc/passwd 2>&1; true")
echo "  $E"
echo "$E" | grep -q 'Permission denied'
check "3 /net/self/etc/passwd is refused" $? "$E"
E=$(q "cat /net/self/tmp/cnx/inside 2>&1; true")
[ "$E" = "plain file inside the share" ]
check "3b a link that stays inside the share is followed" $? "$E"
E=$(q "cat /net/self/tmp/cnx/escape 2>&1; true")
echo "$E" | grep -q 'Permission denied'
check "3c a link that leads out of the share is refused" $? "$E"
E=$(q "cat /net/self/tmp/cnx/missing 2>&1; true")
echo "$E" | grep -q 'No such file'
check "3d a missing name inside the share says so" $? "$E"
E=$(q "cat /net/self/tmp/cnx/probe 2>&1; true")
echo "$E" | grep -q 'Permission denied'
check "3e a link to a missing name outside the share is refused, not reported missing" $? "$E"

echo
echo "== 4. A waiting devctl when the peer is killed"
O=$(q "cd /tmp/cn; ./cnprobe watch $P 600 50 > w4.txt 2>&1 & W=\$!; sleep 1; slay -s STOP crossnode-agent; sleep 1; K=\$(./cnprobe now); slay -s KILL crossnode-agent; wait \$W; echo kill_at=\$K; tail -1 w4.txt")
echo "$O" | sed 's/^/  /'
RC=$(field rc "$O"); TE=$(field t_end "$O"); K=$(field kill_at "$O")
D=$(( ${TE:-0} - ${K:-0} ))
[ "$RC" = EHOSTDOWN ] && le "$D" $SLACK_MS
check "4 the waiting call returns EHOSTDOWN once the peer is killed" $? "rc=$RC, ${D} ms after the kill"

echo
echo "== 5. A waiting devctl when the peer freezes and stays silent"
q "$AGENT"; sleep 1
O=$(q "cd /tmp/cn; ./cnprobe watch $P 600 50 > w5.txt 2>&1 & W=\$!; sleep 1; S=\$(./cnprobe now); slay -s STOP crossnode-agent; wait \$W; echo stop_at=\$S; tail -1 w5.txt")
echo "$O" | sed 's/^/  /'
RC=$(field rc "$O"); TE=$(field t_end "$O"); S=$(field stop_at "$O")
D=$(( ${TE:-0} - ${S:-0} ))
[ "$RC" = EHOSTDOWN ] && le "$D" $((DEAD_MS + SLACK_MS))
check "5 the waiting call returns EHOSTDOWN within the liveness limit" $? "rc=$RC, ${D} ms after the freeze, limit ${DEAD_MS} ms"

echo
echo "== 6. A new open while the peer is still frozen"
O=$(q "cd /tmp/cn; sleep 2; ./cnprobe open $P; ./cnprobe open $P")
echo "$O" | sed 's/^/  /'
FIRST=$(echo "$O" | sed -n 1p); SECOND=$(echo "$O" | sed -n 2p)
[ "$(field rc "$FIRST")" = EHOSTDOWN ] && le "$(field ms "$FIRST")" $((DEAD_MS + SLACK_MS))
check "6a the open fails with EHOSTDOWN within the liveness limit" $? "$FIRST"
[ "$(field rc "$SECOND")" = EHOSTDOWN ] && le "$(field ms "$SECOND")" 100
check "6b a second open right after fails at once" $? "$SECOND"

echo
echo "== 7. A new open after the peer is gone"
O=$(q "cd /tmp/cn; slay -s KILL crossnode-agent; sleep 2; ./cnprobe open $P")
echo "  $O"
[ "$(field rc "$O")" = EHOSTDOWN ] && le "$(field ms "$O")" 1600
check "7 the open fails with EHOSTDOWN within the connect limit" $? "$O"

echo
echo "== 8. Using and closing an fd whose peer died"
q "$AGENT"; sleep 1
O=$(q "cd /tmp/cn; ./cnprobe stale $P 2000 > s8.txt 2>&1 & W=\$!; sleep 1; slay -s KILL crossnode-agent; wait \$W; cat s8.txt")
echo "$O" | sed 's/^/  /'
BEFORE=$(echo "$O" | grep get-before); AFTER=$(echo "$O" | grep get-after); CL=$(echo "$O" | grep op=close)
[ "$(field rc "$BEFORE")" = EOK ] && [ "$(field rc "$AFTER")" = EHOSTDOWN ] && le "$(field ms "$AFTER")" 100
check "8a a call on the old fd fails at once with EHOSTDOWN" $? "$AFTER"
[ "$(field rc "$CL")" = EOK ] && le "$(field ms "$CL")" 100
check "8b close returns at once" $? "$CL"

echo
echo "== 9. A process exits while holding an fd whose peer died"
q "$AGENT"; sleep 1
O=$(q "cd /tmp/cn; ./cnprobe exit $P 2000 > x9.txt 2>&1 & W=\$!; sleep 1; slay -s KILL crossnode-agent; wait \$W; cat x9.txt")
echo "  $O"
[ "$(field opened "$O")" = y ] && le "$(field ms "$O")" 200
check "9 the process exits at once" $? "$O"

echo
echo "== 10. A signal to a waiting caller"
q "$AGENT"; sleep 1
O=$(q "cd /tmp/cn; ./cnprobe eintr $P 300 1500 > e10.txt 2>&1 & W=\$!; sleep 1; slay -s STOP crossnode-agent; wait \$W; slay -s CONT crossnode-agent; cat e10.txt")
echo "  $O"
[ "$(field rc "$O")" = EINTR ] && le "$(field ms "$O")" 1000
check "10 the call returns EINTR when the signal arrives, not when the peer answers" $? "$O"

echo
echo "== 11. The path recovers when the peer comes back"
q 'slay -f -s KILL crossnode-agent >/dev/null 2>&1; true'
q "$AGENT"; sleep 2
O=$(q "cd /tmp/cn; ./cnprobe get $P")
echo "  $O"
[ "$(field rc "$O")" = EOK ] && [ "$(field info "$O")" = 7 ]
check "11 a fresh call works again" $? "$O"

echo
echo "== 12. Only root and the allowed group may use /net/self"
A=$(q "cd /tmp/cn; on -u qnxuser ./cnprobe get $P")
B=$(q "cd /tmp/cn; on -u user1 ./cnprobe get $P")
M=$(q "ls -ld /net/self | cut -c1-10")
echo "  qnxuser (group 1000): $A"; echo "  user1 (group 1001): $B"; echo "  /net/self mode: $M"
[ "$(field rc "$A")" = EOK ]
check "12a a member of the allowed group may use the path" $? "$A"
[ "$(field rc "$B")" = EACCES ]
check "12b any other user is refused before anything is forwarded" $? "$B"

echo
echo "== crossnode-net log"
q 'cat /tmp/cn/net.log' | sed 's/^/  /'
q 'slay -f -s KILL hellodev crossnode-agent crossnode-net >/dev/null 2>&1; true'

echo
echo "== RESULT: $PASS passed, $FAIL failed"
echo "transcript: evidence/$(basename "$OUT")"
[ "$FAIL" -eq 0 ]

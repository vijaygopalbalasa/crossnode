#!/bin/bash
# Crossnode gate 1, part 1: one QNX 8.0 guest, its peer reached over loopback.
#
# Re-runnable evidence. Pushes the build to a booted guest, starts the test device, the agent and
# /net/self, runs every check, and saves the transcript under evidence/.
#
# Usage: test/gate1-loopback.sh [SSH_PORT]     (guest reachable as root on 127.0.0.1:SSH_PORT)
# Env:   CN_SSH_KEY  private key for the guest (required)
set -u
PORT=${1:-2222}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$ROOT/build/x86_64
KEY=${CN_SSH_KEY:?set CN_SSH_KEY to the private key whose public half the images were built with (mkqnximage --ssh-ident)}
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
OUT=$ROOT/evidence/gate1-loopback-$([ "${CN_MUTATE:-0}" = 1 ] && echo mutation-)$STAMP.txt
mkdir -p "$ROOT/evidence"
exec > >(tee "$OUT") 2>&1

DEAD_MS=2000      # crossnode-net liveness limit (CN_DEAD_MS)
CONNECT_MS=1500   # crossnode-net connect limit (CN_CONNECT_MS)
SLACK_MS=200      # stated allowance on top of a limit: process start and scheduling on an emulated guest

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

# Refuse to produce evidence from a stale build: each binary must be newer than its own sources.
stale() { local b=$1; shift; for f in "$@"; do [ "$ROOT/$f" -nt "$BIN/$b" ] && return 0; done; return 1; }
if [ ! -x "$BIN/crossnode-net" ] || stale crossnode-net src/cn_net.c src/cn_proto.h src/cn_auth.h || stale crossnode-agent src/cn_agent.c src/cn_proto.h \
   || stale hellodev tools/hellodev.c tools/hello_dcmd.h || stale cnprobe tools/cnprobe.c tools/hello_dcmd.h src/cn_proto.h \
   || stale cnmitm tools/cnmitm.c src/cn_proto.h || stale crossnode-net-noverify src/cn_net.c src/cn_proto.h src/cn_auth.h; then
    echo "STALE BUILD: a binary in build/x86_64 is older than its sources; run make first"; exit 1
fi

AGENT='cd /tmp/cn && (./crossnode-agent -v -k /tmp/cn/key -e /dev/hello -e /dev/ser1 -e /tmp/cnx -l 127.0.0.1 -p 7777 >> agent.log 2>&1 &)'
P=/net/self/dev/hello

echo "== Crossnode gate 1, loopback, $STAMP"
echo "guest: $(q 'uname -a')"
echo "build: $(cd "$ROOT" && git describe --always --dirty 2>/dev/null || echo uncommitted)"
# CN_MUTATE=1 runs the whole suite with a crossnode-net built to skip the per-frame check: 25a and 25b must fail.
NETBIN=crossnode-net; [ "${CN_MUTATE:-0}" = 1 ] && NETBIN=crossnode-net-noverify
[ "${CN_MUTATE:-0}" = 1 ] && echo "== MUTATION RUN (CN_MUTATE=1): crossnode-net is the build that skips the per-frame check; 25a and 25b must FAIL."
for f in hellodev crossnode-agent $NETBIN cnprobe cnmitm; do
    echo "  $f sha256 $(shasum -a 256 "$BIN/$f" | cut -c1-16)"
done
q 'slay -f -s KILL hellodev crossnode-agent crossnode-net >/dev/null 2>&1; mkdir -p /tmp/cn; rm -f /tmp/cn/*.log /tmp/cn/*.txt; true'
for f in hellodev crossnode-agent cnprobe cnmitm; do
    q "cat > /tmp/cn/$f && chmod +x /tmp/cn/$f" < "$BIN/$f"
done
q "cat > /tmp/cn/crossnode-net && chmod +x /tmp/cn/crossnode-net" < "$BIN/$NETBIN"
q 'umask 077; rm -f /tmp/cn/key /tmp/cn/key2; head -c 32 /dev/urandom > /tmp/cn/key; head -c 32 /dev/urandom > /tmp/cn/key2; ls -l /tmp/cn/key | cut -c1-10'
q 'rm -rf /tmp/cnx && mkdir /tmp/cnx && echo "plain file inside the share" > /tmp/cnx/ok && ln -s /tmp/cnx/ok /tmp/cnx/inside && ln -s /etc/passwd /tmp/cnx/escape && ln -s /etc/no-such-file /tmp/cnx/probe'
# Root, plus group 1000 (qnxuser), may use /net/self.
q 'cd /tmp/cn && (./hellodev > hellodev.log 2>&1 &) && (./crossnode-net -v -k /tmp/cn/key -g 1000 -n self -c 127.0.0.1:7777 > net.log 2>&1 &)'
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
check "4 the waiting call returns EHOSTDOWN once the peer is killed" $? "rc=$RC, ${D} ms after the kill, bound ${SLACK_MS} ms"

echo
echo "== 5. A waiting devctl when the peer freezes and stays silent"
q "$AGENT"; sleep 1
O=$(q "cd /tmp/cn; ./cnprobe watch $P 600 50 > w5.txt 2>&1 & W=\$!; sleep 1; S=\$(./cnprobe now); slay -s STOP crossnode-agent; wait \$W; echo stop_at=\$S; tail -1 w5.txt")
echo "$O" | sed 's/^/  /'
RC=$(field rc "$O"); TE=$(field t_end "$O"); S=$(field stop_at "$O")
D=$(( ${TE:-0} - ${S:-0} ))
[ "$RC" = EHOSTDOWN ] && le "$D" $((DEAD_MS + SLACK_MS))
check "5 the waiting call returns EHOSTDOWN within the liveness limit" $? "rc=$RC, ${D} ms after the freeze, bound ${DEAD_MS}+${SLACK_MS} ms"

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
[ "$(field rc "$O")" = EHOSTDOWN ] && le "$(field ms "$O")" $SLACK_MS
check "7 the open fails with EHOSTDOWN at once (the port is closed)" $? "$O"

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
[ "$(field rc "$O")" = EINTR ] && le "$(field ms "$O")" $((300 + SLACK_MS))
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
echo "== 13. lseek is refused, not faked"
O=$(q "cd /tmp/cn; ./cnprobe seek $P")
echo "  $O"
[ "$(field rc "$O")" = ESPIPE ]
check "13 lseek on a /net fd answers ESPIPE instead of moving a local copy of the offset" $? "$O"

echo
echo "== 14. A signal during a waiting open"
O=$(q "cd /tmp/cn; ./cnprobe get $P >/dev/null; slay -s STOP crossnode-agent; ./cnprobe eintr-open $P 300; slay -s CONT crossnode-agent")
echo "  $O"
[ "$(field rc "$O")" = EINTR ] && le "$(field ms "$O")" $((300 + SLACK_MS))
check "14a the open returns EINTR when the signal arrives" $? "$O"
O=$(q "cd /tmp/cn; for i in 1 2 3 4 5 6 7 8 9 10; do ./cnprobe get $P >/dev/null; slay -s STOP crossnode-agent; ./cnprobe eintr-open $P 100 | cut -d' ' -f2; slay -s CONT crossnode-agent; done | sort | uniq -c; ./cnprobe get $P; grep -c 'unbind after' net.log; true")
echo "$O" | sed 's/^/  /'
[ "$(field rc "$(echo "$O" | grep op=get)")" = EOK ] && echo "$O" | grep -q '10 rc=EINTR' && [ "$(echo "$O" | tail -1)" = 0 ]
check "14b ten more interrupted opens: all EINTR, no unbind error, the path still works" $? "$(echo "$O" | grep op=get)"

echo
echo "== 15. Transfers larger than one network packet"
q 'dd if=/dev/urandom of=/tmp/cnx/big bs=1024 count=200 2>/dev/null; : > /tmp/cnx/w'
O=$(q 'cksum < /tmp/cnx/big; cksum < /net/self/tmp/cnx/big; cat /tmp/cnx/big > /net/self/tmp/cnx/w; cksum < /tmp/cnx/w')
echo "$O" | sed 's/^/  /'
A1=$(echo "$O" | sed -n 1p); A2=$(echo "$O" | sed -n 2p); A3=$(echo "$O" | sed -n 3p)
[ -n "$A1" ] && [ "$A1" = "$A2" ]
check "15a a 200 KB read through /net matches the file" $? "$A2"
[ -n "$A1" ] && [ "$A1" = "$A3" ]
check "15b a 200 KB write through /net lands intact" $? "$A3"

echo
echo "== 16. A peer that is alive but slow: a read that waits for input"
N0=$(q "cd /tmp/cn; ./cnprobe get $P >/dev/null; pidin -P crossnode-agent | wc -l" | tr -d ' ')
O=$(q "cd /tmp/cn; cat /net/self/dev/ser1 > /dev/null 2>&1 & C=\$!; sleep 10; kill -0 \$C && echo still_waiting=yes; ./cnprobe get $P; N1=\$(pidin -P crossnode-agent | wc -l); T0=\$(./cnprobe now); kill -INT \$C; wait \$C; T1=\$(./cnprobe now); echo threads_while_waiting=\$N1 interrupt_ms=\$((T1 - T0)); sleep 1; echo threads_after=\$(pidin -P crossnode-agent | wc -l)")
echo "$O" | sed 's/^/  /'
echo "  threads_before=$N0 (pidin lines, header included)"
echo "$O" | grep -q still_waiting=yes
check "16a the read stays blocked for 10 s while the peer is alive" $? "$(echo "$O" | grep -c still_waiting) of 1"
[ "$(field rc "$(echo "$O" | grep op=get)")" = EOK ]
check "16b another caller is served meanwhile" $? "$(echo "$O" | grep op=get)"
IM=$(field interrupt_ms "$O"); WT=$(field threads_while_waiting "$O" | tr -d ' '); AT=$(field threads_after "$O" | tr -d ' ')
le "$IM" $SLACK_MS
check "16c Ctrl-C on the waiting reader returns at once" $? "${IM} ms"
[ -n "$WT" ] && [ "$WT" -gt "$N0" ] && [ "$AT" = "$N0" ]
check "16d the agent's blocked worker ends when the caller gives up" $? "threads $N0 -> $WT -> $AT"
O=$(q "cd /tmp/cn; for i in 1 2 3 4; do cat /net/self/dev/ser1 > /dev/null 2>&1 & sleep 1; slay -f -s KILL crossnode-net; (./crossnode-net -v -k /tmp/cn/key -g 1000 -n self -c 127.0.0.1:7777 >> net.log 2>&1 &); sleep 1; done; ./cnprobe get $P; echo threads_after_cycles=\$(pidin -P crossnode-agent | wc -l)")
echo "$O" | sed 's/^/  /'
[ "$(field rc "$(echo "$O" | grep op=get)")" = EOK ] && [ "$(field threads_after_cycles "$O" | tr -d ' ')" = "$N0" ]
check "16e four client restarts during blocked reads leave no stuck worker or connection" $? "$(echo "$O" | tail -1 | tr -d ' ')"

echo
echo "== 17. Connection caps"
O=$(q "cd /tmp/cn; ./cnprobe get $P >/dev/null; ./cnprobe conns 127.0.0.1 7777 6; grep -c 'too many from that address' agent.log; true")
echo "$O" | sed 's/^/  /'
[ "$(field kept "$O")" = 2 ]
check "17 one address holds at most 2 connections in the handshake (6 probes: the older ones are dropped for newer ones, 2 stay)" $? "$(echo "$O" | head -1)"

echo
echo "== 18. Replies that break the protocol are refused"
python3 "$ROOT/test/fake-agent.py" 7799 > "$ROOT/evidence/.fake-agent.log" 2>&1 &
FAKE=$!
sleep 1
q 'cd /tmp/cn && (./crossnode-net -v -I -n evil -c 10.0.2.2:7799 > evil.log 2>&1 &)'
sleep 1
O=$(q "cd /tmp/cn; ./cnprobe readn /net/evil/x 10; sleep 1.2; ./cnprobe get /net/evil/x; sleep 2; ./cnprobe stat /net/evil/x; sleep 2; ./cnprobe ls /net/evil/dir; sleep 1")
kill $FAKE 2>/dev/null
echo "$O" | sed 's/^/  /'
echo "  fake agent:"; sed 's/^/    /' "$ROOT/evidence/.fake-agent.log"; rm -f "$ROOT/evidence/.fake-agent.log"
EV=$(q 'cat /tmp/cn/evil.log; slay -f -s KILL crossnode-net >/dev/null 2>&1; true')
echo "$EV" | sed 's/^/  evil.log: /'
R1=$(echo "$O" | grep op=readn); R2=$(echo "$O" | grep op=get)
[ "$(field rc "$R1")" = EHOSTDOWN ] && [ "$(echo "$EV" | grep -c 'did not match')" -ge 2 ]
check "18a an oversized read reply never reaches the program" $? "$R1"
[ "$(field rc "$R2")" = EHOSTDOWN ]
check "18b a reply with the wrong operation is refused" $? "$R2"
R3=$(echo "$O" | grep 'op=stat'); R4=$(echo "$O" | grep 'op=ls')
[ "$(field rc "$R3")" = EHOSTDOWN ] && [ "$(echo "$EV" | grep -c 'did not match')" -ge 4 ]
check "18c a stat reply with an impossible file type never reaches the program" $? "$R3"
[ "$(field rc "$R4")" = EHOSTDOWN ] && [ "$(echo "$O" | grep -c '^ent=')" = 0 ]
check "18d a directory entry whose name contains a slash never reaches the program" $? "$R4"

echo
echo "== 19. stat() and fstat() answer from the peer, through the same containment as open"
q "cd /tmp/cn; $AGENT; (./crossnode-net -v -k /tmp/cn/key -g 1000 -n self -c 127.0.0.1:7777 >> net.log 2>&1 &)"; sleep 2
q 'rm -rf /tmp/cnx/d /tmp/cnx/many; mkdir /tmp/cnx/d /tmp/cnx/many; echo aaa > /tmp/cnx/d/a; echo bbbbbb > /tmp/cnx/d/b; : > /tmp/cnx/d/c; i=0; while [ $i -lt 300 ]; do : > /tmp/cnx/many/f$i; i=$((i + 1)); done; echo fixtures-ready'
same_fields() { for k in mode size uid gid nlink mtime; do [ "$(field $k "$1")" = "$(field $k "$2")" ] || return 1; done; [ -n "$(field mode "$1")" ]; }
L=$(q 'cd /tmp/cn; ./cnprobe stat /tmp/cnx/d/b'); R=$(q 'cd /tmp/cn; ./cnprobe stat /net/self/tmp/cnx/d/b')
echo "  local:  $L"; echo "  remote: $R"
same_fields "$L" "$R"
check "19a stat of a file through /net matches the file's own stat (mode size uid gid nlink mtime)" $? "$R"
L=$(q 'cd /tmp/cn; ./cnprobe stat /dev/hello'); R=$(q 'cd /tmp/cn; ./cnprobe stat /net/self/dev/hello')
echo "  local:  $L"; echo "  remote: $R"
same_fields "$L" "$R"
check "19b stat of the remote device matches the device's own stat" $? "$R"
L=$(q 'cd /tmp/cn; ./cnprobe fstat /tmp/cnx/d/b'); R=$(q 'cd /tmp/cn; ./cnprobe fstat /net/self/tmp/cnx/d/b')
echo "  local:  $L"; echo "  remote: $R"
same_fields "$L" "$R"
check "19c fstat on an open /net descriptor matches" $? "$R"
L=$(q 'cd /tmp/cn; ./cnprobe stat /tmp/cnx/d'); R=$(q 'cd /tmp/cn; ./cnprobe stat /net/self/tmp/cnx/d')
echo "  local:  $L"; echo "  remote: $R"
same_fields "$L" "$R"
check "19d stat of a directory inside the share matches" $? "$R"
V=$(q 'cd /tmp/cn; ./cnprobe stat /net/self/dev; ./cnprobe stat /net/self/tmp')
echo "$V" | sed 's/^/  virtual: /'
[ "$(echo "$V" | wc -l | tr -d ' ')" = 2 ] && ! echo "$V" | grep -v 'mode=40[0-7]*[0-7] ' | grep -q 'mode='
check "19e a directory above a share is a read-only directory (mode 040xxx, no write bits)" $? "$(echo "$V" | head -1 | cut -d' ' -f1-3)"
echo "$V" | cut -d' ' -f3 | grep -q 'mode=40[0-7][0-5][0-5]'
check "19e2 and it grants no write permission" $? "$(echo "$V" | head -1 | cut -d' ' -f3)"
for t in etc/passwd tmp/cnx/escape tmp/cnx/probe; do
    O=$(q "cd /tmp/cn; ./cnprobe stat /net/self/$t")
    [ "$(field rc "$O")" = EACCES ]
    check "19f stat of /net/self/$t is refused" $? "$O"
done
O=$(q 'cd /tmp/cn; ./cnprobe stat /net/self/tmp/cnx/missing')
[ "$(field rc "$O")" = ENOENT ]
check "19g stat of a missing name inside the share says ENOENT" $? "$O"
M=$(q 'cd /tmp/cn; ./cnprobe stat /net/self')
echo "  mount root: $M"
[ "$(field mode "$M")" = 40750 ]
check "19h the mount root's own stat stays local and shows the access policy (group allowed: 40750)" $? "$M"

echo
echo "== 20. Directory listing"
lst() { echo "$1" | sed -n 's/^ent=//p' | sort | tr '\n' ' '; }
O=$(q 'cd /tmp/cn; ./cnprobe ls /net/self')
echo "  /net/self: $(lst "$O")"
[ "$(lst "$O")" = ". .. dev tmp " ]
check "20a the mount root lists only the top of the share list" $? "$(lst "$O")"
O=$(q 'cd /tmp/cn; ./cnprobe ls /net/self/dev')
echo "  /net/self/dev: $(lst "$O")"
[ "$(lst "$O")" = ". .. hello ser1 " ]
check "20b a directory above shares lists exactly the shared names, not the other devices" $? "$(lst "$O")"
L=$(q 'cd /tmp/cn; ./cnprobe ls /tmp/cnx/many'); R=$(q 'cd /tmp/cn; ./cnprobe ls /net/self/tmp/cnx/many')
[ "$(lst "$L")" = "$(lst "$R")" ] && [ "$(echo "$R" | grep -c '^ent=')" = 302 ]
check "20c a 300-entry directory lists completely through /net (several reads)" $? "$(echo "$R" | tail -1)"
O=$(q 'cd /tmp/cn; ./cnprobe lsr /net/self/tmp/cnx/many | grep "^op="')
echo "$O" | sed 's/^/  /'
[ "$(field n "$(echo "$O" | sed -n 1p)")" = 302 ] && [ "$(field n "$(echo "$O" | sed -n 2p)")" = 302 ]
check "20d rewinddir lists the directory again" $? "$(echo "$O" | tr '\n' ' ' | cut -c1-120)"
O=$(q 'cd /tmp/cn; ./cnprobe ls /net/self/etc')
[ "$(field rc "$O")" = EACCES ]
check "20e listing a directory outside the share is refused" $? "$O"
L=$(q 'ls -l /tmp/cnx/d | sed 1d'); R=$(q 'ls -l /net/self/tmp/cnx/d | sed 1d')
echo "$R" | sed 's/^/  ls -l: /'
[ -n "$R" ] && [ "$L" = "$R" ]
check "20f the stock 'ls -l' prints the same through /net as locally" $? "$(echo "$R" | wc -l | tr -d ' ') lines"
R=$(q 'ls /net/self/dev; ls /net/self' | tr '\n' ' ')
echo "  ls: $R"
[ "$R" = "hello ser1 dev tmp " ]
check "20g the stock 'ls' shows the shared devices and the top of the share list" $? "$R"
O=$(q 'ls -l /net/self/tmp/cnx 2>&1')
echo "$O" | sed 's/^/  ls -l: /'
echo "$O" | grep -q ' ok$' && [ "$(echo "$O" | grep -E -c '^-\?+ .* (escape|probe)$')" = 2 ]
check "20h 'ls -l' on the fixture directory lists the plain files and shows the two links that leave the share as refused (all question marks)" $? "$(echo "$O" | grep -E -c '^-\?+ .* (escape|probe)$') refused rows"

echo
echo "== 21. A waiting stat blocks nobody else"
q "$AGENT"; sleep 1
O=$(q "cd /tmp/cn; ./cnprobe eintr $P 300 1500 > e21.txt 2>&1 & W=\$!; sleep 0.8; slay -s STOP crossnode-agent; ./cnprobe stat /net/self/tmp/cnx/d/b > st21.txt 2>&1 & ST=\$!; wait \$W; wait \$ST; slay -s CONT crossnode-agent; cat e21.txt st21.txt")
echo "$O" | sed 's/^/  /'
E=$(echo "$O" | grep get-with-signal); S=$(echo "$O" | grep 'op=stat')
[ "$(field rc "$E")" = EINTR ] && le "$(field ms "$E")" $((300 + SLACK_MS))
check "21a while a stat waits on a frozen peer, another caller still gets EINTR on time" $? "$E"
[ "$(field rc "$S")" = EHOSTDOWN ] && le "$(field ms "$S")" $((DEAD_MS + SLACK_MS + 1000))
check "21b the waiting stat itself fails with EHOSTDOWN within the liveness limit" $? "$S"
q "$AGENT" >/dev/null; sleep 2
O=$(q "cd /tmp/cn; for i in 1 2 3 4 5 6; do ./cnprobe get $P >/dev/null; slay -s STOP crossnode-agent; ./cnprobe eintr-stat /net/self/tmp/cnx/d/b 100 | cut -d' ' -f2; slay -s CONT crossnode-agent; done | sort | uniq -c; sleep 1; ./cnprobe stat /net/self/tmp/cnx/d/b | cut -d' ' -f1-2; grep -c 'unbind after' net.log; true")
echo "$O" | sed 's/^/  /'
echo "$O" | grep -q '6 rc=EINTR' && echo "$O" | grep -q 'op=stat rc=EOK' && [ "$(echo "$O" | tail -1)" = 0 ]
check "21c six interrupted stats: all EINTR, no unbind error, the path still works" $? "$(echo "$O" | head -2 | tr '\n' ' ')"
O=$(q "cd /tmp/cn; ./cnprobe get $P >/dev/null; slay -s STOP crossnode-agent; ./cnprobe eintr-stat /net/self/tmp/cnx/d/b 60000 > sa.txt 2>&1 & A=\$!; ./cnprobe eintr-stat /net/self/tmp/cnx/ok 300 > sb.txt 2>&1 & B=\$!; wait \$B; wait \$A; slay -s CONT crossnode-agent; echo A; cat sa.txt; echo B; cat sb.txt")
echo "$O" | sed 's/^/  /'
SA=$(echo "$O" | sed -n '/^A$/,/^B$/p' | grep op=); SB=$(echo "$O" | sed -n '/^B$/,$p' | grep op=)
[ "$(field rc "$SB")" = EINTR ] && le "$(field ms "$SB")" $((300 + SLACK_MS)) && [ "$(field rc "$SA")" = EHOSTDOWN ]
check "21e two stats wait together; a signal to one interrupts only that one, the other ends with EHOSTDOWN" $? "signalled: $SB / other: $SA"
q "$AGENT" >/dev/null; sleep 2
O=$(q "cd /tmp/cn; ./cnprobe get $P >/dev/null; i=0; while [ \$i -lt 400 ]; do ./cnprobe stat /net/self/tmp/cnx/d/b >/dev/null; i=\$((i + 1)); done; echo threads=\$(pidin -P crossnode-agent | wc -l); ./cnprobe fstat /net/self/tmp/cnx/d/b | cut -d' ' -f1-2")
echo "$O" | sed 's/^/  /'
echo "$O" | grep -q 'op=fstat rc=EOK'
check "21d 400 stats in a row leave the service working" $? "$(echo "$O" | head -1)"

echo
echo "== 22. Peer authentication"
q "cd /tmp/cn; $AGENT"; sleep 1
O=$(q 'cd /tmp/cn; ./cnprobe rawping 127.0.0.1 7777')
echo "  $O"
[ "$(field ping_reply "$O")" = no ] && [ "$(field closed "$O")" = yes ]
check "22a a client that skips the handshake gets no answer and is hung up on" $? "$O"
q 'cd /tmp/cn; (./crossnode-net -v -k /tmp/cn/key2 -n wrongkey -c 127.0.0.1:7777 > wk.log 2>&1 &) ; (./crossnode-net -v -I -n nokey -c 127.0.0.1:7777 > nk.log 2>&1 &); i=0; while [ $i -lt 20 ] && ! ls -d /net/wrongkey /net/nokey >/dev/null 2>&1; do sleep 0.5; i=$((i + 1)); done; true'
O=$(q 'cd /tmp/cn; ./cnprobe open /net/wrongkey/dev/hello; ./cnprobe open /net/nokey/dev/hello; cat wk.log nk.log | grep -c "authentication"')
echo "$O" | sed 's/^/  /'
R1=$(echo "$O" | sed -n 1p); R2=$(echo "$O" | sed -n 2p)
[ "$(field rc "$R1")" = EHOSTDOWN ] && le "$(field ms "$R1")" $((CONNECT_MS + SLACK_MS))
check "22b a client with the wrong key is refused with EHOSTDOWN" $? "$R1"
[ "$(field rc "$R2")" = EHOSTDOWN ] && le "$(field ms "$R2")" $((CONNECT_MS + SLACK_MS))
check "22c a client with no key is refused with EHOSTDOWN" $? "$R2"
[ "$(echo "$O" | tail -1)" -ge 2 ]
check "22d each refusal is logged on the refusing side as an authentication failure" $? "$(echo "$O" | tail -1) lines"
q 'cd /tmp/cn; (./crossnode-agent -v -I -e /dev/hello -l 127.0.0.1 -p 7780 > agent2.log 2>&1 &); (./crossnode-net -v -k /tmp/cn/key -n openagent -c 127.0.0.1:7780 > oa.log 2>&1 &); i=0; while [ $i -lt 20 ] && ! ls -d /net/openagent >/dev/null 2>&1; do sleep 0.5; i=$((i + 1)); done; true'
O=$(q 'cd /tmp/cn; ./cnprobe open /net/openagent/dev/hello; slay -f -s KILL crossnode-net >/dev/null 2>&1; true')
echo "  $O"
[ "$(field rc "$O")" = EHOSTDOWN ] && le "$(field ms "$O")" $((CONNECT_MS + SLACK_MS))
check "22e a keyed client refuses an agent that does not prove it holds the key" $? "$O"
q 'chmod 644 /tmp/cn/key2'
O=$(q 'cd /tmp/cn; ./crossnode-agent -k /tmp/cn/key2 -e /dev/hello -p 7781 2>&1; echo rc=$?; ./crossnode-agent -e /dev/hello -p 7781 2>&1; echo rc=$?')
echo "$O" | sed 's/^/  /'
echo "$O" | grep -q 'readable by others' && [ "$(echo "$O" | grep -c 'rc=0')" = 0 ] && echo "$O" | grep -q 'no -k KEYFILE'
check "22f a key file others can read is refused, and so is starting with neither a key nor -I" $? "$(echo "$O" | head -1)"
echo
echo "== 23. Strangers cannot hold the agent open"
q 'cd /tmp/cn; ./crossnode-net -k /tmp/cn/key -n tk -c 127.0.0.1:7777 > tk.log 2>&1 &'
q "cd /tmp/cn; ./cnprobe trickle 127.0.0.1 7777 6 > tr.txt 2>&1 & sleep 0.7; i=0; while [ \$i -lt 20 ] && ! ls -d /net/tk >/dev/null 2>&1; do sleep 0.3; i=\$((i + 1)); done; ./cnprobe open /net/tk/dev/hello > tk-open.txt 2>&1; wait; true"
O=$(q 'cd /tmp/cn; cat tr.txt tk-open.txt')
echo "$O" | sed 's/^/  /'
TR=$(echo "$O" | grep op=trickle); TO=$(echo "$O" | grep op=open)
[ "$(field never "$TR")" = 0 ] && le "$(field held_max_ms "$TR")" $((2000 + SLACK_MS + 300))
check "23a six trickling strangers: every one is dropped by about 2 s in total (none is held longer by trickling)" $? "$TR"
[ "$(field rc "$TO")" = EOK ]
check "23b an authenticated peer connects while the trickling strangers fill the handshake places (the longest-waiting stranger makes room)" $? "$TO"
q 'slay -f -s KILL crossnode-net >/dev/null 2>&1; sleep 1; true'
q "cd /tmp/cn; (./crossnode-net -v -k /tmp/cn/key -g 1000 -n self -c 127.0.0.1:7777 >> net.log 2>&1 &)"; sleep 2

echo
echo "== 24. A flood of handshakes from one address cannot push out another address's handshake"
q "cd /tmp/cn; (./crossnode-agent -v -k /tmp/cn/key -e /dev/hello -l 0.0.0.0 -p 7782 > a2.log 2>&1 &); sleep 1; true"
LINKIP=$(q 'ifconfig vtnet1 2>/dev/null | grep "inet 10.9.0.1" | wc -l' | tr -d ' ')
O=$(q "cd /tmp/cn; ./cnprobe trickle 10.9.0.1 7782 1 > vb.txt 2>&1 & sleep 0.3; ./cnprobe trickle 127.0.0.1 7782 6 > va.txt 2>&1; wait; echo B; cat vb.txt; echo A; cat va.txt")
echo "$O" | sed 's/^/  /'
VB=$(echo "$O" | sed -n '/^B$/,/^A$/p' | grep op=); VA=$(echo "$O" | sed -n '/^A$/,$p' | grep op=)
[ "$LINKIP" = 1 ] && [ "$(field instant "$VB")" = 0 ] && [ "$(field held "$VB")" = 1 ] && [ "$(field held_max_ms "$VB")" -ge 1700 ] && [ "$(field instant "$VA")" -ge 3 ]
check "24a six handshakes from 127.0.0.1 lose their own places; the older handshake from 10.9.0.1 is held to its full 2 s" $? "other address: $VB / flooder: $VA"
q 'slay -f -s KILL crossnode-agent >/dev/null 2>&1; true'; q "cd /tmp/cn; $AGENT"; sleep 1
ALL=0
q "cd /tmp/cn; ./cnprobe trickle 127.0.0.1 7777 8 > fl.txt 2>&1 & true"
for n in 1 2 3 4; do
    q "cd /tmp/cn; ./crossnode-net -k /tmp/cn/key -n tk$n -c 127.0.0.1:7777 > tk$n.log 2>&1 &"
    sleep 0.6
    O=$(q "cd /tmp/cn; i=0; while [ \$i -lt 20 ] && ! ls -d /net/tk$n >/dev/null 2>&1; do sleep 0.3; i=\$((i + 1)); done; ./cnprobe open /net/tk$n/dev/hello")
    echo "  tk$n: $O"
    [ "$(field rc "$O")" = EOK ] || ALL=1
done
check "24b four authenticated peers from one address connect while that address floods handshakes (dropped handshakes are never counted as peers)" $ALL "all four opens"
q "cd /tmp/cn; ./crossnode-net -k /tmp/cn/key -n tk5 -c 127.0.0.1:7777 > tk5.log 2>&1 &"; sleep 1
O=$(q "cd /tmp/cn; i=0; while [ \$i -lt 20 ] && ! ls -d /net/tk5 >/dev/null 2>&1; do sleep 0.3; i=\$((i + 1)); done; sleep 4; ./cnprobe open /net/tk5/dev/hello")
echo "  tk5: $O"
[ "$(field rc "$O")" = EHOSTDOWN ]
check "24c a fifth authenticated peer from the same address is refused (the per-address cap is exactly 4)" $? "$O"
q 'slay -f -s KILL crossnode-net crossnode-agent >/dev/null 2>&1; sleep 1; true'
q "cd /tmp/cn; $AGENT; (./crossnode-net -v -k /tmp/cn/key -g 1000 -n self -c 127.0.0.1:7777 >> net.log 2>&1 &)"; sleep 2
q 'slay -f -s KILL crossnode-agent crossnode-net >/dev/null 2>&1; sleep 1; true'
q "cd /tmp/cn; $AGENT; (./crossnode-net -v -k /tmp/cn/key -g 1000 -n self -c 127.0.0.1:7777 >> net.log 2>&1 &)"; sleep 2

echo
echo "== 25. Every frame after the handshake carries an integrity tag"
TAG=${CN_MITM_TAG:-32}     # tag bytes per frame; 0 models the stream before tags existed
q "cd /tmp/cn; ./cnprobe set $P 42 >/dev/null; (./cnmitm 7790 127.0.0.1 7777 flip $TAG > mitm-flip.log 2>&1 &); (./crossnode-net -v -k /tmp/cn/key -n mitmflip -c 127.0.0.1:7790 > mitm-flip-net.log 2>&1 &); i=0; while [ \$i -lt 20 ] && ! ls -d /net/mitmflip >/dev/null 2>&1; do sleep 0.3; i=\$((i + 1)); done; sleep 1"
O=$(q "cd /tmp/cn; ./cnprobe get /net/mitmflip/dev/hello; sleep 1; echo flips=\$(grep -c flipped mitm-flip.log) integrity=\$(grep -c 'integrity' mitm-flip-net.log)")
echo "$O" | sed 's/^/  /'
G1=$(echo "$O" | grep 'op=get'); L1=$(echo "$O" | grep 'flips=')
[ "$(field rc "$G1")" = EHOSTDOWN ] && [ "$(field flips "$L1")" -ge 1 ] && [ "$(field integrity "$L1")" -ge 1 ]
check "25a a reply with one bit flipped on the wire is refused: the call fails with EHOSTDOWN and the change is logged as an integrity failure, never handed to the program" $? "$G1; $L1"
q 'slay -f cnmitm >/dev/null 2>&1; true'
q "cd /tmp/cn; (./cnmitm 7791 127.0.0.1 7777 replay $TAG > mitm-replay.log 2>&1 &); (./crossnode-net -v -k /tmp/cn/key -n mitmreplay -c 127.0.0.1:7791 > mitm-replay-net.log 2>&1 &); i=0; while [ \$i -lt 20 ] && ! ls -d /net/mitmreplay >/dev/null 2>&1; do sleep 0.3; i=\$((i + 1)); done; sleep 1"
O=$(q "cd /tmp/cn; ./cnprobe get /net/mitmreplay/dev/hello; sleep 1; echo replays=\$(grep -c replayed mitm-replay.log) integrity=\$(grep -c 'integrity' mitm-replay-net.log)")
echo "$O" | sed 's/^/  /'
G2=$(echo "$O" | grep 'op=get'); L2=$(echo "$O" | grep 'replays=')
[ "$(field rc "$G2")" = EOK ] && [ "$(field value "$G2")" = 42 ] && [ "$(field replays "$L2")" -ge 1 ] && [ "$(field integrity "$L2")" -ge 1 ]
check "25b a reply sent twice is refused the second time: the first copy answers the call, the copy is logged as an integrity failure" $? "$G2; $L2"
q 'slay -f cnmitm >/dev/null 2>&1; true'

echo
echo "== crossnode-net log"
q 'cat /tmp/cn/net.log' | sed 's/^/  /'
q 'slay -f -s KILL hellodev crossnode-agent crossnode-net >/dev/null 2>&1; true'

echo
echo "== RESULT: $PASS passed, $FAIL failed"
echo "transcript: evidence/$(basename "$OUT")"
[ "$FAIL" -eq 0 ]

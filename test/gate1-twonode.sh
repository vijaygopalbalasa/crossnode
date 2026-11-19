#!/bin/bash
# Crossnode gate 1, part 2: two QNX 8.0 guests on a direct link (see twonode-up.sh).
#
# nodey shares /dev/hello and /dev/ser1; nodex reaches them as /net/nodey/... . Then nodey is
# frozen (its QEMU stopped), and later its power is cut (its QEMU killed): nodey goes silent, no
# reset ever arrives, and every caller on nodex must still get a clean error within the limit.
#
# Usage: test/gate1-twonode.sh IMAGES_DIR
#        CN_MUTATE=liveness test/gate1-twonode.sh IMAGES_DIR   nodex runs crossnode-net-noliveness, built without the
#        liveness limit: checks 4, 5a and 5b must FAIL (7 can fail too, depending on timing). Section 6 is skipped there (a new open would wait forever).
#        CN_MUTATE=notify test/gate1-twonode.sh IMAGES_DIR     nodex runs crossnode-net-nonotify, which never asks the peer
#        about input: checks 8b to 8f must FAIL (select on the peer's serial port never wakes).
set -u
IMG=${1:?usage: gate1-twonode.sh IMAGES_DIR}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$ROOT/build/x86_64
KEY=${CN_SSH_KEY:?set CN_SSH_KEY to the private key whose public half the images were built with (mkqnximage --ssh-ident)}
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
MUTATE=${CN_MUTATE:-0}
OUT=$ROOT/evidence/gate1-twonode-$([ "$MUTATE" = liveness ] && echo mutation-liveness-)$([ "$MUTATE" = notify ] && echo mutation-notify-)$STAMP.txt
mkdir -p "$ROOT/evidence"
exec > >(tee "$OUT") 2>&1

DEAD_MS=2000      # crossnode-net liveness limit (CN_DEAD_MS)
CONNECT_MS=1500   # crossnode-net connect limit (CN_CONNECT_MS)
SLACK_MS=200      # stated allowance on top of a limit: process start and scheduling on emulated guests
QPID=$IMG/nodey/output/qemu.pid

qx() { ssh -i "$KEY" -p 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o BatchMode=yes root@127.0.0.1 "$@"; }
qy() { ssh -i "$KEY" -p 2223 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o BatchMode=yes -o ConnectTimeout=10 root@127.0.0.1 "$@"; }
host_ms() { perl -MTime::HiRes=time -e 'printf "%d\n", time*1000'; }
field() { echo "$2" | tr ' ' '\n' | sed -n "s/^$1=//p" | head -1; }
le() { [ -n "$1" ] && [ "$1" -le "$2" ]; }
stale() { local b=$1; shift; for f in "$@"; do [ "$ROOT/$f" -nt "$BIN/$b" ] && return 0; done; return 1; }
if [ ! -x "$BIN/crossnode-net" ] || stale crossnode-net src/cn_net.c src/cn_proto.h src/cn_auth.h || stale crossnode-agent src/cn_agent.c src/cn_proto.h \
   || stale hellodev tools/hellodev.c tools/hello_dcmd.h || stale cnprobe tools/cnprobe.c tools/hello_dcmd.h src/cn_proto.h \
   || stale serialwatch examples/serialwatch.c \
   || stale crossnode-net-noliveness src/cn_net.c src/cn_proto.h src/cn_auth.h || stale crossnode-net-nonotify src/cn_net.c src/cn_proto.h src/cn_auth.h; then
    echo "STALE BUILD: a binary in build/x86_64 is older than its sources; run make first"; exit 1
fi
HOSTKEY=$(mktemp); head -c 32 /dev/urandom > "$HOSTKEY"; trap 'rm -f "$HOSTKEY"' EXIT   # one shared key for this run
PASS=0
FAIL=0
check() {
    if [ "$2" -eq 0 ]; then echo "PASS  $1  [$3]"; PASS=$((PASS + 1));
    else echo "FAIL  $1  [$3]"; FAIL=$((FAIL + 1)); fi
}
push() {   # push SSH_FUNCTION
    $1 'slay -f -s KILL hellodev crossnode-agent crossnode-net >/dev/null 2>&1; mkdir -p /tmp/cn; rm -f /tmp/cn/*.log /tmp/cn/*.txt; true'
    for f in hellodev crossnode-agent crossnode-net cnprobe serialwatch; do
        $1 "cat > /tmp/cn/$f && chmod +x /tmp/cn/$f" < "$BIN/$f"
    done
    $1 'umask 077; cat > /tmp/cn/key' < "$HOSTKEY"
}
start_nodey() {
    qy 'rm -rf /tmp/cnx && mkdir /tmp/cnx && : > /tmp/cnx/w && dd if=/dev/urandom of=/tmp/cnx/big bs=1024 count=200 2>/dev/null'
    qy 'cd /tmp/cn && (./hellodev > hellodev.log 2>&1 &) && (./crossnode-agent -v -k /tmp/cn/key -e /dev/hello -e /dev/ser1 -e /dev/ser2 -e /tmp/cnx -l 10.9.0.2 -p 7777 > agent.log 2>&1 &)'
}
wait_lines() {  # wait_lines FILE PATTERN SECONDS: until FILE on nodex has a line matching PATTERN
    local i
    for i in $(seq 1 $(( $3 * 4 ))); do
        qx "grep -q '$2' $1 2>/dev/null" && return 0
        sleep 0.25
    done
    return 1
}
P=/net/nodey/dev/hello

echo "== Crossnode gate 1, two nodes, $STAMP"
echo "nodex: $(qx 'uname -a')"
echo "nodey: $(qy 'uname -a')"
echo "link:  $(qx 'ifconfig vtnet1 | grep "inet "' | tr -s ' \t' ' ') <-> $(qy 'ifconfig vtnet1 | grep "inet "' | tr -s ' \t' ' ')"
echo "build: $(cd "$ROOT" && git describe --always --dirty 2>/dev/null || echo uncommitted)"
push qx
push qy
if [ "$MUTATE" = liveness ]; then
    echo "== MUTATION RUN (CN_MUTATE=liveness): nodex runs crossnode-net-noliveness, which never declares a silent peer down. Checks 4, 5a and 5b must FAIL (7 can fail too: nodex may not notice that nodey rebooted)."
    qx "cat > /tmp/cn/crossnode-net && chmod +x /tmp/cn/crossnode-net" < "$BIN/crossnode-net-noliveness"
fi
if [ "$MUTATE" = notify ]; then
    echo "== MUTATION RUN (CN_MUTATE=notify): nodex runs crossnode-net-nonotify, which never asks nodey about input. Checks 8b to 8f must FAIL."
    qx "cat > /tmp/cn/crossnode-net && chmod +x /tmp/cn/crossnode-net" < "$BIN/crossnode-net-nonotify"
fi
start_nodey
qx 'cd /tmp/cn && (./crossnode-net -v -k /tmp/cn/key -g 1000 -n nodey -c 10.9.0.2:7777 > net.log 2>&1 &)'
sleep 1

echo
echo "== 1. nodex uses nodey's device through /net/nodey"
R=$(qx "cd /tmp/cn; cat $P; for a in \"set 41\" \"add 1\" \"get\"; do ./cnprobe \${a%% *} $P \${a#* } | cut -d' ' -f1-4; done")
L=$(qy 'cd /tmp/cn; cat /dev/hello; ./cnprobe get /dev/hello | cut -d" " -f1-4')
echo "on nodex, through /net/nodey:"; echo "$R" | sed 's/^/  /'
echo "on nodey, locally:";            echo "$L" | sed 's/^/  /'
[ "$(echo "$R" | head -1)" = "hello from nodey" ] && [ "$(echo "$R" | tail -1)" = "$(echo "$L" | tail -1)" ]
check "1 the answer comes from nodey, and nodey sees the value nodex set" $? "$(echo "$R" | tail -1)"

echo
echo "== 2. stty on nodex against nodey's serial port"
qx 'stty -a < /net/nodey/dev/ser1' > /tmp/cn-stty-x.txt 2>&1
qy 'stty -a < /dev/ser1' > /tmp/cn-stty-y.txt 2>&1
cmp -s /tmp/cn-stty-x.txt /tmp/cn-stty-y.txt
check "2 stty -a through /net/nodey matches stty -a run on nodey" $? "$(wc -l < /tmp/cn-stty-x.txt | tr -d ' ') lines"
rm -f /tmp/cn-stty-x.txt /tmp/cn-stty-y.txt

echo
echo "== 2b. 200 KB each way across the link"
qx 'dd if=/dev/urandom of=/tmp/cn/big2 bs=1024 count=200 2>/dev/null'
R1=$(qx 'cksum < /net/nodey/tmp/cnx/big'); L1=$(qy 'cksum < /tmp/cnx/big')
qx 'cat /tmp/cn/big2 > /net/nodey/tmp/cnx/w'
L2=$(qx 'cksum < /tmp/cn/big2'); R2=$(qy 'cksum < /tmp/cnx/w')
echo "  read:  nodey file $L1, through /net on nodex $R1"
echo "  write: nodex file $L2, landed on nodey $R2"
[ -n "$L1" ] && [ "$L1" = "$R1" ]
check "2b-read a 200 KB read across the link is byte-exact" $? "$R1"
[ -n "$L2" ] && [ "$L2" = "$R2" ]
check "2b-write a 200 KB write across the link is byte-exact" $? "$R2"

echo
echo "== 2c. nodey is alive but its serial port has no input"
N0=$(qy 'pidin -P crossnode-agent | wc -l' | tr -d ' ')
O=$(qx "cd /tmp/cn; cat /net/nodey/dev/ser1 > /dev/null 2>&1 & C=\$!; sleep 10; kill -0 \$C && echo still_waiting=yes; ./cnprobe get $P; T0=\$(./cnprobe now); kill -INT \$C; wait \$C; T1=\$(./cnprobe now); echo interrupt_ms=\$((T1 - T0))")
sleep 1
N1=$(qy 'pidin -P crossnode-agent | wc -l' | tr -d ' ')
echo "$O" | sed 's/^/  /'; echo "  nodey agent threads (pidin lines): before $N0, after $N1"
echo "$O" | grep -q still_waiting=yes && [ "$(field rc "$(echo "$O" | grep op=get)")" = EOK ]
check "2c-1 the read waits 10 s on a live peer while another caller is served" $? "$(echo "$O" | grep op=get)"
le "$(field interrupt_ms "$O")" $SLACK_MS && [ "$N1" = "$N0" ]
check "2c-2 Ctrl-C returns at once and nodey's worker ends" $? "$(field interrupt_ms "$O") ms, threads $N0 -> $N1"

echo
echo "== 2d. nodex lists and stats nodey's paths"
O=$(qx 'ls /net/nodey/dev; ls /net/nodey' | tr '\n' ' ')
echo "  ls /net/nodey/dev, ls /net/nodey: $O"
[ "$O" = "hello ser1 ser2 dev tmp " ]
check "2d-1 the stock ls on nodex shows nodey's shared devices and the top of its share list" $? "$O"
R=$(qx 'cd /tmp/cn; ./cnprobe stat /net/nodey/tmp/cnx/big'); L=$(qy 'cd /tmp/cn; ./cnprobe stat /tmp/cnx/big')
echo "  through /net: $R"; echo "  on nodey:     $L"
[ "$(field size "$R")" = 204800 ] && [ "$(field mode "$R")" = "$(field mode "$L")" ] && [ "$(field mtime "$R")" = "$(field mtime "$L")" ]
check "2d-2 stat through /net/nodey gives the file's size, mode and modification time as nodey has them" $? "$R"
O=$(qx 'ls -l /net/nodey/dev')
echo "$O" | sed 's/^/  ls -l: /'
[ "$(echo "$O" | grep -E -c '^c.* (hello|ser1)$')" = 2 ]
check "2d-3 ls -l on nodex lists nodey's shared devices with their attributes" $? "$(echo "$O" | grep -c .) lines"

echo
echo "== 3. nodey freezes: a signal reaches a waiting caller on nodex"
qx "cd /tmp/cn; ./cnprobe get $P >/dev/null; (./cnprobe eintr $P 300 1500 > e3.txt 2>&1 &)"
sleep 1
kill -STOP "$(cat "$QPID")"
wait_lines /tmp/cn/e3.txt op= 10
kill -CONT "$(cat "$QPID")"
O=$(qx 'cat /tmp/cn/e3.txt')
echo "  $O"
[ "$(field rc "$O")" = EINTR ] && le "$(field ms "$O")" $((300 + SLACK_MS))
check "3 the call returns EINTR when the signal arrives" $? "$O"
qx "cd /tmp/cn; ./cnprobe get $P >/dev/null"
kill -STOP "$(cat "$QPID")"
O=$(qx "cd /tmp/cn; ./cnprobe eintr-open $P 300")
kill -CONT "$(cat "$QPID")"
echo "  $O"
[ "$(field rc "$O")" = EINTR ] && le "$(field ms "$O")" $((300 + SLACK_MS))
check "3b an open waiting on the frozen node returns EINTR when the signal arrives" $? "$O"

echo
echo "== 4. nodey freezes: a waiting call on nodex fails within the liveness limit"
sleep 3
qx "cd /tmp/cn; (./cnprobe watch $P 600 50 > w4.txt 2>&1 &)"
sleep 1.5
T0=$(host_ms); kill -STOP "$(cat "$QPID")"
wait_lines /tmp/cn/w4.txt result= 15
T1=$(host_ms); kill -CONT "$(cat "$QPID")"
O=$(qx 'tail -1 /tmp/cn/w4.txt')
echo "  $O  (host saw it $((T1 - T0)) ms after the freeze, polling included)"
[ "$(field rc "$O")" = EHOSTDOWN ] && le "$(field call_ms "$O")" $((DEAD_MS + SLACK_MS))
check "4 EHOSTDOWN within the liveness limit" $? "call_ms=$(field call_ms "$O"), bound ${DEAD_MS}+${SLACK_MS} ms"

echo
echo "== 5. nodey loses power while nodex has three callers on it"
sleep 3
qx "cd /tmp/cn; ./cnprobe get $P > /dev/null; (./cnprobe watch $P 600 50 > w5.txt 2>&1 &); (./cnprobe stale $P 5000 > s5.txt 2>&1 &); (./cnprobe exit $P 5000 > x5.txt 2>&1 &)"
sleep 1.5
T0=$(host_ms); kill -KILL "$(cat "$QPID")"
wait_lines /tmp/cn/w5.txt result= 15
T1=$(host_ms)
wait_lines /tmp/cn/s5.txt op=close 15
wait_lines /tmp/cn/x5.txt op=exit 15
W=$(qx 'tail -1 /tmp/cn/w5.txt'); S=$(qx 'cat /tmp/cn/s5.txt'); X=$(qx 'cat /tmp/cn/x5.txt')
echo "  waiting call:  $W  (host saw it $((T1 - T0)) ms after the power cut, polling included)"
echo "$S" | sed 's/^/  idle fd:       /'
echo "  holding exit:  $X"
[ "$(field rc "$W")" = EHOSTDOWN ] && le "$(field call_ms "$W")" $((DEAD_MS + SLACK_MS))
check "5a the waiting call gets EHOSTDOWN within the liveness limit" $? "call_ms=$(field call_ms "$W")"
AFTER=$(echo "$S" | grep get-after); CL=$(echo "$S" | grep op=close)
[ "$(field rc "$AFTER")" = EHOSTDOWN ] && le "$(field ms "$AFTER")" 100 && [ "$(field rc "$CL")" = EOK ] && le "$(field ms "$CL")" 100
check "5b a call on an idle fd fails at once, and close returns at once" $? "$AFTER; $CL"
[ "$(field opened "$X")" = y ] && le "$(field ms "$X")" 200
check "5c a process holding an fd exits at once" $? "$X"

echo
echo "== 6. New opens while nodey is off"
if [ "$MUTATE" = liveness ]; then
    echo "  MUTATION: skipped; without the liveness limit nodex still thinks nodey is up, so a new open would wait forever"
else
O=$(qx "cd /tmp/cn; sleep 2; ./cnprobe open $P; ./cnprobe open $P")
echo "$O" | sed 's/^/  /'
FIRST=$(echo "$O" | sed -n 1p); SECOND=$(echo "$O" | sed -n 2p)
[ "$(field rc "$FIRST")" = EHOSTDOWN ] && le "$(field ms "$FIRST")" $((CONNECT_MS + SLACK_MS))
check "6a the open fails with EHOSTDOWN within the connect limit" $? "$FIRST"
[ "$(field rc "$SECOND")" = EHOSTDOWN ] && le "$(field ms "$SECOND")" 100
check "6b a second open right after fails at once" $? "$SECOND"

O=$(qx "cd /tmp/cn; ./cnprobe ls /net/nodey/dev; ./cnprobe stat /net/nodey/dev/hello")
echo "$O" | sed 's/^/  /'
L1=$(echo "$O" | grep 'op=ls'); L2=$(echo "$O" | grep 'op=stat')
[ "$(field rc "$L1")" = EHOSTDOWN ] && [ "$(field rc "$L2")" = EHOSTDOWN ] && le "$(field ms "$L1")" $((CONNECT_MS + SLACK_MS)) && le "$(field ms "$L2")" 100
check "6c listing a dead node fails with EHOSTDOWN within the connect limit, and a stat right after fails at once" $? "$L1; $L2"
fi

echo
echo "== 7. nodey boots again; nodex needs no restart"
"$ROOT/test/twonode-up.sh" "$IMG" nodey | sed 's/^/  /'
push qy
start_nodey
# A call right after a failed connect fails at once for 1 s (crossnode-net's retry window), so try for up to 10 s.
O=""
for i in $(seq 1 20); do O=$(qx "cd /tmp/cn; cat $P; ./cnprobe get $P" 2>&1); [ "$(field rc "$(echo "$O" | tail -1)")" = EOK ] && break; sleep 0.5; done
echo "$O" | sed 's/^/  /'
echo "$O" | head -1 | grep -q 'hello from nodey' && [ "$(field rc "$(echo "$O" | tail -1)")" = EOK ]
check "7 /net/nodey works again within 10 s of its agent starting" $? "$(echo "$O" | tail -1)"

echo
echo "== 8. select on nodey's serial port, from nodex"
# nodey's second serial port is wired to a socket on this host (twonode-up.sh): bytes sent there arrive on nodey's
# /dev/ser2 as if typed on the line.
SER2=$IMG/nodey/output/ser2.sock
for i in $(seq 1 20); do qx "/tmp/cn/cnprobe get $P" 2>/dev/null | grep -q rc=EOK && break; sleep 0.5; done
qy 'for i in 1 2 3 4 5; do /tmp/cn/cnprobe select /dev/ser2 300 | grep -q ready=1 || break; done' >/dev/null 2>&1   # drop anything left on nodey's ser2 from an earlier run
type_on_ser2() {   # type_on_ser2 DELAY_S TEXT: send TEXT one character at a time after DELAY_S
    python3 -I -c 'import socket,sys,time
s=socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); time.sleep(float(sys.argv[2]))
for c in sys.argv[3] + "\n": s.send(c.encode()); time.sleep(0.05)
time.sleep(1); s.close()' "$SER2" "$1" "$2"
}
O=$(qx '/tmp/cn/cnprobe select /net/nodey/dev/ser2 1500')
echo "  nodex# cnprobe select /net/nodey/dev/ser2 1500 (nothing typed) -> $(echo "$O" | sed 's/ ms=[0-9]*//')"
[ "$(field rc "$O")" = EOK ] && [ "$(field ready "$O")" = 0 ] && le "$(field ms "$O")" $((1500 + 500))
check "8a with nothing on the line, select on /net/nodey/dev/ser2 times out with nothing ready" $? "$(echo "$O" | sed 's/ ms=[0-9]*//')"
type_on_ser2 3 typed-on-nodey-ser2 &
O=$(qx '/tmp/cn/cnprobe select /net/nodey/dev/ser2 10000'); wait
echo "  nodex# cnprobe select /net/nodey/dev/ser2 10000 (a line typed on nodey's ser2 after 3 s) -> $(echo "$O" | sed 's/ ms=[0-9]*//')"
[ "$(field ready "$O")" = 1 ] && [ "$(field read "$O")" = EOK ] && [ "$(field data "$O")" = typed-on-nodey-ser2_ ] && [ "$(field ms "$O")" -ge 2500 ]
check "8b select waits on nodey's serial port and wakes when a line is typed there; the read returns that line" $? "$(echo "$O" | sed 's/ ms=[0-9]*//')"
qx 'cd /tmp/cn; rm -f s8d-*.txt; i=0; while [ $i -lt 20 ]; do (./cnprobe select /net/nodey/dev/ser2 6000 > s8d-$i.txt 2>&1 < /dev/null &); i=$((i + 1)); done; sleep 2; ./cnprobe get /net/nodey/dev/hello > s8d-get.txt'
sleep 6
O=$(qx 'cd /tmp/cn; echo "eagain=$(cat s8d-[0-9]*.txt | grep -c rc=EAGAIN) timedout=$(cat s8d-[0-9]*.txt | grep -c ready=0)"; cat s8d-get.txt')
echo "  nodex# 20 programs select on /net/nodey/dev/ser2 at once, then a devctl -> $(echo "$O" | tr '\n' ' ' | sed 's/ ms=[0-9]*//')"
[ "$(field eagain "$O")" = 4 ] && [ "$(field timedout "$O")" = 16 ] && [ "$(field rc "$(echo "$O" | grep op=get)")" = EOK ]
check "8c at most 16 select waiters per link: the 17th to 20th get EAGAIN at once, the 16 others time out normally, and an ordinary call is still answered" $? "$(echo "$O" | head -1)"
# 8d: an ordinary program with no Crossnode code (examples/serialwatch.c: open, tcsetattr, select, read), the way a
# Qnet-era program watches another machine's serial port, run unchanged against /net/nodey/dev/ser2.
qy 'for i in 1 2 3 4 5; do /tmp/cn/cnprobe select /dev/ser2 300 | grep -q ready=1 || break; done' >/dev/null 2>&1
type_on_ser2 3 a-line-for-serialwatch &
O=$(qx '/tmp/cn/serialwatch /net/nodey/dev/ser2 115200 8' 2>&1); wait
B=$(qy 'stty -a < /dev/ser2 | grep -o "baud=[0-9]*"'); qy 'stty baud=57600 < /dev/ser2' >/dev/null 2>&1
echo "$O" | sed 's/^/  nodex# serialwatch \/net\/nodey\/dev\/ser2 115200 4: /'
echo "  nodey# stty -a < /dev/ser2 (after it ran) -> $B"
echo "$O" | grep -q 'set to 115200 baud' && echo "$O" | grep -qx '/net/nodey/dev/ser2: a-line-for-serialwatch' && [ "$B" = baud=115200 ]
check "8d an ordinary POSIX program (tcsetattr, select, read; no Crossnode code) sets nodey's serial port to 115200 baud and prints the line typed there" $? "$(echo "$O" | grep -c 'ser2: a-line') line, nodey reports $B"
type_on_ser2 13 typed-after-13-s &
O=$(qx '/tmp/cn/cnprobe select /net/nodey/dev/ser2 25000'); wait
echo "  nodex# cnprobe select /net/nodey/dev/ser2 25000 (a line typed after 13 s) -> $(echo "$O" | sed 's/ ms=[0-9]*//')"
[ "$(field ready "$O")" = 1 ] && [ "$(field data "$O")" = typed-after-13-s_ ] && [ "$(field ms "$O")" -ge 12000 ]
check "8e a select still wakes when the line comes after the agent's 10 s wait has ended (crossnode-net asks again while someone waits)" $? "$(echo "$O" | sed 's/ ms=[0-9]*//')"
qx '(/tmp/cn/cnprobe select /net/nodey/dev/ser2 15000 > /tmp/cn/s8w.txt 2>&1 < /dev/null &)'
sleep 2
qy 'slay -f -s KILL crossnode-agent >/dev/null 2>&1; true'
wait_lines /tmp/cn/s8w.txt op=select 10
O=$(qx 'cat /tmp/cn/s8w.txt')
echo "  nodex# select waiting while nodey's agent is killed -> $(echo "$O" | sed 's/ ms=[0-9]*//')"
[ "$(field ready "$O")" = 1 ] && [ "$(field read "$O")" = EHOSTDOWN ]
check "8f a select waiting on the peer wakes when the link fails, and the read says EHOSTDOWN" $? "$(echo "$O" | sed 's/ ms=[0-9]*//')"

echo
echo "== crossnode-net log on nodex"
qx 'cat /tmp/cn/net.log' | sed 's/^/  /'
qx 'slay -f -s KILL crossnode-net >/dev/null 2>&1; true'
qy 'slay -f -s KILL hellodev crossnode-agent >/dev/null 2>&1; true'

echo
echo "== RESULT: $PASS passed, $FAIL failed"
echo "transcript: evidence/$(basename "$OUT")"
[ "$FAIL" -eq 0 ]

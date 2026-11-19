#!/bin/bash
# The link demo, for the video: two plain QNX 8.0 test machines (test/twonode-up.sh), nodey sharing its test device
# and serial port, nodex using them through /net/nodey. Shows a stock tool (stty) on the other machine's serial
# port, a devctl that lands on the other machine, a read that waits on a quiet serial line and a real Ctrl-C typed
# at it, the link's status file, and the priority test. Every line printed after a prompt is real output.
# Usage: demo/link-demo.sh IMAGES_DIR
set -u
IMG=${1:?usage: link-demo.sh IMAGES_DIR}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$ROOT/build/x86_64
KEY=${CN_SSH_KEY:?set CN_SSH_KEY to the private key whose public half the images were built with (mkqnximage --ssh-ident)}
SSHO="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o BatchMode=yes"
qx() { ssh -i "$KEY" -p 2222 $SSHO root@127.0.0.1 "$@"; }
qy() { ssh -i "$KEY" -p 2223 $SSHO root@127.0.0.1 "$@"; }
pause() { sleep "${1:-1.2}"; }
say() { printf '\n\033[1;33m# %s\033[0m\n' "$*"; pause 1.5; }
run() {   # run x|y COMMAND: show the prompt and the command, then its real output
    local m=$1; shift
    printf '\033[1;36mnode%s#\033[0m %s\n' "$m" "$*"; pause 0.6
    if [ "$m" = x ]; then qx "$*"; else qy "$*"; fi
    pause
}
HOSTKEY=$(mktemp); head -c 32 /dev/urandom > "$HOSTKEY"; trap 'rm -f "$HOSTKEY"' EXIT
for m in qx qy; do
    $m 'slay -f -s KILL hellodev crossnode-agent crossnode-net >/dev/null 2>&1; mkdir -p /tmp/cn; true'
    for f in hellodev crossnode-agent crossnode-net cnprobe cnmitm serialwatch; do $m "cat > /tmp/cn/$f && chmod +x /tmp/cn/$f" < "$BIN/$f"; done
    $m 'umask 077; cat > /tmp/cn/key' < "$HOSTKEY"
done
qy 'cd /tmp/cn && (./hellodev > hellodev.log 2>&1 &) && (./crossnode-agent -r 30 -P 30 -k /tmp/cn/key -e /dev/hello -e /dev/ser1 -e /dev/ser2 -l 10.9.0.2 -p 7777 > agent.log 2>&1 &)'
sleep 1
qx 'cd /tmp/cn && (./crossnode-net -r 30 -k /tmp/cn/key -n nodey -c 10.9.0.2:7777 > net.log 2>&1 &)'
for i in $(seq 1 20); do qx '/tmp/cn/cnprobe get /net/nodey/dev/hello' 2>/dev/null | grep -q rc=EOK && break; sleep 0.5; done

say "Two QNX 8.0 machines in QEMU, nodex and nodey, on a direct link. nodey shares its serial port and a test device"
run x 'uname -sr; ls /net/nodey/dev'
say "A stock tool on the other machine's serial port: stty through /net"
run x 'stty -a < /net/nodey/dev/ser1 | tail -1'
say "Change its speed from nodex. nodey's own driver now reports it"
run x 'stty baud=115200 < /net/nodey/dev/ser1; stty -a < /net/nodey/dev/ser1 | tail -1'
run y 'stty -a < /dev/ser1 | tail -1'
qx 'stty baud=57600 < /net/nodey/dev/ser1' >/dev/null 2>&1
say "A devctl from nodex lands on nodey's driver"
run x '/tmp/cn/cnprobe set /net/nodey/dev/hello 42 | cut -d" " -f1-4'
run y '/tmp/cn/cnprobe get /dev/hello | cut -d" " -f1-4'
say "A read on nodey's quiet serial line waits until Ctrl-C"
printf '\033[1;36mnodex#\033[0m %s\n' 'cat /net/nodey/dev/ser1'
python3 -I - "$KEY" <<'PY'
import os, pty, sys, time, select
key = sys.argv[1]
pid, fd = pty.fork()
if pid == 0:
    os.execvp("ssh", ["ssh", "-tt", "-i", key, "-p", "2222", "-o", "StrictHostKeyChecking=no", "-o",
                      "UserKnownHostsFile=/dev/null", "-o", "LogLevel=ERROR", "root@127.0.0.1",
                      "cat /net/nodey/dev/ser1"])
def pump(secs):
    end = time.time() + secs
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try:
                d = os.read(fd, 4096)
            except OSError:
                return False
            if not d:
                return False
            sys.stdout.write(d.decode(errors="replace")); sys.stdout.flush()
    return True
pump(4.0)
sys.stdout.write("^C  (typed after 4 s of waiting)\n"); sys.stdout.flush()
os.write(fd, b"\x03")
t0 = time.time()
done = False
while time.time() - t0 < 3.0:
    pump(0.05)
    w, _ = os.waitpid(pid, os.WNOHANG)
    if w:
        done = True
        break
quick = done and time.time() - t0 <= 1.0
print("\n(the read was interrupted and the shell is back: %s)" % ("within 1 s of Ctrl-C" if quick else "NOT within 1 s"))
if not done:
    os.kill(pid, 9)
PY
pause
say "An ordinary program, written the Qnet way (open, tcsetattr, select, read; no Crossnode code), on nodey's second serial port"
SER2=$IMG/nodey/output/ser2.sock
qy 'for i in 1 2 3 4 5; do /tmp/cn/cnprobe select /dev/ser2 300 | grep -q ready=1 || break; done' >/dev/null 2>&1   # drop anything left on the line
printf '\033[1;36mnodex#\033[0m %s\n' 'serialwatch /net/nodey/dev/ser2 115200 8'; pause 0.6
python3 -I -c 'import socket,sys,time
s=socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); time.sleep(4)
sys.stdout.write("\033[1;35mhost$\033[0m typing into nodey'"'"'s ser2 (a QEMU socket): hello from the serial line\n"); sys.stdout.flush()
for c in "hello from the serial line\n": s.send(c.encode()); time.sleep(0.06)
time.sleep(1); s.close()' "$SER2" &
qx '/tmp/cn/serialwatch /net/nodey/dev/ser2 115200 8'
wait
run y 'stty -a < /dev/ser2 | tail -1'
pause
say "Priority travels. Two callers on nodex, at priority 5 and 30, wait on that serial port"
qx '(on -p 5 cat /net/nodey/dev/ser2 > /dev/null 2>&1 < /dev/null &); (on -p 30 cat /net/nodey/dev/ser2 > /dev/null 2>&1 < /dev/null &)'
sleep 2
say "On nodey, the agent's two workers wait in the serial driver (devc-ser8250) at the callers' priorities, 5 and 30"
run y 'pidin -p crossnode-agent; pidin -p devc-ser8250 | sed -n 2p'
qx 'slay -f cat >/dev/null 2>&1; true'
pause
say "Change one bit of a request on the wire (a test proxy between the two machines)"
qx 'cd /tmp/cn && (./cnmitm 7790 10.9.0.2 7777 flipreq 32 > mitm.log 2>&1 < /dev/null &) && (./crossnode-net -k /tmp/cn/key -n tampered -c 127.0.0.1:7790 > tnet.log 2>&1 < /dev/null &); sleep 1; true'
run y '/tmp/cn/cnprobe set /dev/hello 7 | cut -d" " -f1-4'
run x '/tmp/cn/cnprobe set /net/tampered/dev/hello 42 | cut -d" " -f1-2'
run y '/tmp/cn/cnprobe get /dev/hello | cut -d" " -f1-4'
run y 'tail -1 /tmp/cn/agent.log'
qx 'slay -f cnmitm >/dev/null 2>&1; true'
say "The link's own status file, answered on nodex"
run x 'cat /net/nodey/.status'
say "Now fill both of nodey's CPUs with priority 12 work for 8 s, and call in from nodex at priority 30 and at 5"
qx 'cd /tmp/cn; rm -f order.txt; (on -p 30 sh -c "sleep 2; ./cnprobe get /net/nodey/dev/hello > /dev/null; echo \"priority 30 call: answered\" >> order.txt" > /dev/null 2>&1 < /dev/null &); (on -p 5 sh -c "sleep 2; ./cnprobe get /net/nodey/dev/hello > /dev/null; echo \"priority 5 call: answered\" >> order.txt" > /dev/null 2>&1 < /dev/null &); (on -p 40 sh -c "sleep 5; echo \"(nodey: the priority 12 work is still running)\" >> order.txt" > /dev/null 2>&1 < /dev/null &)'
qy "rm -f /tmp/cn/snap.txt; (on -p 40 sh -c 'sleep 4; pidin | grep -E \" (5|12)r \" | grep -v procnto' > /tmp/cn/snap.txt 2>&1 < /dev/null &); (sleep 1; for c in 1 2; do on -p 12 sh -c 'e=\$((SECONDS+8)); while [ \$SECONDS -lt \$e ]; do :; done' & done) > /dev/null 2>&1 < /dev/null &"
sleep 12
say "nodey, seen from a priority 40 snapshot taken while the work ran: the two priority 12 loops, and the priority 5 call's worker waiting behind them"
run y 'cat /tmp/cn/snap.txt'
say "nodex, the order things happened in"
run x 'cat /tmp/cn/order.txt'
say "Crossnode: QNX's priority-driven I/O, carried across machines"
pause 2

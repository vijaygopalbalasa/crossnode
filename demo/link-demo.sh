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
    for f in hellodev crossnode-agent crossnode-net cnprobe; do $m "cat > /tmp/cn/$f && chmod +x /tmp/cn/$f" < "$BIN/$f"; done
    $m 'umask 077; cat > /tmp/cn/key' < "$HOSTKEY"
done
qy 'cd /tmp/cn && (./hellodev > hellodev.log 2>&1 &) && (./crossnode-agent -r 30 -P 30 -k /tmp/cn/key -e /dev/hello -e /dev/ser1 -l 10.9.0.2 -p 7777 > agent.log 2>&1 &)'
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
say "A read on nodey's quiet serial line waits. Ctrl-C returns at once"
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
say "The link's own status file, answered on nodex"
run x 'cat /net/nodey/.status'
say "Priority travels with the request. The test fills both of nodey's CPUs with priority 12 work, then calls in"
"$ROOT/test/priority.sh" "$IMG" 2>&1 | grep -E '^(PASS|FAIL)|RESULT' | sed 's/^/  /'
say "Crossnode: QNX's own priority-driven message passing, across machines"
pause 2

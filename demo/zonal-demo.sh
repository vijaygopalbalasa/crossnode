#!/bin/bash
# The zonal demo as a person would type it, for the video: boots the three machines (image/run-zones.sh), then
# shows central reading both zones by path, rear losing power while central watches it, front still answering,
# and rear coming back with nothing restarted. Commands run on central over SSH from the host; every line printed
# after a prompt is real output from the machines.
# Usage: demo/zonal-demo.sh PROJECTS_DIR        (the zonal set; a confined, enforced set shows the types too)
set -u
D=${1:?usage: zonal-demo.sh PROJECTS_DIR}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
KEY=${CN_SSH_KEY:?set CN_SSH_KEY to the private key whose public half the images were built with (mkqnximage --ssh-ident)}
FRONT=2224; CENTRAL=2225; REAR=2226
ssh_to() { local p=$1; shift; ssh -i "$KEY" -p "$p" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o BatchMode=yes -o ConnectTimeout=20 root@127.0.0.1 "$@"; }
pause() { sleep "${1:-1.2}"; }
say() { printf '\n\033[1;33m# %s\033[0m\n' "$*"; pause 1.5; }
run() {   # run PORT NAME COMMAND: show the prompt and the command, then its real output
    local p=$1 n=$2; shift 2
    printf '\033[1;36m%s#\033[0m %s\n' "$n" "$*"; pause 0.6
    ssh_to "$p" "$*"
    pause
}
trap '"$ROOT/image/run-zones.sh" "$D" stop >/dev/null 2>&1' EXIT

say "Three QNX 8.0 machines in QEMU: front, rear and central (the sensors are simulated)"
"$ROOT/image/run-zones.sh" "$D" | sed 's/^/  /'
sleep 6
for i in $(seq 1 10); do ssh_to $CENTRAL 'ls /net/front/dev /net/rear/dev' >/dev/null 2>&1 && break; sleep 2; done
run $CENTRAL central 'uname -sr; ls /net'
say "Nothing was typed after power-on. Stock ls and cat on central reach both zones by path"
run $CENTRAL central 'ls /net/front/dev /net/rear/dev'
run $CENTRAL central 'cat /net/front/dev/temp /net/rear/dev/radar'
say "Device control (devctl, the call stty uses), sent by our test tool, lands on front's driver"
run $CENTRAL central 'cnprobe set /net/front/dev/hello 42 | cut -d" " -f1-4'
run $FRONT front 'cnprobe get /dev/hello | cut -d" " -f1-4'
say "One file lists the zones: a supervisor runs one process per zone, each with its own key"
run $CENTRAL central 'cat /data/etc/crossnode.conf'
say "No root: each service runs as its own user, under its own security type"
run $CENTRAL central "pidin -f an_U | awk '/crossnode/ {n = split(\$2, a, \"/\"); printf \"%-9s %-15s %-17s uid %s\\n\", \$1, a[n], \$3, \$4}'"
say "And the security policy cannot be changed while the machine runs"
run $CENTRAL central 'secpol -Q'
say "Central reads the rear radar with a plain cat in a loop"
ssh_to $CENTRAL "rm -f /tmp/catloop.log; nohup sh -c 'n=0; while :; do n=\$((n + 1)); v=\$(cat /net/rear/dev/radar 2>&1); echo \"read \$n: \$v\"; case \"\$v\" in *rear-radar*) ;; *) break ;; esac; sleep 0.25; done' > /tmp/catloop.log 2>&1 &"
for i in 1 2 3 4 5; do ssh_to $CENTRAL 'tail -1 /tmp/catloop.log'; sleep 0.6; done
say "Cut the rear zone's power"
printf '\033[1;35mhost$\033[0m kill rear   (its QEMU process: the link goes silent)\n'
kill "$(cat "$D/rear/output/qemu.pid")"
sleep 3.5
run $CENTRAL central 'tail -3 /tmp/catloop.log'
say "The waiting cat ends with Host is down once the liveness limit passes. Front still answers"
run $CENTRAL central 'cat /net/front/dev/temp; cat /net/rear/dev/radar'
say "The link's status file says why"
run $CENTRAL central 'cat /net/rear/.status'
say "Power the rear zone back on. Nothing is restarted on central"
"$ROOT/image/run-zones.sh" "$D" rear | sed 's/^/  /'
for i in $(seq 1 30); do ssh_to $CENTRAL 'cnprobe watch /net/rear/dev/radar 1 0' 2>/dev/null | grep -q EOK && break; sleep 2; done
run $CENTRAL central 'cat /net/rear/dev/radar'
say "The same three processes as before, and the status file tells the story"
run $CENTRAL central "pidin -f an_U | awk '/crossnode-net/ {print \$1, \"crossnode-net\"}'"
run $CENTRAL central 'cat /net/rear/.status'
say "Crossnode: the path part of Qnet, authenticated, confined, honest about failure"
pause 2

#!/bin/bash
# Boot check for the Crossnode demo images: after power-on, with no command typed, central reaches front's
# device by path. Boots both images (image/run-pair.sh), checks, saves a transcript, shuts both down.
# Usage: test/boot-image.sh PROJECTS_DIR
set -u
D=${1:?usage: boot-image.sh PROJECTS_DIR}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
KEY=${CN_SSH_KEY:-$HOME/qnx-work/home/.ssh/qnx_gate1}
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
OUT=$ROOT/evidence/boot-image-$STAMP.txt
exec > >(tee "$OUT") 2>&1
ssh_to() { local p=$1; shift; ssh -i "$KEY" -p "$p" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o BatchMode=yes -o ConnectTimeout=20 root@127.0.0.1 "$@"; }
PASS=0; FAIL=0
check() { if [ "$2" -eq 0 ]; then echo "PASS  $1  [$3]"; PASS=$((PASS + 1)); else echo "FAIL  $1  [$3]"; FAIL=$((FAIL + 1)); fi; }
trap '"$ROOT/image/run-pair.sh" "$D" stop >/dev/null 2>&1' EXIT

echo "== Crossnode boot image check, $STAMP, build $(cd "$ROOT" && git describe --always --dirty)"
"$ROOT/image/run-pair.sh" "$D"
sleep 8     # sshd is up before the services are; give start-up its last seconds
echo "front:   $(ssh_to 2224 uname -a)"
echo "central: $(ssh_to 2225 uname -a)"
O=$(ssh_to 2224 'pidin arg | grep -E "crossnode-agent|hellodev" | grep -v grep')
echo "$O" | sed 's/^/  front processes: /'
[ "$(echo "$O" | grep -c .)" = 2 ]
check "1 front started hellodev and crossnode-agent by itself at boot" $? "$(echo "$O" | tr -s ' ' | tr '\n' ';')"
O=$(ssh_to 2225 'pidin arg | grep -E "crossnode-net" | grep -v grep')
[ "$(echo "$O" | grep -c .)" = 1 ]
check "2 central started crossnode-net by itself at boot" $? "$(echo "$O" | tr -s ' ')"
for i in 1 2 3 4 5 6 7 8; do ssh_to 2225 'ls /net/front/dev' >/dev/null 2>&1 && break; sleep 2; done
O=$(ssh_to 2225 'ls /net/front/dev; cat /net/front/dev/hello' | tr '\n' ' ')
echo "  on central: ls /net/front/dev; cat /net/front/dev/hello -> $O"
[ "$O" = "hello hello from front " ]
check "3 central lists front's shared device and reads it, with no command typed on either node" $? "$O"
O=$(ssh_to 2224 'ls -l /data/etc/crossnode.key | cut -c1-10')
[ "$O" = "-rw-------" ]
check "4 the key file in the image is readable by root only" $? "$O"
O=$(ssh_to 2225 'cat /net/front/etc/passwd 2>&1; true')
echo "$O" | grep -q "Permission denied"
check "5 a path outside front's share list is refused from central" $? "$O"
echo
echo "== RESULT: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]

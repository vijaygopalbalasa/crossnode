#!/bin/bash
# Boot check for the Crossnode demo images: after power-on, with no command typed, central reaches front's
# device by path. Boots both images (image/run-pair.sh), checks, saves a transcript, shuts both down.
# Usage: test/boot-image.sh PROJECTS_DIR
set -u
D=${1:?usage: boot-image.sh PROJECTS_DIR}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
KEY=${CN_SSH_KEY:?set CN_SSH_KEY to the private key whose public half the images were built with (mkqnximage --ssh-ident)}
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
# Images built with CN_SECPOL=open or secure run both services as the non-root user crossnode (uid 70).
CONF=$(ssh_to 2224 'grep -c "^crossnode:" /etc/passwd' 2>/dev/null); CONF=${CONF:-0}
SECURE=$(ssh_to 2224 'secpol -Q 2>/dev/null | grep -c immutable' 2>/dev/null); SECURE=${SECURE:-0}
echo "  confined services: $([ "$CONF" = 1 ] && echo yes || echo no); security policy enforced: $([ "$SECURE" = 1 ] && echo yes || echo no)"
O=$(ssh_to 2224 'ls -ln /data/etc/crossnode.key' | awk '{print $1, $3, $4}')
if [ "$CONF" = 1 ]; then
    [ "$O" = "-rw------- 70 70" ]
    check "4 the key file belongs to the service user (uid 70) and no one else may read it" $? "$O"
else
    [ "$O" = "-rw------- 0 0" ]
    check "4 the key file in the image is readable by root only" $? "$O"
fi
O=$(ssh_to 2225 'cat /net/front/etc/passwd 2>&1; true')
echo "$O" | grep -q "Permission denied"
check "5 a path outside front's share list is refused from central" $? "$O"
if [ "$CONF" = 1 ]; then
    O=$(ssh_to 2224 'pidin -f an_U' | awk '/crossnode-agent/ {print $3, $4}')
    case "$O" in "crossnode_agent"*" 70") true ;; *) false ;; esac
    check "6 front's crossnode-agent runs as uid 70 under its own security type" $? "$O"
    O=$(ssh_to 2225 'pidin -f an_U' | awk '/crossnode-net/ {print $3, $4}')
    [ "$O" = "crossnode_net_t 70" ]
    check "7 central's crossnode-net runs as uid 70 under its own security type" $? "$O"
fi
if [ "$SECURE" = 1 ]; then
    O="front: $(ssh_to 2224 'secpol -Q'); central: $(ssh_to 2225 'secpol -Q')"
    [ "$(echo "$O" | grep -o immutable | grep -c .)" = 2 ]
    check "8 both nodes run under a security policy that cannot be changed while they run" $? "$O"
    O=$(ssh_to 2224 'secpol -f capability -t crossnode_agent_t' | sed -n '/^crossnode_agent_t/,/EOL/p')
    echo "$O" | sed 's/^/  front policy: /'
    ! echo "$O" | grep -q -E "PATHSPACE|PUBLIC_CHANNEL" && echo "$O" | grep -q "EOL deny"
    check "9 the agent's type may not attach any path or offer a public channel; every ability not listed is denied" $? "$(echo "$O" | grep -c .) policy lines"
    O=$(ssh_to 2225 'secpol -c' | grep "^allow_attach crossnode_net_t ")
    [ "$O" = "allow_attach crossnode_net_t /net/front;" ]
    check "10 crossnode-net's type may attach exactly one path, /net/front" $? "$O"
    O=$(ssh_to 2225 'on -T crossnode_net_t -u 70:70 /system/bin/hellodev /dev/cnx-probe 2>&1; ls /dev/cnx-probe 2>&1' | tr '\n' ' ')
    echo "$O" | grep -q "Operation not permitted"
    check "11 root cannot start another program under crossnode-net's type; only the boot image does" $? "$O"
fi
echo
echo "== RESULT: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]

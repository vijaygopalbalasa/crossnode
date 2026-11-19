#!/bin/bash
# Record a security policy for the confined demo pair, then save it for a secure build.
#
#   image/record-policy.sh PROJECTS_DIR
#
# PROJECTS_DIR/front and PROJECTS_DIR/central (and PROJECTS_DIR/rear for CN_LAYOUT=zones) must be built with
# CN_SECPOL=open (image/build-image.sh). In that
# mode secpolgenerate runs on each node and writes down every privilege the running system uses. This script boots
# the pair, runs what the demo and test/boot-image.sh do (read, devctl, stat, a refused path, the process and key
# checks), saves each node's policy as PROJECTS_DIR/<role>-policy.txt, then cuts power to front and boots it again
# so central's policy also covers a lost peer and the reconnect. Build the enforcing pair from the result:
#
#   CN_SECPOL=secure CN_POLICY=PROJECTS_DIR/front-policy.txt   image/build-image.sh DIR/front front MKQNXIMAGE
#   CN_SECPOL=secure CN_POLICY=PROJECTS_DIR/central-policy.txt image/build-image.sh DIR/central central MKQNXIMAGE
#
# Record on freshly built images: anything run on a recording pair becomes allowed in its policy.
set -eu
D=${1:?usage: record-policy.sh PROJECTS_DIR}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
KEY=${CN_SSH_KEY:?set CN_SSH_KEY to the private key whose public half the images were built with (mkqnximage --ssh-ident)}
ssh_to() { local p=$1; shift; ssh -i "$KEY" -p "$p" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o BatchMode=yes -o ConnectTimeout=30 root@127.0.0.1 "$@"; }
trap '"$ROOT/image/run-pair.sh" "$D" stop >/dev/null 2>&1' EXIT

if [ -d "$D/rear" ]; then
    # Zonal layout (CN_LAYOUT=zones): front, rear and central. Save the zones' policies before rear's power cut.
    trap '"$ROOT/image/run-zones.sh" "$D" stop >/dev/null 2>&1' EXIT
    "$ROOT/image/run-zones.sh" "$D"
    sleep 8
    for p in 2224 2225 2226; do
        ssh_to $p 'uname -a; uname -n; uname -r; pidin arg; pidin -f an_U; ls -ln /data/etc/crossnode.key; secpol -Q; secpol -c' >/dev/null 2>&1 || true
    done
    ssh_to 2225 'for i in 1 2 3 4 5 6 7 8 9 10; do ls /net/front/dev /net/rear/dev >/dev/null 2>&1 && break; sleep 2; done
        ls /net/front/dev; ls /net/rear/dev; cat /net/front/dev/temp; cat /net/rear/dev/radar; cat /net/front/dev/hello
        cat /net/front/etc/passwd; ls -l /net/front/dev /net/rear/dev; stat /net/rear/dev/radar
        cnprobe watch /net/front/dev/temp 3 100; cnprobe watch /net/rear/dev/radar 3 100; cnprobe open /net/rear/dev/radar
        cksum < /data/etc/crossnode-front.key; cksum < /data/etc/crossnode-rear.key; date +%T
        sh -c "for i in 1 2 3; do v=\$(cat /net/rear/dev/radar 2>&1); echo \"\$(date +%T) \$v\"; sleep 0.25; done" > /tmp/catloop.log; tail -1 /tmp/catloop.log' >/dev/null 2>&1 || true
    for p in 2224 2226; do ssh_to $p 'cksum < /data/etc/crossnode.key' >/dev/null 2>&1 || true; done
    ssh_to 2224 'cat /dev/secpolgenerate/policy' > "$D/front-policy.txt"
    ssh_to 2226 'cat /dev/secpolgenerate/policy' > "$D/rear-policy.txt"
    echo "front: $(grep -c . "$D/front-policy.txt") policy lines; rear: $(grep -c . "$D/rear-policy.txt")"
    ssh_to 2225 'rm -f /tmp/watch.log; nohup cnprobe watch /net/rear/dev/radar 400 250 > /tmp/watch.log 2>&1 &' || true
    sleep 3
    kill "$(cat "$D/rear/output/qemu.pid")"
    sleep 5
    ssh_to 2225 'cat /tmp/watch.log; cnprobe watch /net/front/dev/temp 3 100; cnprobe open /net/rear/dev/radar; cat /net/front/dev/temp; cat /net/rear/dev/radar' >/dev/null 2>&1 || true
    "$ROOT/image/run-zones.sh" "$D" rear >/dev/null
    sleep 8
    ssh_to 2225 'for i in $(seq 1 30); do cnprobe watch /net/rear/dev/radar 1 0 2>/dev/null | grep -q EOK && break; sleep 2; done
        cat /net/rear/dev/radar; pidin arg' >/dev/null 2>&1 || true
    ssh_to 2225 'cat /dev/secpolgenerate/policy' > "$D/central-policy.txt"
    echo "central: $(grep -c . "$D/central-policy.txt") policy lines"
    TYPES='crossnode_agent_t|crossnode_net_t|hellodev_t|zonesensor_t'
    for f in front rear central; do
        echo "== $f: rules for the Crossnode and sensor types"
        awk -v T="$TYPES" '$0 ~ "=== Rules for type ("T") " {p=1; print; next} /^# ===/ {p=0} p' "$D/$f-policy.txt"
    done
    exit 0
fi

"$ROOT/image/run-pair.sh" "$D"
sleep 8
for p in 2224 2225; do
    ssh_to $p 'uname -a; pidin arg; pidin -f an_U; ls -ln /data/etc/crossnode.key; ls -l /data/etc/crossnode.key; secpol -Q' >/dev/null 2>&1 || true
done
ssh_to 2225 'for i in 1 2 3 4 5 6 7 8; do ls /net/front/dev >/dev/null 2>&1 && break; sleep 2; done
    ls /net/front/dev; cat /net/front/dev/hello; cat /net/front/etc/passwd; ls -l /net/front/dev; stat /net/front/dev/hello
    cnprobe set /net/front/dev/hello 5; cnprobe get /net/front/dev/hello; cnprobe add /net/front/dev/hello 1' >/dev/null 2>&1 || true
ssh_to 2224 'cat /dev/secpolgenerate/policy' > "$D/front-policy.txt"
echo "front: $(grep -c . "$D/front-policy.txt") policy lines"

kill "$(cat "$D/front/output/qemu.pid")"
sleep 1
ssh_to 2225 'cnprobe get /net/front/dev/hello; cnprobe open /net/front/dev/hello' >/dev/null 2>&1 || true
"$ROOT/image/run-pair.sh" "$D" >/dev/null
sleep 8
ssh_to 2225 'for i in 1 2 3 4 5 6 7 8; do cnprobe get /net/front/dev/hello 2>/dev/null | grep -q EOK && break; sleep 2; done
    cat /net/front/dev/hello' >/dev/null 2>&1 || true
ssh_to 2225 'cat /dev/secpolgenerate/policy' > "$D/central-policy.txt"
echo "central: $(grep -c . "$D/central-policy.txt") policy lines"
for f in front central; do
    echo "== $f: rules for the Crossnode types"
    awk '/=== Rules for type (crossnode_agent_t|crossnode_net_t|hellodev_t) /{p=1} /=== Rules for type/ && !/(crossnode_agent_t|crossnode_net_t|hellodev_t) /{p=0} p' "$D/$f-policy.txt"
done

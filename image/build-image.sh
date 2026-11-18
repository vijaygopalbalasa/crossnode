#!/bin/bash
# Build a QNX 8.0 x86_64 QEMU image that starts Crossnode at boot (run inside the SDP environment, in the
# build container, with mkqnximage from scripts/patch-mkqnximage.sh).
#
#   image/build-image.sh PROJECT_DIR ROLE MKQNXIMAGE [SSH_PUBKEY]
#
# ROLE "front":   a zone that owns a device. Starts hellodev (a stand-in sensor at /dev/hello) and the agent,
#                 which shares /dev/hello on its link address 10.9.0.2.
# ROLE "central": a zone that uses it. Starts crossnode-net so that /net/front appears at boot.
# Both bring up the second network interface (vtnet1) on the 10.9.0.0/24 link. The shared key is generated
# once, kept next to the project as ../crossnode-demo.key, and copied into each image (mode 600).
# Nothing from the SDP is stored in this repository; the images stay on the build machine.
#
# Confined variant: CN_SECPOL=open (record a security policy) or CN_SECPOL=secure (enforce one, CN_POLICY=FILE).
# Both services then run as the non-root user crossnode (uid 70), each under its own security type
# (crossnode_agent_t, crossnode_net_t), and the key file belongs to that user. Default CN_SECPOL=no: as before.
set -eu
DIR=${1:?usage: build-image.sh PROJECT_DIR ROLE MKQNXIMAGE [SSH_PUBKEY]}
ROLE=${2:?role front or central}
MK=${3:?path to the patched mkqnximage}
PUB=${4:-}
HERE=$(cd "$(dirname "$0")/.." && pwd)
# mkqnximage merges its start-up snippets in name order; under en_US.UTF-8 "~30" sorts before "50" and the system
# start-up would run before the policy generator (observed). The C locale gives the intended order.
export LC_ALL=C
SECPOL=${CN_SECPOL:-no}
POLICY=${CN_POLICY:-none}
SVC=70
case $SECPOL in no|open|secure) ;; *) echo "CN_SECPOL must be no, open or secure"; exit 1 ;; esac
[ "$SECPOL" != secure ] || [ -f "$POLICY" ] || { echo "CN_SECPOL=secure needs CN_POLICY=FILE"; exit 1; }
[ "$POLICY" = none ] || POLICY=$(cd "$(dirname "$POLICY")" && pwd)/$(basename "$POLICY")
BIN=$HERE/build/x86_64
for f in crossnode-net crossnode-agent hellodev cnprobe; do
    [ -x "$BIN/$f" ] || { echo "missing $BIN/$f: run make first"; exit 1; }
done
case $ROLE in
    front)   NAME=front;   LINK=10.9.0.2; MAC=52:54:00:d4:27:a2 ;;
    central) NAME=central; LINK=10.9.0.1; MAC=52:54:00:d4:27:a1 ;;
    *) echo "role must be front or central"; exit 1 ;;
esac
mkdir -p "$DIR"
KEY=$(cd "$DIR/.." && pwd)/crossnode-demo.key
[ -f "$KEY" ] || { (umask 077; head -c 32 /dev/urandom > "$KEY"); }
cd "$DIR"
if [ ! -d local ]; then
    OPTS="--type=qemu --arch=x86_64 --hostname=$NAME --tcg=yes --macaddr=$MAC --sshd-pregen=yes"
    [ -n "$PUB" ] && OPTS="$OPTS --ssh-ident=$PUB"
    "$MK" $OPTS
fi
[ "$SECPOL" = no ] || "$MK" --secpol=$SECPOL --policy=$POLICY
S=local/snippets
cat > $S/system_files.custom <<SYS
[perms=755] bin/crossnode-net=$BIN/crossnode-net
[perms=755] bin/crossnode-agent=$BIN/crossnode-agent
[perms=755] bin/hellodev=$BIN/hellodev
[perms=755] bin/cnprobe=$BIN/cnprobe
SYS
if [ "$SECPOL" = no ]; then
    OWNER="uid=0 gid=0"; RUN_DEV=""; RUN_AGENT=""; RUN_NET=""
    rm -f $S/passwd_file.custom $S/group_file.custom
else
    OWNER="uid=$SVC gid=$SVC"
    RUN_DEV="on -T hellodev_t "
    RUN_AGENT="on -T crossnode_agent_t -u $SVC:$SVC "
    RUN_NET="on -T crossnode_net_t -u $SVC:$SVC "
    echo "crossnode:x:$SVC:$SVC:Crossnode service:/:/bin/false" > $S/passwd_file.custom
    echo "crossnode::$SVC:" > $S/group_file.custom
fi
cat > $S/data_files.custom <<DAT
[type=dir dperms=755] etc
[perms=600 $OWNER] etc/crossnode.key=$KEY
DAT
{
    echo "# Crossnode starts here, at the end of system start-up (role: $ROLE)."
    echo "if_up -p -r 20 vtnet1"
    echo "ifconfig vtnet1 $LINK/24 up"
    if [ "$ROLE" = front ]; then
        echo "${RUN_DEV}/system/bin/hellodev &"
        echo "${RUN_AGENT}/system/bin/crossnode-agent -k /data/etc/crossnode.key -e /dev/hello -l $LINK -p 7777 > /tmp/crossnode-agent.log 2>&1 &"
    else
        echo "${RUN_NET}/system/bin/crossnode-net -k /data/etc/crossnode.key -g 1000 -n front -c 10.9.0.2:7777 > /tmp/crossnode-net.log 2>&1 &"
    fi
} > $S/post_start.custom
"$MK" --build
echo "image ready: $DIR/output (role $ROLE, link $LINK, secpol $SECPOL)"

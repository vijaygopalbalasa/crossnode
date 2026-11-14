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
set -eu
DIR=${1:?usage: build-image.sh PROJECT_DIR ROLE MKQNXIMAGE [SSH_PUBKEY]}
ROLE=${2:?role front or central}
MK=${3:?path to the patched mkqnximage}
PUB=${4:-}
HERE=$(cd "$(dirname "$0")/.." && pwd)
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
S=local/snippets
cat > $S/system_files.custom <<SYS
[perms=755] bin/crossnode-net=$BIN/crossnode-net
[perms=755] bin/crossnode-agent=$BIN/crossnode-agent
[perms=755] bin/hellodev=$BIN/hellodev
[perms=755] bin/cnprobe=$BIN/cnprobe
SYS
cat > $S/data_files.custom <<DAT
[type=dir dperms=755] etc
[perms=600 uid=0 gid=0] etc/crossnode.key=$KEY
DAT
{
    echo "# Crossnode starts here, at the end of system start-up (role: $ROLE)."
    echo "if_up -p -r 20 vtnet1"
    echo "ifconfig vtnet1 $LINK/24 up"
    if [ "$ROLE" = front ]; then
        echo "/system/bin/hellodev &"
        echo "/system/bin/crossnode-agent -k /data/etc/crossnode.key -e /dev/hello -l $LINK -p 7777 > /tmp/crossnode-agent.log 2>&1 &"
    else
        echo "/system/bin/crossnode-net -k /data/etc/crossnode.key -g 1000 -n front -c 10.9.0.2:7777 > /tmp/crossnode-net.log 2>&1 &"
    fi
} > $S/post_start.custom
"$MK" --build
echo "image ready: $DIR/output (role $ROLE, link $LINK)"

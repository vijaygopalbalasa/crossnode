#!/bin/sh
# Make the key for one Crossnode link and print what to put on each machine.
#
#   scripts/new-link.sh NAME ADDRESS:PORT [KEY_DIR]
#
# Writes KEY_DIR/crossnode-NAME.key (32 random bytes, mode 600; KEY_DIR defaults to /data/etc) and prints the line for
# the using machine's crossnode.conf and the command for the sharing machine. Copy the same key file to the sharing
# machine (mode 600, owned by the user its agent runs as). Refuses to overwrite an existing key.
set -eu
NAME=${1:?usage: new-link.sh NAME ADDRESS:PORT [KEY_DIR]}
ADDR=${2:?usage: new-link.sh NAME ADDRESS:PORT [KEY_DIR]}
DIR=${3:-/data/etc}
case $NAME in ""|.|..|*/*|.status) echo "new-link: bad name $NAME"; exit 2 ;; esac
case $ADDR in *:*) ;; *) echo "new-link: give ADDRESS:PORT"; exit 2 ;; esac
KEY=$DIR/crossnode-$NAME.key
[ -e "$KEY" ] && { echo "new-link: $KEY exists; not replacing it"; exit 1; }
mkdir -p "$DIR"
( umask 077; head -c 32 /dev/urandom > "$KEY" )
echo "key: $KEY (mode $(ls -l "$KEY" | cut -c1-10))"
echo
echo "on the using machine, add to crossnode.conf, then kill -HUP the crossnode-net -f supervisor:"
printf '%-8s %-18s %s\n' "$NAME" "$ADDR" "$KEY"
echo
echo "on the sharing machine (copy $KEY there first):"
echo "crossnode-agent -k $KEY -e /dev/... -l ${ADDR%:*} -p ${ADDR##*:}"

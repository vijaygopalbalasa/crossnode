#!/bin/bash
# Make a private copy of QNX SDP 8.0's mkqnximage whose QEMU images can be reached over SSH.
#
# The stock template starts sshd but leaves out sshd-auth, which OpenSSH 10 needs, so every login
# is reset during key exchange. The copy adds the binary and points sshd at it. QNX's installed
# files are not changed, and nothing from the SDP is stored in this repo.
#
# Usage (inside a shell where qnxsdp-env.sh was sourced): scripts/patch-mkqnximage.sh DEST_DIR
# Then build images with DEST_DIR/mkqnximage instead of mkqnximage.
set -eu
DEST=${1:?usage: patch-mkqnximage.sh DEST_DIR}
: "${QNX_HOST:?source qnxsdp-env.sh first}"
SRC_BIN=$QNX_HOST/../../common/bin/mkqnximage
SRC_DIR=$QNX_HOST/../../common/mkqnximage
[ -f "$SRC_BIN" ] || SRC_BIN=$(command -v mkqnximage)
mkdir -p "$DEST"
cp "$SRC_BIN" "$DEST/mkqnximage"
rm -rf "$DEST/tool_dir"
cp -R "$SRC_DIR" "$DEST/tool_dir"          # the launcher prefers a tool_dir next to itself
F=$DEST/tool_dir/inputs/system.build
grep -q 'sshd-auth' "$F" && { echo "already patched: $F"; exit 0; }
python3 - "$F" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
a = 'bin/sshd-session=usr/libexec/sshd-session\n'
b = 'SshdSessionPath /system/bin/sshd-session\n'
assert s.count(a) == 1 and s.count(b) == 1, 'template changed; check it by hand'
s = s.replace(a, a + 'bin/sshd-auth=usr/libexec/sshd-auth\n').replace(b, b + 'SshdAuthPath /system/bin/sshd-auth\n')
open(p, 'w').write(s)
PY
echo "patched copy ready: $DEST/mkqnximage"

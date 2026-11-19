#!/bin/bash
# One command from source to the zonal demo: build, make the three images that record a security policy, boot them and
# record it, build the three enforced images from it, run the 16 zonal checks, then the demo.
#
#   scripts/zonal-from-scratch.sh WORK_DIR [SSH_KEY]
#
# Needs QNX SDP 8.0 (qnxsdp-env.sh at ~/qnx800 or QNXSDP_ENV) and QEMU. WORK_DIR receives the patched mkqnximage, the
# images (WORK_DIR/zo: recording, WORK_DIR/zs: enforced) and the logs. SSH_KEY defaults to WORK_DIR/cn_demo, made if
# missing. Takes a while: six image builds and a policy recording.
#
# Where the SDP lives in a container and QEMU on the host (macOS, for example), set CN_BUILD="docker exec NAME bash -c"
# and CN_HOST_ROOT/CN_BUILD_ROOT to the same folder as seen from the host and from the container; build steps then run
# there, everything else here.
set -eu
WORK=${1:?usage: zonal-from-scratch.sh WORK_DIR [SSH_KEY]}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$WORK"; WORK=$(cd "$WORK" && pwd)
KEY=${2:-$WORK/cn_demo}
[ -f "$KEY" ] || ssh-keygen -q -t ed25519 -N '' -f "$KEY"
export CN_SSH_KEY=$KEY
ENVF=${QNXSDP_ENV:-\$HOME/qnx800/qnxsdp-env.sh}
inb() {   # inb PATH: the same path as the build side sees it
    if [ -n "${CN_BUILD:-}" ]; then echo "${1/#$CN_HOST_ROOT/$CN_BUILD_ROOT}"; else echo "$1"; fi
}
build() { # build COMMAND: run in the SDP environment (here, or through CN_BUILD)
    local cmd="export LC_ALL=C; source $ENVF >/dev/null; cd $(inb "$ROOT") && $1"
    if [ -n "${CN_BUILD:-}" ]; then $CN_BUILD "$cmd"; else bash -c "$cmd"; fi
}
image() { # image SET ROLE ENV...: one image, again if the emulated compiler crashed (build-image.sh says so)
    local set=$1 r=$2; shift 2
    for t in 1 2 3 4 5; do
        build "env $* image/build-image.sh $(inb "$WORK/$set/$r") $r $(inb "$WORK/tools/mkqnximage") $(inb "$KEY.pub") > $(inb "$WORK/$set-$r.log") 2>&1" \
            && { echo "  $set/$r built"; return 0; }
        echo "  $set/$r: try $t failed ($(tail -1 "$WORK/$set-$r.log")), again"
    done
    echo "$set/$r did not build; see $WORK/$set-$r.log"; exit 1
}
echo "== 1. Build (commit $(git -C "$ROOT" rev-parse --short HEAD))"
for t in 1 2 3 4; do build "make" > "$WORK/make.log" 2>&1 && break; grep -q 'signal 11' "$WORK/make.log" || { tail "$WORK/make.log"; exit 1; }; done
build "scripts/patch-mkqnximage.sh $(inb "$WORK/tools")" > "$WORK/patch.log" 2>&1
echo "== 2. Images that record a security policy"
for r in front rear central; do rm -rf "$WORK/zo/$r"; image zo $r CN_LAYOUT=zones CN_SECPOL=open; done
echo "== 3. Boot them, run what the demo does, save each machine's policy"
"$ROOT/image/record-policy.sh" "$WORK/zo" > "$WORK/record.log" 2>&1
grep 'policy lines' "$WORK/record.log" | sed 's/^/  /'
echo "== 4. Images that enforce it"
for r in front rear central; do rm -rf "$WORK/zs/$r"; image zs $r CN_LAYOUT=zones CN_SECPOL=secure "CN_POLICY=$(inb "$WORK/zo/$r-policy.txt")"; done
echo "== 5. The zonal checks on the enforced set"
CN_EXPECT=secure "$ROOT/test/zonal.sh" "$WORK/zs" | grep -E '^(PASS|FAIL)|RESULT'
echo "== 6. The demo"
"$ROOT/demo/zonal-demo.sh" "$WORK/zs"

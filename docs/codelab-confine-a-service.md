id: confine-a-service-with-secpol
title: Confine a QNX 8.0 Service with a Security Policy You Record
summary: Run a small resource manager as a non-root user under its own security type, record its policy from real use, and enforce it from the boot image so that root can neither change the policy nor borrow the type.
categories: Security
tags: intermediate
platforms: QEMU
published: 2026-11-19
featured: false
status: published
authors: Vijaygopal Balasa
feedback_link: https://github.com/qnx/codelabs/issues


# Confine a QNX 8.0 Service with a Security Policy You Record

## Overview
Duration: 3:00

On QNX 8.0 every process runs under a security type, and a security policy says what each type may do. In this
codelab you take a tiny resource manager that serves `/dev/hello` and:

1. run it as an ordinary user (uid 71) under its own type, `hellodev_t`;
2. let `secpolgenerate` record what it actually does while you use it;
3. compile that recording into the boot image and boot with the policy **enforced** and **immutable**;
4. check that root cannot load a different policy or start a program under `hellodev_t`;
5. check that the checks themselves fail on an image without the policy, so a pass means something.

### What you'll need

- A QNX Everywhere licence and QNX SDP 8.0 installed on a Linux host (or a Linux container), with `qnxsdp-env.sh`
  sourced.
- QEMU (`qemu-system-x86_64`).
- `mkqnximage` from the SDP. To log in over SSH you need an image whose sshd can authenticate; the script
  `scripts/patch-mkqnximage.sh` in the [Crossnode repository](https://github.com/vijaygopalbalasa/crossnode) makes a
  copy of `mkqnximage` that adds `sshd-auth` to the image (your SDP installation is not changed).
- About 40 minutes.

### What you'll learn

- `on -T TYPE -u UID:GID` to start a program under a type and a user
- `mkqnximage --secpol=open` to record and `--secpol=secure --policy=FILE` to enforce
- `/dev/secpolgenerate/policy`, `secpol -Q`, `secpol -f capability -t TYPE`
- how to prove a confinement check can fail

## Build the sample service
Duration: 5:00

The sample is `hellodev`, about 110 lines of C: a resource manager that attaches `/dev/hello` with
`resmgr_attach`, answers `read` with `hello from <hostname>`, and keeps one integer for three `devctl` commands.
It is in the Crossnode repository under `tools/` (MIT licence).

```bash
git clone https://github.com/vijaygopalbalasa/crossnode && cd crossnode
source ~/qnx800/qnxsdp-env.sh
mkdir -p ~/lab && qcc -Vgcc_ntox86_64 -O2 -Wall -o ~/lab/hellodev tools/hellodev.c
file ~/lab/hellodev     # an x86_64 QNX executable
```

## Make a recording image
Duration: 10:00

Create an image project with SSH access and turn on **recording** mode. In this mode the system runs normally and
`secpolgenerate` writes down every permission each type uses.

```bash
scripts/patch-mkqnximage.sh ~/tools                 # once: a copy of mkqnximage whose images accept SSH logins
ssh-keygen -t ed25519 -N '' -f ~/.ssh/lab
export LC_ALL=C                                     # keeps mkqnximage's start-up snippets in the same order on every host
mkdir -p ~/lab/rec && cd ~/lab/rec
~/tools/mkqnximage --type=qemu --arch=x86_64 --hostname=lab --tcg=yes --sshd-pregen=yes --ssh-ident=$HOME/.ssh/lab.pub
~/tools/mkqnximage --secpol=open --policy=none
```

Now tell the image about the service. Three snippet files do it: the binary, a user and group for it, and the
line that starts it under its type:

```bash
cat > local/snippets/system_files.custom <<EOF
[perms=755] bin/hellodev=$HOME/lab/hellodev
EOF
printf 'sensor:x:71:71:Sample service:/:/bin/false\n' > local/snippets/passwd_file.custom
printf 'sensor::71:\n' > local/snippets/group_file.custom
cat > local/snippets/post_start.custom <<EOF
on -T hellodev_t -u 71:71 /system/bin/hellodev &
EOF
~/tools/mkqnximage --build
```

Check that nothing in `output/build` is empty before you go on (`find output/build -maxdepth 1 -type f -size 0`
should print nothing other than `qemu_options`). If a generated file is empty, delete `output/build` and build again.

## Boot it and record the policy
Duration: 8:00

```bash
qemu-system-x86_64 -cpu max -m 1G -smp 2 -display none -daemonize \
  -serial file:output/serial.log -pidfile output/qemu.pid \
  -kernel output/ifs.bin -drive file=output/disk-qemu.vmdk,if=ide,id=drv0 \
  -netdev user,id=lan,hostfwd=tcp:127.0.0.1:2230-:22 -device virtio-net-pci,netdev=lan \
  -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0
until grep -q "Startup complete" output/serial.log; do sleep 2; done
alias lab='ssh -i ~/.ssh/lab -p 2230 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@127.0.0.1'
```

Use the service the way it will be used for real, because only what happens now ends up allowed:

```bash
lab 'pidin -f an_U | grep hellodev'      # uid 71, type hellodev_t
lab 'cat /dev/hello'                     # hello from lab
lab 'secpol -Q'                          # policy id:1 (the recording policy, not immutable)
lab 'cat /dev/secpolgenerate/policy' > ~/lab/policy.txt
awk '$0 ~ "=== Rules for type hellodev_t " {p=1; print; next} /^# ===/ {p=0} p' ~/lab/policy.txt
kill "$(cat output/qemu.pid)"
```

The `awk` line prints the rules recorded for `hellodev_t`. In our run they were exactly what the service did:

```
# === Rules for type hellodev_t ============
allow_attach hellodev_t {
    /dev/hello
};
allow hellodev_t self:ability {
    nonroot
    pathspace
    prot_exec
    map_fixed
    public_channel
};
```

Everything not listed will be denied once the policy is enforced.

## Build the enforcing image
Duration: 8:00

Build a **fresh** project (anything run on the recording image is already in the recording, so do not reuse it),
same snippets, but `--secpol=secure` with your policy:

```bash
mkdir -p ~/lab/enf && cd ~/lab/enf
~/tools/mkqnximage --type=qemu --arch=x86_64 --hostname=lab --tcg=yes --sshd-pregen=yes --ssh-ident=$HOME/.ssh/lab.pub
~/tools/mkqnximage --secpol=secure --policy=$HOME/lab/policy.txt
cp ~/lab/rec/local/snippets/*.custom local/snippets/
~/tools/mkqnximage --build
```

Boot it with the same `qemu-system-x86_64` command as before (from `~/lab/enf`).

## Check the confinement
Duration: 5:00

```bash
lab 'pidin -f an_U | grep hellodev'                     # still uid 71, type hellodev_t
lab 'cat /dev/hello'                                    # still answers
lab 'secpol -Q'                                         # the policy id, marked (immutable)
lab 'secpolpush; secpol -Q'                             # root's attempt to push a policy is refused; same id after
lab 'on -T hellodev_t -u 71:71 uname -n; echo rc=$?'    # root cannot start a program under hellodev_t
lab 'secpol -f capability -t hellodev_t'                # the type's abilities, ending in EOL deny
```

What to look for:

What our run printed:

```
system/bin/hellodev    ...    hellodev_t    71
hello from lab
policy id:88dd520c (immutable)
secpolpush: failed to push policy: Operation not permitted
policy id:88dd520c (immutable)
on: Operation not permitted (uname)
rc=1
hellodev_t  ...
    PATHSPACE allow root non-root inherit lock
    PROT_EXEC allow root non-root inherit lock
    MAP_FIXED allow root non-root inherit lock
    PUBLIC_CHANNEL allow root non-root inherit lock
    EOL deny root non-root inherit lock
allow_attach hellodev_t /dev/hello;
```

- `pidin` shows `hellodev_t` and uid 71: the service is not root.
- `secpol -Q` says `(immutable)`, and the policy id is the same before and after `secpolpush` (your id will differ).
- `on -T hellodev_t ...` prints `on: Operation not permitted`, so root cannot borrow the type.
- The capability list ends in `EOL deny`: every ability not listed is denied.

## Prove the checks can fail
Duration: 5:00

A check that cannot fail proves nothing. Boot the **recording** image again (`~/lab/rec`, policy not enforced) and
run the same commands:

```bash
lab 'secpol -Q'                                         # policy id:1, not immutable
lab 'on -T hellodev_t -u 71:71 uname -n; echo rc=$?'    # prints lab and rc=0: this time it succeeds
```

If the enforcing image passes and the recording image fails the same checks, your checks are telling you something.
Put them in a script and run both, the way the Crossnode repo's `test/zonal.sh` does with `CN_EXPECT=secure` and a
deliberately unconfined build.

## What you learned
Duration: 2:00

- A service on QNX 8.0 can run as an ordinary user under its own security type, started with `on -T`.
- `mkqnximage --secpol=open` records what a system really does; `--secpol=secure --policy=FILE` enforces it from the
  boot image, and the running system reports it immutable.
- Root is not exempt: it cannot push another policy or start programs under a confined type.
- Every confinement check should have a run that fails it.

Further reading: the QNX 8.0 documentation for `secpolgenerate`, `secpol`, `secpolpush`, `on` and `mkqnximage`; the
[Crossnode repository](https://github.com/vijaygopalbalasa/crossnode) applies the same steps to three machines and
five services.

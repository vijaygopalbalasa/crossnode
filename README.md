# Crossnode

Reach a device on another QNX 8.0 machine by its path, with the programs you already have.

```
nodex# cat /net/nodey/dev/hello
hello from nodey
nodex# stty -a < /net/nodey/dev/ser1      # the same output as running stty on nodey itself
```

QNX 7 had Qnet: every machine's `/dev` and files appeared under `/net/<node>/`, and an unmodified program could
open, read, write and devctl a device on another machine. QNX 8.0 removed Qnet together with io-pkt and lists
its alternative as "N/A". Crossnode brings the path-based part back for QNX 8.0, as two small user-space
programs, secure by default: a machine shares only the paths it lists, only root (or one named group) may use a
peer's paths, and when a peer dies every waiting caller gets a clean error within a stated limit instead of
hanging.

Status: gate 1 of the build (November 2026). What works is listed below, with the test run that shows it.
What does not work yet is listed too.

## What works today

Each row names the checks that show it. Check numbers are the ones printed in the transcripts: two-node
(`evidence/gate1-twonode-*.txt`, two QNX 8.0 machines on a direct link), loopback (`evidence/gate1-loopback-*.txt`,
one machine) and boot image (`evidence/boot-image-*.txt`). The values quoted are from the runs
`gate1-twonode-20261118T201732Z`, `gate1-loopback-20261118T201529Z` and `boot-image-20261118T201902Z`.

| Behaviour | Checks | What the transcript shows |
|---|---|---|
| Unmodified programs `open`, `read`, `write` and `devctl` a device on the other machine | two-node 1, 2; loopback 1, 2 | nodey sees the value nodex set (`info=7 value=42`); `stty -a` through `/net` gives the same 13 lines as on nodey |
| Generic `devctl`: data goes and comes back by the command's direction bits; the info value travels | loopback 1 | `cat` and `devctl` set, add and get give identical results locally and through `/net` |
| 200 KB each way across the link, byte-exact | two-node 2b; loopback 15a, 15b | the same `cksum` on both sides |
| A read that waits on a live peer keeps waiting, while other callers are served | two-node 2c-1; loopback 16a, 16b | another caller is answered in 4 ms during a 10 s wait |
| A peer that loses power or freezes fails every waiting call with `EHOSTDOWN` within the 2 s liveness limit | two-node 4, 5a; loopback 5 | frozen: `call_ms=1954`; power cut: `call_ms=1951` (bound 2000+200 ms) |
| A peer whose agent is killed fails waiting calls at once | loopback 4, 7 | `EHOSTDOWN`, 18 ms after the kill |
| A dead peer fails a new open within the 1.5 s connect limit, and the next one at once | two-node 6a to 6c; loopback 6a, 6b | `ms=1518`, then `ms=0` |
| A signal (Ctrl-C) to a waiting caller returns `EINTR` at once, and the work on the peer is stopped | two-node 2c-2, 3, 3b; loopback 10, 14a, 14b, 16c to 16e | 10 ms; the agent's thread count goes 3, 4, 3 as the work starts and is stopped |
| `close` and process exit never wait on the peer | two-node 5b, 5c; loopback 8a, 8b, 9 | close 1 ms, exit 1 ms with the peer powered off |
| The peer reboots and `/net/<name>` works again, with no restart on the using machine | two-node 7 | `op=get rc=EOK` after the reboot |
| Only listed paths are shared; links are resolved and must stay inside a share; names outside all answer `EACCES` | loopback 3, 3b to 3e, 19f, 20e; boot image 5 | `cat: /net/front/etc/passwd: Permission denied` |
| Only root, or members of the group given with `-g`, may use `/net/<name>` | loopback 12a, 12b | any other user is refused before anything is forwarded |
| Both ends must prove they hold a shared key (HMAC-SHA256 challenge, no key on the wire); no key, a wrong key, or an agent that cannot prove it are refused, and each refusal is logged | loopback 22a to 22f; `test/auth-vectors.c` (RFC 4231 vectors) | `rc=EHOSTDOWN` for each refused case; a key file others can read is refused |
| Strangers cannot hold the handshake: at most 2 handshakes wait per address, each for about 2 s, and authenticated peers still get in (at most 4 per address) | loopback 17, 23a, 23b, 24a to 24c | six trickling strangers: none is held longer than 2052 ms |
| `ls`, `ls -l`, `stat` and `fstat` on a peer's paths match the peer's own; a directory above a share lists only the way to the shares | two-node 2d-1 to 2d-3; loopback 19a to 19h, 20a to 20h | the stock `ls -l` prints the same through `/net` as locally |
| A waiting `stat` blocks nobody else, and a signal interrupts only the caller it was sent to | loopback 21a to 21e | two waiting stats: the signalled one gets `EINTR`, the other `EHOSTDOWN` |
| Replies that break the protocol never reach a program | loopback 18a to 18d | from `test/fake-agent.py`, a deliberately broken agent |
| A pair built with `image/build-image.sh` boots already connected, with the key readable by root only | boot image 1 to 4 | `hello from front` on central with no command typed; key `-rw-------` |

Evidence: `evidence/gate1-loopback-*.txt` (69 checks, one machine over loopback) and
`evidence/gate1-twonode-*.txt` (19 checks, two machines on a direct link). Each transcript names the commit it
ran on. The times in them are behaviour bounds on emulated machines, not performance figures.

## Not done yet

- `lseek` on a file, `pread` and `pwrite` answer `ESPIPE` or `ENOSYS` rather than pretending (a directory can be rewound).
- `stat` follows links, so `ls -l` shows a link inside a share as the file it points to.
- No `mmap`, shared memory, or server-to-client events (`select`, `ionotify`).
- Programs that find a server by name (`name_open`) rather than by path are not covered.
- One peer per `crossnode-net` process.
- Authentication proves both ends hold a shared key; the traffic after it is not encrypted or integrity-protected, so use a protected link.
- Built and tested for x86_64 under QEMU. The base SDP has no QEMU startup for aarch64, so a mixed pair is not shown.

## Requirements

- QNX Software Development Platform 8.0 (the free non-commercial licence works) with `qcc` (gcc 12.2).
- Target: QNX 8.0, x86_64. Tested on QEMU 11.1.3 guests built with `mkqnximage --type=qemu --arch=x86_64`
  (QEMU on macOS needs `-cpu max`).
- For the test scripts: bash, ssh, perl and python3 on the host.

## Build

```
source ~/qnx800/qnxsdp-env.sh
make                     # build/x86_64/{crossnode-net,crossnode-agent,hellodev,cnprobe}
make ARCH=aarch64le      # the same for ARM
```

## Run

On the machine that shares (here nodey, link address 10.9.0.2):

```
./hellodev &                                                     # optional test device at /dev/hello
./crossnode-agent -k /etc/crossnode.key -e /dev/hello -e /dev/ser1 -l 10.9.0.2 -p 7777 &
```

On the machine that uses it (here nodex):

```
./crossnode-net -k /etc/crossnode.key -n nodey -c 10.9.0.2:7777 -g 1000 &              # nodey appears as /net/nodey
cat /net/nodey/dev/hello
```

Both programs need `-k KEYFILE` (the same 16 or more random bytes on both machines, mode 600, for example `head -c 32 /dev/urandom > /etc/crossnode.key`) or, for a throwaway test, `-I` (no authentication, on both). `crossnode-agent -e PATH` shares PATH and everything under it; `-l` and `-p` choose the listening address and
port. `crossnode-net -n NAME -c ADDRESS:PORT` mounts the peer at `/net/NAME`; `-g GID` lets that group use it
besides root; `-m DIR` changes `/net`.

## Boot image

`image/build-image.sh` builds two QNX 8.0 QEMU images with `mkqnximage` that start Crossnode at boot, so a
pair comes up already connected: `front` (a stand-in sensor at `/dev/hello` and the agent) and `central`
(`crossnode-net`, so `/net/front` exists). A random shared key is generated once and copied into each image as
`/data/etc/crossnode.key`, mode 600. `image/run-pair.sh` boots the pair on a direct link, and
`test/boot-image.sh` checks that, with no command typed, central lists and reads front's device
(`evidence/boot-image-*.txt`). The images stay on the build machine; nothing from QNX is in this repository.

## How it works

```
 program on nodex                nodex                              nodey
 open("/net/nodey/dev/hello") -> crossnode-net  --- TCP --->  crossnode-agent -> /dev/hello
                                 (resource manager on          (opens the real path,
                                  /net/nodey)                   one worker per request)
```

- `crossnode-net` is a resource manager on `/net/<name>`. Its handlers never wait on the network: each request
  goes to a pending table, the handler returns `_RESMGR_NOREPLY`, and an I/O thread that owns the socket rings a
  pulse when the answer arrives, so the dispatch thread replies to the waiting client.
- The I/O thread sends a liveness probe every 500 ms and declares the peer down after 2 s of silence; it wakes
  exactly at each deadline. A connection attempt has 1.5 s.
- A signal to a waiting client arrives as an unblock pulse; the client gets `EINTR` at once and the peer is told
  to stop the work.
- `crossnode-agent` answers liveness probes on the connection's reader thread and runs every other request on a
  worker thread, so a slow device never looks like a dead machine. A worker's device call is interrupted with a
  signal when its caller gives up, its handle is closed or the connection ends.
- Each open is resolved with `realpath()` and must land inside a share. As a second check, the agent asks the opened
  descriptor where it really points (`iofdinfo`); no test yet wins the race that this second check is there for.

Wire format: `src/cn_proto.h` (a 28-byte header and a payload; both ends little-endian).

## Tests

```
test/twonode-up.sh IMAGES_DIR             # boot nodex and nodey in QEMU with a direct link
test/gate1-loopback.sh 2222               # 69 checks on one machine
test/gate1-twonode.sh IMAGES_DIR          # 19 checks across the two machines
```

Each suite refuses to run on a stale build, pushes the binaries to the guests over SSH, and saves its
transcript under `evidence/`. `scripts/patch-mkqnximage.sh` makes a copy of `mkqnximage` whose QEMU images
accept SSH logins (the stock template leaves out `sshd-auth`).

## Licence

MIT (see `LICENSE`). This repository contains no QNX files: building it needs your own QNX SDP 8.0.

# Crossnode

Reach a device on another QNX 8.0 machine by its path, with the programs you already have.

Simulated sensors on QEMU guests (`t` is seconds since the sensor started):

```
central# ls /net/front/dev /net/rear/dev
/net/front/dev:
hello
temp

/net/rear/dev:
radar
central# cat /net/front/dev/temp /net/rear/dev/radar
cabin-temp(sim) value=22 C t=20
rear-radar(sim) value=60 cm t=20
```

QNX 7 had Qnet: every machine's `/dev` and files appeared under `/net/<node>/`, and an unmodified program could
open, read, write and devctl a device on another machine. QNX 8.0 discontinued Qnet ("Qnet code was in the io-pkt
package, which has been removed"; alternative "N/A", [QNX 8.0 discontinuation notice](https://www.qnx.com/developers/docs/relnotes8.0/com.qnx.doc.discontinuation_notice/topic/networking.html)).
Crossnode brings the path-based part back for QNX 8.0 as two small user-space programs:

- A machine shares only the paths it lists, and only root or one named group may use a peer's paths.
- Each link has its own key. Both ends prove they hold it when they connect, and every frame after that carries an
  HMAC-SHA256 tag with a frame counter, checked at both ends. The tests change and replay replies on the wire and
  show them refused. The traffic is not encrypted.
- When a peer goes silent, every waiting caller gets a clean error instead of hanging, and the path works again when
  the peer comes back, with nothing restarted.
- Both programs, and the simulated sensors, can run as non-root users under a QNX security policy that is enforced
  and cannot be changed while the system runs.

Crossnode has no AI component. It is infrastructure: the point is a non-obvious use of QNX's own resource manager,
message passing, boot image and security policy features to bring back something QNX 8.0 lost.

Who it is for: a QNX 7 system whose programs reach a device on another board through Qnet's `/net/<node>/` paths (a
supervisor on one board reading a sensor driver on another, for example) has no listed alternative on QNX 8.0, so
each such program would have to be rewritten against sockets. Board support follows the same line: TI's QNX SDK for
its J7 automotive SoCs says "In SDP 8.0 io-sock remains the only supported networking stack"
([TI developer note](https://software-dl.ti.com/jacinto7/esd/processor-sdk-qnx-jacinto7/latest/exports/docs/developer_notes_devnp_vs_iosock_710.html)),
while Qnet lived in io-pkt. Crossnode keeps them unchanged, because on QNX every
open, read and devctl is already a message to whichever server owns the path, and a user-space server can own a path
that stands for another machine. It is not a real-time transport: requests go over TCP, and a lost peer is noticed
after 2 s of silence. What it bounds is failure: a caller gets an error within a stated time instead of hanging.

![Three QNX 8.0 machines: front and rear zones share simulated sensors, central reads them by path](docs/architecture.svg)

## Where QNX is used

| QNX feature | Where | Exercised by |
|---|---|---|
| A resource manager on a pathname prefix (`resmgr_attach` with `_RESMGR_FLAG_DIR` on `/net/<name>`) | `src/cn_net.c`, `main` | zonal 1, two-node 1 |
| The dispatch thread never waits on the network: requests go to a pending table and the handler returns `_RESMGR_NOREPLY` | `src/cn_net.c`, `submit` | loopback 16a, 16b, two-node 2c-1 |
| A pulse as a doorbell from the network thread to the dispatch thread (`pulse_attach`, `MsgSendPulse`) | `src/cn_net.c`, `main`, `io_thread`, `on_doorbell` | loopback 16b |
| Replies built from the peer's answer (`MsgReply`, `MsgReplyv`, `MsgError`) | `src/cn_net.c`, `complete` | two-node 1, 2b |
| Unblock handling: a signal to a waiting client becomes an unblock pulse and the client gets `EINTR` at once (`cf.unblock`, `iof.unblock`, `resmgr_unbind`) | `src/cn_net.c`, `connect_unblock`, `io_unblock`, `cancel_waiting`, `drop_opening_ocb` | loopback 10, 14a, 14b, 21a to 21e, two-node 3, 3b |
| `stat` and `fstat`, including `stat` by name inside a combined message (`iofunc_stat_default`) | `src/cn_net.c`, `cn_stat`, `complete` | loopback 19a to 19h, two-node 2d-2 |
| Directory reads (`_IO_XTYPE_READDIR`) | `src/cn_net.c`, `cn_read`; `src/cn_agent.c`, `do_readdir` (`fdopendir`) | loopback 20a to 20h |
| Generic `devctl` forwarded by the command's direction bits | `src/cn_agent.c`, `do_devctl` | loopback 1, two-node 1 |
| Path containment (`realpath`), then the opened descriptor is asked where it points (`iofdinfo`) | `src/cn_agent.c`, `resolve`, `do_open` | loopback 3, 3b to 3e |
| A blocked device call interrupted with a targeted signal (`pthread_kill`) | `src/cn_agent.c`, `cancel_job_locked` | loopback 16d, 16e, two-node 2c-2 |
| Security policy: one type per program, recorded with `secpolgenerate`, compiled into the boot image and enforced (`on -T`, `mkqnximage --secpol --policy`, `secpol -Q`) | `image/build-image.sh`, `image/record-policy.sh` | zonal 4 to 8, boot image 6 to 13 |
| A boot image that starts everything with no command typed (`mkqnximage` snippets, `waitfor`) | `image/build-image.sh` | boot image 1 to 3, zonal 1 |
| Simulated sensors as resource managers | `tools/zonesensor.c`, `tools/hellodev.c` | zonal 2, 3 |

## Shown by tests

| What | One line from the transcripts (check, run) |
|---|---|
| Unmodified programs read, write and devctl a device on another machine; `stty -a` through `/net` prints the same 13 lines as on the other machine | PASS 2 [cabin-temp(sim) value=19 C t=15 / rear-radar(sim) value=88 cm t=16], `zonal-20261119T024608Z`<br>PASS 2 [13 lines], `gate1-twonode-20261119T021933Z` |
| A peer that loses power: the waiting call fails with `EHOSTDOWN` after about 2 s of silence (test bound 2000 + 200 ms), and the other zone keeps answering | PASS 10 [12 readings, then result=stopped rc=EHOSTDOWN call_ms=1755 t_end=43421, bound 2000+200 ms], `zonal-20261119T024608Z`<br>PASS 11 [3 readings and hello], `zonal-20261119T024608Z` |
| The peer boots again and its path works with nothing restarted on the using machine | PASS 14 [pids before: 413714 438292  after: 413714 438292], `zonal-20261119T024608Z` |
| Every frame after the handshake carries an integrity tag: a reply changed or replayed on the wire is refused (requests are tagged and checked the same way in `src/cn_agent.c`, `reader`; no test changes one yet); a build that skips the check fails this check | PASS 25a [op=get rc=EHOSTDOWN info=-1 value=0 flips=1 integrity=1], `gate1-loopback-20261119T021328Z`<br>FAIL 25a [op=get rc=EOK info=7 value=43 flips=1 integrity=0], `gate1-loopback-mutation-20261119T021618Z` |
| Confined: non-root programs under their own security types; the policy is immutable, root's `secpolpush` is refused, and root cannot start a program under crossnode-net's type | PASS 5 [front: hellodev_t 71 zonesensor_t 71 crossnode_agent_ 70 ; rear: zonesensor_t 71 crossn...], `zonal-20261119T024608Z`<br>PASS 7 [3 of 3 refused], `zonal-20261119T024608Z`<br>PASS 13 [on: Operation not permitted (/system/bin/hellodev) on: Operation not permitted (/bin/tr...], `boot-image-20261119T023406Z` |
| A key per link, and only listed paths are shared | PASS 9 [checksums front 4223641969, rear 3478472649, central 4223641969 and 3478472649], `zonal-20261119T024608Z`<br>PASS 3 [cat: /net/self/etc/passwd: Permission denied], `gate1-loopback-20261119T021328Z` |

Every check, with what its transcript shows: [docs/EVIDENCE.md](docs/EVIDENCE.md). All transcripts directly in
`evidence/` ran at commit `aa53a3c` (earlier runs are in `evidence/history/`); each names its commit. The power-cut
checks (zonal 10 to 14), the confinement checks (zonal 4 to 8) and the integrity checks (loopback 25a, 25b) each have
a deliberately broken run that fails them. The times are pass or fail against bounds stated in the tests, for this code on emulated machines, not
performance figures for QNX.

## Why not Qnet, NFS, SSHFS or plain sockets?

| Option | What it gives | What it does not cover (by design or protocol; not a test of QNX) |
|---|---|---|
| Qnet | QNX 7's `/net/<node>/` for any resource manager | Discontinued in QNX 8.0, alternative "N/A" (notice linked above) |
| NFS v3 (QNX 8.0 has [`fs-nfs3`](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/f/fs-nfs3.html) and [`nfsd`](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/n/nfsd.html)) | Remote files | In the NFS v3 protocol (RFC 1813, section 3.3) there is no device-control procedure, and a device file is only a type and two numbers, so a sensor's `devctl` or `stty` has no way to reach the other machine's driver. We did not evaluate QNX's NFS. |
| SSHFS over a FUSE port | Remote files over SSH | Files only: no devices, no `devctl` |
| Plain sockets | Anything, per program | Every program needs new code; stock tools such as `cat`, `ls` and `stty` cannot use it |
| Crossnode | `open`, `read`, `write`, `devctl`, `stat` and `readdir` on a peer's listed paths, from unmodified programs | User space; the limits are below |

## Confined: non-root under an enforced security policy

With `CN_SECPOL`, `image/build-image.sh` runs both Crossnode programs as the user `crossnode` (uid 70) and the
simulated sensors as `sensor` (uid 71), each under its own security type. `CN_SECPOL=open` builds images where
`secpolgenerate` records what the running system uses; `image/record-policy.sh` boots them, runs the demo and the
checks (including a power cut and a reboot) and saves each machine's policy; `CN_SECPOL=secure` compiles that policy
into the boot image. There it is enforced and immutable: `secpol -Q` prints `(immutable)`, and root's `secpolpush`
(which reloads `/proc/boot/secpol.bin`) is refused (zonal 6 and 7, boot image 9 and 10). The rules were recorded from the demo, not hand-designed or minimised. What they
give the Crossnode and sensor types (`image/crossnode-types-zones.secpol`):

| Type | Runs as | May attach | May connect to | Other abilities granted (recorded) |
|---|---|---|---|---|
| `crossnode_net_t` | `crossnode-net` on central, uid 70 | `/net/front` and `/net/rear` only | `io_sock_t` (the network stack), `pipe_t`, `random_t` | `nonroot`, `pathspace`, `public_channel`, `network/jailroot`; `prot_exec`, `map_fixed` (recorded for every program here; not shown to be required) |
| `crossnode_agent_t` | `crossnode-agent` on front and rear, uid 70 | nothing | its own zone's sensors (`hellodev_t`, `zonesensor_t`), `io_sock_t`, `random_t` | `nonroot`, `network/jailroot`; `prot_exec`, `map_fixed` (as above) |
| `hellodev_t`, `zonesensor_t` | the simulated sensors, uid 71 | each only its own device (`/dev/hello`, `/dev/temp`, `/dev/radar`) | nothing | `nonroot`, `pathspace`, `public_channel`; `prot_exec`, `map_fixed` (as above) |

Every ability not listed is denied.

## Zonal demo: three machines

`CN_LAYOUT=zones` adds a rear zone. Front shares a simulated cabin temperature (`/dev/temp`) and a test device
(`/dev/hello`); rear shares a simulated radar (`/dev/radar`, `tools/zonesensor.c`), whose reading moves every second.
Central reaches each zone over its own direct link and mounts both, one `crossnode-net` per zone, each with that
link's key. `image/run-zones.sh` boots the three. Nothing is typed on the machines after power-on; `test/zonal.sh`
and `demo/zonal-demo.sh` then run commands on central over SSH from the host. Rear loses power while central reads
it; the read fails with `EHOSTDOWN` after about 2 s of silence, front keeps answering, and when rear boots again
central reads it with the same `crossnode-net` processes.

On the enforced set all 14 checks pass (`evidence/zonal-20261119T024608Z.txt`). Two deliberately broken runs show the checks can
fail: `CN_MUTATE=1` cuts front instead of rear, leaves rear down and kills central's services, on the open
(policy-recording) set where root may do that, and checks 10 to 14 fail (`evidence/zonal-mutation-20261119T024711Z.txt`);
`CN_MUTATE=confinement` runs the secure checks against a set built without the policy, and checks 4 to 8 fail
(`evidence/zonal-mutation-confinement-20261119T025158Z.txt`). The demo shows the failure with a plain `cat` loop, which prints "Host is down".

## Reproduce

Needs QNX SDP 8.0 (the free non-commercial licence works), a Linux shell where `qnxsdp-env.sh` is sourced, and QEMU
(on macOS QEMU needs `-cpu max`, which the scripts pass). The toolchain is SDP 8.0's `qcc` (the Makefile calls
`qcc -Vgcc_ntox86_64`); the target is x86_64 QEMU guests built with `mkqnximage`. The images stay on your machine;
nothing from QNX is in this repository.

```
source ~/qnx800/qnxsdp-env.sh && make                       # build/x86_64: the two programs, the sensors and the test tools
scripts/patch-mkqnximage.sh ~/tools                         # a copy of mkqnximage whose images accept SSH logins
ssh-keygen -t ed25519 -N '' -f ~/.ssh/cn_demo && export CN_SSH_KEY=~/.ssh/cn_demo
for r in front rear central; do CN_LAYOUT=zones CN_SECPOL=open image/build-image.sh ~/zo/$r $r ~/tools/mkqnximage ~/.ssh/cn_demo.pub; done
image/record-policy.sh ~/zo                                 # boots, runs the demo, saves ~/zo/<role>-policy.txt
for r in front rear central; do CN_LAYOUT=zones CN_SECPOL=secure CN_POLICY=~/zo/$r-policy.txt image/build-image.sh ~/zs/$r $r ~/tools/mkqnximage ~/.ssh/cn_demo.pub; done
CN_EXPECT=secure test/zonal.sh ~/zs                         # the zonal checks on the enforced set
demo/zonal-demo.sh ~/zs                                     # the demo
```

The pair (`front` and `central` without `CN_LAYOUT`) is built the same way and checked with `test/boot-image.sh`;
the loopback and two-node suites use two plain test machines (`test/twonode-up.sh`).

## Run by hand

On the machine that shares (here front, link address 10.9.0.2):

```
./hellodev &                                                     # optional test device at /dev/hello
./crossnode-agent -k /etc/crossnode-front.key -e /dev/hello -e /dev/ser1 -l 10.9.0.2 -p 7777 &
```

On the machine that uses it (here central):

```
./crossnode-net -k /etc/crossnode-front.key -n front -c 10.9.0.2:7777 -g 1000 &     # front appears as /net/front
cat /net/front/dev/hello
```

Both programs need `-k KEYFILE` (the same 16 or more random bytes at both ends of a link, mode 600, for example
`(umask 077; head -c 32 /dev/urandom > /etc/crossnode-front.key)`) or, for a throwaway test, `-I` on both, which has no handshake
and no frame tags. `crossnode-agent -e PATH` shares PATH and everything under it; `-l` and `-p` choose the listening
address and port. `crossnode-net -n NAME -c ADDRESS:PORT` mounts the peer at `/net/NAME`; `-g GID` lets that group
use it besides root; `-m DIR` changes `/net`.

## How it works

```
 program on central              central                             front
 open("/net/front/dev/hello") -> crossnode-net  --- TCP --->  crossnode-agent -> /dev/hello
                                 (resource manager on          (opens the real path,
                                  /net/front)                   one worker per request)
```

- `crossnode-net` is a resource manager on `/net/<name>`. Its handlers never wait on the network: each request goes
  to a pending table, the handler returns `_RESMGR_NOREPLY`, and an I/O thread that owns the socket rings a pulse
  when the answer arrives, so the dispatch thread replies to the waiting client.
- The I/O thread sends a liveness probe every 500 ms and declares the peer down after 2 s of silence; a connection
  attempt has 1.5 s.
- A signal to a waiting client arrives as an unblock pulse; the client gets `EINTR` at once and the peer is told to
  stop the work.
- `crossnode-agent` answers liveness probes on the connection's reader thread and runs every other request on a
  worker thread, so a slow device never looks like a dead machine. A worker's device call is interrupted with a
  signal when its caller gives up, its handle is closed or the connection ends.
- Each open is resolved with `realpath()` and must land inside a share. As a second check, the agent asks the opened
  descriptor where it really points (`iofdinfo`).
- After the handshake, each direction has its own key, derived from both nonces, and every frame carries
  HMAC-SHA256 over a per-direction frame counter, its header and its payload (`src/cn_auth.h`). `crossnode-net` tags
  whole frames as they move to the socket, under the lock that orders them; the agent tags replies under the
  connection's lock. A tag that does not match drops the connection.

Wire format: `src/cn_proto.h`, version 2: a 28-byte header, the payload and, on a keyed link after the handshake, a
32-byte tag; both ends little-endian.

## Limits

- The traffic is not encrypted: on an untrusted network it can be read, though not changed or replayed unnoticed.
  `-I` links have no handshake and no tags. The man-in-the-middle tests (loopback 25a, 25b) change and replay replies
  only; the agent's check of request tags is in the code (`src/cn_agent.c`, `reader`) but not shown by a test.
- The handshake caps (2 waiting handshakes per address, 4 authenticated connections per address) can drop a
  legitimate handshake from an address that is also flooding.
- `crossnode-net` has one dispatch thread per process and one peer per process (central runs one per zone).
- For a directory, the second check compares device and inode of a second open made with `O_DIRECTORY` (the
  descriptor `fdopendir` needs), not `iofdinfo`.
- A device or FIFO is checked by name before and after a second resolve, not through an opened descriptor (opening
  it would run its open handler), so a path swapped in between is narrowed but not ruled out.
- `stat` follows links, so `ls -l` shows a link inside a share as the file it points to.
- `lseek`, `pread` and `pwrite` on a file answer `ESPIPE` or `ENOSYS` rather than pretending (a directory can be
  rewound).
- No `mmap`, shared memory, or server-to-client events (`select`, `ionotify`); programs that find a server by name
  (`name_open`) rather than by path are not covered.
- The loopback and two-node suites run the services as root on plain test machines; the confined, enforced build is
  shown on the boot-image pair and the zonal set. The policy is recorded from the demo, so a use the recording did
  not see is denied until it is recorded too.
- The caller's priority does not travel: on the sharing machine a request runs at `crossnode-agent`'s own priority,
  whatever the caller's priority on the using machine.
- Built and tested for x86_64 under QEMU; a mixed x86_64 and aarch64 pair is not shown.

## Tests

```
test/twonode-up.sh IMAGES_DIR                           # boot two plain test machines (nodex, nodey) with a direct link
test/gate1-loopback.sh 2222                             # 71 checks on one machine
CN_MUTATE=1 test/gate1-loopback.sh 2222                 # the same with a crossnode-net that skips the frame check: 25a and 25b fail
test/gate1-twonode.sh IMAGES_DIR                        # 19 checks across the two machines
CN_EXPECT=none|open|secure test/boot-image.sh DIR       # the pair: 5 checks, 13 when confined and enforced
CN_EXPECT=secure test/zonal.sh DIR                      # the three-machine zonal set: 14 checks when confined and enforced
cc -o /tmp/auth-vectors test/auth-vectors.c && /tmp/auth-vectors   # HMAC-SHA256 against RFC 4231 vectors, on the host
```

Each suite pushes or boots the current build, saves its transcript under `evidence/`, and names the commit it ran on.
All scripts that reach the machines need `CN_SSH_KEY`.

## Licence

MIT (see `LICENSE`). This repository contains no QNX files: building it needs your own QNX SDP 8.0.

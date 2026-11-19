# Crossnode

Reach a device on another QNX 8.0 machine by its path, with the programs you already have (blocking `open`, `read`,
`write`, `devctl` and `stat`; the [limits](#limits) list what is not covered).

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
  HMAC-SHA256 tag with a frame counter, checked at both ends in the code. The tests change and replay replies on the
  wire and show them refused. The traffic is not encrypted.
- The caller's priority travels with each request. The other machine does the work at that priority, never above a
  cap its operator sets, so a priority 30 caller is still answered while priority 12 work fills every CPU on either
  machine. A build that drops the priority fails that test.
- `select`, `poll` and `ionotify` wait for input on another machine's device: a program waiting on
  `/net/<peer>/dev/ser2` wakes when a line is typed on that machine's serial port.
- When a peer goes silent, every waiting caller gets a clean error instead of hanging, and the path works again when
  the peer comes back, with nothing restarted. `cat /net/<peer>/.status` says whether the link is up, authenticated,
  how many calls wait, and how often it dropped or refused a frame, each with its last reason.
- One file lists every peer (`crossnode-net -f`). That process supervises and holds no key (only each key file's
  path and a hash of it, to see a changed key on reload): each peer gets its own process, which reads only its own
  key, so one peer that hangs cannot hold up another; a peer process that dies is started
  again, and `kill -HUP` re-reads the file (new peers start, removed ones stop, the others are left alone).
  `scripts/new-link.sh NAME ADDRESS:PORT` makes a link's key and prints both ends' lines.
- An ordinary program written the Qnet way runs unchanged: `examples/serialwatch.c` (open, `tcsetattr`, `select`,
  `read`; no Crossnode code) sets another machine's serial port to 115200 baud through `/net` and prints the lines
  typed on it.
- Both programs, and the simulated sensors, can run as non-root users under a QNX security policy that is enforced
  and cannot be changed while the system runs.

QNX's own documentation describes Qnet as "deployed as a network of tightly coupled trusted machines"
([QNX 7.1 System Architecture, Native Networking](https://www.qnx.com/developers/docs/7.1/com.qnx.doc.neutrino.sys_arch/topic/qnet.html)).
Crossnode does not trust the network: per-link keys, tagged frames, non-root programs under an immutable policy, and
one process per peer, so one hung peer cannot hold up another.

Who it is for: systems built from several QNX boards that talk through Qnet's `/net/<node>/` paths, such as
distributed control systems, test rigs and lab instruments with a supervisor on one board and drivers on others,
and multi-board controllers. Their users noticed. On the OpenQNX forum, when the QNX 7.1 notes said Qnet would be
removed in a future release, the maintainer of a QNX multi-port serial driver, who has "exploited it since 1987", described
reaching serial ports on remote machines through Qnet instead of running serial cables around buildings and
factories, named wind farms and even cars and motorbikes as places where linking remote CPUs matters, and wrote
"Arguably QNX ain't QNX without Qnet" ([thread, 2020](https://forums.openqnx.com/t/topic/8120)); Crossnode's tests run the stock `stty -a` on
another machine's serial port through `/net`, and wait with `select` on it until a line is typed there. A 2025 write-up of Qnet notes it is gone in 8.0 and hopes it returns
([blog](https://linguini1.github.io/blog/2025/06/03/qnet.html)). QNX 8.0 lists no drop-in
replacement for those paths, so each such program would otherwise move to sockets or to middleware such as SOME/IP
or DDS. Crossnode keeps them unchanged, because on QNX every open, read and devctl is already a message to whichever
server owns the path, and a user-space server can own a path that stands for another machine.

Why a real-time OS matters here: the receiving thread of a QNX server inherits the priority of the client whose
message it handles. Crossnode extends that across the wire: the caller's priority goes in each request and the other
machine's worker runs at it, so important work stays ahead of less important work on both machines. It is still not a
hard real-time transport: requests go over TCP, the network stack itself runs at priority 21 in these images, and a
lost peer is noticed after 2 s of silence, and requests share one TCP stream in the order they were sent. What it
bounds is failure; what it carries is the caller's priority for the work on the other machine. The demo's
radar and temperature sensors are labelled stand-ins: the fit is supervision, diagnostics and configuration across
boards, not a sensor's real-time data path.

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
| The caller's priority, as the kernel gave it to the receiving thread (`ctp->info.priority`), sent with the request and used for the worker on the other machine (`PTHREAD_EXPLICIT_SCHED`), capped by `-P` | `src/cn_net.c`, `submit`; `src/cn_agent.c`, job start | priority 1 to 4 |
| A file answered by the resource manager itself, never forwarded (`/net/<peer>/.status`) | `src/cn_net.c`, `cn_open`, `status_text` | loopback 25c, 26a to 26d, zonal 15 |
| A stale unblock pulse told apart from a live one (`MsgInfo`, `_NTO_MI_UNBLOCK_REQ`) | `src/cn_net.c`, `cancel_waiting` | loopback 21a to 21e, two-node 3, 3b |
| `select`, `poll` and `ionotify` on a peer's device: the caller is armed (`iofunc_notify`), the peer's worker waits with `poll` on the real descriptor, and its answer wakes the caller (`iofunc_notify_trigger`) | `src/cn_net.c`, `cn_notify`, `notify_peer`, `complete`; `src/cn_agent.c`, `do_notify` | two-node 8a to 8e |
| Path containment (`realpath`), then the opened descriptor is asked where it points (`iofdinfo`) | `src/cn_agent.c`, `resolve`, `do_open` | loopback 3, 3b to 3e |
| A blocked device call interrupted with a targeted signal (`pthread_kill`) | `src/cn_agent.c`, `cancel_job_locked` | loopback 16d, 16e, two-node 2c-2 |
| Security policy: one type per program, recorded with `secpolgenerate`, compiled into the boot image and enforced (`on -T`, `mkqnximage --secpol --policy`, `secpol -Q`) | `image/build-image.sh`, `image/record-policy.sh` | zonal 4 to 8, boot image 6 to 13 |
| Boot image configuration: images that start everything with no command typed (`mkqnximage` snippets, `waitfor`) | `image/build-image.sh` | boot image 1 to 3, zonal 1 |
| Simulated sensors as resource managers | `tools/zonesensor.c`, `tools/hellodev.c` | zonal 2, 3 |

## Shown by tests

| What | One line from the transcripts (check, run) |
|---|---|
| Unmodified programs read, write and devctl a device on another machine; `stty -a` through `/net` prints the same 13 lines as on the other machine | PASS 2 [cabin-temp(sim) value=23 C t=11 \| rear-radar(sim) value=123 cm t=11], `zonal-20261119T124324Z`<br>PASS 2 [13 lines], `gate1-twonode-20261119T123328Z` |
| The caller's priority travels: with priority 12 work on both CPUs of either machine, a priority 30 call is answered within the bound; a build that does not carry the priority fails | PASS 2 [rc=EOK, inside the bound], PASS 4 [rc=EOK, inside the bound], `priority-20261119T123935Z`<br>FAIL 2 [rc=EOK, outside the bound], `priority-mutation-20261119T124030Z` |
| A peer that loses power: the waiting call fails with `EHOSTDOWN` after about 2 s of silence (test bound 2000 + 200 ms), and the other zone keeps answering | PASS 10 [12 readings, then result=stopped rc=EHOSTDOWN (time cut), bound 2000+200 ms], `zonal-20261119T124324Z`<br>PASS 11 [3 readings and hello], `zonal-20261119T124324Z` |
| The peer boots again and its path works with nothing restarted; its status file tells what happened | PASS 14 [pids before: 430095 446485  after: 430095 446485], PASS 15 [connects 2;drops 1;last down no answer within the liveness limit;], `zonal-20261119T124324Z` |
| `select` on another machine's serial port wakes when a line is typed there; at most 16 waiters per link; a build that does not forward it fails | PASS 8b [op=select rc=EOK ready=1 read=EOK data=typed-on-nodey-ser2_], PASS 8c [eagain=4 timedout=16], `gate1-twonode-20261119T123328Z`<br>FAIL 8b [op=select rc=EOK ready=0], `gate1-twonode-mutation-notify-20261119T123755Z` |
| Every frame after the handshake carries an integrity tag: a reply or a request changed on the wire is refused; a build that skips the check fails | PASS 25a [op=get rc=EHOSTDOWN info=-1 value=0 (time cut); flips=1 integrity=1], `gate1-loopback-20261119T122843Z`<br>FAIL 25a [op=get rc=EOK info=7 value=43 (time cut); flips=1 integrity=0], `gate1-loopback-mutation-20261119T123104Z` |
| Confined: non-root programs under their own security types; the policy is immutable, root's `secpolpush` is refused, and root cannot start a program under crossnode-net's type | PASS 5 [front: hellodev_t 71 zonesensor_t 71 crossnode_agent_ 70 ; rear: zonesensor_t 71 crossnode_agent_ 70], `zonal-20261119T124324Z`<br>PASS 7 [3 of 3 refused], `zonal-20261119T124324Z`<br>PASS 13 [on: Operation not permitted (/system/bin/hellodev) on: Operation not permitted ...], `boot-image-20261119T124303Z` |
| A key per link, central's links from one file, and only listed paths are shared | PASS 9 [checksums front 3631666464, rear 1329987501, central 3631666464 and 1329987501], PASS 16 [2 processes; peers in the file: front rear], `zonal-20261119T124324Z`<br>PASS 3 [cat: /net/self/etc/passwd: Permission denied], `gate1-loopback-20261119T122843Z` |

Every check, with what its transcript shows: [docs/EVIDENCE.md](docs/EVIDENCE.md). All transcripts directly in
`evidence/` ran at commit `8b04ac9` (earlier runs, including the first submission's, are in `evidence/history/`); each
names its commit. Every important check has a deliberately broken build or run that fails it: the frame check
(loopback 25a to 25d), priority carrying (priority 2), the I/O thread's priority (priority 4), the liveness limit
(two-node 4, 5a, 5b), `select` forwarding (two-node 8b to 8e), the power cut (zonal 10 to 16) and confinement (zonal 4 to 8). The times are pass or fail
against bounds stated in the tests, for this code on emulated machines, not performance figures for QNX.

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
| `crossnode_net_t` | `crossnode-net` on central, uid 70 | `/net/front` and `/net/rear` only | `io_sock_t` (the network stack), `pipe_t`, `random_t` | `nonroot`, `pathspace`, `public_channel`, `network/jailroot`, `fork` (one process per peer from `-f`); `prot_exec`, `map_fixed` (recorded for every program here; not shown to be required) |
| `crossnode_agent_t` | `crossnode-agent` on front and rear, uid 70 | nothing | its own zone's sensors (`hellodev_t`, `zonesensor_t`), `io_sock_t`, `random_t` | `nonroot`, `network/jailroot`; `prot_exec`, `map_fixed` (as above) |
| `hellodev_t`, `zonesensor_t` | the simulated sensors, uid 71 | each only its own device (`/dev/hello`, `/dev/temp`, `/dev/radar`) | nothing | `nonroot`, `pathspace`, `public_channel`; `prot_exec`, `map_fixed` (as above) |

Every ability not listed is denied.

## Zonal demo: three machines

`CN_LAYOUT=zones` adds a rear zone. Front shares a simulated cabin temperature (`/dev/temp`) and a test device
(`/dev/hello`); rear shares a simulated radar (`/dev/radar`, `tools/zonesensor.c`), whose reading moves every second.
Central reaches each zone over its own direct link and mounts both from one file, `/data/etc/crossnode.conf`, with
a supervising `crossnode-net` and one process per zone, each keeping only that link's key in memory (all run as
the same user under the same type, so this is not a boundary between peers on disk). `image/run-zones.sh` boots the three. Nothing is typed on the machines after power-on; `test/zonal.sh`
and `demo/zonal-demo.sh` then run commands on central over SSH from the host. Rear loses power while central reads
it; the read fails with `EHOSTDOWN` after about 2 s of silence, front keeps answering, and when rear boots again
central reads it with the same `crossnode-net` processes.

On the enforced set all 16 checks pass (`evidence/zonal-20261119T124324Z.txt`). Two deliberately broken runs show the checks can
fail: `CN_MUTATE=1` cuts front instead of rear, leaves rear down and kills central's services, on the open
(policy-recording) set where root may do that, and checks 10 to 16 fail (`evidence/zonal-mutation-20261119T124409Z.txt`);
`CN_MUTATE=confinement` runs the secure checks against a set built without the policy, and checks 4 to 8 fail
(`evidence/zonal-mutation-confinement-20261119T124459Z.txt`). The demo shows the failure with a plain `cat` loop, which prints "Host is down".

## Reproduce

Needs QNX SDP 8.0 (the free non-commercial licence works), a Linux shell where `qnxsdp-env.sh` is sourced, and QEMU
(on macOS QEMU needs `-cpu max`, which the scripts pass). The toolchain is SDP 8.0's `qcc` (the Makefile calls
`qcc -Vgcc_ntox86_64`); the target is x86_64 QEMU guests built with `mkqnximage`. The images stay on your machine;
nothing from QNX is in this repository.

One command does all of it, from source to the demo (six image builds and a policy recording, so it takes a while):

```
scripts/zonal-from-scratch.sh ~/cn-work                     # build, record the policy, enforce it, 16 zonal checks, the demo
```

Where the SDP runs in a container and QEMU on the host, set `CN_BUILD`, `CN_HOST_ROOT` and `CN_BUILD_ROOT` (see the
script's header). The same steps by hand:

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
cat /net/front/.status
```

With several peers, list them in a file and start once:

```
# /etc/crossnode.conf: name, address:port, key file
front    10.9.0.2:7777    /etc/crossnode-front.key
rear     10.10.0.2:7777   /etc/crossnode-rear.key
```

```
./crossnode-net -f /etc/crossnode.conf -r 30 -g 1000 &     # /net/front and /net/rear, one process each, supervised
kill -HUP <the supervisor's pid>                             # after editing the file: add, remove or change peers
```

Both programs need `-k KEYFILE` (the same 16 or more random bytes at both ends of a link, mode 600, for example
`(umask 077; head -c 32 /dev/urandom > /etc/crossnode-front.key)`) or, for a throwaway test, `-I` on both, which has no handshake
and no frame tags. `crossnode-agent -e PATH` shares PATH and everything under it; `-l` and `-p` choose the listening
address and port; `-P CAP` runs each request at its caller's priority up to CAP (default: the agent's own priority,
so a peer can never raise work above it), and `-r PRIO` sets the priority of the threads that read the links.
`crossnode-net -n NAME -c ADDRESS:PORT` mounts the peer at `/net/NAME`; `-g GID` lets that group use it besides root;
`-m DIR` changes `/net`; `-r PRIO` runs the link's I/O thread at PRIO, so lower-priority work on the using machine
cannot hold the link up.

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
  stop the work. A pulse left over from a call already answered is told apart with `MsgInfo` and ignored.
- Priority: the dispatch thread runs each request at its caller's priority (QNX does that for message passing), and
  `crossnode-net` writes that priority into the request. The agent starts the request's worker at it, capped by `-P`.
- `crossnode-agent` answers liveness probes on the connection's reader thread and runs every other request on a
  worker thread, so a slow device never looks like a dead machine. A worker's device call is interrupted with a
  signal when its caller gives up, its handle is closed or the connection ends.
- Each open is resolved with `realpath()` and must land inside a share. As a second check, for a file the agent asks
  the opened descriptor where it really points (`iofdinfo`); directories and devices are checked as the limits say.
- After the handshake, each direction has its own key, derived from both nonces, and every frame carries
  HMAC-SHA256 over a per-direction frame counter, its header and its payload (`src/cn_auth.h`). `crossnode-net` tags
  whole frames as they move to the socket, under the lock that orders them; the agent tags replies under the
  connection's lock. A tag that does not match drops the connection.

Wire format: `src/cn_proto.h`, version 2: a 28-byte header, the payload and, on a keyed link after the handshake, a
32-byte tag; both ends little-endian.

## Limits

- The traffic is not encrypted: on an untrusted network it can be read, though not changed or replayed unnoticed.
  `-I` links have no handshake and no tags.
- The handshake caps (2 waiting handshakes per address, 4 authenticated connections per address) can drop a
  legitimate handshake from an address that is also flooding.
- `crossnode-net` has one dispatch thread per process and one peer per process (`-f` starts one per peer). Peers are
  listed, not discovered.
- For a directory, the second check compares device and inode of a second open made with `O_DIRECTORY` (the
  descriptor `fdopendir` needs), not `iofdinfo`.
- A device or FIFO is checked by name before and after a second resolve, not through an opened descriptor (opening
  it would run its open handler), so a path swapped in between is narrowed but not ruled out.
- `stat` follows links, so `ls -l` shows a link inside a share as the file it points to.
- Not forwarded: `chmod`, `chown` and `utime` (refused), and `unlink`, `rename`, `mkdir`, `link` and private
  messages (`_IO_MSG`), which have no handler. `/net/<peer>` is for using devices and files, not managing them.
- The worker's priority is set once, when it starts; a priority boost the caller gets later, and the caller's
  scheduling policy, are not carried. Locally a QNX server thread follows its client's priority as it changes.
- `lseek`, `pread` and `pwrite` on a file answer `ESPIPE` or `ENOSYS` rather than pretending (a directory can be
  rewound).
- No `mmap` or shared memory. `select`, `poll` and `ionotify` cover input only (output always reads as ready, since a
  write waits on the peer anyway). At most 16 opens per link can wait for input at once (`CN_MAX_NOTIFY`, enforced at
  both ends); the next `select` fails with `EAGAIN` instead of taking room ordinary calls need. A wait stays out at
  the peer until input comes or the descriptor is closed. Programs that find a server by name (`name_open`) rather than by path are not
  covered.
- The loopback and two-node suites run the services as root on plain test machines; the confined, enforced build is
  shown on the boot-image pair and the zonal set. The policy is recorded from the demo, so a use the recording did
  not see is denied until it is recorded too.
- Priority keeps order, not deadlines: the link is TCP, and the network stack (`io-sock`) runs at priority 21 in
  these images, so work above 21 on either machine can still delay the link. The agent starts a thread per request.
- Built and tested for x86_64 under QEMU; a mixed x86_64 and aarch64 pair is not shown.

## Tests

```
test/twonode-up.sh IMAGES_DIR                           # boot two plain test machines (nodex, nodey) with a direct link
test/gate1-loopback.sh 2222                             # 81 checks on one machine
CN_MUTATE=1 test/gate1-loopback.sh 2222                 # builds that skip the frame check: 25a to 25d fail
test/gate1-twonode.sh IMAGES_DIR                        # 24 checks across the two machines
CN_MUTATE=notify test/gate1-twonode.sh IMAGES_DIR       # a crossnode-net that never forwards select: 8b to 8e fail
CN_MUTATE=liveness test/gate1-twonode.sh IMAGES_DIR     # a crossnode-net without the liveness limit: 4, 5a and 5b fail
test/priority.sh IMAGES_DIR                             # 4 checks: the caller's priority on both machines
CN_MUTATE=1|central test/priority.sh IMAGES_DIR         # priority not carried: 2 fails; no -r on the caller's side: 4 fails
CN_EXPECT=none|open|secure test/boot-image.sh DIR       # the pair: 5 checks, 13 when confined and enforced
CN_EXPECT=secure test/zonal.sh DIR                      # the three-machine zonal set: 17 checks when confined and enforced
cc -o /tmp/auth-vectors test/auth-vectors.c && /tmp/auth-vectors   # SHA-256, HMAC-SHA256 and frame-tag vectors, on the host
```

Each suite pushes or boots the current build, saves its transcript under `evidence/`, and names the commit it ran on.
All scripts that reach the machines need `CN_SSH_KEY`.

## Tutorial

[docs/codelab-confine-a-service.md](docs/codelab-confine-a-service.md) walks through the security-policy part on its own:
run one QNX 8.0 resource manager as a non-root user under its own type, record its policy from real use, enforce it
from the boot image, and check that the checks fail on an image without it. Every step was run on QEMU.

## Licence

MIT (see `LICENSE`). This repository contains no QNX files: building it needs your own QNX SDP 8.0.

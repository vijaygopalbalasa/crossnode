# Evidence: every claim and the checks that show it

All transcripts below ran at commit `24e8610`; later commits change only documentation and comments. Check numbers
are the ones printed in the transcripts under `evidence/`: loopback (one machine), two-node (two plain test machines
on a direct link), priority (the same two machines), boot image (the pair built by `image/build-image.sh`) and zonal
(three machines). Runs:

- `gate1-loopback-20261119T144602Z` (81 of 81); `gate1-loopback-mutation-20261119T144831Z` (`crossnode-net` and
  `crossnode-agent` built to skip the frame check: 25a, 25b, 25c and 25d fail).
- `gate1-twonode-20261119T145100Z` (24 of 24); `gate1-twonode-mutation-liveness-20261119T145249Z` (no liveness limit:
  4, 5a and 5b fail); `gate1-twonode-mutation-notify-20261119T145603Z` (no `select` forwarding: 8b to 8e fail).
- `priority-20261119T145756Z` (4 of 4); `priority-mutation-20261119T145900Z` (priority not carried: check 2 fails);
  `priority-mutation-central-20261119T145958Z` (`crossnode-net` started without `-r`: check 4 fails). Checks 2 and 4 are timed inside the caller's machine against a 1000 ms bound, and the transcript records `io-sock` at priority 21 on both machines. At an earlier
  commit, one priority run in a release sequence failed check 4 once while four reruns passed; that transcript,
  `evidence/history/priority-20261119T110726Z`, is kept. The cause was not found (a lost frame on the emulated link
  while both CPUs are busy would make TCP wait to resend).
- `boot-image-20261119T150055Z` (default pair, 5 of 5), `boot-image-20261119T150114Z` (confined, policy recording,
  8 of 8), `boot-image-20261119T150133Z` (confined and enforced, 13 of 13).
- `zonal-20261119T150155Z` (confined and enforced, 16 of 16); `zonal-mutation-20261119T150242Z` (`CN_MUTATE=1`:
  checks 10 to 17 fail); `zonal-mutation-confinement-20261119T150351Z` (secure checks against a set built without the
  policy: checks 4 to 8 fail).
- `zonal-20261119T151118Z` (17 of 17): the same checks run by `scripts/zonal-from-scratch.sh` from an empty folder
  (build, record the policy, enforce it, check, demo).
- `auth-vectors-24e8610` (SHA-256 against the FIPS 180-4 examples, HMAC-SHA256 against RFC 4231, the frame tag).

Earlier runs, including the submitted version's (`aa53a3c`), are in `evidence/history/`.

Times are pass or fail against the bound each test states, for this code on QEMU guests. They are not performance
figures for QNX. Each bound adds 200 ms to a limit of our own code for process start and scheduling on an emulated
guest.

| Behaviour | Checks | What the transcript shows |
|---|---|---|
| Unmodified programs `open`, `read`, `write` and `devctl` a device on the other machine | two-node 1, 2; loopback 1, 2; zonal 2 | the other machine sees the value this one set (`info=7 value=42`); `stty -a` through `/net` gives the same 13 lines as on the other machine; `cat` prints both zones' (sim) sensor lines |
| Generic `devctl`: data goes and comes back by the command's direction bits; the info value travels | loopback 1 | `cat` and `devctl` set, add and get give identical results locally and through `/net` |
| 200 KB each way across the link, byte-exact | two-node 2b; loopback 15a, 15b | the same `cksum` on both sides |
| A read that waits on a live peer keeps waiting, while other callers are served | two-node 2c-1; loopback 16a, 16b | the other caller is answered while the 10 s read is still waiting |
| A peer that freezes or loses power fails every waiting call with `EHOSTDOWN` after 2 s of silence | two-node 4, 5a; loopback 5, 21b; zonal 10 | within the bound of 2000 + 200 ms in every run; in two-node 5a to 5c three callers use the peer when it loses power |
| A peer whose agent is killed fails waiting calls at once | loopback 4, 7 | within the bound of 200 ms |
| A dead peer fails a new open within the 1.5 s connect limit, and the next one at once | two-node 6a to 6c; loopback 6a, 6b; zonal 12 | within 1500 + 200 ms, then within 100 ms (zonal 12 checks only that the open fails with `EHOSTDOWN` instead of hanging) |
| A signal (Ctrl-C) to a waiting caller returns `EINTR` at once, and the work on the peer is stopped | two-node 2c-2, 3, 3b; loopback 10, 14a, 14b, 16c to 16e | the signal is sent 300 ms into the call and `EINTR` returns within 300 + 200 ms; the agent's worker count goes back to where it was |
| `close` and process exit never wait on the peer | two-node 5b, 5c; loopback 8a, 8b, 9 | with the peer powered off, `close` within 100 ms and exit within 200 ms |
| The peer reboots and `/net/<name>` works again, with nothing restarted on the using machine | two-node 7; zonal 13, 14 | `rc=EOK` after the reboot; the same `crossnode-net` process ids before and after |
| While one zone is down, the other zone keeps answering | zonal 11 | three readings and `hello from front` while rear is off |
| Only listed paths are shared; links are resolved and must stay inside a share; names outside answer `EACCES` | loopback 3, 3b to 3e, 19f, 20e; boot image 5 | `cat: /net/front/etc/passwd: Permission denied` |
| Only root, or members of the group given with `-g`, may use `/net/<name>` | loopback 12a, 12b | any other user is refused before anything is forwarded |
| Both ends of a link must prove they hold its key at connect (HMAC-SHA256 challenge, no key on the wire); no key, a wrong key, or an agent that cannot prove it are refused, and each refusal is logged | loopback 22a to 22f; `test/auth-vectors.c` | `rc=EHOSTDOWN` for each refused case; a key file others can read is refused |
| Every frame after the handshake carries an integrity tag: a reply changed on the wire, or sent twice, is refused and logged, and never reaches the program | loopback 25a, 25b | with `tools/cnmitm.c` in the middle: the flipped reply gives `EHOSTDOWN` instead of a wrong value; the copy of a reply is logged as an integrity failure |
| A request changed on the wire is refused by the agent: the device keeps its value | loopback 25d | one bit flipped in a devctl SET of 42: the call fails with `EHOSTDOWN`, the device still reads 7, and the agent logs an integrity failure |
| A refused frame shows in the link's status file | loopback 25c | `refused     1 frame, last: a frame failed its integrity check` and `state       down` |
| The integrity checks can fail | loopback 25a, 25b, 25c, 25d with `CN_MUTATE=1` | with a `crossnode-net` built to skip the check, the program reads 43 for 42 and the copy goes unnoticed: both FAIL |
| Each link has its own key, and a zone holds only its own | zonal 9 | front's and rear's key checksums differ; central's copy for each link matches that zone's |
| Strangers cannot hold the handshake: at most 2 handshakes wait per address, each for about 2 s, and authenticated peers still get in (at most 4 per address) | loopback 17, 23a, 23b, 24a to 24c | six trickling strangers are all dropped near the 2 s limit |
| `ls`, `ls -l`, `stat` and `fstat` on a peer's paths match the peer's own; a directory above a share lists only the way to the shares | two-node 2d-1 to 2d-3; loopback 19a to 19h, 20a to 20h; zonal 1 | the stock `ls -l` prints the same through `/net` as locally; central lists only `hello temp` and `radar` |
| A waiting `stat` blocks nobody else, and a signal interrupts only the caller it was sent to | loopback 21a to 21e | two waiting stats: the signalled one gets `EINTR`, the other `EHOSTDOWN` |
| Replies that break the protocol never reach a program | loopback 18a to 18d | from `test/fake-agent.py`, a deliberately broken agent |
| A pair or a zonal set built with `image/build-image.sh` boots already connected | boot image 1 to 3; zonal 1, 2 | `hello from front` on central with nothing typed after power-on |
| Confined: the Crossnode programs run as uid 70 and the sensors as uid 71, each under its own security type | boot image 6 to 8; zonal 4, 5 | `crossnode_net_t 70`, `crossnode_agent_t 70`, `hellodev_t 71`, `zonesensor_t 71` |
| Every machine runs a policy that cannot be changed while it runs | boot image 9, 10; zonal 6, 7 | `secpol -Q` prints `(immutable)`; root's `secpolpush`, which reloads `/proc/boot/secpol.bin`, prints `failed to push policy`, and the policy id is the same after |
| The agent's type may attach no path and offer no public channel; every ability not listed is denied | boot image 11 | its capability list has no `PATHSPACE` and ends in `EOL deny` |
| `crossnode-net`'s type may attach exactly its `/net` paths | boot image 12; zonal 8 | `allow_attach crossnode_net_t /net/front;` and, in the zonal set, `/net/rear` |
| Root cannot start programs under `crossnode-net`'s type | boot image 13 | two attempts (a test device and `true`): `on: Operation not permitted` both times |
| The confinement checks can fail | zonal 4 to 8 with `CN_MUTATE=confinement` | against a set built without the policy, all five FAIL |
| The caller's priority travels: with priority 12 work on both of the sharing machine's CPUs, a priority 30 caller is answered within the bound, while a priority 5 caller waits for the work to end | priority 1, 2, 3 | `rc=EOK, inside the bound`; the control: `rc=EOK, after the hogs stopped` |
| Priority on the caller's own machine: with priority 12 work on both of its CPUs, the link's I/O thread (`-r 30`) still serves a priority 30 caller | priority 4 | `rc=EOK, inside the bound` (one run at an earlier commit failed this once; see above) |
| The priority checks can fail | priority 2 with `CN_MUTATE=1`; priority 4 with `CN_MUTATE=central` | without carrying the priority: `rc=EOK, outside the bound`; without `-r`: `rc=EHOSTDOWN, outside the bound` (the starved I/O thread let the link look dead) |
| `/net/<name>/.status` says up or down, authenticated, waiting calls, connects, drops and refused frames with their last reasons; it is read-only, answered locally, and works with the peer down or never reached | loopback 26a to 26d; zonal 15 | after rear's power cut and reboot: `connects    2`, `drops       1`, `last down   no answer within the liveness limit`, `refused     0 frames` |
| One file lists every peer (`crossnode-net -f`): a bad file is refused before anything is attached; each peer gets its own process, and an unreachable peer does not hold up another | loopback 27a, 27b; zonal 16 | `named twice`, a missing key and the name `..` refused; `confa EOK, confb EHOSTDOWN, confa again EOK`; on central, both processes started by one `-f /data/etc/crossnode.conf` |
| `select`, `poll` and `ionotify` on a peer's device: a program waiting on another machine's serial port wakes when a line is typed there; with nothing typed it times out; a wait wakes when the link fails | two-node 8a, 8b, 8e | `ready=1 read=EOK data=typed-on-nodey-ser2_`; `ready=0`; `ready=1 read=EHOSTDOWN` |
| At most 16 `select` waiters per link (both ends enforce it): the 17th gets `EAGAIN` at once and ordinary calls still go through | two-node 8c | `eagain=4 timedout=16` with 20 waiters, and the devctl after them `rc=EOK` |
| An ordinary POSIX program with no Crossnode code (`examples/serialwatch.c`: `open`, `tcsetattr`, `select`, `read`), written the way a Qnet-era program watches another machine's serial port, runs unchanged through `/net` | two-node 8d | it sets nodey's port to 115200 baud (nodey's own `stty` reports `baud=115200`) and prints the line typed on it |
| The supervisor (`crossnode-net -f`): a peer process that dies is started again; `kill -HUP` adds and removes peers and leaves the others alone; under the enforced policy too | loopback 27c, 27d; zonal 17 | `confa EOK from` a new pid; `confc EOK, confb gone, confa` same pid; on central, rear back from a new pid after its process is killed |
| The `select` checks can fail | two-node 8b to 8e with `CN_MUTATE=notify` | `ready=0` where a line was typed; `eagain=0 timedout=20`; no wake on link failure |
| The liveness checks can fail | two-node 4, 5a, 5b with `CN_MUTATE=liveness` | the waiting calls never end with `EHOSTDOWN` (`call_ms=` is empty) |
| The power-cut checks can fail | zonal 10 to 17 with `CN_MUTATE=1` | 7 FAIL lines, each for the reason the mutation causes |

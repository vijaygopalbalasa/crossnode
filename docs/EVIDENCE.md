# Evidence: every claim and the checks that show it

All transcripts below ran at commit `aa53a3c`; later commits changed only the README, this file and the
demo's captions. Check numbers are the ones printed in the transcripts under `evidence/`: loopback (one machine),
two-node (two plain test machines on a direct link), boot image (the pair built by `image/build-image.sh`) and zonal
(three machines). Runs: `gate1-loopback-20261119T021328Z` (71 of 71), `gate1-loopback-mutation-20261119T021618Z` (crossnode-net built to skip the frame check: 25a and 25b fail),
`gate1-twonode-20261119T021933Z` (19 of 19), `boot-image-20261119T022324Z` (default pair, 5 of 5), `boot-image-20261119T023406Z` (confined and enforced pair, 13 of 13), `zonal-20261119T024608Z`
(zonal, confined and enforced, 14 of 14), `zonal-mutation-20261119T024711Z` (zonal with `CN_MUTATE=1`: checks 10 to 14 fail) and `zonal-mutation-confinement-20261119T025158Z`
(zonal secure checks against a set built without the policy: checks 4 to 8 fail), and `auth-vectors-aa53a3c` (HMAC-SHA256 against
RFC 4231 vectors). Earlier runs from the build-up are in `evidence/history/`.

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
| The integrity checks can fail | loopback 25a, 25b with `CN_MUTATE=1` | with a `crossnode-net` built to skip the check, the program reads 43 for 42 and the copy goes unnoticed: both FAIL |
| Each link has its own key, and a zone holds only its own | zonal 9 | front's and rear's key checksums differ; central's copy for each link matches that zone's |
| Strangers cannot hold the handshake: at most 2 handshakes wait per address, each for about 2 s, and authenticated peers still get in (at most 4 per address) | loopback 17, 23a, 23b, 24a to 24c | six trickling strangers are all dropped near the 2 s limit |
| `ls`, `ls -l`, `stat` and `fstat` on a peer's paths match the peer's own; a directory above a share lists only the way to the shares | two-node 2d-1 to 2d-3; loopback 19a to 19h, 20a to 20h; zonal 1 | the stock `ls -l` prints the same through `/net` as locally; central lists only `hello temp` and `radar` |
| A waiting `stat` blocks nobody else, and a signal interrupts only the caller it was sent to | loopback 21a to 21e | two waiting stats: the signalled one gets `EINTR`, the other `EHOSTDOWN` |
| Replies that break the protocol never reach a program | loopback 18a to 18d | from `test/fake-agent.py`, a deliberately broken agent |
| A pair or a zonal set built with `image/build-image.sh` boots already connected | boot image 1 to 3; zonal 1, 2 | `hello from front` on central with nothing typed after power-on |
| Confined: the Crossnode programs run as uid 70 and the sensors as uid 71, each under its own security type | boot image 6 to 8; zonal 4, 5 | `crossnode_net_t 70`, `crossnode_agent_t 70`, `hellodev_t 71`, `zonesensor_t 71` |
| Every machine runs a policy that cannot be changed while it runs, and root's attempt to load another is refused | boot image 9, 10; zonal 6, 7 | `secpol -Q` prints `(immutable)`; `secpolpush` fails with `Operation not permitted` and the policy id is the same after |
| The agent's type may attach no path and offer no public channel; every ability not listed is denied | boot image 11 | its capability list has no `PATHSPACE` and ends in `EOL deny` |
| `crossnode-net`'s type may attach exactly its `/net` paths | boot image 12; zonal 8 | `allow_attach crossnode_net_t /net/front;` and, in the zonal set, `/net/rear` |
| Root cannot start programs under `crossnode-net`'s type | boot image 13 | two attempts (a test device and `true`): `on: Operation not permitted` both times |
| The confinement checks can fail | zonal 4 to 8 with `CN_MUTATE=confinement` | against a set built without the policy, all five FAIL |
| The power-cut checks can fail | zonal 10 to 14 with `CN_MUTATE=1` | 5 FAIL lines, each for the reason the mutation causes |

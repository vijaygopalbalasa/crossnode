# Evidence: every claim and the checks that show it

Check numbers are the ones printed in the transcripts under `evidence/`: loopback (one machine,
`gate1-loopback-*.txt`), two-node (two plain test machines on a direct link, `gate1-twonode-*.txt`), boot image (the
pair built by `image/build-image.sh`, `boot-image-*.txt`) and zonal (three machines, `zonal-*.txt`). Each transcript
names the commit it ran on. Runs referred to here: `gate1-loopback-20261118T201529Z` (69 of 69),
`gate1-twonode-20261118T201732Z` (19 of 19), `@@BOOTPAIR@@` (default pair, 5 of 5), `@@BOOTCONF@@` (confined pair,
@@BOOTCONF_RESULT@@), `@@ZONAL@@` (zonal, confined and enforced, @@ZONAL_RESULT@@) and `@@ZONALMUT@@` (zonal with
`CN_MUTATE=1`, where checks 9 to 13 must fail).

Times are pass or fail against the bound each test states, for this code on QEMU guests. They are not performance
figures for QNX. Each bound adds 200 ms to a limit of our own code for process start and scheduling on an emulated
guest.

| Behaviour | Checks | What the transcript shows |
|---|---|---|
| Unmodified programs `open`, `read`, `write` and `devctl` a device on the other machine | two-node 1, 2; loopback 1, 2 | the other machine sees the value this one set (`info=7 value=42`); `stty -a` through `/net` gives the same 13 lines as on the other machine |
| Generic `devctl`: data goes and comes back by the command's direction bits; the info value travels | loopback 1 | `cat` and `devctl` set, add and get give identical results locally and through `/net` |
| 200 KB each way across the link, byte-exact | two-node 2b; loopback 15a, 15b | the same `cksum` on both sides |
| A read that waits on a live peer keeps waiting, while other callers are served | two-node 2c-1; loopback 16a, 16b | the other caller is answered while the 10 s read is still waiting |
| A peer that freezes or loses power fails every waiting call with `EHOSTDOWN` after 2 s of silence | two-node 4, 5a; loopback 5; zonal 9 | within the bound of 2000 + 200 ms in every run |
| A peer whose agent is killed fails waiting calls at once | loopback 4, 7 | within the bound of 200 ms |
| A dead peer fails a new open within the 1.5 s connect limit, and the next one at once | two-node 6a to 6c; loopback 6a, 6b; zonal 11 | within 1500 + 200 ms, then within 100 ms (zonal 11 checks only that the open fails with `EHOSTDOWN` instead of hanging) |
| A signal (Ctrl-C) to a waiting caller returns `EINTR` at once, and the work on the peer is stopped | two-node 2c-2, 3, 3b; loopback 10, 14a, 14b, 16c to 16e | the signal is sent 300 ms into the call and `EINTR` returns within 300 + 200 ms; the agent's worker count goes back to where it was |
| `close` and process exit never wait on the peer | two-node 5b, 5c; loopback 8a, 8b, 9 | with the peer powered off, `close` within 100 ms and exit within 200 ms |
| The peer reboots and `/net/<name>` works again, with nothing restarted on the using machine | two-node 7; zonal 12, 13 | `rc=EOK` after the reboot; the same `crossnode-net` process ids before and after |
| While one zone is down, the other zone keeps answering | zonal 10 | three readings and `hello from front` while rear is off |
| Only listed paths are shared; links are resolved and must stay inside a share; names outside answer `EACCES` | loopback 3, 3b to 3e, 19f, 20e; boot image 5 | `cat: /net/front/etc/passwd: Permission denied` |
| Only root, or members of the group given with `-g`, may use `/net/<name>` | loopback 12a, 12b | any other user is refused before anything is forwarded |
| Both ends of a link must prove they hold its key (HMAC-SHA256 challenge, no key on the wire); no key, a wrong key, or an agent that cannot prove it are refused, and each refusal is logged | loopback 22a to 22f; `test/auth-vectors.c` (RFC 4231 vectors) | `rc=EHOSTDOWN` for each refused case; a key file others can read is refused |
| Each link has its own key, and a zone holds only its own | zonal 8 | front's and rear's key checksums differ; central's copy for each link matches that zone's |
| Strangers cannot hold the handshake: at most 2 handshakes wait per address, each for about 2 s, and authenticated peers still get in (at most 4 per address) | loopback 17, 23a, 23b, 24a to 24c | six trickling strangers are all dropped near the 2 s limit |
| `ls`, `ls -l`, `stat` and `fstat` on a peer's paths match the peer's own; a directory above a share lists only the way to the shares | two-node 2d-1 to 2d-3; loopback 19a to 19h, 20a to 20h; zonal 1 | the stock `ls -l` prints the same through `/net` as locally; central lists only `hello temp` and `radar` |
| A waiting `stat` blocks nobody else, and a signal interrupts only the caller it was sent to | loopback 21a to 21e | two waiting stats: the signalled one gets `EINTR`, the other `EHOSTDOWN` |
| Replies that break the protocol never reach a program | loopback 18a to 18d | from `test/fake-agent.py`, a deliberately broken agent |
| A pair or a zonal set built with `image/build-image.sh` boots already connected | boot image 1 to 3; zonal 1, 2 | `hello from front` on central with nothing typed after power-on |
| Confined: the Crossnode programs run as uid 70 and the sensors as uid 71, each under its own security type | boot image 6, 7; zonal 4, 5 | `crossnode_net_t 70`, `crossnode_agent_t 70`, `zonesensor_t 71` |
| Every machine runs a policy that cannot be changed while it runs | boot image 8; zonal 6 | `secpol -Q` prints `(immutable)` on each machine |
| The agent's type may attach no path and offer no public channel; every ability not listed is denied | boot image 9 | its capability list has no `PATHSPACE` and ends in `EOL deny` |
| `crossnode-net`'s type may attach exactly its `/net` paths | boot image 10; zonal 7 | `allow_attach crossnode_net_t /net/front;` and, in the zonal set, `/net/rear` |
| Root cannot start another program under `crossnode-net`'s type | boot image 11 | `on: Operation not permitted` |
| The power-cut checks can fail: a deliberately broken run fails them | zonal 9 to 13 with `CN_MUTATE=1` | 5 FAIL lines, each for the reason the mutation causes |

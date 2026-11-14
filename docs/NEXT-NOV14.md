# Crossnode: next steps, ordered by what a judge sees

Written Nov 14 2026. The entry deadline is Dec 16 00:30 IST. Judges are QNX engineers who may score from the
description, images and a video under 3 minutes without running anything. Each of the four criteria
(implementation, design, impact, innovation) counts 25 percent.

## What a judge sees, and what it needs

A demo viewer types commands on one node and watches another node answer. The first thing they try is
`ls /net/<node>/dev` and `ls -l`. Before this step that printed nothing useful, which would have ended the
demo in five seconds. So the order below goes from "the demo works" to "the claims are true" to "the
package is complete".

## Order

1. **Directory listing and real stat (done in this step).** `ls`, `ls -l`, `stat` and `fstat` on `/net/<node>/...`
   work with the stock tools. A directory above a share lists only the way to the shares, so
   `ls /net/nodey/dev` shows the shared devices and nothing else in `/dev`. Tests 19 to 21 in the loopback
   suite, 2d and 6c in the two-node suite.
2. **Peer authentication and a peers file.** This goes ahead of the boot image, which the plan also does
   (authentication Dec 9, boot image Dec 10), and it goes ahead of what was first expected. Why: the README
   says "secure by default" while the agent accepts anyone who can reach its port, and a QNX engineer will see
   that in a minute. The boot image should carry the real configuration (a peers file and a shared key), so
   building it first would mean building it twice. Design: a shared key per peer pair, a challenge and response
   at connect time with an HMAC (no new dependencies: write the hash in plain C), no key on the wire, a peers
   file read by both programs, keys readable by root only. Estimate: 4 to 5 hours with tests (wrong key refused,
   no key refused, replayed challenge refused).
3. **Boot image that starts the service at boot.** `mkqnximage` with a build file that starts `hellodev`, the
   agent and `crossnode-net` from the peers file, so the two guests come up already connected. This is the first
   shot of the video: power on, `ls /net/nodey/dev`. Estimate: 3 to 4 hours (the image tooling is patched
   already, see scripts/patch-mkqnximage.sh).
4. **Status tool (`cnstat`).** One command that shows each peer, up or down, the last error, how many calls are
   waiting, and the agent's shares. Reads a small status resource that `crossnode-net` exposes under its own
   mount (`/net/<name>/.status`, local, answered without the peer). Estimate: 2 to 3 hours.
5. **Zonal demo.** A front zone node with a simulated sensor device and a central node that reads it through
   `/net/front/dev/...`, then the front node is killed mid-read and the reader gets a clean error inside the stated
   limit. Labelled as a simulation (licence 4.2.5), no performance numbers (licence 4.6(i)). A script that
   runs the whole thing and a terminal recording of it. Estimate: 3 hours.
6. **Non-root confinement of the agent.** The plan lists "a confined non-root service" as must-ship and security
   policies are one of the six items the best-use bonus names. Run the agent as a service user that can open
   only what the share list names, with the policy file in the image. Estimate: 4 hours, with the risk that the
   policy tooling needs reading first.
7. **pread, pwrite and lseek on files.** Completes the file semantics (`tail`, `dd`, editors need offsets).
   The wire already carries everything but an offset field. Estimate: 3 hours. Lower than the demo items
   because the demo uses devices, which have no offsets.
8. **README, architecture page, honest limits table, demo-video shots.** Update the status table with what
   works now, keep the not-done list truthful (notify and select, mmap, mixed architectures, lstat versus stat,
   one peer per process). Estimate: 3 hours, spread across the other steps.
9. **Stretch, only if ahead:** several peers in one `crossnode-net` process; `select` and `ionotify`
   forwarding; a mixed-architecture pair (blocked: the base SDP has no aarch64 QEMU startup).

Dates: items 2 and 3 by Nov 20, items 4 and 5 by Nov 25, item 6 by Dec 1, item 7 and the README by Dec 5
(gate 2). That leaves Dec 6 to 11 for review rounds, the stranger test, and video.

## Known limits of step 1 (also for the README)

- stat follows links, so `ls -l` shows a link inside a share as the file it points to, and a link that leaves
  the share shows as refused (question marks). Plain `ls -l` in the fixture directory shows exactly that.
- Stat of a regular file or a directory goes through a checked descriptor (opened with O_NOFOLLOW, checked
  with iofdinfo against the share and against the resolved name's device and inode, then fstat'ed), so a path
  swapped after the resolve is refused. A device or FIFO is not opened, because that would run its open handler,
  so it is looked at by name before and after a second resolve; that narrows a path swap but does not rule it
  out (commit efe5fc1).
- A directory handle is the second of two opens of the same directory, checked to be the same object by device
  and inode, because `iofdinfo()` could not check a descriptor opened with `O_DIRECTORY` (observed: ENOSYS) and
  `fdopendir()` needs one (observed: ENOTDIR otherwise).
- Directory positions are entry indexes kept on the using side. Entries added or removed in a directory while
  it is being listed can be skipped or shown twice, as on most file systems.
- Inside a combined message (stat by name) the resource manager library hands the handler an id that differs
  from the one the unblock pulse carries (observed), so the waiting caller is found by process, thread and
  connection instead. A test (21e) shows a signal interrupts only the caller it was sent to.

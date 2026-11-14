#!/usr/bin/env python3
"""A deliberately broken Crossnode agent, for checking that crossnode-net refuses bad replies.

It answers PING and OPEN correctly, then breaks the protocol on purpose:
  READ   -> sends 64 bytes more than were asked for
  DEVCTL -> answers with the wrong operation code
  STAT   -> answers with a file type that does not exist
  READDIR -> answers with an entry whose name contains a slash
A correct crossnode-net drops the connection and fails the caller with EHOSTDOWN; it never hands
the extra bytes or the mismatched answer to the program.

Usage: test/fake-agent.py [PORT]   (listens on 127.0.0.1; QEMU guests reach it as 10.0.2.2)
"""
import socket
import struct
import sys

MAGIC = 0x31304E43
HDR = struct.Struct("<IHHIiiiI")          # magic, op, flags, seq, handle, a, b, len
PING, OPEN, READ, WRITE, DEVCTL, CLOSE, CANCEL, STAT, READDIR = 1, 2, 3, 4, 5, 6, 7, 8, 9
REPLY = 0x8000


def recv_exact(conn, n):
    data = b""
    while len(data) < n:
        chunk = conn.recv(n - len(data))
        if not chunk:
            raise ConnectionError("peer closed")
        data += chunk
    return data


def serve(conn):
    while True:
        magic, op, _flags, seq, handle, a, b, length = HDR.unpack(recv_exact(conn, HDR.size))
        if magic != MAGIC:
            return
        payload = recv_exact(conn, length) if length else b""
        if op == PING:
            conn.sendall(HDR.pack(MAGIC, PING | REPLY, 0, seq, handle, 0, 0, 0))
        elif op == OPEN:
            flags = 1 if payload.endswith(b"dir") else 0          # CN_F_DIR for a path ending in "dir"
            conn.sendall(HDR.pack(MAGIC, OPEN | REPLY, flags, seq, -1, 0, 0, 0))
        elif op == READ:
            extra = b"X" * (a + 64)            # more than the caller asked for
            conn.sendall(HDR.pack(MAGIC, READ | REPLY, 0, seq, handle, 0, len(extra), len(extra)) + extra)
            print(f"fake-agent: answered a {a}-byte read with {len(extra)} bytes", flush=True)
        elif op == STAT:
            st = struct.pack("<QQqqqIIIIII", 1, 10, 0, 0, 0, 0o170000, 0, 0, 1, 0, 4096)   # mode 0170000: no such type
            conn.sendall(HDR.pack(MAGIC, STAT | REPLY, 0, seq, handle, 0, 0, len(st)) + st)
            print("fake-agent: answered a stat with an impossible file type", flush=True)
        elif op == READDIR:
            name = b"a/b"
            ent = struct.pack("<QII", 5, len(name), 0) + name + b"\0" * (8 - len(name) % 8)
            conn.sendall(HDR.pack(MAGIC, READDIR | REPLY, 0, seq, handle, 0, 1, len(ent)) + ent)
            print("fake-agent: answered a directory read with a name that contains a slash", flush=True)
        elif op == DEVCTL:
            conn.sendall(HDR.pack(MAGIC, READ | REPLY, 0, seq, handle, 0, 0, 0))   # wrong operation
            print("fake-agent: answered a devctl as if it were a read", flush=True)


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 7799
    ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("127.0.0.1", port))
    ls.listen(4)
    print(f"fake-agent: listening on 127.0.0.1:{port}", flush=True)
    while True:
        conn, _ = ls.accept()
        try:
            serve(conn)
        except (ConnectionError, OSError):
            pass
        finally:
            conn.close()


if __name__ == "__main__":
    main()

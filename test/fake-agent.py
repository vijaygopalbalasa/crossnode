#!/usr/bin/env python3
"""A deliberately broken Crossnode agent, for checking that crossnode-net refuses bad replies.

It answers PING and OPEN correctly, then breaks the protocol on purpose:
  READ   -> sends 64 bytes more than were asked for
  DEVCTL -> answers with the wrong operation code
A correct crossnode-net drops the connection and fails the caller with EHOSTDOWN; it never hands
the extra bytes or the mismatched answer to the program.

Usage: test/fake-agent.py [PORT]   (listens on 127.0.0.1; QEMU guests reach it as 10.0.2.2)
"""
import socket
import struct
import sys

MAGIC = 0x31304E43
HDR = struct.Struct("<IHHIiiiI")          # magic, op, flags, seq, handle, a, b, len
PING, OPEN, READ, WRITE, DEVCTL, CLOSE, CANCEL = 1, 2, 3, 4, 5, 6, 7
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
        if length:
            recv_exact(conn, length)
        if op == PING:
            conn.sendall(HDR.pack(MAGIC, PING | REPLY, 0, seq, handle, 0, 0, 0))
        elif op == OPEN:
            conn.sendall(HDR.pack(MAGIC, OPEN | REPLY, 0, seq, -1, 0, 0, 0))
        elif op == READ:
            extra = b"X" * (a + 64)            # more than the caller asked for
            conn.sendall(HDR.pack(MAGIC, READ | REPLY, 0, seq, handle, 0, len(extra), len(extra)) + extra)
            print(f"fake-agent: answered a {a}-byte read with {len(extra)} bytes", flush=True)
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

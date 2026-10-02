#!/usr/bin/env python3
"""Telnet to a node, log in, issue one command, print the transcript.

usage: NODE_USER=... NODE_PASS=... drive.py <host> <port> <command...>

Used with -14's telnet port (a remote user arriving at IR2UFV over L2)
and IR2UFV's own telnet port as its sysop user (the local path).
Credentials come from the environment, never from this file.
"""
import os
import socket
import sys
import time


def read_for(s, secs):
    s.settimeout(0.5)
    out = b""
    end = time.time() + secs
    while time.time() < end:
        try:
            d = s.recv(4096)
            if not d:
                break
            out += d
        except socket.timeout:
            pass
    return out


def main():
    host, port = sys.argv[1], int(sys.argv[2])
    cmd = " ".join(sys.argv[3:]).encode()
    user = os.environ["NODE_USER"].encode()
    pw = os.environ["NODE_PASS"].encode()
    s = socket.create_connection((host, port), timeout=10)
    t = read_for(s, 3)
    s.sendall(user + b"\r")
    t += read_for(s, 2)
    s.sendall(pw + b"\r")
    t += read_for(s, 3)
    s.sendall(cmd + b"\r")
    t += read_for(s, 25)
    s.sendall(b"hello from the patch test\r")
    t += read_for(s, 6)
    s.close()
    print(t.decode("latin-1").replace("\r\n", "\n").replace("\r", "\n"))


main()

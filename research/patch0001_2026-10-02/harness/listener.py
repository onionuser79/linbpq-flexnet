#!/usr/bin/env python3
"""Stand-in for DXSpider: 127.0.0.1:63999, banner + log of what LinBPQ sends."""
import socket
import threading
import time

LOG = "/tmp/patchtest-listener.log"


def log(msg):
    with open(LOG, "a") as f:
        f.write(f"{time.strftime('%H:%M:%S')} {msg}\n")


def serve(conn, addr):
    log(f"ACCEPT {addr}")
    conn.sendall(b"PATCHTEST-APP: connected to the dummy application\r\n")
    conn.settimeout(30)
    try:
        while True:
            data = conn.recv(1024)
            if not data:
                break
            log(f"RX {data!r}")
            conn.sendall(b"PATCHTEST-APP echo: " + data)
    except OSError as e:
        log(f"ERR {e}")
    finally:
        log("CLOSE")
        conn.close()


s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 63999))
s.listen(5)
log("LISTEN 127.0.0.1:63999")
while True:
    c, a = s.accept()
    threading.Thread(target=serve, args=(c, a), daemon=True).start()

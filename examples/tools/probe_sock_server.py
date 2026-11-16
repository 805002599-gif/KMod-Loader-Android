import socket, threading, hashlib, base64, struct, sys
def tcp_echo(c):
    try:
        while True:
            d = c.recv(4096)
            if not d: break
            c.sendall(b"echo:" + d)
    finally: c.close()
def ws_frame(op, payload, fin=True):
    b = bytes([(0x80 if fin else 0) | op])
    n = len(payload)
    if n < 126: b += bytes([n])
    elif n < 65536: b += bytes([126]) + struct.pack(">H", n)
    else: b += bytes([127]) + struct.pack(">Q", n)
    return b + payload
def ws_client(c):
    try:
        req = b""
        while b"\r\n\r\n" not in req: req += c.recv(4096)
        key = [l.split(b": ")[1] for l in req.split(b"\r\n") if l.lower().startswith(b"sec-websocket-key")][0]
        acc = base64.b64encode(hashlib.sha1(key + b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest())
        c.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + acc + b"\r\n\r\n")
        c.sendall(ws_frame(1, b"welcome"))
        buf = b""
        while True:
            d = c.recv(65536)
            if not d: break
            buf += d
            while len(buf) >= 2:
                op = buf[0] & 15; ln = buf[1] & 127; i = 2
                if ln == 126: ln = struct.unpack(">H", buf[2:4])[0]; i = 4
                elif ln == 127: ln = struct.unpack(">Q", buf[2:10])[0]; i = 10
                if len(buf) < i + 4 + ln: break
                mask = buf[i:i+4]; data = bytes(buf[i+4+j] ^ mask[j % 4] for j in range(ln)); buf = buf[i+4+ln:]
                if op == 8: c.sendall(ws_frame(8, data[:2])); return
                if op == 1 or op == 2:
                    if data == b"frag":
                        c.sendall(ws_frame(1, b"frag-", fin=False)); c.sendall(ws_frame(0, b"men", fin=False)); c.sendall(ws_frame(0, b"ts", fin=True))
                    elif data == b"ping":
                        c.sendall(ws_frame(9, b"hi"))
                    elif data == b"bye":
                        c.sendall(ws_frame(8, struct.pack(">H", 1001) + b"going away")); return
                    elif data == b"big":
                        c.sendall(ws_frame(2, bytes(range(256)) * 400))
                    else:
                        c.sendall(ws_frame(op, b"echo:" + data))
    finally: c.close()
def serve(port, fn):
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(("127.0.0.1", port)); s.listen(8)
    while True:
        c, _ = s.accept(); threading.Thread(target=fn, args=(c,), daemon=True).start()
threading.Thread(target=serve, args=(1146, tcp_echo), daemon=True).start()
serve(1147, ws_client)

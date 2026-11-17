# SPDX-License-Identifier: GPL-2.0-or-later
"""The server side of a WebSocket (RFC 6455), enough for the web deck.

The browser page talks to web_deck.py over one of these: LCD frames go out as
binary messages, the hand's presses come back as text. Only what that needs
is here: the handshake, unmasking the client's frames, unmasked frames back,
fragmented messages up to MAX_MESSAGE, ping/pong and close. No extensions,
so no compression: the frames are deflated already.
"""

import base64
import hashlib
import socket
import struct
import threading

GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

CONT, TEXT, BINARY, CLOSE, PING, PONG = 0x0, 0x1, 0x2, 0x8, 0x9, 0xA

# The page sends short JSON events; anything near this is not the page.
MAX_MESSAGE = 64 * 1024

PROTOCOL_ERROR = 1002
TOO_BIG = 1009


class WebSocketError(Exception):
    def __init__(self, code, reason):
        super().__init__(reason)
        self.code = code


def accept_key(key):
    """Sec-WebSocket-Accept for a client's Sec-WebSocket-Key."""
    return base64.b64encode(hashlib.sha1(key.encode("ascii") + GUID).digest()).decode("ascii")


def upgrade_error(headers):
    """Why these request headers are not a WebSocket upgrade, or None."""
    if headers.get("Upgrade", "").lower() != "websocket":
        return "not a WebSocket upgrade"
    if "upgrade" not in [t.strip().lower() for t in headers.get("Connection", "").split(",")]:
        return "no Connection: Upgrade"
    if headers.get("Sec-WebSocket-Version") != "13":
        return "WebSocket version 13 only"
    try:
        if len(base64.b64decode(headers.get("Sec-WebSocket-Key", ""), validate=True)) != 16:
            return "bad Sec-WebSocket-Key"
    except ValueError:
        return "bad Sec-WebSocket-Key"
    return None


def handshake_response(key):
    return ("HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Accept: {accept_key(key)}\r\n\r\n").encode("ascii")


def frame(opcode, payload, fin=True, mask=None):
    """One frame; unmasked as a server sends it, or masked with the 4-byte
    `mask` as a client must (the tests use that)."""
    head = bytes([(0x80 if fin else 0) | opcode])
    n = len(payload)
    bit = 0x80 if mask else 0
    if n < 126:
        head += bytes([bit | n])
    elif n < 1 << 16:
        head += struct.pack(">BH", bit | 126, n)
    else:
        head += struct.pack(">BQ", bit | 127, n)
    if mask:
        return head + mask + _unmask(payload, mask)
    return head + payload


def _unmask(data, mask):
    n = len(data)
    key = (mask * (n // 4 + 1))[:n]
    return (int.from_bytes(data, "little") ^ int.from_bytes(key, "little")).to_bytes(n, "little")


class WebSocket:
    """An upgraded connection. recv() from one thread; send from any."""

    def __init__(self, sock, rfile):
        self.sock = sock
        self.rfile = rfile
        self.send_lock = threading.Lock()
        self.closed = False

    def recv(self):
        """The next whole message as (TEXT or BINARY, bytes), answering pings
        on the way; None once the connection has closed."""
        parts, opcode, size = [], None, 0
        try:
            while True:
                fin, op, payload = self._read_frame()
                if op >= CLOSE:
                    if op == CLOSE:
                        self.close(struct.unpack(">H", payload[:2])[0] if len(payload) >= 2 else 1000)
                        return None
                    if op == PING:
                        self._send(PONG, payload)
                    continue
                if op == CONT:
                    if opcode is None:
                        raise WebSocketError(PROTOCOL_ERROR, "continuation without a start")
                elif opcode is not None:
                    raise WebSocketError(PROTOCOL_ERROR, "new message inside a fragmented one")
                elif op in (TEXT, BINARY):
                    opcode = op
                else:
                    raise WebSocketError(PROTOCOL_ERROR, f"unknown opcode {op}")
                size += len(payload)
                if size > MAX_MESSAGE:
                    raise WebSocketError(TOO_BIG, "message too big")
                parts.append(payload)
                if fin:
                    return opcode, b"".join(parts)
        except WebSocketError as e:
            self.close(e.code)
            return None
        except (OSError, EOFError):
            self.closed = True
            return None

    def _read(self, n):
        data = self.rfile.read(n)
        if data is None or len(data) < n:
            raise EOFError
        return data

    def _read_frame(self):
        b0, b1 = self._read(2)
        if b0 & 0x70:
            raise WebSocketError(PROTOCOL_ERROR, "reserved bits set")
        fin, op, n = bool(b0 & 0x80), b0 & 0x0F, b1 & 0x7F
        if not b1 & 0x80:
            raise WebSocketError(PROTOCOL_ERROR, "client frames must be masked")
        if op >= CLOSE and (n > 125 or not fin):
            raise WebSocketError(PROTOCOL_ERROR, "bad control frame")
        if n == 126:
            (n,) = struct.unpack(">H", self._read(2))
        elif n == 127:
            (n,) = struct.unpack(">Q", self._read(8))
        if n > MAX_MESSAGE:
            raise WebSocketError(TOO_BIG, "frame too big")
        mask = self._read(4)
        return fin, op, _unmask(self._read(n), mask)

    def _send(self, opcode, payload):
        with self.send_lock:
            if self.closed:
                raise OSError("closed")
            self.sock.sendall(frame(opcode, payload))

    def send_text(self, text):
        self._send(TEXT, text.encode("utf-8"))

    def send_binary(self, data):
        self._send(BINARY, data)

    def ping(self):
        self._send(PING, b"")

    def close(self, code=1000):
        """Say goodbye and shut the socket; a blocked recv() then returns None."""
        with self.send_lock:
            if self.closed:
                return
            self.closed = True
            try:
                self.sock.sendall(frame(CLOSE, struct.pack(">H", code)))
            except OSError:
                pass
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass

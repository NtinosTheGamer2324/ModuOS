#!/usr/bin/env python3
"""URS client - talks to the ModuOS URS daemon (ursd) from a Windows host.

!! URS HAS NO ENCRYPTION. Passwords and keystrokes are sent in clear text.
!! The destination is hardcoded to 127.0.0.1 on purpose: QEMU forwards that
!! loopback port into the guest (hostfwd=tcp:127.0.0.1:8080-10.0.2.15:8080).
!! There is deliberately no option to connect anywhere else.

Usage:  python urs_client.py
Escape: Ctrl+]  disconnects locally. Ctrl+C is sent to the remote shell.

Wire protocol (mirrors the header comment of ursd.c; big-endian):

    'U' 'R' type version(1) u16 length payload[length <= 1024]

    0x01 HELLO     S->C  banner            0x06 OUTPUT   S->C  pty bytes
    0x02 AUTH_REQ  C->S  rows cols ulen    0x07 WINSIZE  C->S  u16 rows, cols
                         user plen pass    0x08 CLOSE    both  S->C: u8 reason
    0x03 AUTH_OK   S->C  (empty)           0x09 ERROR    S->C  u8 code, text
    0x04 AUTH_FAIL S->C  u8 attempts left  0x05 INPUT    C->S  keyboard bytes
"""

import codecs
import getpass
import select
import shutil
import socket
import struct
import sys
import time

HOST = "127.0.0.1"      # hardcoded: never taken from argv or the environment
PORT = 8080

MAGIC = b"UR"
VERSION = 1
HDR_LEN = 6
MAX_PAYLOAD = 1024

MSG_HELLO = 0x01
MSG_AUTH_REQ = 0x02
MSG_AUTH_OK = 0x03
MSG_AUTH_FAIL = 0x04
MSG_INPUT = 0x05
MSG_OUTPUT = 0x06
MSG_WINSIZE = 0x07
MSG_CLOSE = 0x08
MSG_ERROR = 0x09
MSG_MAX = 0x09

SERVER_TO_CLIENT = {MSG_HELLO, MSG_AUTH_OK, MSG_AUTH_FAIL, MSG_OUTPUT,
                    MSG_CLOSE, MSG_ERROR}

CLOSE_REASONS = {0: "shell exited", 1: "too many failed logins",
                 2: "closed by client", 3: "server shutting down"}
ERROR_CODES = {1: "protocol error", 2: "frame too large", 3: "server busy",
               4: "forbidden", 5: "server error", 6: "timeout"}

USER_MAX = 31           # userctl field sizes minus the NUL
PASS_MAX = 63
ESCAPE_KEY = "\x1d"     # Ctrl+]

# Extended-key scan codes from msvcrt.getwch() -> what the ModuOS keyboard
# layer (tty.c nav_seq) would have produced for the same key.
SPECIAL_KEYS = {
    "H": b"\x1b[A", "P": b"\x1b[B", "M": b"\x1b[C", "K": b"\x1b[D",
    "G": b"\x1b[H", "O": b"\x1b[F", "R": b"\x1b[2~", "S": b"\x1b[3~",
    "I": b"\x1b[5~", "Q": b"\x1b[6~",
}


class UrsError(Exception):
    pass


class ProtocolError(UrsError):
    pass


def encode_frame(msg_type, payload=b""):
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload too large")
    return MAGIC + bytes([msg_type, VERSION]) + struct.pack(">H", len(payload)) + payload


class FrameReader:
    """Incremental parser: TCP gives a byte stream, not messages."""

    def __init__(self):
        self._buf = bytearray()

    def feed(self, data):
        self._buf += data

    def next(self):
        """Return (type, payload), or None if a full frame isn't buffered."""
        if len(self._buf) < HDR_LEN:
            return None
        if bytes(self._buf[0:2]) != MAGIC or self._buf[3] != VERSION:
            raise ProtocolError("bad magic or version from server")
        msg_type = self._buf[2]
        length = struct.unpack(">H", bytes(self._buf[4:6]))[0]
        if msg_type == 0 or msg_type > MSG_MAX:
            raise ProtocolError("unknown message type 0x%02x" % msg_type)
        if length > MAX_PAYLOAD:
            raise ProtocolError("oversized frame (%d bytes)" % length)
        if len(self._buf) < HDR_LEN + length:
            return None
        payload = bytes(self._buf[HDR_LEN:HDR_LEN + length])
        del self._buf[:HDR_LEN + length]
        if msg_type not in SERVER_TO_CLIENT:
            raise ProtocolError("unexpected message type 0x%02x" % msg_type)
        return msg_type, payload


def encode_auth_req(user, password, rows, cols):
    ub = user.encode("ascii")
    pb = password.encode("utf-8")
    if not 1 <= len(ub) <= USER_MAX or any(c <= 0x20 or c >= 0x7F for c in ub):
        raise ValueError("username must be 1-%d printable ASCII characters" % USER_MAX)
    if not 1 <= len(pb) <= PASS_MAX or 0 in pb:
        raise ValueError("password must be 1-%d bytes" % PASS_MAX)
    payload = (struct.pack(">HH", rows, cols) + bytes([len(ub)]) + ub +
               bytes([len(pb)]) + pb)
    return encode_frame(MSG_AUTH_REQ, payload)


def describe_close(payload):
    return CLOSE_REASONS.get(payload[0] if payload else -1, "closed by server")


def describe_error(payload):
    if not payload:
        return "server error"
    text = payload[1:].decode("utf-8", "replace")
    return "%s: %s" % (ERROR_CODES.get(payload[0], "error %d" % payload[0]), text)


class Connection:
    def __init__(self, sock):
        self.sock = sock
        self.reader = FrameReader()

    @classmethod
    def open(cls):
        sock = socket.create_connection((HOST, PORT), timeout=5)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        return cls(sock)

    def send(self, msg_type, payload=b""):
        self.sock.sendall(encode_frame(msg_type, payload))

    def wait_frame(self, timeout=15.0):
        """Blocking read of one frame (handshake only)."""
        deadline = time.monotonic() + timeout
        while True:
            frame = self.reader.next()
            if frame:
                return frame
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise UrsError("timed out waiting for the server")
            self.sock.settimeout(remaining)
            data = self.sock.recv(4096)
            if not data:
                raise UrsError("server closed the connection")
            self.reader.feed(data)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def login(conn, rows, cols):
    """Prompt for credentials until the server accepts or gives up."""
    msg_type, payload = conn.wait_frame()
    if msg_type == MSG_ERROR:           # e.g. BUSY: sent instead of HELLO
        raise UrsError(describe_error(payload))
    if msg_type != MSG_HELLO:
        raise ProtocolError("expected HELLO from server")
    print(payload.decode("utf-8", "replace"))
    print("connected to %s:%d  (Ctrl+] disconnects)\n" % (HOST, PORT))

    while True:
        user = input("login: ").strip()
        password = getpass.getpass("password: ")
        try:
            request = encode_auth_req(user, password, rows, cols)
        except (ValueError, UnicodeEncodeError) as exc:
            print("invalid input: %s" % exc)
            continue
        conn.sock.sendall(request)
        request = password = None

        msg_type, payload = conn.wait_frame()
        if msg_type == MSG_AUTH_OK:
            return
        if msg_type == MSG_AUTH_FAIL:
            left = payload[0] if payload else 0
            print("Login incorrect (%d attempt%s left)" % (left, "" if left == 1 else "s"))
            if left == 0:
                raise UrsError("too many failed logins")
            continue
        if msg_type == MSG_ERROR:
            raise UrsError(describe_error(payload))
        if msg_type == MSG_CLOSE:
            raise UrsError(describe_close(payload))
        raise ProtocolError("unexpected reply during login")


class WindowsConsole:
    """Raw keyboard + VT output on a Windows console; restores it on exit."""

    ENABLE_PROCESSED_INPUT = 0x0001
    ENABLE_LINE_INPUT = 0x0002
    ENABLE_ECHO_INPUT = 0x0004
    ENABLE_PROCESSED_OUTPUT = 0x0001
    ENABLE_VIRTUAL_TERMINAL_PROCESSING = 0x0004
    CP_UTF8 = 65001

    def __init__(self):
        import ctypes
        from ctypes import wintypes
        import msvcrt
        self._msvcrt = msvcrt
        self._k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        k = self._k32
        k.GetStdHandle.restype = wintypes.HANDLE
        k.GetConsoleMode.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        k.SetConsoleMode.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        self._wintypes = wintypes
        self._ctypes = ctypes
        self._saved = []
        self._pending_high = ""
        self._decoder = codecs.getincrementaldecoder("utf-8")("replace")

    def _mode(self, handle):
        mode = self._wintypes.DWORD()
        if not self._k32.GetConsoleMode(handle, self._ctypes.byref(mode)):
            raise UrsError("stdin/stdout must be a Windows console")
        return mode.value

    def __enter__(self):
        k = self._k32
        h_in = k.GetStdHandle(-10)
        h_out = k.GetStdHandle(-11)
        in_mode = self._mode(h_in)
        out_mode = self._mode(h_out)
        self._saved = [(h_in, in_mode), (h_out, out_mode)]
        self._old_cp = k.GetConsoleOutputCP()
        # Without clearing PROCESSED_INPUT, Ctrl+C would never reach getwch().
        k.SetConsoleMode(h_in, in_mode & ~(self.ENABLE_PROCESSED_INPUT |
                                           self.ENABLE_LINE_INPUT |
                                           self.ENABLE_ECHO_INPUT))
        k.SetConsoleMode(h_out, out_mode | self.ENABLE_PROCESSED_OUTPUT |
                         self.ENABLE_VIRTUAL_TERMINAL_PROCESSING)
        k.SetConsoleOutputCP(self.CP_UTF8)
        return self

    def __exit__(self, *exc):
        for handle, mode in self._saved:
            self._k32.SetConsoleMode(handle, mode)
        self._k32.SetConsoleOutputCP(self._old_cp)
        sys.stdout.write("\x1b[0m")
        sys.stdout.flush()
        return False

    def size(self):
        cols, rows = shutil.get_terminal_size((80, 25))
        return rows, cols

    def write(self, data):
        text = self._decoder.decode(data)
        if text:
            sys.stdout.write(text)
            sys.stdout.flush()

    def read_keys(self):
        """Bytes for every key waiting; raises EOFError on the escape key."""
        out = bytearray()
        msvcrt = self._msvcrt
        while msvcrt.kbhit():
            ch = msvcrt.getwch()
            if ch in ("\x00", "\xe0"):
                out += SPECIAL_KEYS.get(msvcrt.getwch(), b"")
            elif ch == ESCAPE_KEY:
                raise EOFError
            elif ch == "\x08":
                out += b"\x7f"             # what the ModuOS keyboard sends
            elif "\ud800" <= ch <= "\udbff":
                self._pending_high = ch    # first half of a surrogate pair
            else:
                if "\udc00" <= ch <= "\udfff" and self._pending_high:
                    ch = chr(0x10000 + ((ord(self._pending_high) - 0xD800) << 10)
                             + (ord(ch) - 0xDC00))
                self._pending_high = ""
                try:
                    out += ch.encode("utf-8")
                except UnicodeEncodeError:
                    pass
        return bytes(out)


def run_session(conn, console):
    """Pump keyboard -> INPUT and OUTPUT -> screen until the session ends.

    Returns a process exit code.
    """
    last_size = console.size()
    last_check = time.monotonic()
    conn.sock.settimeout(None)

    while True:
        readable, _, _ = select.select([conn.sock], [], [], 0.01)
        if readable:
            data = conn.sock.recv(4096)
            if not data:
                return finish(console, "connection closed by server", 1)
            conn.reader.feed(data)
            while True:
                frame = conn.reader.next()
                if frame is None:
                    break
                msg_type, payload = frame
                if msg_type == MSG_OUTPUT:
                    console.write(payload)
                elif msg_type == MSG_CLOSE:
                    code = 0 if payload[:1] == b"\x00" else 1
                    return finish(console, describe_close(payload), code)
                elif msg_type == MSG_ERROR:
                    return finish(console, describe_error(payload), 1)
                else:
                    raise ProtocolError("unexpected message type 0x%02x" % msg_type)

        try:
            keys = console.read_keys()
        except EOFError:
            try:
                conn.send(MSG_CLOSE)
            except OSError:
                pass
            return finish(console, "disconnected", 0)
        for off in range(0, len(keys), MAX_PAYLOAD):
            conn.send(MSG_INPUT, keys[off:off + MAX_PAYLOAD])

        now = time.monotonic()
        if now - last_check >= 0.5:
            last_check = now
            size = console.size()
            if size != last_size:
                last_size = size
                conn.send(MSG_WINSIZE, struct.pack(">HH", *size))


def finish(console, message, code):
    console.write(b"\r\n")
    sys.stdout.write("[URS: %s]\n" % message)
    sys.stdout.flush()
    return code


def main():
    if sys.platform != "win32":
        print("urs_client.py targets a Windows console (msvcrt).")
        return 2
    print("URS client - UNENCRYPTED, loopback only (%s:%d)" % (HOST, PORT))

    try:
        conn = Connection.open()
    except OSError as exc:
        print("cannot connect to %s:%d: %s" % (HOST, PORT, exc))
        print("Is QEMU running with hostfwd=tcp:127.0.0.1:%d-10.0.2.15:%d and ursd started?"
              % (PORT, PORT))
        return 1

    try:
        console = WindowsConsole()
        rows, cols = console.size()
        login(conn, rows, cols)
        with console:
            return run_session(conn, console)
    except KeyboardInterrupt:
        print("\n[URS: interrupted]")
        return 130
    except (UrsError, OSError) as exc:
        print("\n[URS: %s]" % exc)
        return 1
    finally:
        conn.close()


if __name__ == "__main__":
    sys.exit(main())
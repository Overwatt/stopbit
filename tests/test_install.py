#!/usr/bin/env python3
"""Refusals and interruptions of POST /install in the app.  tests/test_install.py HOST
Installs nothing. Needs the update key."""
import hashlib, hmac, http.client, json, os, socket, struct, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import n540
H = sys.argv[1]; d = n540.Device(H) if hasattr(n540, "Device") else n540.Dev(H)
app = open("build/app.bin", "rb").read()
ldr = open("build/loader_flash.bin", "rb").read()
st = lambda: json.loads(d.get("/status"))
s0 = st(); assert s0["mode"] == "app"

def send(body, claim=None, length=None, stop=None, key=None, path="/install"):
    d.load_key()
    nonce = d.get("/nonce").decode().strip()
    sha = hashlib.sha256(body if claim is None else claim).hexdigest()
    mac = hmac.new(key or d.key, f"{nonce}\nPOST {path}\n{sha}".encode(), hashlib.sha256).hexdigest()
    s = socket.create_connection((H, 80), timeout=15)
    s.sendall(f"POST {path} HTTP/1.1\r\nX-Auth: {mac}\r\nX-Body-SHA256: {sha}\r\nContent-Length: {length or len(body)}\r\n\r\n".encode())
    if stop is not None: s.sendall(body[:stop]); s.close(); return "closed"
    try: s.sendall(body)
    except OSError: pass
    r = b""
    try:
        while (x := s.recv(4096)): r += x
    except OSError: pass
    return r.decode(errors="replace").split("\r\n")[0] + " | " + r.decode(errors="replace").split("\r\n\r\n")[-1].strip()

bad = bytearray(app); bad[0x2A0] ^= 1
addr = bytearray(app); struct.pack_into("<I", addr, 0x2A4, 0x24010000)
vec = bytearray(app); struct.pack_into("<I", vec, 4, 0x08000101)
for name, want, args in [
    ("wrong key", "401", dict(body=app, key=bytes(32))),
    ("loader image", "400", dict(body=ldr)),
    ("no app marker", "400", dict(body=bytes(bad))),
    ("wrong link address", "400", dict(body=bytes(addr))),
    ("reset vector outside", "400", dict(body=bytes(vec))),
    ("random bytes", "400", dict(body=os.urandom(50000))),
    ("one byte", "400", dict(body=b"x")),
    ("empty", "400", dict(body=b"")),
    ("hash mismatch", "400", dict(body=app, claim=app + b"!")),
    ("too big", "413", dict(body=b"\0" * 1000, length=393217)),
]:
    r = send(**args); print(f"{name:22} {r[:90]}"); assert want in r.split("|")[0], r
for stop in (0, 1, 1000, len(app) // 2, len(app) - 1):
    send(app, stop=stop)
time.sleep(1)
r = send(ldr); print("after 5 cut uploads    ", r[:80]); assert "400" in r      # stage is free again
s1 = st()
assert s1["slots"] == s0["slots"] and s1["boots"] == s0["boots"], "something was installed or it rebooted"
print("nothing installed, no reboot")

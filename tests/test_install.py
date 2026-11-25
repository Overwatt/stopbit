#!/usr/bin/env python3
"""POST /install and /rollback in the app.

  tests/test_install.py HOST             refusals and interruptions; installs nothing
  tests/test_install.py HOST --install   also installs and rolls back: it RESTARTS

HOST is a gateway, or 127.0.0.1:8541 for tests/sim/run.sh. Needs the update key."""
import hashlib, hmac, json, os, socket, struct, sys, threading, time
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import n540
d = n540.Device(sys.argv[1])
SIM = d.port == 8541
app = open("build/app.bin", "rb").read()
ldr = open("build/loader_flash.bin", "rb").read()
st = lambda: json.loads(d.get("/status"))
lib = lambda: json.loads(d.get("/api/library"))
s0 = st(); assert s0["mode"] == "app"

def named(v, extra=b""):
    b = bytearray(app + extra)
    b[0x2A8:0x2C0] = v.encode().ljust(24, b"\0")
    return bytes(b)

def send(body, claim=None, length=None, stop=None, key=None, path="/install", slow=0, half=False):
    d.load_key()
    nonce = d.get("/nonce").decode().strip()
    sha = hashlib.sha256(body if claim is None else claim).hexdigest()
    mac = hmac.new(key or d.key, f"{nonce}\nPOST {path}\n{sha}".encode(), hashlib.sha256).hexdigest()
    s = socket.create_connection((d.host, d.port), timeout=40)
    n = length if length is not None else len(body)
    s.sendall(f"POST {path} HTTP/1.1\r\nX-Auth: {mac}\r\nX-Body-SHA256: {sha}\r\nContent-Length: {n}\r\n\r\n".encode())
    if stop is not None: s.sendall(body[:stop]); s.close(); return "closed"
    try:
        if slow:
            for i in range(0, len(body), slow): s.sendall(body[i:i + slow]); time.sleep(.01)
        else: s.sendall(body)
        if half: s.shutdown(socket.SHUT_WR)
    except OSError: pass
    r = b""
    try:
        while (x := s.recv(4096)): r += x
    except OSError: pass
    r = r.decode(errors="replace")
    return r.split("\r\n")[0] + " | " + r.split("\r\n\r\n")[-1].strip()

def idle(secs=40):
    end = time.time() + secs
    while time.time() < end:
        if not lib()["job"]: return
        time.sleep(.2)
    raise SystemExit("still busy")

bad = bytearray(app); bad[0x2A0] ^= 1
addr = bytearray(app); struct.pack_into("<I", addr, 0x2A4, 0x24010000)
vec = bytearray(app); struct.pack_into("<I", vec, 4, 0x08000101)
images0 = lib()["images"]
for name, want, why, args in [
    ("wrong key", "401", "bad auth", dict(body=app, key=bytes(32))),
    ("loader image", "400", "", dict(body=ldr)),
    ("no app marker", "400", "not an app image", dict(body=bytes(bad))),
    ("wrong link address", "400", "not linked", dict(body=bytes(addr))),
    ("reset vector outside", "400", "reset vector", dict(body=bytes(vec))),
    ("random bytes", "400", "", dict(body=os.urandom(50000))),
    ("one byte", "400", "bad size", dict(body=b"x")),
    ("empty", "400", "bad size", dict(body=b"")),
    ("too big", "413", "too big", dict(body=b"\0" * 1000, length=393217)),
    ("rollback, no such", "400", "no longer kept", dict(body=b"sha=" + b"0" * 64, path="/rollback")),
    ("rollback, nonsense", "400", "need sha", dict(body=b"hello", path="/rollback")),
    ("rollback, empty", "400", "need sha", dict(body=b"", path="/rollback")),
    ("rollback, wrong key", "401", "bad auth", dict(body=b"sha=" + b"0" * 64, path="/rollback", key=bytes(32))),
    ("rollback, too long", "413", "too big", dict(body=b"s" * 3000, path="/rollback")),
]:
    r = send(**args); print(f"{name:22} {r[:90]}"); assert want in r.split("|")[0] and why in r, r
assert lib()["images"] == images0, "a refused image was kept"
assert not lib()["job"]

if "--install" in sys.argv or SIM:
    r = send(app, claim=app + b"!"); print("hash mismatch         ", r); assert "400" in r and "mismatch" in r
    for stop in (0, 1, 1000, 5000, len(app) // 2, len(app) - 1):
        send(app, stop=stop)
        time.sleep(.3); idle()
    assert lib()["images"] == images0, "a cut upload was kept"
    r = send(ldr); print("after cut uploads     ", r[:80]); assert "400" in r
    s1 = st()
    assert s1["slots"] == s0["slots"] and s1["boots"] == s0["boots"], "something was installed or it rebooted"
    print("nothing installed, no reboot")
else:
    s1 = st()
    assert s1["slots"] == s0["slots"] and s1["boots"] == s0["boots"], "something was installed or it rebooted"
    raise SystemExit("nothing installed, no reboot (cut uploads and installs need --install)")

def wait_boot(n):
    end = time.time() + 60
    while time.time() < end:
        try:
            s = st()
            if s["mode"] == "app" and s["boots"] > n: return s
        except (OSError, SystemExit, ValueError): pass
        time.sleep(.5)
    raise SystemExit("did not come back")

def install(img, v, during=None, confirm=True, **kw):
    b = st()["boots"]
    out = []
    t = time.time()
    th = threading.Thread(target=lambda: out.append(send(img, **kw))); th.start()
    if during: during()
    th.join()
    print(f"{v:22} {out[0]}  {time.time() - t:.1f} s"); assert "200" in out[0], out[0]
    s = wait_boot(b)
    assert s["slots"][s["running_slot"]]["version"] == v, s
    if confirm: d.auth("POST", "/confirm")
    return s

def busy():
    stages, t = set(), time.time()
    while time.time() - t < 1.5:
        j = lib(); stages.add(j["job"]); assert 0 <= j["percent"] <= 100, j
        time.sleep(.05)
    for path, body in (("/install", app), ("/rollback", b"sha=" + b"0" * 64), ("/reboot", b""), ("/loader", b"")):
        r = send(body, path=path); assert "409" in r or r == " | ", (path, r)
    print("  while busy: refused a second install, rollback, reboot and loader")

if SIM: d.auth("POST", "/api/gap")
s = install(named("one"), "one", during=busy)
if SIM: print("  longest the loop was away:", json.loads(d.auth("POST", "/api/gap"))["gap"], "ms")
im = {i["version"]: i for i in lib()["images"]}
assert im["one"]["running"] and im["one"]["kept"], im
assert "first-0" in im or not SIM, "the replaced image was not saved"
s = install(named("two"), "two", slow=1460)
s = install(named("three", b"\0" * 3001), "three", half=True)
# roll back by hash
im = {i["version"]: i for i in lib()["images"]}
b = st()["boots"]
r = send(b"sha=" + im["one"]["sha"].encode(), path="/rollback"); print("rollback to one       ", r); assert "200" in r
s = wait_boot(b); assert s["slots"][s["running_slot"]]["version"] == "one"
r = send(app); print("install while on trial", r); assert "still on trial" in r
r = send(b"sha=" + im["two"]["sha"].encode(), path="/rollback"); assert "still on trial" in r, r
d.auth("POST", "/confirm")
r = send(b"sha=" + im["one"]["sha"].encode(), path="/rollback"); print("rollback to running   ", r); assert "already running" in r
# the sender vanishing after the last byte does not stop the install
b = st()["boots"]
img = named("four"); send(img, stop=len(img)); s = wait_boot(b)
assert s["slots"][s["running_slot"]]["version"] == "four"; d.auth("POST", "/confirm"); print("four                   installed though the sender had gone")
# same image twice: not stored twice
n = len(lib()["images"])
s = install(named("one"), "one"); assert len(lib()["images"]) == n
for i in range(8):
    s = install(named(f"n{i}", bytes(i)), f"n{i}", confirm=True)
assert len(lib()["images"]) == lib()["spaces"]
if SIM:
    d.auth("POST", "/api/need", b"1")
    old = app.replace(b"listen_only", b"listen_0nly")
    r = send(old); print("without listen-only   ", r); assert "listen-only" in r
    d.auth("POST", "/api/need", b"0")
    d.auth("POST", "/api/check")
print("ok")

#!/usr/bin/env python3
"""Console tests on the device, including the interactive shell in a real terminal.

  tests/test_cli_device.py HOST

Uses on-chip loopback on ports 1 and 2; nothing may be attached. Restores settings.
"""
import hashlib, hmac, json, os, pty, re, select, socket, subprocess, sys, tempfile, time
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, os.path.join(ROOT, "tools"))
import n540
d = n540.Device(sys.argv[1])
TOOL = os.path.join(ROOT, "tools", "n540.py")
results = []
cfg = lambda: json.loads(d.get("/api/config"))
run = lambda line: n540.cli(d, line)


def tool(*a, inp=None):
    r = subprocess.run([TOOL, sys.argv[1], *a], input=inp, capture_output=True, timeout=60)
    return r.returncode, r.stdout.decode(errors="replace"), r.stderr.decode(errors="replace")


def ok(line):
    r = run(line)
    assert r.startswith("ok"), f"{line}: {r}"
    return r


def test(f):
    t = time.time()
    try:
        r, good = f(), True
    except Exception as e:
        r, good = f"{type(e).__name__}: {e}", False
    results.append(good)
    print(f"{'PASS' if good else 'FAIL'}  {f.__name__:26} {time.time()-t:5.1f}s  {r or ''}", flush=True)


def set_get():
    ok('set port1.baud=57600 port1.parity=odd "port1.name=Pump \\"A\\""')
    c = cfg()["ports"][0]
    assert (c["baud"], c["parity"], c["name"]) == (57600, "odd", 'Pump "A"'), c
    assert "port1.baud=57600\n" in run("get port1.baud")
    b = cfg()
    assert run("set port1.baud=9600 port2.baud=1").startswith("error")
    assert cfg() == b
    ok("set port1.baud=115200 port1.parity=none port1.loopback=1")


def send_and_log():
    assert "is off" in run("send 1 hi")
    ok("set port1.gap_ms=20")
    ok("enable 1")
    ok("log 1 clear")
    time.sleep(.2)
    ok('send 1 "hello\\r\\n"')
    time.sleep(.3)
    ok("send 1 hex 01 03 00 ff")
    time.sleep(.3)
    t = run("log 1")
    rows = [x.split("ago  ")[1] for x in t.splitlines()]
    assert len(rows) == 4 and rows[0].startswith("to line") and rows[1].startswith("from line"), t
    assert "68 65 6c 6c 6f 0d 0a  |hello..|" in rows[1] and "01 03 00 ff " in rows[3], t
    s = socket.create_connection((d.host, 4001), 5)
    assert "1 connected" in run("port 1")
    s.sendall(b"from net")
    s.settimeout(3)
    assert s.recv(20) == b"from net"
    ok("send 1 to-client")                       # a console send's answer reaches clients too
    assert s.recv(20) == b"to-client"
    s.close()
    ok('send 1 "%s"' % ("x" * 250))


def big_and_odd_lines():
    assert run("set " + " ".join(f"port1.gap_ms={k}" for k in range(23))).startswith("ok")
    assert run("set " + " ".join(f"port1.gap_ms={k}" for k in range(24))).startswith("error")
    assert run('set "a').startswith("error") and run("\0").strip() == "" and "unknown" in run("\xff\xfe%n%s")
    assert "unknown" in run("x" * 2000)
    for _ in range(40):
        run(os.urandom(200).hex())
    assert len(run("get")) > 1000


def needs_auth():
    import http.client
    for h in ({}, {"X-Auth": "0" * 64, "X-Body-SHA256": hashlib.sha256(b"status").hexdigest()}):
        st, b = d._raw("POST", "/api/cli", b"status", h)
        assert st == 401 and b"firmware" not in b, (st, b)


def no_secrets():
    t = run("get") + run("status") + run("help") + run("resets")
    assert "password" not in t and "web_" not in t and "key" not in t.replace("<key>", ""), t


def wiretest_guards():
    assert "Disconnect" in run("wiretest") and "port 1 is on" in run("wiretest yes")
    ok("disable 1")
    t = run("wiretest yes")
    assert len([x for x in t.splitlines() if x.startswith("sent on")]) == 4, t
    assert all(p["state"] == "off" for p in json.loads(d.get("/api/state"))["ports"])
    ok("enable 1")                                # ports still work afterwards
    s = socket.create_connection((d.host, 4001), 5)
    s.sendall(b"ok?")
    s.settimeout(3)
    assert s.recv(5) == b"ok?"
    s.close()
    return t.strip().splitlines()[-1]


def save_and_load():
    f = tempfile.mktemp()
    assert tool("save", f)[0] == 0
    before = cfg()
    ok('set port2.baud=1200 "port3.name=changed" device.name=x')
    assert cfg() != before
    rc, out, err = tool("load", f)
    assert rc == 0, (out, err)
    os.unlink(f)
    assert cfg() == before


def one_shot_commands():
    rc, out, _ = tool("ports")
    assert rc == 0 and "TCP server 4001" in out
    assert "firmware" in tool("status")[1] and json.loads(tool("info")[1])["mode"] == "app"
    rc, out, _ = tool("set", "device.name=two words", "port2.baud=4800")
    assert rc == 0 and cfg()["device"]["name"] == "two words" and cfg()["ports"][1]["baud"] == 4800, out
    rc, out, _ = tool("set", "port2.name", 'say "hi"')
    assert rc == 0 and cfg()["ports"][1]["name"] == 'say "hi"', out
    rc, out, _ = tool("set", "port1.baud=3")
    assert rc == 1 and "error" in out
    r = subprocess.run([TOOL, "ports"], env=dict(os.environ, N540_HOST=sys.argv[1]), capture_output=True, timeout=30)
    assert r.returncode == 0 and b"TCP server" in r.stdout, r
    rc, out, _ = tool("connect", "1", inp=b"piped\n")
    assert out == "piped\n", out
    assert "not an enabled TCP server" in tool("connect", "4")[2]


class Term:
    def __init__(self, *args):
        self.pid, self.fd = pty.fork()
        if not self.pid:
            os.environ["TERM"] = "xterm"
            os.environ["HOME"] = tempfile.mkdtemp()   # not the user's history
            os.execv(TOOL, [TOOL, sys.argv[1], *args])
        self.buf = ""

    def expect(self, text, t=10):
        end = time.time() + t
        while text not in self.buf:
            assert time.time() < end, f"wanted {text!r}, got {self.buf[-400:]!r}"
            if select.select([self.fd], [], [], .2)[0]:
                self.buf += os.read(self.fd, 4096).decode(errors="replace")
        i = self.buf.index(text) + len(text)
        got, self.buf = self.buf[:i], self.buf[i:]
        return got

    def send(self, s):
        os.write(self.fd, s.encode())

    def close(self):
        end = time.time() + 5
        while time.time() < end:
            try:
                if select.select([self.fd], [], [], .1)[0]:
                    os.read(self.fd, 4096)
            except OSError:
                pass
            if os.waitpid(self.pid, os.WNOHANG)[0]:
                return
        raise AssertionError("did not exit")


def interactive_shell():
    t = Term()
    t.expect("Type help")
    t.expect("> ")
    t.send("ports\r")
    t.expect("TCP server 4001")
    t.expect("> ")
    t.send("sta\tus\r" if False else "resets\r")
    t.expect("newest first")
    t.expect("> ")
    t.send("set port2.ba\t19200\r")                  # tab completes to port2.baud=
    t.expect("ok")
    t.expect("> ")
    assert cfg()["ports"][1]["baud"] == 19200
    t.send("nonsense\r")
    t.expect("unknown command nonsense")
    t.send("connect 1\r")
    t.expect("Ctrl-] to quit")
    time.sleep(.5)
    t.send("typed\r")
    t.expect("typed")
    t.send("\x1d")
    t.expect("> ")
    t.send("watch 1\r")
    t.expect("Ctrl-C to stop")
    time.sleep(1)
    ok("send 1 seen-live")
    t.expect("|seen-live|")
    t.send("\x03")
    t.expect("> ")
    t.send("save /nonexistent/x\r")
    t.expect("error")
    t.expect("> ")
    t.send("exit\r")
    t.close()


def watch_shows_each_packet_once():
    t = Term("watch", "1")
    t.expect("Ctrl-C to stop")
    time.sleep(1.2)
    for k in range(12):
        ok(f"send 1 pkt{k:02d}")
        time.sleep(.15)
    t.expect("|pkt11|")
    time.sleep(1.5)
    while select.select([t.fd], [], [], .3)[0]:
        t.buf += os.read(t.fd, 4096).decode(errors="replace")
    t.send("\x03")
    t.close()
    n = len(re.findall(r"\|pkt\d\d\|", t.buf))
    assert n <= 1, t.buf                              # only the echo of pkt11 may follow


def restart_command():
    u = json.loads(d.get("/api/state"))["uptime_s"]
    assert "restarting" in run("restart yes")
    n540.wait_back(d)
    assert json.loads(d.get("/api/state"))["uptime_s"] < u
    assert "restart" in run("status")


orig = cfg()
assert not any(p["enabled"] for p in orig["ports"]), "a port is in use; not touching it"
port0 = orig["mqtt"]["port"]
if orig["mqtt"]["enabled"]:
    run("set mqtt.port=1")                            # keep test noise out of Home Assistant
try:
    for f in (set_get, send_and_log, big_and_odd_lines, needs_auth, no_secrets, wiretest_guards, save_and_load,
              one_shot_commands, interactive_shell, watch_shows_each_packet_once, restart_command):
        test(f)
finally:
    L = ["device.name=" + orig["device"]["name"], f"mqtt.port={port0}"]
    for i, p in enumerate(orig["ports"]):
        L += [f"port{i+1}.{k}={int(v) if isinstance(v, bool) else v}" for k, v in p.items()]
    d.auth("POST", "/api/config", "\n".join(L).encode())
    print("settings restored:", cfg() == orig)
s = json.loads(d.get("/status"))
print(f"{sum(results)}/{len(results)} passed; rx_err {s['rx_err']} tx_drop {s['tx_drop']}")
sys.exit(0 if all(results) else 1)

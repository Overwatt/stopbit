#!/usr/bin/env python3
"""Control an N540 gateway over the network.

  n540.py HOST                             interactive console
  n540.py HOST COMMAND ...                 run one console command (see: n540.py HOST help)

Set N540_HOST to leave HOST out. Besides the console's own commands:

  n540.py HOST info                        versions, slots and resets as JSON
  n540.py HOST dump FLASH OUT [OFF LEN]    read external flash 0 or 1
  n540.py HOST run IMAGE.bin               load into SRAM and jump (no flash)
  n540.py HOST selfupdate LOADER.bin       replace the loader in internal flash
  n540.py HOST update APP.bin              install to the spare slot, boot, confirm
  n540.py HOST loader                      leave the app, stay in the loader
  n540.py HOST config | state              settings or live counters as JSON
  n540.py HOST save FILE                   save settings (no passwords)
  n540.py HOST load FILE                   load settings saved earlier
  n540.py HOST connect PORT                talk to a TCP-server port; Ctrl-] quits
  n540.py HOST watch PORT                  follow a port's traffic; Ctrl-C quits
  n540.py HOST mqtt-passwd                 set the MQTT broker password
  n540.py HOST passwd [--clear]            set or remove the web password
  n540.py HOST reboot
"""
import atexit, getpass, hashlib, hmac, http.client, json, os, select, socket, sys, termios, time, tty

KEYFILE = os.environ.get("N540_KEY", os.path.join(os.path.dirname(__file__), "..", "secrets", "n540.key"))

PBKDF2_ROUNDS = 30000        # must match web/index.html


class Device:
    def __init__(self, host):
        # Resolve once, IPv4 only: macOS stalls 5 s waiting for a .local AAAA record.
        self.name = host
        self.resolve()
        self.key = None

    def resolve(self):
        self.host = socket.getaddrinfo(self.name, 80, socket.AF_INET)[0][4][0]

    def _raw(self, method, path, body=b"", headers=None, timeout=60):
        c = http.client.HTTPConnection(self.host, 80, timeout=timeout)
        try:
            c.request(method, path, body=body or None, headers=headers or {})
            r = c.getresponse()
            data = r.read()
        finally:
            c.close()
        return r.status, data

    def get(self, path):
        st, data = self._raw("GET", path, timeout=10)
        if st != 200:
            raise SystemExit(f"GET {path}: {st} {data.decode(errors='replace').strip()}")
        return data

    def load_key(self):
        if self.key is None:
            self.key = bytes.fromhex(open(KEYFILE).read().strip())

    def auth(self, method, path, body=b"", nonce=None):
        self.load_key()
        nonce = nonce or self.get("/nonce").decode().strip()
        sha = hashlib.sha256(body).hexdigest()
        mac = hmac.new(self.key, f"{nonce}\n{method} {path}\n{sha}".encode(), hashlib.sha256).hexdigest()
        hdr = {"X-Auth": mac, "X-Body-SHA256": sha}
        if method == "POST":
            hdr["Content-Length"] = str(len(body))
        st, data = self._raw(method, path, body, hdr)
        if st != 200:
            raise SystemExit(f"{method} {path}: {st} {data.decode(errors='replace').strip()}")
        return data


def wait_back(d, secs=40):
    end = time.time() + secs
    time.sleep(1.5)
    while time.time() < end:
        try:
            d.resolve()      # loader and app may not share an address
            return json.loads(d.get("/status"))
        except (OSError, http.client.HTTPException, ValueError):
            time.sleep(0.5)
    raise SystemExit("device did not come back")


def derive(pw, salt):
    return hashlib.pbkdf2_hmac("sha256", pw.encode(), salt, PBKDF2_ROUNDS)


def set_password(d, pw):
    d.load_key()
    salt = os.urandom(16)
    nonce = d.get("/nonce").decode().strip()
    pad = hmac.new(d.key, b"newkey\n" + nonce.encode(), hashlib.sha256).digest()
    blob = bytes(x ^ y for x, y in zip(derive(pw, salt), pad))
    d.auth("POST", "/api/password", f"salt={salt.hex()}\nkey={blob.hex()}".encode(), nonce)


def set_mqtt_password(d, pw, extra=""):
    d.load_key()
    raw = pw.encode()
    if len(raw) > 63:
        raise SystemExit("at most 63 bytes")
    nonce = d.get("/nonce").decode().strip()
    pad = b"".join(hmac.new(d.key, b"mqttpw%d\n" % i + nonce.encode(), hashlib.sha256).digest() for i in (0, 1))
    enc = bytes(x ^ y for x, y in zip(raw.ljust(64, b"\0"), pad))
    d.auth("POST", "/api/config", f"mqtt.password_enc={enc.hex()}\n{extra}".encode(), nonce)


def enter_loader(d):
    s = json.loads(d.get("/status"))
    if s["mode"] != "loader":
        d.auth("POST", "/loader")
        s = wait_back(d)
        if s["mode"] != "loader":
            raise SystemExit("could not get into the loader")
    return s


LOCAL = ("info", "config", "state", "save", "load", "connect", "watch", "update", "dump", "passwd", "mqtt-passwd", "reboot",
         "loader", "selfupdate", "run")


def join(words):
    """Back to one command line; the console does its own quoting."""
    return " ".join('"' + w.replace('"', '\\"') + '"' if not w or " " in w or '"' in w else w for w in words)


def cli(d, line):
    if json.loads(d.get("/status"))["mode"] != "app":
        raise SystemExit("the gateway is in its loader, which has no console; try info, update or reboot")
    return d.auth("POST", "/api/cli", line.encode()).decode(errors="replace")


def port_cfg(d, n):
    c = json.loads(d.get("/api/config"))["ports"]
    if not 1 <= n <= len(c):
        raise SystemExit("port must be 1 to 4")
    return c[n - 1]


def connect(d, n):
    p = port_cfg(d, n)
    if not p["enabled"] or p["mode"] != "server":
        raise SystemExit(f"port {n} is not an enabled TCP server")
    s = socket.create_connection((d.host, p["local_port"]), timeout=5)
    s.settimeout(None)
    tty_in = sys.stdin.isatty()
    print(f"connected to port {n} ({p['baud']} baud). Ctrl-] to quit.", file=sys.stderr)
    old = termios.tcgetattr(0) if tty_in else None
    try:
        if tty_in:
            tty.setraw(0)
        eof_at = None
        while True:
            r, _, _ = select.select([s] + ([0] if eof_at is None else []), [], [], 0.2)
            if s in r:
                b = s.recv(4096)
                if not b:
                    break
                sys.stdout.buffer.write(b)
                sys.stdout.buffer.flush()
                if eof_at:
                    eof_at = time.time()
            if 0 in r:
                b = os.read(0, 1024)
                if tty_in and b"\x1d" in b:
                    break
                if not b:
                    eof_at = time.time()
                else:
                    s.sendall(b)
            if eof_at and time.time() - eof_at > 1:      # piped input: wait for the answer
                break
    finally:
        if old:
            termios.tcsetattr(0, termios.TCSADRAIN, old)
        s.close()


def watch(d, n):
    port_cfg(d, n)
    seen = None
    print(f"traffic on port {n}; Ctrl-C to stop", file=sys.stderr)
    try:
        while True:
            t = d.auth("POST", "/api/cli", f"log {n}".encode()).decode(errors="replace").splitlines()
            rows = [x.split("ago  ", 1)[1] for x in t if "ago  " in x]
            if seen is None:
                new = rows
            else:                                        # longest old tail that starts the new list
                k = next((k for k in range(len(rows), -1, -1) if seen[len(seen) - k:] == rows[:k] and k <= len(seen)), 0)
                new = rows[k:]
            for r in new:
                print(time.strftime("%H:%M:%S"), r, flush=True)
            seen = rows
            time.sleep(0.5)
    except KeyboardInterrupt:
        print()


def shell(d):
    import readline
    hist = os.path.expanduser("~/.n540_history")
    try:
        readline.read_history_file(hist)
    except OSError:
        pass
    readline.set_history_length(500)
    atexit.register(lambda: readline.write_history_file(hist))
    words = [l.split()[0] for l in cli(d, "help").splitlines()] + [x for x in LOCAL if x not in ("run", "selfupdate", "loader")] + ["exit"]
    keys = [l.split("=")[0] + "=" for l in cli(d, "get").splitlines()]

    def complete(text, i):
        line = readline.get_line_buffer()
        pool = words if readline.get_begidx() == 0 else [k if line.startswith("set") else k[:-1] for k in keys]
        m = [w for w in pool if w.startswith(text)]
        return m[i] if i < len(m) else None
    readline.set_completer(complete)
    readline.set_completer_delims(" ")
    readline.parse_and_bind("bind ^I rl_complete" if "libedit" in (readline.__doc__ or "") else "tab: complete")
    name = json.loads(d.get("/api/state"))["host"]
    print(cli(d, "status"), end="")
    print("Type help for commands, exit to leave.")
    while True:
        try:
            line = input(f"{name}> ").strip()
        except EOFError:
            print()
            return
        except KeyboardInterrupt:
            print()
            continue
        if line in ("exit", "quit"):
            return
        try:
            w = line.split()
            if w and w[0] in LOCAL:
                main([d.name] + w)
            elif line:
                print(cli(d, line), end="")
                if line == "help":
                    print("\nOn this computer: " + ", ".join(x for x in words if x in LOCAL))
        except SystemExit as e:
            if e.code:
                print(e.code)
        except (OSError, http.client.HTTPException, ValueError, IndexError) as e:
            print(f"error: {e}")


def main(a):
    if a and a[0] in ("-h", "--help"):
        print(__doc__)
        return
    env = os.environ.get("N540_HOST")
    if env and (not a or a[0] not in (env,) and "." not in a[0]):
        a = [env] + a
    if not a:
        raise SystemExit(__doc__)
    d = Device(a[0])
    if len(a) == 1:
        return shell(d)
    cmd = a[1]
    if cmd == "status":
        s = json.loads(d.get("/status"))
        print(cli(d, "status") if s["mode"] == "app" else json.dumps(s, indent=2), end="" if s["mode"] == "app" else "\n")
    elif cmd == "info":
        print(json.dumps(json.loads(d.get("/status")), indent=2))
    elif cmd == "dump":
        dev, out = int(a[2]), a[3]
        off = int(a[4], 0) if len(a) > 4 else 0
        n = int(a[5], 0) if len(a) > 5 else 0x200000 - off
        t = time.time()
        buf = bytearray()
        while len(buf) < n:
            k = min(0x40000, n - len(buf))
            part = d.auth("GET", f"/flash/{dev}?off={off + len(buf)}&len={k}")
            if len(part) != k:
                raise SystemExit("short read")
            buf += part
        with open(out, "wb") as f:
            f.write(buf)
        print(f"{n} bytes in {time.time()-t:.1f}s  sha256 {hashlib.sha256(buf).hexdigest()}")
    elif cmd == "update":
        img = open(a[2], "rb").read()
        want = img[0x2A8:0x2A8 + 24].split(b"\0")[0].decode()
        enter_loader(d)
        t = time.time()
        slot = int(d.auth("POST", "/install", img).split(b"slot=")[1])
        print(f"installed {want} ({len(img)} bytes) in {time.time()-t:.1f}s; booting")
        d.auth("POST", "/reboot")
        s = wait_back(d)
        # The version alone can't tell a rollback apart from success; the slot can.
        if s["mode"] != "app" or s["running_slot"] != slot or s["version"] != want:
            raise SystemExit("new image did not come up " f"(rolled back: now {s['mode']} "
                             f"{s['version']} slot {s['running_slot']}, wanted slot {slot}); not confirmed")
        d.auth("POST", "/confirm")
        print(f"running {s['version']} from slot {s['running_slot']}, confirmed")
    elif cmd in ("config", "state"):
        print(json.dumps(json.loads(d.get("/api/" + cmd)), indent=2))
    elif cmd == "save":
        text = d.auth("POST", "/api/cli", b"get").decode()
        if "cut short" in text or text.startswith("error"):
            raise SystemExit("could not read all settings")
        with open(a[2], "w") as f:
            f.write(text)
        print(f"{len(text.splitlines())} settings saved to {a[2]}")
    elif cmd == "load":
        body = "\n".join(l for l in open(a[2]).read().splitlines() if l.strip() and not l.startswith("#"))
        r = json.loads(d.auth("POST", "/api/config", body.encode()))
        print("network settings are on trial: run keep within 2 minutes or they revert" if r["net_pending"] else "loaded")
    elif cmd == "connect":
        connect(d, int(a[2]))
    elif cmd == "watch":
        watch(d, int(a[2]))
    elif cmd == "mqtt-passwd":
        set_mqtt_password(d, getpass.getpass("MQTT broker password (empty to remove): "))
        print("saved")
    elif cmd == "passwd":
        if a[2:] == ["--clear"]:
            d.auth("POST", "/api/password", b"clear")
            print("web password removed; settings are read-only in the browser")
            return
        pw = getpass.getpass("New web password: ")
        if len(pw) < 8 or pw != getpass.getpass("Again: "):
            raise SystemExit("passwords differ or are shorter than 8 characters")
        set_password(d, pw)
        print("web password set")
    elif cmd == "loader":
        print("in loader", enter_loader(d)["version"])
    elif cmd in ("run", "selfupdate"):
        img = open(a[2], "rb").read()
        enter_loader(d)
        t = time.time()
        d.auth("POST", "/" + cmd, img)
        print(f"sent {len(img)} bytes in {time.time()-t:.1f}s")
        if cmd == "selfupdate":
            s = wait_back(d)
            print(f"back up: {s['mode']} {s['version']}")
    elif cmd == "reboot":
        d.auth("POST", "/reboot")
        print("back up:", wait_back(d)["version"])
    else:
        text = cli(d, join(a[1:]))
        print(text, end="")
        if text.startswith("error"):
            sys.exit(1)


if __name__ == "__main__":
    main(sys.argv[1:])

#!/usr/bin/env python3
"""Control an N540 gateway over the network.

  n540.py HOST                             interactive console
  n540.py HOST COMMAND ...                 run one console command (see: n540.py HOST help)

Set N540_HOST to leave HOST out. Besides the console's own commands:

  n540.py HOST info                        versions, slots and resets as JSON
  n540.py HOST dump FLASH OUT [OFF LEN]    read external flash 0 or 1
  n540.py HOST run IMAGE.bin               load into SRAM and jump (no flash)
  n540.py HOST selfupdate LOADER.bin       replace the loader in internal flash
  n540.py HOST update APP.bin [--loader]   install to the spare slot, boot, confirm
                                           (--loader: through the loader, not the app)
  n540.py HOST https [on|off|forget]       HTTPS: show, or turn on or off; forget its certificate
  n540.py HOST cert-request NAME [OUT]     make a key and a signing request for NAME
  n540.py HOST cert-install FULLCHAIN.pem  install the certificate made from it
  n540.py HOST renewal [ZONE KEY_ID [on|off]]  renewal state, or set its Route 53 details
                                           (asks for the secret; N540_AWS_SECRET to pass it)
  n540.py HOST renew [--test]              renew now, or try on Let's Encrypt staging
  n540.py HOST firmware                    firmware kept on the gateway
  n540.py HOST rollback VERSION            start a kept one, confirm
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
        self.name, _, port = host.partition(":")
        self.port = int(port or 80)
        self.resolve()
        self.key = None

    def resolve(self):
        self.host = socket.getaddrinfo(self.name, self.port, socket.AF_INET)[0][4][0]

    def _raw(self, method, path, body=b"", headers=None, timeout=60):
        c = http.client.HTTPConnection(self.host, self.port, timeout=timeout)
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

    def auth(self, method, path, body=b"", nonce=None, timeout=60):
        self.load_key()
        nonce = nonce or self.get("/nonce").decode().strip()
        sha = hashlib.sha256(body).hexdigest()
        mac = hmac.new(self.key, f"{nonce}\n{method} {path}\n{sha}".encode(), hashlib.sha256).hexdigest()
        hdr = {"X-Auth": mac, "X-Body-SHA256": sha}
        if method == "POST":
            hdr["Content-Length"] = str(len(body))
        st, data = self._raw(method, path, body, hdr, timeout)
        if st != 200:
            raise SystemExit(f"{method} {path}: {st} {data.decode(errors='replace').strip()}")
        return data


def tls_command(d, body):
    d.auth("POST", "/api/tls", body)
    for _ in range(100):
        time.sleep(.2)
        t = json.loads(d.get("/api/tls"))
        if not t["busy"]:
            if t["error"]:
                raise SystemExit(t["error"])
            return


def has_library(d):
    if json.loads(d.get("/status"))["mode"] != "app":
        return False
    return d._raw("GET", "/api/library", timeout=10)[0] == 200


def started(d, want, slot, before):
    """Wait for the image just installed, then keep it."""
    s = wait_back(d, boots=before)
    # The version alone can't tell a rollback apart from success; the slot can.
    if s["mode"] != "app" or s["running_slot"] != slot or s["version"] != want:
        raise SystemExit(f"new image did not come up (rolled back: now {s['mode']} "
                         f"{s['version']} slot {s['running_slot']}, wanted slot {slot}); not confirmed")
    d.auth("POST", "/confirm")
    print(f"running {s['version']} from slot {s['running_slot']}, confirmed")


def wait_back(d, secs=40, boots=None):
    """`boots`: the count before; an answer with the same count is from before the restart."""
    end = time.time() + secs
    time.sleep(1.5)
    while time.time() < end:
        try:
            d.resolve()      # loader and app may not share an address
            s = json.loads(d.get("/status"))
            if boots is None or s["mode"] != "app" or s["boots"] > boots:
                return s
            time.sleep(0.5)
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


def acme_post(d, lines, secret=None):
    d.load_key()
    nonce = d.get("/nonce").decode().strip()
    body = lines
    if secret is not None:
        raw = secret.encode()
        if len(raw) > 63:
            raise SystemExit("at most 63 bytes")
        pad = b"".join(hmac.new(d.key, b"acmesec%d\n" % i + nonce.encode(), hashlib.sha256).digest() for i in (0, 1))
        body += "\nsecret_enc=" + bytes(x ^ y for x, y in zip(raw.ljust(64, b"\0"), pad)).hex()
    d.auth("POST", "/api/acme", body.encode(), nonce)


def show_renewal(d):
    a = json.loads(d.get("/api/acme"))
    print(f"clock {a['clock'] or 'not set'}, automatic {'on' if a['auto'] else 'off'}, zone {a['zone'] or '-'}, "
          f"key {a['key_id'][:4] + '...' if a['key_id'] else '-'}, secret {'set' if a['secret_set'] else 'not set'}")
    if a["name"]:
        print(f"{a['name']}: due {'now' if a['due_in_s'] <= 0 else 'in %d days' % (a['due_in_s'] // 86400)}")
    if a["running"]:
        print(("testing: " if a["staging"] else "renewing: ") + a["step"])
    if a["result"]:
        print("last:", a["result"])
    return a


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
        t = time.time()
        if "--loader" not in a and has_library(d):
            before = json.loads(d.get("/status"))["boots"]
            slot = int(d.auth("POST", "/install", img, timeout=150).split(b"slot=")[1])
            print(f"installed {want} ({len(img)} bytes) in {time.time()-t:.1f}s; booting")
        else:
            before = None
            enter_loader(d)
            t = time.time()
            slot = int(d.auth("POST", "/install", img).split(b"slot=")[1])
            print(f"installed {want} ({len(img)} bytes) in {time.time()-t:.1f}s; booting")
            d.auth("POST", "/reboot")
        started(d, want, slot, before)
    elif cmd == "https":
        if a[2:]:
            tls_command(d, a[2].encode())
        t = json.loads(d.get("/api/tls"))
        what = {"none": "no certificate", "self": "self-signed", "issued": "issued"}[t["kind"]]
        print(("on" if t["live"] else "off") + f", {what}" + (f" for {t['name']}, until {t['until']}" if t["until"] else ""))
        if t["sha256"]:
            print("fingerprint", t["sha256"])
        if t["request"]:
            print("waiting for a certificate for", t["request"])
        if t["error"]:
            print("error:", t["error"])
        print(f"{t['handshakes']} handshakes ({t['failed']} failed), last {t['last_ms']} ms in {t['last_slices']} slices, longest slice {t['slice_us']} us, "
              f"stack {t['stack'][0]} of {t['stack'][1]}, memory peak {t['heap'][1]} of {t['heap'][2]}")
    elif cmd == "cert-request":
        tls_command(d, b"request " + a[2].encode())
        b = json.loads(d.get("/api/tls/csr"))["csr"]
        pem = "-----BEGIN CERTIFICATE REQUEST-----\n" + "".join(b[i:i + 64] + "\n" for i in range(0, len(b), 64)) + "-----END CERTIFICATE REQUEST-----\n"
        if len(a) > 3:
            with open(a[3], "w") as f:
                f.write(pem)
            print("written to", a[3])
        else:
            print(pem, end="")
    elif cmd == "cert-install":
        import re, base64
        ders = [base64.b64decode(x) for x in re.findall(r"-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----", open(a[2]).read(), re.S)]
        if not ders:
            raise SystemExit("no certificate in that file")
        tls_command(d, b"cert\n" + b"".join(ders))
        t = json.loads(d.get("/api/tls"))
        print(f"installed: {t['name']}, until {t['until']}")
    elif cmd == "renewal":
        if len(a) > 3:
            secret = os.environ.get("N540_AWS_SECRET") or getpass.getpass("AWS secret access key: ")
            auto = "1" if a[4:] == ["on"] else "0" if a[4:] == ["off"] else None
            acme_post(d, f"zone={a[2]}\nkey_id={a[3]}" + (f"\nauto={auto}" if auto else ""), secret)
        show_renewal(d)
    elif cmd == "renew":
        d.auth("POST", "/api/acme", b"test" if a[2:] == ["--test"] else b"renew")
        last = ""
        while True:
            time.sleep(1)
            r = json.loads(d.get("/api/acme"))
            if r["step"] and r["step"] != last:
                print(" ", r["step"])
                last = r["step"]
            if not r["running"]:
                print(r["result"])
                if not r["result"].startswith(("Renewed", "Test passed")):
                    raise SystemExit(1)
                break
    elif cmd == "firmware":
        for i in sorted(json.loads(d.get("/api/library"))["images"], key=lambda i: -i["seq"]):
            state = "running" if i["running"] else "kept" if i["kept"] else "did not pass its trial" if i["tried"] else "not started yet"
            print(f"{i['version']:24} {i['size']:7}  {state:22}  {i['sha'][:12]}")
    elif cmd == "rollback":
        hit = [i for i in json.loads(d.get("/api/library"))["images"] if i["version"] == a[2] or i["sha"].startswith(a[2])]
        if len(hit) != 1:
            raise SystemExit("no such firmware kept" if not hit else "more than one matches; use its hash (see: firmware)")
        before = json.loads(d.get("/status"))["boots"]
        slot = int(d.auth("POST", "/rollback", b"sha=" + hit[0]["sha"].encode(), timeout=150).split(b"slot=")[1])
        started(d, hit[0]["version"], slot, before)
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

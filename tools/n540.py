#!/usr/bin/env python3
"""Control an N540 loader over the network.

  n540.py HOST status
  n540.py HOST dump FLASH OUT [OFF LEN]    read external flash 0 or 1
  n540.py HOST run IMAGE.bin               load into SRAM and jump (no flash)
  n540.py HOST selfupdate LOADER.bin       replace the loader in internal flash
  n540.py HOST update APP.bin              install to the spare slot, boot, confirm
  n540.py HOST loader                      leave the app, stay in the loader
  n540.py HOST reboot
"""
import hashlib, hmac, http.client, json, os, socket, sys, time

KEYFILE = os.environ.get("N540_KEY", os.path.join(os.path.dirname(__file__), "..", "secrets", "n540.key"))


class Device:
    def __init__(self, host):
        # Resolve once, IPv4 only: macOS stalls 5 s waiting for a .local AAAA record.
        self.host = socket.getaddrinfo(host, 80, socket.AF_INET)[0][4][0]
        self.key = None

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

    def auth(self, method, path, body=b""):
        if self.key is None:
            self.key = bytes.fromhex(open(KEYFILE).read().strip())
        nonce = self.get("/nonce").decode().strip()
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
            return json.loads(d.get("/status"))
        except (OSError, http.client.HTTPException, ValueError):
            time.sleep(0.5)
    raise SystemExit("device did not come back")


def enter_loader(d):
    s = json.loads(d.get("/status"))
    if s["mode"] != "loader":
        d.auth("POST", "/loader")
        s = wait_back(d)
        if s["mode"] != "loader":
            raise SystemExit("could not get into the loader")
    return s


def main(a):
    if len(a) < 2:
        raise SystemExit(__doc__)
    d, cmd = Device(a[0]), a[1]
    if cmd == "status":
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
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])

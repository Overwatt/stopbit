#!/usr/bin/env python3
"""HTTPS.  tests/test_tls.py HOST:PORT TLSPORT   (simulator: 127.0.0.1:8541 8543)
Turns HTTPS on and off and replaces its certificate. Simulator only: it also restarts."""
import base64, concurrent.futures as cf, hashlib, hmac, http.client, json, os, random, socket, ssl, struct, subprocess, sys, tempfile, threading, time
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import n540
d = n540.Device(sys.argv[1]); TP = int(sys.argv[2]); H = d.host
T = tempfile.mkdtemp(); random.seed(5)
tls = lambda: json.loads(d.get("/api/tls"))
def cmd(body, ok=True):
    d.load_key(); body = body if isinstance(body, bytes) else body.encode()
    nonce = d.get("/nonce").decode().strip(); sha = hashlib.sha256(body).hexdigest()
    mac = hmac.new(d.key, f"{nonce}\nPOST /api/tls\n{sha}".encode(), hashlib.sha256).hexdigest()
    st, out = d._raw("POST", "/api/tls", body, {"X-Auth": mac, "X-Body-SHA256": sha, "Content-Length": str(len(body))})
    assert (st == 200) == ok, (st, out)
    for _ in range(100):
        if not tls()["busy"]: break
        time.sleep(.1)
    return out.decode()
def ctx(**kw):
    c = ssl.create_default_context(**kw)
    if not kw: c.check_hostname = False; c.verify_mode = ssl.CERT_NONE
    return c
C = ctx()
def raw(timeout=10):
    for _ in range(200):                        # all four places may be taken for a moment
        try:
            s = socket.create_connection((H, TP), timeout=timeout); s.setblocking(False)
            time.sleep(.01)
            try:
                if s.recv(1) == b"": raise ConnectionResetError
            except BlockingIOError: s.settimeout(timeout); return s
        except ConnectionResetError: time.sleep(.05)
    raise SystemExit("no place came free")
def conn(c=None, name=None, timeout=20, session=None):
    s = socket.create_connection((H, TP), timeout=timeout)
    return (c or C).wrap_socket(s, server_hostname=name or "x", session=session)
def get(s, path, extra=""):
    s.sendall(f"GET {path} HTTP/1.1\r\nHost: x\r\n{extra}\r\n".encode()); return answer(s)
def answer(s):
    b = b""
    while b"\r\n\r\n" not in b:
        x = s.recv(4096)
        if not x: raise EOFError(b)
        b += x
    h, b = b.split(b"\r\n\r\n", 1); n = int([l for l in h.split(b"\r\n") if l.lower().startswith(b"content-length")][0].split(b":")[1])
    while len(b) < n:
        x = s.recv(4096)
        if not x: raise EOFError
        b += x
    return int(h.split()[1]), h.decode(), b[:n]
def quiet(what, secs=30):
    end = time.time() + secs
    while time.time() < end:
        t = tls()
        if t["connections"] == 0 and t["heap"][0] == BASE and int(d.get("/api/pbufs")) <= 2: return     # this request's own
        time.sleep(.3)
    raise SystemExit(f"{what}: left over {t['connections']} connections, heap {t['heap'][0]} of {BASE}, pbufs {d.get('/api/pbufs')}")
def ok(what): print("ok  ", what, flush=True)
def run(*a, **k): return subprocess.run(a, capture_output=True, check=True, **k).stdout

cmd("forget"); t = tls(); assert not t["on"] and not t["live"] and t["kind"] == "none"
try: conn(); raise SystemExit("443 open while off")
except ConnectionRefusedError: ok("off: port closed")
cmd("on"); t = tls(); assert t["live"] and t["kind"] == "self" and t["name"].endswith(".local"); BASE = t["heap"][0]
s = conn(); der = s.getpeercert(True); assert hashlib.sha256(der).hexdigest() == t["sha256"] and s.version() == "TLSv1.2"
open(T + "/self.der", "wb").write(der)
txt = run("openssl", "x509", "-inform", "DER", "-in", T + "/self.der", "-noout", "-text").decode()
assert "DNS:" + t["name"] in txt and "ecdsa-with-SHA256" in txt and "prime256v1" in txt; s.close()
run("openssl", "x509", "-inform", "DER", "-in", T + "/self.der", "-out", T + "/self.pem")
s = conn(ctx(cafile=T + "/self.pem"), t["name"]); s.close(); ok("self-signed: trusted by its own name")
try: conn(ctx(cafile=T + "/self.pem"), "other.local"); raise SystemExit("wrong name accepted")
except ssl.SSLCertVerificationError: pass

for lo, hi, why in ((ssl.TLSVersion.TLSv1_3, ssl.TLSVersion.TLSv1_3, "1.3 only"),):
    c = ctx(); c.minimum_version, c.maximum_version = lo, hi
    try: conn(c); raise SystemExit("accepted " + why)
    except ssl.SSLError: pass
for ciphers in ("ECDHE-RSA-AES128-GCM-SHA256", "ECDHE-ECDSA-AES256-SHA", "AES128-SHA"):
    c = ctx(); c.maximum_version = ssl.TLSVersion.TLSv1_2; c.set_ciphers(ciphers + ":@SECLEVEL=0")
    try: conn(c); raise SystemExit("accepted " + ciphers)
    except ssl.SSLError: pass
s = conn(); assert s.cipher()[0] == "ECDHE-ECDSA-AES128-GCM-SHA256"; s.close(); quiet("versions"); ok("one version, one cipher suite")

s = conn()
for i in range(200):
    st, h, b = get(s, "/status"); assert st == 200 and "keep-alive" in h.lower() and json.loads(b)["mode"] == "app"
st, h, b = get(s, "/nothing"); assert st == 404 and "keep-alive" in h.lower()
st, h, b = get(s, "/status", "Connection: close\r\n"); assert "Connection: close" in h
assert s.recv(10) == b""; s.close(); ok("200 requests on one connection, then closed when asked")
s = conn(); s.sendall(b"GET /status HTTP/1.0\r\n\r\n"); assert "close" in answer(s)[1]; s.close()
s = conn(); s.sendall(b"".join(b"GET /status HTTP/1.1\r\nHost: x\r\n\r\n" for _ in range(3)))
try:
    for _ in range(3): assert answer(s)[0] == 200
except EOFError: pass
s.close(); quiet("pipelining")
s = conn()
for ch in b"GET /status HTTP/1.1\r\nHost: x\r\n\r\n": s.sendall(bytes([ch]))
assert answer(s)[0] == 200
s.sendall(b"GET /" + b"a" * 2000 + b" HTTP/1.1\r\n\r\n"); assert answer(s)[0] in (431, 400); s.close(); quiet("odd requests"); ok("byte by byte, too long, pipelined")

# the whole page, every time; also for a client that is slow to take it
hc = http.client.HTTPConnection(H, d.port); hc.request("GET", "/"); page = hc.getresponse().read()
s = conn()
for i in range(400):
    st, h, b = get(s, "/"); assert b == page, (i, len(b), len(page))
s.close()
for i in range(8):
    s = conn(); s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2048); s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n"); b = b""
    while not b.endswith(page[-64:]):
        time.sleep(.01); x = s.recv(700); assert x, len(b); b += x
    s.close()
quiet("pages"); ok(f"the whole page ({len(page)} bytes) 408 times, some slowly")

# the web password's or the update key's signature works the same over TLS
def post(s, path, body, claim=None, mac=None, chunk=None):
    nonce = get(s, "/nonce")[2].decode().strip(); sha = hashlib.sha256(body if claim is None else claim).hexdigest()
    mac = mac or hmac.new(d.key, f"{nonce}\nPOST {path}\n{sha}".encode(), hashlib.sha256).hexdigest()
    s.sendall(f"POST {path} HTTP/1.1\r\nHost: x\r\nX-Auth: {mac}\r\nX-Body-SHA256: {sha}\r\nContent-Length: {len(body)}\r\n\r\n".encode())
    for i in range(0, len(body), chunk or 65536): s.sendall(body[i:i + (chunk or 65536)])
    return answer(s)
s = conn()
for i in range(20): assert post(s, "/api/check", b"x" * (i * 100))[0] == 200
assert post(s, "/api/check", b"")[0] == 200
st, h, b = post(s, "/api/check", b"", mac="0" * 64); assert st == 401 and "keep-alive" in h.lower()
assert get(s, "/status")[0] == 200
st, h, b = post(s, "/api/check", b"abc", mac="0" * 64); assert st == 401 and "close" in h.lower()     # its body was not read
s.close(); quiet("posts"); ok("signed posts, refusals")

c = ctx(); c.maximum_version = ssl.TLSVersion.TLSv1_2
s = conn(c); get(s, "/status"); ses = s.session; s.close()
t0 = tls(); s = conn(c, session=ses); assert s.session_reused and get(s, "/status")[0] == 200; s.close(); ok(f"resumed ({tls()['last_slices']} slices against {t0['last_slices']})")
quiet("resumption")

n0 = tls()["handshakes"]
for i in range(60): s = conn(); get(s, "/status"); s.close()
quiet("60 in turn"); assert tls()["handshakes"] >= n0 + 60; ok("60 connections one after another")
res = []
def one():
    for tries in range(60):
        try:
            s = conn()
            for _ in range(5): assert get(s, "/api/library")[0] == 200
            s.close(); res.append(tries); return
        except (OSError, EOFError): time.sleep(.2 + random.random() / 2)
    res.append(None)
th = [threading.Thread(target=one) for _ in range(12)]; [x.start() for x in th]; [x.join() for x in th]
assert None not in res, res; quiet("12 at once"); t = tls(); ok(f"12 at once through 4 places (heap peak {t['heap'][1]} of {t['heap'][2]}, {t['refused']} turned away and retried)")
# data on two connections while others are in the middle of their handshakes
stop = time.time() + 12; done = [0, 0]; bad = []
def talk(k):
    s = None
    while time.time() < stop:
        try:
            s = s or conn()
            if get(s, "/")[2] != page or json.loads(get(s, "/api/library")[2])["spaces"] != 7: bad.append("wrong data")
            done[k] += 1
        except (OSError, EOFError): s = None
def shakes():
    while time.time() < stop:
        try: s = conn(); get(s, "/status"); s.close()
        except (OSError, EOFError): time.sleep(.05)
n0 = tls()["handshakes"]; th = [threading.Thread(target=talk, args=(k,)) for k in (0, 1)] + [threading.Thread(target=shakes) for _ in range(2)]
[x.start() for x in th]; [x.join() for x in th]; assert not bad and min(done) > 20, (bad[:3], done)
quiet("mixed"); ok(f"{sum(done)} pages during {tls()['handshakes'] - n0} handshakes, all right")
# idle kept-alive connections give way to new ones
idle = [conn() for _ in range(4)]; [get(s, "/status") for s in idle]
for i in range(4): s = conn(); assert get(s, "/status")[0] == 200; s.close()
[s.close() for s in idle]; quiet("idle"); ok("idle ones make room")

# cut off at every byte of a real handshake
rec = []
tap = raw()
inb, outb = ssl.MemoryBIO(), ssl.MemoryBIO(); o = C.wrap_bio(inb, outb, server_hostname="x")
sent = 0
for _ in range(20):
    try: o.do_handshake(); break
    except ssl.SSLWantReadError: pass
    b = outb.read(); sent += len(b); rec.append(b); tap.sendall(b); inb.write(tap.recv(8192))
b = outb.read(); tap.close()
hello = rec[0]
for cut in list(range(0, len(hello), 7)) + [len(hello) - 1, len(hello)]:
    s = raw(); s.sendall(hello[:cut])
    if cut % 2: s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    s.close()
quiet("cut hellos"); ok(f"cut at {len(hello) // 7 + 2} points of the hello")
for i in range(150):                            # at every later stage, and part way through the sums
    s = raw()
    inb, outb = ssl.MemoryBIO(), ssl.MemoryBIO(); o = C.wrap_bio(inb, outb, server_hostname="x"); left = random.randrange(0, 900)
    try:
        for _ in range(8):
            try: o.do_handshake(); break
            except ssl.SSLWantReadError: pass
            b = outb.read()
            s.sendall(b[:left]); left -= len(b)
            if left < 0: break
            s.settimeout(5); inb.write(s.recv(8192))
    except (OSError, ssl.SSLError): pass
    if i % 3: s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    s.close()
quiet("cut later"); ok("cut at 150 random points later on")
for i in range(150):
    s = raw(); b = bytearray(hello)
    k = i % 3
    if k == 0: b = os.urandom(random.randrange(1, 3000))
    elif k == 1:
        for _ in range(random.randrange(1, 8)): b[random.randrange(len(b))] = random.randrange(256)
    else: b = b"\x16\x03\x01" + struct.pack(">H", random.randrange(65536)) + os.urandom(random.randrange(2000))
    try: s.sendall(bytes(b)); s.settimeout(.02); s.recv(100)
    except OSError: pass
    s.close()
quiet("garbage", 60); ok("150 pieces of garbage")
for i in range(40):
    s = conn(); get(s, "/status")
    try: socket.socket(fileno=os.dup(s.fileno())).sendall(os.urandom(random.randrange(1, 500))); s.recv(100)
    except (OSError, ssl.SSLError): pass
    s.close()
quiet("garbage in the stream"); ok("40 pieces of garbage inside open connections")
# connections that say nothing hold no memory and time out
h = [socket.create_connection((H, TP), timeout=40) for _ in range(4)]; time.sleep(.3); t = tls(); assert t["heap"][0] == BASE and t["connections"] == 4
time.sleep(21); assert tls()["connections"] == 0; [x.close() for x in h]; ok("silent connections cost nothing and are dropped")

# uploads, up to the largest record a browser sends
app = open("build/app.bin", "rb").read()
def named(v): b = bytearray(app); b[0x2A8:0x2C0] = v.encode().ljust(24, b"\0"); return bytes(b)
lib = lambda: [i["version"] for i in json.loads(d.get("/api/library"))["images"]]
def back(n):
    for _ in range(100):
        try:
            x = json.loads(d.get("/status"))
            if x["boots"] > n: return x
        except (OSError, SystemExit, ValueError): pass
        time.sleep(.2)
for v, chunk in (("tls-a", None), ("tls-b", 100), ("tls-c", 16384)):
    n = json.loads(d.get("/status"))["boots"]; s = conn(timeout=60); t0 = time.time(); st, h, b = post(s, "/install", named(v), chunk=chunk); s.close()
    assert st == 200 and b.startswith(b"ok slot"), (st, b); x = back(n); assert x["version"] == v; d.auth("POST", "/confirm"); assert v in lib()
    print(f"     {v}: {time.time() - t0:.1f} s")
s = conn(timeout=60); st, h, b = post(s, "/install", named("bad"), claim=b"x"); assert st == 400 and "bad" not in lib(); s.close()
s = conn(timeout=60); st, h, b = post(s, "/install", os.urandom(60000)); assert st == 400; s.close()
s = conn(timeout=60); s.sendall(b"GET /nonce HTTP/1.1\r\n\r\n"); s.close()
for i in range(12):                             # cut uploads
    s = conn(); nonce = get(s, "/nonce")[2].decode().strip(); body = named("cut"); sha = hashlib.sha256(body).hexdigest()
    mac = hmac.new(d.key, f"{nonce}\nPOST /install\n{sha}".encode(), hashlib.sha256).hexdigest()
    s.sendall(f"POST /install HTTP/1.1\r\nX-Auth: {mac}\r\nX-Body-SHA256: {sha}\r\nContent-Length: {len(body)}\r\n\r\n".encode())
    try: s.settimeout(1); s.sendall(body[:random.randrange(len(body) - 1)])
    except OSError: pass
    s.close(); time.sleep(.5)
for _ in range(100):
    if not json.loads(d.get("/api/library"))["job"]: break
    time.sleep(.2)
assert "cut" not in lib(); BASE = tls()["heap"][0] if tls()["connections"] == 0 else BASE; quiet("uploads"); ok("uploads: three sizes of piece, wrong, junk, cut off")

# the private key does not leave in a flash dump
for off in (0xF4000, 0xF5FF0, 0xF6000, 0xF7FF0):
    assert d.auth("GET", f"/flash/1?off={off}&len=16") == b"\xff" * 16
dump = d.auth("GET", "/flash/1?off=978944&len=49152"); assert dump[0xF4000 - 978944:0xF8000 - 978944] == b"\xff" * 0x4000
ok("keys blanked in flash dumps")

# a certificate, through a CA that signs with other algorithms than ours
def ca(name, key, parent=None, md="-sha384"):
    run("openssl", "req", "-new", "-key", key, "-subj", "/CN=" + name, "-out", T + "/c.csr")
    open(T + "/ca.ext", "w").write("basicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign\nsubjectKeyIdentifier=hash\n" + ("authorityKeyIdentifier=keyid\n" if parent else ""))
    a = ["openssl", "x509", "-req", "-in", T + "/c.csr", "-days", "90", "-extfile", T + "/ca.ext", md, "-out", T + f"/{name}.pem"]
    run(*a, *(["-CA", T + f"/{parent[0]}.pem", "-CAkey", parent[1], "-CAcreateserial"] if parent else ["-key", key]))
run("openssl", "genrsa", "-out", T + "/root.key", "4096"); run("openssl", "ecparam", "-genkey", "-name", "secp384r1", "-out", T + "/int.key")
ca("root", T + "/root.key", md="-sha256"); ca("inter", T + "/int.key", ("root", T + "/root.key"), md="-sha256")
for bad in ("request", "request ", "request Gateway.example.com", "request nodots", "request a..b", "request -", "request a b.c", "request " + "a" * 70 + ".com", "nope", "on\n", "cert\n", "cert\nxxxx", "cert\n0\x82\xff\xff"):
    cmd(bad, ok=False)
N = "gateway.example.com"; cmd("request " + N); t = tls(); assert t["csr"] and t["request"] == N and t["kind"] == "self"
csr = base64.b64decode(json.loads(d.get("/api/tls/csr"))["csr"]); open(T + "/r.der", "wb").write(csr)
txt = run("openssl", "req", "-inform", "DER", "-in", T + "/r.der", "-noout", "-text", "-verify", stderr=None).decode() if False else subprocess.run(["openssl", "req", "-inform", "DER", "-in", T + "/r.der", "-noout", "-text", "-verify"], capture_output=True).stdout.decode()
assert "DNS:" + N in txt and "CN" in txt, txt
def sign(names, csrfile=T + "/r.der"):
    open(T + "/l.ext", "w").write("subjectAltName=" + ",".join("DNS:" + n for n in names) + "\nauthorityKeyIdentifier=keyid\n")
    run("openssl", "x509", "-req", "-inform", "DER", "-in", csrfile, "-CA", T + "/inter.pem", "-CAkey", T + "/int.key", "-CAcreateserial", "-days", "89", "-sha384", "-extfile", T + "/l.ext", "-out", T + "/leaf.pem")
    return b"".join(run("openssl", "x509", "-in", T + f"/{f}.pem", "-outform", "DER") for f in ("leaf", "inter"))
run("openssl", "ecparam", "-genkey", "-name", "prime256v1", "-out", T + "/o.key"); run("openssl", "req", "-new", "-key", T + "/o.key", "-subj", "/CN=" + N, "-outform", "DER", "-out", T + "/o.der")
assert "different key" in cmd(b"cert\n" + sign([N], T + "/o.der"), ok=False)
assert "different name" in cmd(b"cert\n" + sign(["other.example.com"]), ok=False)
chain = sign(["www.example.com", N])
assert "not a certificate" in cmd(b"cert\n" + chain[:-5], ok=False)
assert tls()["kind"] == "self"
cmd(b"cert\n" + chain); t = tls(); assert t["kind"] == "issued" and t["name"] == N and t["live"] and t["until"], t
# as long as the real ones: four certificates, about 3.5 KB, the last cross-signed with RSA
good = ctx(cafile=T + "/root.pem")
ca("x1", T + "/root.key", md="-sha256"); run("openssl", "ecparam", "-genkey", "-name", "secp384r1", "-out", T + "/x2.key"); run("openssl", "ecparam", "-genkey", "-name", "secp384r1", "-out", T + "/ye.key")
ca("x2", T + "/x2.key", ("x1", T + "/root.key"), md="-sha256"); ca("ye", T + "/ye.key", ("x2", T + "/x2.key")); ca("inter", T + "/int.key", ("ye", T + "/ye.key"))
sign([N, "www." + N]); long = b"".join(run("openssl", "x509", "-in", T + f"/{f}.pem", "-outform", "DER") for f in ("leaf", "inter", "ye", "x2", "x1"))
print("     chain of", len(long), "bytes"); assert 3300 < len(long) <= 4096, len(long)
cmd("request " + N); csr = base64.b64decode(json.loads(d.get("/api/tls/csr"))["csr"]); open(T + "/r.der", "wb").write(csr)
sign([N]); long = b"".join(run("openssl", "x509", "-in", T + f"/{f}.pem", "-outform", "DER") for f in ("leaf", "inter", "ye", "x2", "x1"))
cmd(b"cert\n" + long); c1 = ctx(cafile=T + "/x1.pem"); s = conn(c1, N); assert get(s, "/status")[0] == 200; s.close(); ok("a long chain is sent whole and verifies")
assert "too long" in cmd(b"cert\n" + long + b"x" * (4100 - len(long)), ok=False)
run("openssl", "req", "-new", "-key", T + "/int.key", "-subj", "/CN=inter", "-out", T + "/c.csr"); ca("inter", T + "/int.key", ("root", T + "/root.key"), md="-sha256")
cmd("request " + N); csr = base64.b64decode(json.loads(d.get("/api/tls/csr"))["csr"]); open(T + "/r.der", "wb").write(csr)
cmd(b"cert\n" + sign(["www.example.com", N])); s = conn(good, N); assert get(s, "/status")[0] == 200; s.close(); ok("certificate installed; full chain verifies from the root")
def host(h):
    c = http.client.HTTPConnection(H, d.port, timeout=5); c.request("GET", "/", headers={"Host": h}); r = c.getresponse(); r.read(); return r.status, r.getheader("Location")
assert host(N) == (302, f"https://{N}/") and host(N.upper() + ":80") == (302, f"https://{N}/")
for x in (H, "n540-sim.local", N + ".evil.com", "x" + N, N[:-1], ""): assert host(x)[0] == 200, x
ok("only that name is redirected")
# a new certificate while connections have just closed (lwIP keeps the port for a while)
for i in range(5): s = conn(good, N); get(s, "/status"); s.close()
cmd("request " + N); csr = base64.b64decode(json.loads(d.get("/api/tls/csr"))["csr"]); open(T + "/r.der", "wb").write(csr)
cmd(b"cert\n" + sign([N])); time.sleep(2); t = tls(); assert t["live"] and not t["error"], t
s = conn(good, N); assert get(s, "/status")[0] == 200; s.close(); ok("a new certificate goes live straight after connections closed")

# long replies share a few buffers: many at once, plain and secure, each whole or "busy"
def big_plain(n):
    c = http.client.HTTPConnection(H, d.port, timeout=10); c.request("GET", f"/api/big?n={n}"); r = c.getresponse(); b = r.read(); c.close()
    assert r.status == 503 or len(b) == n, (r.status, len(b)); return r.status
def big_secure(k):
    s = conn(good, N); out = []
    for i in range(12):
        st, h, b = get(s, "/api/big?n=4000"); assert st == 503 or len(b) == 4000, (st, len(b)); out.append(st)
    s.close(); return out
def burst():
    with cf.ThreadPoolExecutor(8) as ex:
        fs = [ex.submit(big_secure, k) for k in range(4)]; plain = list(ex.map(big_plain, [4000] * 120))
        r = plain + [x for f in fs for x in f.result()]
    assert set(r) <= {200, 503} and r.count(200) >= len(r) * 0.9, {k: r.count(k) for k in set(r)}; return r
burst(); time.sleep(1)
BASE = tls()["heap"][0] if tls()["connections"] == 0 else BASE   # the ticket key may have turned over meanwhile
r = burst(); quiet("long replies at once")                        # and nothing more is held after another
s = conn(good, N); assert get(s, "/api/big?n=4300")[0] == 200 and big_plain(4300) == 200    # the longest that fits
assert get(s, "/api/big?n=4400")[0] == 500; s.close()                                     # never cut short
quiet("long replies"); ok(f"long replies share buffers: {r.count(200)} of {len(r)} at once whole, the rest busy")
# it survives a restart, and so does a pending request
n = json.loads(d.get("/status"))["boots"]; cmd("request new.example.com"); d.auth("POST", "/reboot"); back(n); time.sleep(1)
t = tls(); assert t["kind"] == "issued" and t["live"] and t["request"] == "new.example.com" and t["csr"], t
s = conn(good, N); s.close(); ok("certificate and request survive a restart")
# off, asked for over HTTPS itself
BASE = tls()["heap"][0]; s = conn(good, N); st, h, b = post(s, "/api/tls", b"off"); assert st == 200; s.close(); time.sleep(1)
try: conn(); raise SystemExit("still open")
except OSError: pass
assert host(N)[0] == 200; cmd("on"); s = conn(good, N); s.close(); ok("off and on again, keeping the certificate")
cmd("forget"); t = tls(); assert t["kind"] == "none" and not t["csr"] and not t["request"]
t = tls(); print("longest slice", t["slice_us"], "us, stack", t["stack"], "heap", t["heap"])
print("all ok")

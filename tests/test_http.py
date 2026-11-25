#!/usr/bin/env python3
"""HTTP robustness: parallel load, malformed input, slow clients.  tests/test_http.py HOST"""
import concurrent.futures as cf, http.client, json, os, random, socket, sys, time
H = sys.argv[1]
def get(p):
    c = http.client.HTTPConnection(H, 80, timeout=8); c.request("GET", p); r = c.getresponse(); b = r.read(); c.close(); return r.status, b
def up(): return json.loads(get("/status")[1])
s0 = up()
def one(i):
    try: return get(("/api/state", "/api/config", "/", "/status")[i % 4])[0]
    except OSError: return "err"
with cf.ThreadPoolExecutor(4) as ex: r = list(ex.map(one, range(400)))
print("400 requests, 4 at a time:", {k: r.count(k) for k in set(r)})
assert r.count(200) == 400
raw = [b"\r\n\r\n", b"GET\r\n\r\n", b"GET  \r\n\r\n", b"A" * 5000, b"GET /" + b"x" * 900 + b" HTTP/1.1\r\n\r\n",
       b"POST /api/config HTTP/1.1\r\nContent-Length: 4294967295\r\n\r\n", b"POST /api/config HTTP/1.1\r\nContent-Length: -1\r\n\r\n",
       b"POST /install HTTP/1.1\r\nContent-Length: 99999999\r\n\r\n", b"GET /flash/9?off=0&len=1 HTTP/1.1\r\n\r\n", b"GET /flash/ HTTP/1.1\r\n\r\n",
       b"GET /api/../status HTTP/1.1\r\n\r\n", b"\0" * 300, b"GET /%s%n%x HTTP/1.1\r\n\r\n",
       b"POST /api/config HTTP/1.1\r\nX-Auth: zz\r\nX-Body-SHA256: \r\nContent-Length: 5\r\n\r\nabcde"] + [os.urandom(random.randrange(1, 3000)) for _ in range(60)]
for m in raw:
    try:
        s = socket.create_connection((H, 80), timeout=4); s.sendall(m); s.settimeout(.4)
        try: s.recv(200)
        except OSError: pass
        s.close()
    except OSError: pass
print("malformed input: still up,", up()["version"])
held = [socket.create_connection((H, 80), timeout=4) for _ in range(8)]   # hold every slot open
t = time.time(); ok = False
while time.time() - t < 40:
    try: ok = get("/status")[0] == 200; break
    except OSError: time.sleep(1)
print(f"8 silent connections: recovered after {time.time()-t:.0f}s"); assert ok
[x.close() for x in held]
s1 = up(); assert s1["boots"] == s0["boots"], "rebooted during the test"
print("no reboots; rx_err", s1["rx_err"], "tx_drop", s1["tx_drop"])

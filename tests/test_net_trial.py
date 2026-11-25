#!/usr/bin/env python3
"""Network-change safety: unkept settings must revert. Takes about 5 minutes.

  tests/test_net_trial.py HOST GATEWAY
"""
import json, os, socket, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import n540
d = n540.Device(sys.argv[1]); gw = sys.argv[2]
get = lambda p: json.loads(d.get(p))
post = lambda b: json.loads(d.auth("POST", "/api/config", b.encode()))
ip0 = get("/api/state")["ip"]; mask = get("/api/state")["mask"]

assert post(f"net.mode=static\nnet.ip={ip0}\nnet.mask={mask}\nnet.gateway={gw}")["net_pending"]
time.sleep(3)
s = get("/api/state"); assert s["net_pending"] and s["ip"] == ip0, s
assert get("/api/config")["net"]["mode"] == "static"
print("1 static on trial, reachable,", s["net_pending_s"], "s left", flush=True)
t = time.time()
while get("/api/state")["net_pending"]:
    assert time.time() - t < 140; time.sleep(2)
assert get("/api/config")["net"]["mode"] == "dhcp"
time.sleep(8); assert get("/api/state")["ip"] != "0.0.0.0"
print(f"2 reverted by itself after {time.time()-t:.0f}s", flush=True)

post("net.mode=static\nnet.ip=192.168.250.2\nnet.mask=255.255.255.0")
time.sleep(3)
try:
    socket.create_connection((ip0, 80), 2).close(); raise SystemExit("still reachable?!")
except OSError:
    print("3 unreachable, as intended; waiting for it to come back", flush=True)
t = time.time(); s = n540.wait_back(d, 170)
assert get("/api/config")["net"]["mode"] == "dhcp" and not get("/api/state")["net_pending"]
print(f"4 came back on {get('/api/state')['ip']} after {time.time()-t:.0f}s, uptime {s['uptime_ms']//1000}s (no reboot)", flush=True)

post(f"net.mode=static\nnet.ip={ip0}\nnet.mask={mask}\nnet.gateway={gw}"); time.sleep(2)
d.auth("POST", "/api/keep"); assert not get("/api/state")["net_pending"]
d.auth("POST", "/reboot"); n540.wait_back(d); assert get("/api/config")["net"]["mode"] == "static"
print("5 kept static survives reboot", flush=True)
post("net.mode=dhcp\nnet.ip=\nnet.mask=\nnet.gateway="); time.sleep(10); d.resolve()
d.auth("POST", "/api/keep"); assert get("/api/config")["net"]["mode"] == "dhcp"
print("6 back to DHCP and saved,", get("/api/state")["ip"])

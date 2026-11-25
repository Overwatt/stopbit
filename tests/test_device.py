#!/usr/bin/env python3
"""On-device tests over the network, using on-chip UART loopback.

  tests/test_device.py HOST

Loopback also drives the TX pins, so run it with nothing on the RS485 ports.
Restores the original port settings when it finishes.
"""
import hashlib, json, os, random, socket, sys, threading, time
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import n540

d = n540.Device(sys.argv[1])
H = d.host
results = []


def cfg():
    return json.loads(d.get("/api/config"))


def state(i):
    return json.loads(d.get("/api/state"))["ports"][i]


def setcfg(**kv):
    d.auth("POST", "/api/config", "\n".join(f"{k.replace('__', '.')}={v}" for k, v in kv.items()).encode())
    time.sleep(0.25)


def reject(body):
    try:
        st, data = d.auth("POST", "/api/config", body.encode()), b""
    except SystemExit as e:
        return str(e)
    raise AssertionError(f"accepted: {body!r}")


def recvn(s, n, t=10):
    s.settimeout(t)
    b = b""
    try:
        while len(b) < n:
            k = s.recv(65536)
            if not k:
                break
            b += k
    except socket.timeout:
        pass
    return b


def conn(i):
    s = socket.create_connection((H, 4001 + i), timeout=5)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return s


def test(f):
    t = time.time()
    try:
        r = f()
        ok = True
    except Exception as e:
        r, ok = f"{type(e).__name__}: {e}", False
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {f.__name__:28} {time.time()-t:5.1f}s  {r or ''}", flush=True)


def base(i, **kw):
    v = dict(enabled=1, mode="server", baud=115200, data_bits=8, parity="none", stop_bits=1, gap_ms=0,
             packet_len=1024, local_port=4001 + i, remote_host="", remote_port=0, echo_cancel=0, loopback=1)
    v.update(kw)
    setcfg(**{f"port{i+1}__{k}": x for k, x in v.items()})


def line_settings():
    n = 0
    for i in range(4):
        for baud, bits, par, stop in [(9600, 8, "none", 1), (19200, 8, "even", 1), (115200, 8, "odd", 2),
                                      (1200, 7, "even", 1), (300, 8, "none", 1), (921600, 8, "none", 1),
                                      (2000000, 8, "none", 1), (57600, 7, "none", 2)]:
            base(i, baud=baud, data_bits=bits, parity=par, stop_bits=stop)
            size = max(8, min(2000, baud // 20))
            msg = bytes(random.randrange(128 if bits == 7 else 256) for _ in range(size))
            s = conn(i)
            t = time.time()
            s.sendall(msg)
            got = recvn(s, size, 6)
            s.close()
            assert got == msg, f"port {i+1} {baud} {bits}{par}{stop}: {len(got)}/{size} bytes, equal prefix " \
                               f"{next((k for k,(a,b) in enumerate(zip(got,msg)) if a!=b), min(len(got),size))}"
            st = state(i)
            assert not (st["parity"] or st["framing"] or st["noise"] or st["overrun"] or st["rx_lost"]), st
            n += 1
        base(i, enabled=0)
    return f"{n} combinations, no line errors"


def baud_is_accurate():
    base(0, baud=9600)
    s = conn(0)
    msg = os.urandom(960)
    t = time.time()
    s.sendall(msg)
    assert recvn(s, 960, 5) == msg
    dt = time.time() - t
    s.close()
    assert 0.98 < dt < 1.15, f"{dt:.3f}s for 1.000s of data"
    return f"960 bytes at 9600 took {dt:.3f}s"


def all_ports_at_once():
    for i in range(4):
        base(i, baud=921600)
    out = {}

    def run(i):
        msg = os.urandom(150000)
        s = conn(i)
        th = threading.Thread(target=s.sendall, args=(msg,))
        th.start()
        got = recvn(s, len(msg), 10)
        th.join()
        s.close()
        out[i] = (got == msg, len(got))
    ts = [threading.Thread(target=run, args=(i,)) for i in range(4)]
    t = time.time()
    [x.start() for x in ts]
    [x.join() for x in ts]
    dt = time.time() - t
    sts = [state(i) for i in range(4)]
    assert all(v[0] for v in out.values()), (out, [(s["rx_lost"], s["net_lost"], s["overrun"]) for s in sts])
    return f"600 kB, {600000/dt/1000:.0f} kB/s total (line limit 369)"


def backpressure():
    base(0, baud=9600)
    s = conn(0)
    n0 = state(0)["net_rx"]
    s.setblocking(False)
    sent = 0
    end = time.time() + 2
    while time.time() < end:
        try:
            sent += s.send(b"x" * 4096)
        except BlockingIOError:
            time.sleep(0.05)
    q = json.loads(d.get("/api/state"))       # web must stay responsive meanwhile
    s.close()
    time.sleep(0.5)
    took = q["ports"][0]["net_rx"] - n0
    assert took < 12000, took
    base(0, enabled=0)
    return f"device took {took} of {sent} bytes offered; web still answered"


def four_clients_fifth_refused():
    base(0)
    ss = [conn(0) for _ in range(4)]
    time.sleep(0.2)
    assert len(state(0)["peers"]) == 4
    r = state(0)["refused"]
    s5 = socket.create_connection((H, 4001), timeout=5)
    s5.settimeout(3)
    try:
        assert s5.recv(1) == b""
    except ConnectionResetError:
        pass
    assert state(0)["refused"] == r + 1
    ss[0].sendall(b"hello all")
    got = [recvn(s, 9, 3) for s in ss]
    assert got == [b"hello all"] * 4, got
    [s.close() for s in ss]
    time.sleep(1.6)
    assert state(0)["peers"] == [], state(0)["peers"]
    return "broadcast to 4, slots freed on close"


def half_close_gets_reply():
    base(0, baud=9600)
    s = conn(0)
    s.sendall(b"ping")
    s.shutdown(socket.SHUT_WR)
    assert recvn(s, 4, 4) == b"ping"
    assert s.recv(1) == b""
    s.close()


def packet_gap():
    base(0, gap_ms=100)
    s = conn(0)
    chunks = []
    s.settimeout(1)
    for _ in range(5):
        s.sendall(b"ab")
        time.sleep(0.03)
    try:
        while True:
            chunks.append(s.recv(100))
    except socket.timeout:
        pass
    assert chunks == [b"ab" * 5], chunks
    base(0, gap_ms=1000, packet_len=4)
    s = conn(0)
    s.sendall(b"0123456789")
    t = time.time()
    first = recvn(s, 8, 3)
    dt = time.time() - t
    assert first == b"01234567" and dt < 0.6, (first, dt)
    assert recvn(s, 2, 3) == b"89" and time.time() - t > 0.9
    s.close()


def tcp_client_mode():
    l = socket.socket()
    l.bind(("", 0))
    l.listen(1)
    l.settimeout(8)
    me = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    me.connect((H, 80))
    ip = me.getsockname()[0]
    base(1, mode="client", remote_host=ip, remote_port=l.getsockname()[1])
    c, _ = l.accept()
    c.sendall(b"from server")
    assert recvn(c, 11, 3) == b"from server"
    c.close()
    l.settimeout(12)
    c, _ = l.accept()                          # must reconnect by itself
    c.sendall(b"again")
    assert recvn(c, 5, 3) == b"again"
    c.close()
    l.close()
    time.sleep(2.5)
    st = state(1)["state"]
    base(1, enabled=0)
    assert st in ("retrying", "connecting"), st
    return "connects, reconnects, retries"


def udp_mode():
    base(2, mode="udp", local_port=4003)
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.settimeout(3)
    for n in (1, 50, 1000):
        m = os.urandom(n)
        u.sendto(m, (H, 4003))
        got = u.recv(2000)
        assert got == m, (n, len(got))
    base(2, enabled=0)


def peer_that_never_reads():
    base(0, baud=921600)
    lazy, good = conn(0), conn(0)
    lazy.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2048)
    msg = os.urandom(300000)
    th = threading.Thread(target=good.sendall, args=(msg,))
    th.start()
    got = recvn(good, len(msg), 8)
    th.join()
    lazy.close()
    good.close()
    assert got == msg, f"{len(got)}/{len(msg)}"
    return "other client unaffected"


def connection_churn():
    base(0)
    for k in range(60):
        s = conn(0)
        s.sendall(b"k%02d" % k)
        assert recvn(s, 3, 3) == b"k%02d" % k, k
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b"\1\0\0\0\0\0\0\0")   # RST
        s.close()
        time.sleep(0.02)
    for _ in range(10):                        # restart the listener under load
        base(0, baud=random.choice([9600, 115200]))
        s = conn(0)
        s.sendall(b"z")
        assert recvn(s, 1, 3) == b"z"
        s.close()


def packet_boundaries_hold():
    # Regression: a timestamp race used to split packets at random.
    base(0, baud=115200, gap_ms=20)
    s = conn(0)
    for k in range(60):
        m = os.urandom(random.randrange(2, 900))
        s.sendall(m)
        s.settimeout(3)
        got = s.recv(2000)
        assert got == m, (k, len(got), len(m))
    s.close()
    return "60 packets, none split"


def config_rejects_bad_input():
    before = cfg()
    for b in ("port1.baud=5", "nonsense", "port1.enabled=1\nport1.local_port=80", "port9.baud=9600", "net.mode=static",
              "port1.name=" + "x" * 24, "port2.local_port=4001\nport2.enabled=1\nport1.enabled=1"):
        assert "400" in reject(b), b
    assert "413" in reject("x" * 2048)
    assert cfg() == before
    import http.client
    for hdrs in ({}, {"X-Auth": "0" * 64, "X-Body-SHA256": "0" * 64}):
        st, _ = d._raw("POST", "/api/config", b"port1.baud=9600", hdrs)
        assert st == 401, st


def rename_keeps_connections():
    base(0)
    s = conn(0)
    setcfg(port1__name="Boiler")
    s.sendall(b"still here")
    assert recvn(s, 10, 3) == b"still here"
    s.close()


def settings_survive_reboot():
    base(0, baud=38400, loopback=1)
    setcfg(device__name="Persist me")
    before = cfg()
    d.auth("POST", "/reboot")
    n540.wait_back(d)
    time.sleep(1)
    assert cfg() == before
    s = conn(0)
    s.sendall(b"after reboot")
    assert recvn(s, 12, 3) == b"after reboot"
    s.close()


orig = cfg()
try:
    for f in (config_rejects_bad_input, line_settings, baud_is_accurate, all_ports_at_once, backpressure,
              four_clients_fifth_refused, half_close_gets_reply, packet_gap, packet_boundaries_hold, tcp_client_mode, udp_mode,
              peer_that_never_reads, connection_churn, rename_keeps_connections, settings_survive_reboot):
        test(f)
finally:
    lines = ["device.name=" + orig["device"]["name"]]
    for i, p in enumerate(orig["ports"]):
        lines += [f"port{i+1}.{k}={int(v) if isinstance(v, bool) else v}" for k, v in p.items()]
    d.auth("POST", "/api/config", "\n".join(lines).encode())
    assert cfg() == orig, "could not restore settings"
s = json.loads(d.get("/status"))
print(f"{sum(results)}/{len(results)} passed; settings restored; crashes {s['crashes']} rx_err {s['rx_err']} "
      f"tx_drop {s['tx_drop']} up {s['uptime_ms']//1000}s")
sys.exit(0 if all(results) else 1)

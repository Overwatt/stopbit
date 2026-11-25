#!/usr/bin/env python3
"""MQTT / Home Assistant tests against a throwaway Mosquitto.

  uv run --with paho-mqtt tests/test_mqtt.py HOST SSH_HOST BROKER_IP MOSQ_DIR [discovery.json]

MOSQ_DIR on SSH_HOST holds Mosquitto unpacked with `dpkg -x ... root`.

Uses on-chip loopback, so run it with nothing on the RS485 ports. Restores the
original settings. Writes every discovery message to discovery.json for
tests/check_ha.py.
"""
import json, os, secrets, socket, subprocess, sys, threading, time
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import n540
import paho.mqtt.client as paho

PORT, USER, PW = 18831, "n540", secrets.token_urlsafe(20)
TPW = secrets.token_urlsafe(12)
d = n540.Device(sys.argv[1])
results, msgs, log = [], {}, []


class RemoteMosquitto:
    """Real Mosquitto on another machine, run from an unpacked directory over ssh."""
    def __init__(self, host, path):
        self.host, self.path = host, path

    def sh(self, script, inp=None):
        subprocess.run(["ssh", self.host, f"cd {self.path} && export LD_LIBRARY_PATH=$PWD/$(ls -d root/usr/lib/*linux-gnu | head -1) && {script}"],
                       input=inp, text=True, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def setup(self):
        conf = f"listener {PORT}\nallow_anonymous false\npassword_file {self.path}/pw\npersistence true\npersistence_location {self.path}/data/\nmax_packet_size 100000\n"
        self.sh("mkdir -p data && rm -f data/* pw && cat > conf && touch pw && chmod 600 pw", conf)
        for u, p in ((USER, PW), ("tester", TPW)):
            self.sh(f"root/usr/bin/mosquitto_passwd -b pw {u} {p}")

    def start(self):
        self.sh("(setsid root/usr/sbin/mosquitto -c conf > log 2>&1 < /dev/null & echo $! > pid); sleep 1; kill -0 $(cat pid)")

    def stop(self):
        self.sh("kill $(cat pid); sleep 1")

    def cleanup(self):
        self.sh("kill $(cat pid) 2>/dev/null; rm -rf data pw conf pid; true")


def client():
    c = paho.Client(paho.CallbackAPIVersion.VERSION2)
    c.username_pw_set("tester", TPW)
    c.on_message = lambda c, u, m: (msgs.__setitem__(m.topic, m.payload), log.append((time.time(), m.topic, m.payload)))
    c.on_connect = lambda c, *a: c.subscribe("#")
    c.connect(ME, PORT)
    c.loop_start()
    return c


def until(f, t=8, what=""):
    end = time.time() + t
    while time.time() < end:
        try:
            v = f()
        except (KeyError, ValueError, OSError):
            v = None
        if v:
            return v
        time.sleep(0.1)
    raise AssertionError("timed out: " + what)


cfg = lambda: json.loads(d.get("/api/config"))
st = lambda: json.loads(d.get("/api/state"))
pj = lambda i: json.loads(msgs[f"{ID}/port{i}"])
dj = lambda: json.loads(msgs[f"{ID}/device"])
disc = lambda: {t: p for t, p in msgs.items() if t.startswith("homeassistant/") and p}


def setcfg(*lines):
    d.auth("POST", "/api/config", "\n".join(lines).encode())


def cmd(path, v, **k):
    c.publish(f"{ID}/{path}", v, **k).wait_for_publish(5)


def test(f):
    t = time.time()
    try:
        r, ok = f(), True
    except Exception as e:
        r, ok = f"{type(e).__name__}: {e}", False
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {f.__name__:30} {time.time()-t:5.1f}s  {r or ''}", flush=True)


def wrong_password_is_reported():
    n540.set_mqtt_password(d, "wrong", f"mqtt.enabled=1\nmqtt.host={ME}\nmqtt.port={PORT}\nmqtt.user={USER}\nmqtt.control=0")
    m = until(lambda: st()["mqtt"]["error"] and st()["mqtt"], 12, "error shown")
    assert m["state"] in ("retrying", "connecting"), m
    return m["error"]


def connects_and_announces():
    n540.set_mqtt_password(d, PW)
    until(lambda: st()["mqtt"] == {"state": "connected", "error": ""}, 15, f"connected: {st()['mqtt']}")
    until(lambda: msgs.get(f"{ID}/status") == b"online", 15, "online")
    kinds = {t.split("/")[1] for t in disc()}
    assert kinds == {"sensor"}, kinds            # no controls unless allowed
    for i in range(1, 5):
        assert pj(i)["state"] == "off" and pj(i)["baud"] == 9600, pj(i)
    assert dj()["ip"] == d.host
    return f"{len(disc())} sensors, no controls"


def commands_ignored_when_control_off():
    cmd("port1/set/baud", "19200")
    cmd("device/set/restart", "PRESS")
    time.sleep(1.5)
    assert cfg()["ports"][0]["baud"] == 9600 and st()["uptime_s"] > up0


def control_adds_entities():
    setcfg("mqtt.control=1")
    until(lambda: len(disc()) == 80, 10, f"{len(disc())} entities")
    if len(sys.argv) > 5:
        json.dump({t: json.loads(p) for t, p in disc().items()}, open(sys.argv[5], "w"))
    ids = [json.loads(p)["unique_id"] for p in disc().values()]
    assert len(set(ids)) == 80
    return "80 entities, unique ids"


def every_control_works():
    time.sleep(0.5)
    cases = [("baud", "57600", 57600), ("mode", "udp", "udp"), ("mode", "server", "server"), ("parity", "even", "even"),
             ("data_bits", "7", 7), ("stop_bits", "2", 2), ("data_bits", "8", 8), ("parity", "no parity", "none"), ("local_port", "4100", 4100),
             ("remote_host", "plc.lan", "plc.lan"), ("remote_port", "502", 502), ("gap_ms", "25", 25),
             ("packet_len", "256", 256), ("echo_cancel", "ON", True), ("echo_cancel", "OFF", False),
             ("loopback", "ON", True), ("name", 'A "b" \\ c', 'A "b" \\ c'), ("enabled", "ON", True), ("baud", "9600.0", 9600)]
    for p in range(1, 5):
        for k, v, want in cases:
            if k == "local_port":
                v, want = str(4100 + p), 4100 + p
            cmd(f"port{p}/set/{k}", v)
            until(lambda: pj(p)[k] == want, 4, f"port{p}.{k}={v}: {pj(p)[k]!r}")
            assert cfg()["ports"][p - 1][k] == want
    until(lambda: pj(1)["state"] == "listening", 3, "listening")
    cmd("device/set/name", "Named from HA")
    until(lambda: dj()["name"] == "Named from HA", 4, "device name")
    return f"{4*len(cases)} changes"


def traffic_sensors():
    s = socket.create_connection((d.host, 4101), 5)
    s.sendall(b"x" * 500)
    until(lambda: pj(1)["clients"] == 1, 3, "clients")
    until(lambda: pj(1)["serial_rx"] >= 500 and pj(1)["serial_tx"] >= 500, 14, "counters")
    s.close()
    until(lambda: pj(1)["clients"] == 0, 5, "client gone")


def bad_values_rejected_and_shown():
    before = cfg()
    for k, v in (("baud", "5"), ("baud", "abc"), ("baud", ""), ("mode", "Server"), ("local_port", "80"), ("parity", "9"),
                 ("remote_host", "a b"), ("name", "x" * 24), ("enabled", "maybe")):
        msgs.pop(f"{ID}/device", None)
        cmd(f"port1/set/{k}", v)
        time.sleep(0.4)
    until(lambda: dj()["last_error"] != "Nothing", 4, "error shown")
    for t, v in (("port1/set/nope", "1"), ("port9/set/baud", "9600"), ("port1/set/baud", "x" * 400), ("net/set/mode", "static"),
                 ("mqtt/set/host", "evil"), ("device/set/web_key", "00"), ("port1/set/baud", "9600\nmqtt.host=evil"),
                 ("port1/set/name", "a\r\nport1.baud=300"), ("port1/set/name", "nul\0"), ("port1/set/name.x", "q")):
        cmd(t, v)
        time.sleep(0.15)
    time.sleep(0.5)
    assert cfg() == before
    return dj()["last_error"]


def retained_commands_ignored():
    cmd("port1/set/baud", "1200", retain=True)      # live delivery: may apply once
    cmd("device/set/restart", "PRESS_NOT", retain=True)
    time.sleep(1)
    setcfg("port1.baud=9600")
    u = st()["uptime_s"]
    setcfg("mqtt.control=0")
    time.sleep(1)
    cmd("device/set/restart", "PRESS", retain=True)  # stored while nobody listens
    setcfg("mqtt.control=1")                         # resubscribes; broker replays both
    until(lambda: len(disc()) == 80, 10, "entities back")
    time.sleep(3)
    assert st()["uptime_s"] > u, "a retained restart was obeyed"
    assert cfg()["ports"][0]["baud"] == 9600, "a retained setting was obeyed"
    cmd("port1/set/baud", "", retain=True)
    cmd("device/set/restart", "", retain=True)


def changes_survive_restart_button():
    cmd("port2/set/baud", "4800")
    until(lambda: pj(2)["baud"] == 4800, 4)
    u = st()["uptime_s"]
    cmd("device/set/restart", "PRESS")               # inside the 3 s save delay
    until(lambda: msgs[f"{ID}/status"] == b"offline", 3, "goodbye")
    until(lambda: st()["uptime_s"] < u, 30, "rebooted")
    until(lambda: msgs[f"{ID}/status"] == b"online", 30, "back online")
    assert cfg()["ports"][1]["baud"] == 4800
    return "said goodbye at once, came back, setting kept"


def broker_restart():
    global c
    B.stop()
    time.sleep(3)
    assert st()["mqtt"]["state"] in ("retrying", "connecting"), st()["mqtt"]
    s = socket.create_connection((d.host, 4101), 5)  # bridge unaffected meanwhile
    s.sendall(b"alive")
    s.settimeout(3)
    assert s.recv(10) == b"alive"
    s.close()
    c.loop_stop()
    msgs.clear()
    B.start()
    c = client()
    until(lambda: st()["mqtt"]["state"] == "connected", 70, "reconnect")
    until(lambda: len(disc()) == 80 and msgs.get(f"{ID}/status") == b"online", 15, "re-announced")


def command_flood():
    for k in range(300):
        c.publish(f"{ID}/port3/set/gap_ms", str(k % 100))
    time.sleep(3)
    cmd("port3/set/gap_ms", "42")
    until(lambda: cfg()["ports"][2]["gap_ms"] == 42, 5, "still responsive")
    assert st()["mqtt"]["state"] == "connected"


def bridge_not_disturbed():
    """Serial data integrity while MQTT is announcing and being changed."""
    s = socket.create_connection((d.host, 4101), 5)
    setcfg("port1.baud=921600")
    time.sleep(0.5)
    s.close()
    s = socket.create_connection((d.host, 4101), 5)
    msg = os.urandom(200000)
    got = bytearray()
    th = threading.Thread(target=s.sendall, args=(msg,))
    th.start()
    for k in range(40):
        c.publish(f"{ID}/port2/set/gap_ms", str(k))
        if k % 10 == 0:
            setcfg(f"device.name=churn {k}")
    s.settimeout(5)
    while len(got) < len(msg):
        b = s.recv(65536)
        if not b:
            break
        got += b
    th.join()
    s.close()
    assert bytes(got) == msg, (len(got), len(msg))
    return "200 kB intact"


def broker_names():
    name = socket.gethostname().split(".")[0] + ".local"      # this machine: no broker here
    setcfg(f"mqtt.host={name}", f"mqtt.port={PORT + 1}")
    e = until(lambda: st()["mqtt"]["error"], 30, f"{name}: {st()['mqtt']}")
    assert "not found" not in e, (name, e)                    # it resolved, then was refused
    setcfg("mqtt.host=no-such-host.invalid")
    e2 = until(lambda: st()["mqtt"]["error"], 30, "dns error")
    setcfg(f"mqtt.host={ME}", f"mqtt.port={PORT}")
    until(lambda: st()["mqtt"]["state"] == "connected", 20, "reconnected")
    return f"{name}: {e} | bad name: {e2}"


def power_cut_takeover():
    """A reset with no goodbye: the broker still holds the old session."""
    u = st()["uptime_s"]
    d.auth("POST", "/api/config", b"device.name=before cut")
    # /reboot signs off; a TCP-level vanish is approximated by dropping the broker link
    B.sh("kill -STOP $(cat pid)")
    time.sleep(2)
    d.auth("POST", "/reboot")
    time.sleep(8)
    B.sh("kill -CONT $(cat pid)")
    until(lambda: st()["uptime_s"] < u, 30, "rebooted")
    until(lambda: st()["mqtt"]["state"] == "connected", 60, f"reconnect: {st()['mqtt']}")
    time.sleep(2)
    assert msgs[f"{ID}/status"] == b"online", msgs[f"{ID}/status"]


def disabling_removes_entities():
    setcfg("mqtt.enabled=0")
    until(lambda: not disc(), 15, f"{len(disc())} left")
    until(lambda: msgs[f"{ID}/status"] == b"offline", 5, "offline")
    until(lambda: st()["mqtt"]["state"] == "off", 5, "off")


def password_never_exposed():
    for p in ("/api/config", "/api/state", "/status", "/api/login"):
        assert PW.encode() not in d.get(p), p
    assert not any(PW.encode() in p for _, _, p in log)


orig = cfg()
assert not orig["mqtt"]["enabled"], "MQTT is already in use on this device; not touching it"
up0 = st()["uptime_s"] - 1
ID = st()["host"]
ME = sys.argv[3]
B = RemoteMosquitto(sys.argv[2], sys.argv[4])
B.setup()
B.start()
c = client()
until(c.is_connected, 10, "test client could not log in to the test broker")
try:
    for f in (wrong_password_is_reported, connects_and_announces, commands_ignored_when_control_off, control_adds_entities,
              every_control_works, traffic_sensors, bad_values_rejected_and_shown, retained_commands_ignored,
              changes_survive_restart_button, broker_restart, command_flood, bridge_not_disturbed, broker_names, power_cut_takeover,
              password_never_exposed, disabling_removes_entities):
        test(f)
finally:
    L = ["device.name=" + orig["device"]["name"], "mqtt.enabled=0", "mqtt.host=", "mqtt.port=1883", "mqtt.user=", "mqtt.control=0"]
    for i, p in enumerate(orig["ports"]):
        L += [f"port{i+1}.{k}={int(v) if isinstance(v, bool) else v}" for k, v in p.items()]
    n540.set_mqtt_password(d, "", "\n".join(L))
    time.sleep(4)
    print("settings restored:", cfg() == orig)
    B.cleanup()
s = json.loads(d.get("/status"))
print(f"{sum(results)}/{len(results)} passed; crashes {s['crashes']} rx_err {s['rx_err']} tx_drop {s['tx_drop']}")
sys.exit(0 if all(results) else 1)

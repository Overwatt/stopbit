#!/usr/bin/env python3
"""GET /metrics over real sockets (the simulator): HTTP/1.1 in chunks, HTTP/1.0 ended by
the close, and HTTPS with the connection kept for the next request.
   tests/test_metrics_http.py HOST:PORT [HTTPS_PORT]"""
import http.client, json, re, socket, ssl, sys

host, port = sys.argv[1].split(":")
port = int(port)
tls_port = int(sys.argv[2]) if len(sys.argv) > 2 else 0
LINE = re.compile(r'^(# (HELP|TYPE) [a-zA-Z_:][a-zA-Z0-9_:]* .+|[a-zA-Z_:][a-zA-Z0-9_:]*(\{([a-zA-Z_][a-zA-Z0-9_]*="([^"\\]|\\.)*",?)*\})? (-?[0-9.]+|\+Inf))$')

def check(body, how):
    text = body.decode()
    assert text.endswith("\n"), how
    lines = text.splitlines()
    bad = [l for l in lines if not LINE.match(l)]
    assert not bad, (how, bad[:3])
    types = [l.split()[2] for l in lines if l.startswith("# TYPE ")]
    assert len(types) == len(set(types)), how                      # each family once
    assert 'n540_build_info{version="sim"} 1' in lines, how
    assert 'n540_port_info{port="2",name="Meter \\"A\\"",mode="modbus"} 1' in lines, how
    assert any(l.startswith('n540_modbus_reply_seconds_bucket{port="3",le="+Inf"}') for l in lines), how
    print("ok  %s: %d bytes, %d families" % (how, len(body), len(types)))
    return text

# HTTP/1.1: chunked, as http.client reads it, and as it comes on the wire.
c = http.client.HTTPConnection(host, port, timeout=10)
c.request("GET", "/metrics")
r = c.getresponse()
assert r.status == 200 and r.getheader("Transfer-Encoding") == "chunked" and r.getheader("Content-Type").startswith("text/plain; version=0.0.4")
a = check(r.read(), "HTTP/1.1, chunked")
c.close()

c = http.client.HTTPConnection(host, port, timeout=10)
for path in ("/api/devices", "/api/settings"):
    c.request("GET", path); r = c.getresponse(); j = json.loads(r.read())
    assert r.status == 200 and r.getheader("Transfer-Encoding") == "chunked", path
print("ok  /api/devices and /api/settings in parts")
c.close()

def raw(req, sock):
    sock.sendall(req)
    data = b""
    while True:
        d = sock.recv(65536)
        if not d: return data
        data += d

s = socket.create_connection((host, port), timeout=10)
w = raw(b"GET /metrics HTTP/1.1\r\nHost: x\r\n\r\n", s)
assert w.endswith(b"\r\n0\r\n\r\n"), w[-40:]                          # the last chunk
chunks = w.split(b"\r\n\r\n", 1)[1].count(b"\r\n") // 2
print("ok  on the wire: ends with the last chunk, %d chunks" % chunks)

s = socket.create_connection((host, port), timeout=10)
w = raw(b"GET /metrics HTTP/1.0\r\n\r\n", s)
head, body = w.split(b"\r\n\r\n", 1)
assert b"chunked" not in head and b"Connection: close" in head
assert check(body, "HTTP/1.0, ended by the close") == a

if tls_port:
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    try:
        c = http.client.HTTPSConnection(host, tls_port, timeout=20, context=ctx)
        c.request("GET", "/metrics")
        r = c.getresponse()
        assert r.status == 200 and r.getheader("Connection") == "keep-alive"
        assert check(r.read(), "HTTPS, chunked") == a
        c.request("GET", "/status")                                   # the same connection, next request
        r = c.getresponse()
        assert r.status == 200 and b'"version"' in r.read()
        print("ok  HTTPS: the connection served the next request")
    except ConnectionRefusedError:
        print("--  HTTPS is not on: skipped")
print("ok")

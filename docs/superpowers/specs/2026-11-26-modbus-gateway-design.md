# Modbus TCP gateway

Approved 2026-11-26. A device will be wired to port 2, 3 or 4 later; until then it is
built general-purpose and tested on the computer.

## Goal

A port can be a Modbus TCP to Modbus RTU gateway: Modbus TCP clients on the network,
Modbus RTU devices on the RS485 line, several clients sharing the line safely.

## Design

**Mode.** "Modbus TCP gateway" (`modbus`) is a fourth port mode beside TCP server, TCP
client and UDP. It listens on a TCP port (502 by default) and takes up to 4 clients, as a
TCP server does. It is stored as mode 3 in the port's record; older firmware treats an
unknown mode as UDP, and Modbus clients use TCP, so after a rollback nothing reaches the
bus. A listen-only port cannot be a Modbus gateway (it must transmit).

**Translation.** A request arrives as an MBAP header (transaction, protocol 0, length,
unit) and a PDU (function, data); requests may be split across TCP segments or several
may arrive in one. It goes on the line as an RTU frame: unit, PDU, CRC-16 (Modbus,
low byte first). The reply's CRC, unit and function (or function | 0x80, an exception)
are checked; it goes back to the client that asked, with its transaction ID.

**One at a time.** One request is on the line at a time, first come first served, up to
8 waiting per port across its clients (a client may send several before the first is
answered). Before a frame goes out the line must have been quiet for 3.5 characters
(at least 1.75 ms above 19200 baud); a reply ends at the same silence.

**Failures** are answered with Modbus exceptions, as gateways do:

| | Answer |
|---|---|
| No valid reply within the timeout (1000 ms, settable per port, 50 to 10000) | 0x0B, gateway target device failed to respond |
| 8 already waiting | 0x06, server device busy |
| A function the client may not use (see access) | 0x01, illegal function |
| A reply with a bad CRC, or for another unit or function | ignored; the wait goes on |
| Malformed request (protocol not 0, length out of range) | the connection is closed |

A broadcast (unit 0) goes out with no reply expected; the next request waits 100 ms.
A client that disconnects loses its waiting requests; one already on the line finishes
and its reply is dropped.

**Access.** The port's access list applies as now: addresses not on it are refused.
Those that may only read may use the read functions (1, 2, 3, 4, and 43 for device
identification); anything else gets 0x01. Those that may read and write may use any.

**Figures**, per port: requests, replies, exceptions passed on from devices, timeouts,
bad CRCs, busy answers, refused functions; response time last, average and longest. In
`/api/state` (`ports[].modbus`) and on the port page. Item 4 (traffic and latency
figures, Prometheus) builds on them.

**Setting.** `portN.mode=modbus`; `portN.modbus_timeout` in ms, kept in the extra
settings record (`more_cfg_t`, from `spare`). The web form offers the mode, 502 when it
is chosen, and the timeout.

## Code

- `src/app/modbus.c`: framing, CRC, queue, timing and figures, with no lwIP: it takes
  bytes from clients and the line and gives back bytes for each, with the time passed
  in. Tested on the computer with a fake line and clock.
- `bridge.c` keeps connections and access lists; a Modbus port's client data goes to
  `modbus.c` instead of the ring, and its replies go to the client that asked.

## Tests

- **Host:** CRC against published values; a request split byte by byte and several in one
  segment; replies to the right client with the right transaction ID; several clients
  in turn; timeout, bad CRC, wrong unit, exception from the device, busy, broadcast;
  read-only clients; disconnect while waiting or on the line; settings.
- **Simulator:** a Modbus TCP client in Python against the simulator, with a simulated
  RTU device on the port.
- **Gateway:** once a device is wired; TigoBus on port 1 untouched.

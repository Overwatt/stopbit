# Modbus ports without TCP

Approved 2026-11-26.

## Goal

A Modbus port whose line serves only its devices: the gateway polls them and publishes
them to MQTT, and nothing listens on the network for that port.

## Background

Phase 1 of device profiles first did this with local port 0, and 5bee875 removed it.
Older firmware refuses an enabled port (other than a TCP client) whose local port is 0,
and then loads defaults for the whole settings record, network settings included. After a
downgrade the gateway would be at its default address. So today every Modbus port
listens, and an access list is the only way to keep clients out.

## Design

**Setting.** `portN.modbus_tcp`:
- 1 (the default): the port listens on its local port for Modbus TCP clients, as today.
- 0: no listener; only the port's devices use the line.

It applies in Modbus mode and is ignored in the other modes, as `modbus_broadcast` is.

**Storage.** The setting is one bit per port in a spare byte of the extra side record
(`more_cfg_t`), beside `modbus_broadcast`.
- The bit is set when TCP is off, so a blank byte means "listen". Blank is what every
  older record holds.
- The side record keeps its size (256 bytes) and its schema.
- The main record is unchanged. An enabled Modbus port still needs a local port, which
  may not be 80 and must differ from the other TCP ports' local ports. The local port is
  kept while TCP is off, so older firmware still accepts the record.

Firmware versions:
- On a downgrade, older firmware loads the settings, listens on the port again and has no
  devices. Nothing is reset.
- Older firmware (cf3fd14) loads and saves the whole side record, spare bytes included.
  So the bit survives a downgrade and the upgrade back.
- A settings file holds a `portN.modbus_tcp` line. Older firmware refuses that line, as it
  already refuses `devN.*` lines.

**Behaviour.**
- With TCP off, the port's state is "ready" (LS_BOUND), not "listening", and it counts as
  up.
- No "no client connected" alert, as for UDP. The "nothing heard" alert still applies.
- A change opens or closes the listener only. The serial line, the Modbus queue and the
  device polls continue, so the change cannot go through the port_cfg_t comparison that
  restarts a port.
- Turning TCP off closes the connected clients and drops their queued requests. The
  devices' requests stay queued.
- Devices on the port poll as they do now.

**Web page.** In Modbus mode, the page gets an "Accept TCP clients" checkbox, on by
default. Its hint: "Off, the port only polls its devices for MQTT. The local port is kept
so that older firmware still accepts these settings." The Access fieldset is hidden while
TCP is off.

**API.** In `/api/config`, each port gains a `modbus_tcp` boolean. No new metrics or
Home Assistant entities.

## Testing

- **test_cfg:**
  - The default is 1, and 0 and 1 parse.
  - Text and JSON round-trip.
  - The bit is independent of `modbus_broadcast`.
  - The local port rules are unchanged with TCP off.
- **test_store:**
  - A side record with the bit set loads.
  - A blank byte means "listen".
  - The bit survives a save, and a power cut during the save.
- **test_bridge:**
  - With TCP off, there is no listener and a connection is refused.
  - Devices still poll.
  - Changing the setting at runtime opens or closes the listener without reopening the
    line.
  - Turning TCP off closes the clients and keeps the devices' requests.
  - There is no "no client" alert.
- **Fuzz:** the cfg round trip includes the setting.
- **Live, port 2 (EG4 BMS, address 5):**
  - With TCP off, a connection to 4002 is refused, and the EG4 keeps publishing with no
    misses.
  - Then TCP is turned back on.
  - Port 1 is checked before and after (serial_tx 0, both TigoBus clients back).

## Out of scope

Other modes. A TCP server or UDP port without its network endpoint has no use.

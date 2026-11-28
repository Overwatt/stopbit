# Extra IP addresses: a port listening on an address of its own

Approved 2026-11-27, section by section. A port can listen on an address of its own, which
the gateway then takes on beside its main one, so that two ports can both be on 502. The
first use is two inverters, on ports 2 and 3, each found by SolarAssistant as a gateway of
its own.

## What SolarAssistant does

Observed on 2026-11-27, from SolarAssistant's device log and a silent listener on 502 on
another machine on the LAN:

- Its "Modbus RTU gateway" connection speaks Modbus TCP, and only to port 502; there is no
  setting for the port, and devices are added only by a scan.
- The scan covers its own network (a /22 there), and tries port 502 on each host it finds.
- At each, it asks units 0 to 15 in turn, about 4.5 s apart, all with the same read
  (function 3, holding registers 2-6), and stops at the first unit that answers. On the
  gateway: unit 0 got nothing (port 2 had `modbus_broadcast=1`, so it went out as a
  broadcast, unanswered), and unit 1, the inverter, was found. Nothing above 1 was asked.
- Each later connection starts with a read of unit 0 again.

So one address shows SolarAssistant one device. Routing a second unit to another port (unit
2 to port 3) was designed first and dropped: the scan would never ask for unit 2.

## Settings

`portN.ip`: an IPv4 address, or blank (the default).

- Blank: the port listens on every address the gateway has, as before.
- An address: the port listens only there. If it is not the main address, the gateway
  takes it on as an extra address. With 4 ports, up to 3 extra addresses.
- An extra address is static, in the main address's subnet, with the main mask and router.
  The main address may be static or from DHCP; with DHCP the subnet is checked once the
  main address is known, and an extra address outside it is not used (an event says so).
- Two ports may share a local port only if each has an address of its own and the two
  differ. A blank port and an addressed one cannot share: blank is every address. 80 and
  443 stay refused for ports, as the web page listens on every address.
- Server, Modbus and UDP modes use it. Client mode refuses an address: it connects out,
  from the main address.
- Checked when settings are saved: the form of the address; not the main address's subnet
  broadcast or network address; in the main subnet when the main address is static; no
  more than 3 extra; the sharing rule above; client mode. Stored in `more_cfg_t`, 4 bytes a
  port (16 of the 146 spare); in the settings text as `portN.ip=` and in the JSON as `"ip"`.

## Behaviour

- Before an extra address is used, the gateway checks the LAN for it with ARP probes
  (lwIP's address conflict detection, ACD: about 5 to 9 s). If another device has it, it is
  not used, and an event names that device's MAC address. If another device claims it
  later, the gateway defends it (an announcement); a second claim within 10 s and it gives
  it up, with an event. An address not in
  use is tried again at a restart, the next settings save, or when the cable is back.
- The web page, and ports left blank, answer on every address.
- What the gateway starts itself keeps the main address: MQTT, NTP, DNS, certificate
  renewal, client-mode ports. The mDNS name stays on the main address.
- Replies on a connection made to an extra address carry that address.
- A port's address changed: its listener is bound again, as when `local_port` changes (its
  clients are closed).
- `/api/state` lists the extra addresses and their state: checking, in use, conflict, or
  outside the subnet. The web page shows them under Network; the port form has an Address
  field (blank: every address).
- Events: an extra address in use; refused, with the other device's MAC; given up, with
  its MAC; outside the main subnet.

## The module: `src/net/ipalias.c`, `ipalias.h`

lwIP gives an interface one IPv4 address. The module gives the main interface up to
`IPALIAS_MAX` more, using only lwIP (netif, etharp, ACD), nothing of the gateway's own, so
that others can use it.

```c
#define IPALIAS_MAX 3
typedef enum { IPALIAS_OFF, IPALIAS_CHECKING, IPALIAS_LIVE, IPALIAS_CONFLICT } ipalias_state_t;
typedef void (*ipalias_cb)(int i, ipalias_state_t st, const struct eth_addr *other);

void ipalias_init(struct netif *main, ipalias_cb cb);
void ipalias_set(const ip4_addr_t *addr, int n);           /* the list; entries unchanged keep their state */
void ipalias_link(int up);                                 /* the main interface's link changed */
struct netif *ipalias_input_netif(const struct pbuf *p);   /* where a received frame goes */
ipalias_state_t ipalias_state(int i, ip4_addr_t *addr);
```

- Each extra address has a `struct netif` of its own, with the main one's MAC address, MTU
  and link output, Ethernet and ARP flags, no IGMP, no DHCP. It is up, and never has its
  link up. So lwIP's routing (`ip4_route`), which passes over an interface without a link,
  sends everything out through the main interface, replies to the extra address included,
  with the extra address as their source (a TCP connection keeps the address it was made
  to). No routing hook is needed. And lwIP's input (`ip4_input`) already takes a packet
  for any interface that is up, so a listener bound to the extra address gets its
  connections.
- An ARP frame whose sender or target is an extra address (configured, whether checked yet
  or not) goes to that address's interface; every other frame goes to the main one. The
  target, so a request for the extra address is answered (by `etharp_input` on that
  interface, with its address); the sender, so another device's probe for it, or claim to
  it, reaches its conflict detection. Frames shorter than an ARP frame, and anything not
  ARP, go to the main interface. ACD's callback gives no MAC address, so the sorting keeps
  the sender's MAC of the last such frame not from the gateway itself, for the report.
- The interface has no address until ACD's probes pass (`acd_start` with the address);
  then the address is set and ACD announces it. ACD goes on watching while it is in use;
  a conflict it declines (`ACD_DECLINE`) clears the address and is reported, and its
  `ACD_RESTART_CLIENT` that follows is not acted on (the address is tried again only as
  under Behaviour). Its own link
  never changes, so the main interface's link is passed on by `ipalias_link`: down stops
  the check, up checks again (the address may have been taken meanwhile).
- `ipalias_set` compares with what it has: an address no longer listed is removed (its
  ACD and interface), a new one added and checked, one kept is left as it is.
- About 400 bytes of RAM for 3 (an interface and its ACD each), 1 to 2 KB of code.
- Licence: Apache-2.0, as the repository. If it is offered to lwIP's contrib add-ons, the
  two files may go under lwIP's BSD-3 instead.

## The gateway's side

- `net.c`: a received frame goes to the interface `ipalias_input_netif` gives; link
  changes go to `ipalias_link` as well; `ipalias_init` after the main interface is added.
  All inside `APP_IMAGE`: `net.c` is built into the loader too, which stays byte-identical
  (checked with `cmp`, as always). `lwipopts.h` does not change.
- `cfg.c`: `portN.ip`, its checks and its text and JSON.
- `bridge.c`: a port's listener (TCP) or socket (UDP) is bound to its address, or to every
  address when blank. The extra addresses are the ports' addresses that are not the main
  one, given to `ipalias_set` when settings are applied, and again when the main address
  changes (a new DHCP lease); an address outside the main subnet then is left out and
  reported.
- `events.h`: a kind for the extra address events; `api.c` and `web/index.html`: the list,
  the Address field.

## Tests

- `tests/test_ipalias.c`: the module against real lwIP on the host (the copy in `build/`),
  frames in and the frames sent caught at the link output. An ARP request for an extra
  address is answered with that address and the gateway's MAC, and one for the main
  address as before; nothing is answered before the check passes; a conflict during the
  check, and one after, are found and reported with the other MAC; a TCP SYN to the extra
  address on a port listened to there is answered from that address, through the main
  interface; the same SYN to the main address is refused when only the extra address
  listens there; a connection made out from an unbound PCB has the main address; link down
  and up checks again; `ipalias_set` keeps unchanged entries as they are. In
  `tests/host.sh`.
- `test_cfg`: every rule under Settings, text and JSON both ways.
- `test_bridge`: a listener bound to its address, bound again when it changes; the list
  given to `ipalias_set`.
- Before pushing: the whole of CI, the simulator tests, MQTT over TLS and the fuzzing
  included; the loader compared byte for byte.

## On the gateway

Only with the user present (SolarAssistant reads ports 2 and 3):

1. Install with no address set: nothing changes. The usual checks: port 1 has sent nothing
   and both its clients are back, SolarAssistant reconnects.
2. The user picks an address outside the router's DHCP range; nothing answers it, checked
   from another machine.
3. `port3.mode=modbus port3.local_port=502 port3.ip=<address>`: SolarAssistant's socat
   link to inverter B ends. The address answers ARP and 502 takes a connection, checked
   from another machine without a Modbus request (nothing sent on port 3's line).
4. SolarAssistant scans and finds inverter B at the new address, unit 1; port 3's counts
   watched meanwhile.
5. The socat unit for port 3 on SolarAssistant's machine is retired (`n540-tty@`).
6. Back at any point: `port3.ip= port3.local_port=4003 port3.mode=server` brings back
   the socat path; the firmware before stays in the other slot.

## Docs

`docs/GATEWAY.md`: a section on addresses; `proxy/README.md`: what SolarAssistant's scan
does, as above; a header in `ipalias.h` for anyone using the module alone. Example
addresses only.

## Not in this design

- Routing units to other ports on one address (dropped: see above).
- A MAC address of its own for each extra address, or extra addresses from DHCP.
- IPv6 (lwIP has several addresses an interface there already).
- The web page on the main address only.

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
  main address is known, and an extra address outside it, or equal to its network or broadcast address, is not used (an event says so).
- Two ports may share a local port only if each has an address of its own and the two
  differ. A blank port and an addressed one cannot share: blank is every address. 80 stays
  refused for ports, as the web page listens on every address.
- Older firmware reads only the main settings record, and checks it alone: two enabled
  ports on one local port would make it refuse the record and load defaults (port 1's
  listen-only and the static address with them). So a port with an address is stored in
  the main record as off, and whether it is on is kept with its address in `more_cfg_t`
  (`ip_enabled`, in the stored record only). After a rollback to firmware without extra
  addresses, such a port is off; this firmware reads it back as it was.
- Server, Modbus and UDP modes use it. Client mode refuses an address: it connects out,
  from the main address.
- Checked when settings are saved: the form of the address; not the main address's subnet
  broadcast or network address; in the main subnet when the main address is static; no
  more than 3 extra; the sharing rule above; client mode. Stored in `more_cfg_t`, 4 bytes a
  port and `ip_enabled` (17 of the 146 spare); in the settings text as `portN.ip=` and in
  the JSON as `"ip"`.

## Behaviour

- Before an extra address is used, the gateway checks the LAN for it with ARP probes
  (lwIP's address conflict detection, ACD: about 6 to 9 s). If another device has it, it is
  not used, and an event names that device's MAC address. If another device claims it
  later, the gateway defends it (an announcement); a second claim within 10 s and it gives
  it up, with an event. An address not in
  use is tried again at a restart, the next settings save, or when the cable is back. One
  in use, when the cable is back, is announced again and kept, as lwIP does for the main
  address: connections to it survive a short unplugging.
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

## The module: `src/ipalias/ipalias.c`, `ipalias.h`

lwIP gives an interface one IPv4 address. The module gives the main interface up to
`IPALIAS_MAX` more, using only lwIP (netif, etharp, ACD), nothing of the gateway's own, so
that others can use it. A directory of its own, built into the application only:
everything in `src/net` is built into the loader too.

```c
#define IPALIAS_MAX 3
typedef enum { IPALIAS_OFF, IPALIAS_CHECKING, IPALIAS_LIVE, IPALIAS_CONFLICT } ipalias_state_t;
typedef void (*ipalias_cb)(int i, ipalias_state_t st, const struct eth_addr *other);

void ipalias_init(struct netif *main, ipalias_cb cb);
void ipalias_set(const ip4_addr_t *addr, int n);           /* the list; entries unchanged keep their state */
void ipalias_link(int up);                                 /* the main interface's link changed */
struct netif *ipalias_input_netif(const struct pbuf *p);   /* an extra address's interface, or NULL: the main one */
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
- An ARP frame whose sender or target is an extra address (configured and not in
  conflict, whether checked yet or not) goes to that address's interface; every other frame goes to the main one. The
  target, so a request for the extra address is answered (by `etharp_input` on that
  interface, with its address); the sender, so another device's probe for it, or claim to
  it, reaches its conflict detection. Frames shorter than an ARP frame, and anything not
  ARP, go to the main interface. ACD's callback gives no MAC address, so the sorting keeps
  the sender's MAC of the last such frame not from the gateway itself, for the report.
- The interface has no address until ACD has probed and announced it (`acd_start` with
  the address; `ACD_IP_OK` comes after the announcements); then the address is set. ACD
  goes on watching while it is in use;
  a conflict it declines (`ACD_DECLINE`) clears the address and is reported, and its
  `ACD_RESTART_CLIENT` that follows is not acted on (the address is tried again only as
  under Behaviour). Its own link
  never changes, so the main interface's link is passed on by `ipalias_link`: down stops
  a check under way; up announces again an address in use (`etharp_gratuitous`) and
  checks again one that is not.
- A UDP PCB bound to an extra address must send with `udp_sendto_if_src` and the main
  interface: `udp_sendto` refuses a source address that the route's interface lacks
  (`ERR_RTE`). TCP has no such check.
- `ipalias_set` compares with what it has: an address no longer listed is removed (its
  ACD and interface), a new one added and checked, one kept is left as it is.
- About 400 bytes of RAM for 3 (an interface and its ACD each), 1 to 2 KB of code.
- Licence: Apache-2.0, as the repository. If it is offered to lwIP's contrib add-ons, the
  two files may go under lwIP's BSD-3 instead.

## The gateway's side

- `net.c`: a received frame goes to the interface `ipalias_input_netif` gives (the main
  one when NULL); link changes go to `ipalias_link` as well; `net_netif()` gives the main
  interface. All inside `APP_IMAGE`: `net.c` is built into the loader too, which stays
  byte-identical (checked with `cmp`, as always). `lwipopts.h` does not change.
- `cfg.c`: `portN.ip`, its checks and its text and JSON. `cfg_store.c`: a port with an
  address stored as off in the main record, its `ip_enabled` bit beside its address.
- `addrs.c` (new): the list. `addrs_want` takes each port's address (from
  `bridge_configure`: enabled ports not in client mode); `addrs_poll` (main loop, after
  `net_poll`) makes the list when that or the main address has changed (a new DHCP
  lease): the addresses that are not the main one, in its subnet, given to `ipalias_set`;
  one outside it left out and reported. It turns the module's callbacks into events, and
  gives `"addresses"` for `/api/state`.
- `bridge.c`: a port's listener (TCP) or socket (UDP) is bound to its address, or to every
  address when blank, and bound again when it changes. UDP from a port with an address
  goes out with `udp_sendto_if_src`.
- `events.h`: a kind for the extra address events; `api.c` and `web/index.html`: the list,
  the Address field.

## Tests

- `tests/ipalias/run.sh`, against real lwIP on the host (its source from `build/_deps`, so
  it runs in CI's firmware job, after the build is configured):
  - `test_ipalias.c`, the module alone, frames in and the frames sent caught at the link
    output. An ARP request for an extra address is answered with that address and the
    gateway's MAC, and one for the main address as before; nothing is answered before the
    check passes; a conflict during the check, and one after, are found and reported with
    the other MAC; a TCP SYN to the extra address on a port listened to there is answered
    from that address, through the main interface; the same SYN to the main address is
    refused when only the extra address listens there; a connection made out from an
    unbound PCB has the main address; UDP from the extra address through the main
    interface; unplugged and back; `ipalias_set` keeps unchanged entries as they are.
  - `test_addrs.c`, `addrs.c` with the module: which addresses become extra ones, the
    events, a change of main address, the JSON.
- `test_cfg`: every rule under Settings, text and JSON both ways. `test_store`: the main
  record as older firmware reads it, and read back whole.
- `test_bridge`: a listener bound to its address, bound again when it changes; UDP from
  its address; the list given to `addrs_want`.
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

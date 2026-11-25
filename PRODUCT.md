# Product

<!-- impeccable:product-schema 1 -->

## Platform

web

## Users

The owner of one gateway today: technical, built the firmware, knows the hardware.
Later, other owners of the same box (a USR-N540) who have flashed this firmware. They
are technical strangers: comfortable with RS485, baud rates and IP addresses, but they
have not read the source.

Four jobs, all confirmed:

- **Check it is healthy:** are ports connected, is data flowing, any errors or restarts.
- **Set up or change a port,** the network, or MQTT. Occasional.
- **Diagnose a bus problem:** traffic, error counts, wiring.
- **Update firmware.** Regular while developing.

## Product Purpose

Open firmware that turns a USR-N540, a 4-port RS485-to-Ethernet box, into a gateway its
owner can inspect, change and update over the network, replacing the closed vendor
firmware. Built for fun; it does not aim to reproduce the vendor's whole feature list.

Success: a real bus's bytes reach the network intact, the owner can see at a glance that
they do, and no mistake made over the network leaves the box needing a debug probe.

## Positioning

The owner holds the whole stack: source, update key and device. An update that fails to
start is undone by the gateway itself. A port can be made listen-only at the transmitter,
so watching a live bus cannot disturb it.

## Operating Context

- Four RS485 ports, each a TCP server, TCP client or UDP endpoint.
- Used mostly on a desktop browser on the LAN; a phone must be equally good. The box may
  sit somewhere awkward, such as a garage, with nobody at it.
- Ports can be attached to live equipment. One currently listens to a solar monitoring
  bus that another program reads over TCP. A restart interrupts that.
- Home Assistant over MQTT and a command-line console are the other two ways in. The
  console and the web page expose the same settings.
- A reload button on the box puts the network back to DHCP and clears the web password.

## Capabilities and Constraints

- Pages: Status (a verdict, every part's state, and one part's report), Network (address and
  MQTT), Hardware, Firmware.
- Reading needs no password; saving, restarting and installing firmware do. With no
  password set, nothing can be saved from the page.
- The web password can install firmware, which is full control of the device. Replacing
  the loader and reading flash need a separate key file.
- Ports' data sockets have no login.
- HTTP only, so browser features that require a secure context are unavailable. HTTPS is
  a documented stretch goal, not a capability (`docs/HTTPS.md`).
- **Must work with no internet:** no outside fonts, scripts, images or services. Size may
  grow when that helps.
- The page is part of the firmware image, gzipped (about 20 KB today). The image runs
  from a 384 KB RAM that also receives uploads, so images beyond roughly 190 KB could not
  replace one another from the web page. The image is about 128 KB today. Larger assets
  would have to be served from the 4 MB of external flash, which is not built.
- Up to 8 simultaneous web connections, closed after each response; the page polls once a
  second.
- Not tested: several ports busy at once, long or terminated buses, more than 1,000,000
  baud, and silence on the wire in listen-only (checked by counters, not measured).

Terms used in the UI and docs: port, line (the serial side), network, listen only, slot
(A or B), loader, trial, keep.

## Brand Commitments

- **Terse, facts only.** No explanatory filler. Set by the owner after seeing a page that
  had it. Warnings about consequences, such as transmitting or losing access, stay.
- **Name: open.** "N540 gateway" is what the page and Home Assistant show; "lino" is the
  project folder; the repository is "RS485-To-TCP-Bridge". The owner is happy for
  something catchy to be proposed. Nothing has been chosen. Each device's own name is
  editable and is what its page title shows.
- It is not the vendor's product and must not look like it or use its name as its own.

## Evidence on Hand

- Adapter tests on all four ports, 9600 to 1,000,000 baud: `docs/GATEWAY.md`.
- 30 seconds of a real bus through the gateway: 2,632 of 2,632 packet checksums valid.
- Live readings from the device: `/api/state`, `/api/hardware`, `/api/config`.
- Hardware facts and how sure each one is: `docs/HARDWARE.md`.
- None of these exist and must not be invented: other users, testimonials, uptime
  history, benchmarks against the vendor firmware, certifications.

## Product Principles

1. Nothing done over the network should need a debug probe to undo.
2. Show what was measured; say plainly when something was not.
3. Do not disturb the bus. Transmitting is always a deliberate act.
4. Say it once, in the fewest words that are still true.
5. Healthy should be visible at a glance, and a fault should name itself.

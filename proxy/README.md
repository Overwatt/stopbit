# Gateway ports as local serial devices

`n540-tty@.service` gives a Linux machine a serial device for a gateway port, such as
`/dev/ttyN540-1`, so a program that only talks to serial ports can use the RS485 line over the
network. socat joins a pseudo-terminal to the port's TCP server.

```sh
sudo apt install socat                      # dnf / pacman: socat
sudo install -m 644 n540-tty@.service /etc/systemd/system/
sudo mkdir -p /etc/n540-tty
printf 'HOST=n540-a1b2c3.local\nPORT=4001\n' | sudo tee /etc/n540-tty/1.conf
sudo systemctl daemon-reload
sudo systemctl enable --now n540-tty@1      # starts now and at every boot
sudo usermod -aG dialout $USER              # then log in again
```

One instance per port, each with its own file: `n540-tty@NAME` reads `/etc/n540-tty/NAME.conf`
and makes `/dev/ttyN540-NAME` (see [example.conf](example.conf)).

| Setting | |
|---|---|
| `HOST` | the gateway. A `.local` name needs mDNS (`libnss-mdns`); an address always works |
| `PORT` | the port's TCP server, 4001 to 4004 unless changed |
| `LINK` | another device name. Programs that list ports from `/sys/class/tty` (most do) never show it, whatever the name: give them the path. SolarAssistant also lists `/dev/ttyXRUSB*`, so use that there |
| `GROUP` | who may open it (`dialout`; `uucp` on Arch) |

## What it does

- Nothing goes over the network until a program first opens the device. The connection then
  stays up, and the device stays put when the program closes it, so it can open it again at
  once (SolarAssistant closes and reopens within half a second while it looks for a device).
- Bytes heard while no program has the device open wait in it, up to about 4 KB; a program
  that flushes on opening (SolarAssistant does) never sees them.
- If the gateway goes away (a restart, the network), the program gets end of file, as with an
  unplugged USB adapter, and should reopen; the device is gone for a second while the service
  makes it again. Opening while the gateway is unreachable ends the same way. The service keeps
  trying forever.
- A connection with nothing to send is checked after 30 s of quiet, so one the gateway no
  longer answers (it lost power, say) ends within about a minute, not the kernel's two hours.

## Things to know

- **Baud, parity and the rest are set on the gateway** (`tools/n540.py $H set port1.baud=...`).
  A program setting them on the device changes nothing.
- **The port must be in server mode, or a Modbus gateway with `modbus_framing=rtu`.** The device
  carries RTU bytes as they are, so Modbus RTU programs work as on a USB adapter. A Modbus gateway
  also lets the gateway's own devices poll the same line, in turn with the program; set
  `modbus_broadcast=answer` if the program asks unit 0 (SolarAssistant does, of EG4 inverters).
  Allow a little longer for replies, for the network. With devices on the port, the
  program's requests also wait the profile's turnaround (1 s for `eg4-hybrid`) and any poll
  ahead of them, so allow for that too.
- **There are no modem lines.** A program that sets RTS or DTR through ioctls may complain;
  the gateway switches the line's direction itself.
- **Port 1 is on the Tigo bus.** Anything a program writes goes onto it; set
  `port1.listen_only=1` to only watch.

```sh
systemctl status n540-tty@1
journalctl -u n540-tty@1 -f                 # connects, closes, errors
sudo systemctl stop 'n540-tty@*'            # all of them
```

Tested on Debian 12 (socat 1.7.4) with pyserial: bytes both ways, reopening, the gateway
going away, and a killed socat leaving its link behind. The keepalive settings came later and
were checked only to be accepted by socat 1.7.4.4.

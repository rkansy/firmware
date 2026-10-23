# Our meshtasticd — RPi5 / RAK6421 packaging

Deployment packaging for **our** multi-client meshtasticd build
(see the MULT-173 API changes: `src/mesh/MultiClientHub.*`,
`src/platform/portduino/MultiClientApiServerPort.*`,
`src/mesh/api/WiFiServerAPI.cpp`, `src/mesh/MeshService.cpp`,
`src/mesh/PhoneAPI.{h,cpp}`).

Target hardware mirrors the reference node **k4nsy-long**:
Raspberry Pi 5, Debian trixie, RAK6421 (SX1262) in HAT slot 1 via
`spidev0.0` + libgpiod, serial GPS on `/dev/ttyAMA0`.

## What is different from stock

The TCP API on port **4403** accepts **N concurrent clients**. Stock
meshtasticd force-closes the previous connection on every new one, which is
why the reference node had to run an external Python proxy
(`meshtastic-multiplexer` on 4404) in front of the daemon. Our build
retires that proxy:

- each client independently performs the full config handshake
  (my_info / metadata / config / moduleConfig / channels / node DB) with a
  `config_complete_id` matching *that client's* request;
- live upstream FromRadio traffic (positions, telemetry, node-DB updates,
  received packets) fans out to all connected clients;
- each client's outgoing `MeshPacket` id is remapped server-side; matching
  responses (routing ACK/NAK, queue status, client notifications) are
  delivered only to the originating client with its id restored;
- a client disconnect never disturbs the other clients' sessions;
  reconnects re-serve the full state.

Wire framing is byte-identical to stock (`0x94 0xC3` + 2-byte BE length +
ToRadio/FromRadio), so existing clients (`meshtastic` CLI/Python,
`aprstasticd`, `meshtastic-mcp`) work unmodified.

## Layout

```
/usr/bin/meshtasticd                 daemon binary (this build)
/etc/meshtasticd/config.yaml         main config (API 4403, web 9443, JSON log)
/etc/meshtasticd/config.d/           active radio profiles (RAK6421 slot 1)
/etc/meshtasticd/available.d/        stock radio profiles (copy to activate)
/etc/meshtasticd/ssl/                HTTPS key/cert for the webserver
/var/lib/meshtasticd                 VFS (prefs), owned by user meshtasticd
/usr/share/meshtasticd/web           web UI assets
/etc/systemd/system/meshtasticd.service
```

Dedicated system user `meshtasticd` (no login). Ports: API **4403** (TCP),
web UI **9443** (HTTPS). JSON packet logging to `/packets.json` (rotated
every 60 min, filtered to position packets) and trace log to
`/var/log/meshtasticd.json`.

## Build

```bash
python3 -m venv pioenv && ./pioenv/bin/pip install -U platformio
export PATH="$PWD/pioenv/bin:$PATH" VIRTUAL_ENV="$PWD/pioenv"
pio pkg install -e native
pio run -e native
# binary: .pio/build/native/meshtasticd
# (note: `pio test -e native` wipes the built binary — re-run `pio run` after)
```

## Install on the host (RPi5 / trixie)

```bash
sudo ./packaging/rpi5-rak6421/install-meshtasticd.sh \
    .pio/build/native/meshtasticd --start
```

Options: `--web-tarball FILE` (skip the web download), `--skip-web`,
`--skip-ssl` (bring your own cert), `--start`. The script is idempotent and
preserves any pre-existing config as `<file>.dist`.

## Docker

```bash
docker build -f packaging/rpi5-rak6421/Dockerfile -t meshtasticd-multi .
docker run --rm -it --net host \
    --device /dev/spidev0.0 --device /dev/gpiochip0 \
    -v meshtasticd-vfs:/var/lib/meshtasticd meshtasticd-multi
```

Runs as root inside the container (spidev/gpio device access); the dedicated
`meshtasticd` user + VFS layout are still created inside the image.

**MAC note:** the container has no hardware MAC, so the daemon needs a
resolvable MAC to derive its node ID — set `MACAddress` in `config.yaml`, or
`MACAddressSource: <iface>` under `General:`.

## Verify (concurrent clients)

With the daemon running (hardware or `-s` sim mode), run:

```bash
pip install meshtastic   # only for the bundled protobufs
python3 packaging/rpi5-rak6421/smoke_multiclient.py
```

The script connects **two** clients simultaneously to TCP 4403 and checks:
independent config handshakes (per-client `config_complete_id`), per-client
queue-status id remapping, live-traffic fan-out, owner-only response routing,
and that dropping one client leaves the other untouched. Exit code 0 = all
checks passed.

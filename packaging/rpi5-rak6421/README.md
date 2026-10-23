# Our meshtasticd — RPi5 / RAK6421 packaging

Deployment packaging for **our** multi-client meshtasticd build
(see the MULT-173 API changes: `src/mesh/MultiClientHub.*`,
`src/platform/portduino/MultiClientApiServerPort.*`,
`src/mesh/api/WiFiServerAPI.cpp`, `src/mesh/MeshService.cpp`,
`src/mesh/PhoneAPI.{h,cpp}`).

Target hardware mirrors the reference node **k4nsy-long**:
Raspberry Pi 5, Debian trixie, RAK6421 (SX1262) in HAT slot 1 via
`spidev0.0` + libgpiod, serial GPS on `/dev/ttyAMA0`.

The active profile (`config.d/lora-RAK6421-13300-slot1.yaml`) carries the
RAK13300-in-HAT-slot-1 pin mapping observed on k4nsy-long: IRQ 22 (IO6),
Reset 16 (IO4), Busy 24 (IO5), Ant_sw 13 (IO3), CS 8.

**config.d merge semantics:** *every* `*.yaml` file in `config.d/` is merged
on top of the main config at startup, in directory read order (filesystem
order, not guaranteed alphabetical), last file wins per key. Dropping a
second radio profile in will override the first's keys — keep exactly one
active profile.

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
web UI **9443** (HTTPS). Packets are logged to
`/var/lib/meshtasticd/packets.json` — the daemon's **single packet stream**
(JSON, mode `0600`, filtered to position packets). There is no separate
trace log; console output goes to the systemd journal (host) or container
stdout (Docker). The log is **append-only and unbounded** at this firmware
tag: daemon-side rotation (`JSONFileRotate`) is not usable — any non-zero
value makes the daemon exit at startup (`*** JSONFile open failure`,
`PortduinoGlue.cpp:688-699`), so the shipped config leaves it at 0. Prune
the file (VFS dir / Docker volume) if you keep the box for a long time.

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
`--skip-ssl` (bring your own cert), `--start`, `--force`. The script is
idempotent and **never overwrites an existing config** — re-running it keeps
your edits; pass `--force` to replace a config with the packaged version
(each current file is backed up with a timestamp first). It also installs the
daemon's runtime libraries (apt, no-op when already present) and verifies the
binary runs, failing with an `ldd` list of missing libraries otherwise.

## Docker

The image is **single-arch**: the builder stage compiles the daemon with the
host toolchain (`pio run -e native` — no cross-compilation). Build it on the
RPi5, or from an x86 host with `docker buildx build --platform linux/arm64
-f packaging/rpi5-rak6421/Dockerfile -t meshtasticd-multi .` (QEMU emulation,
slow). An x86 image built on a laptop will not run on the Pi.

```bash
docker build -f packaging/rpi5-rak6421/Dockerfile -t meshtasticd-multi .
docker run --rm -it --net host \
    --device /dev/spidev0.0 --device /dev/gpiochip0 \
    -v meshtasticd-vfs:/var/lib/meshtasticd meshtasticd-multi
```

Runs as root inside the container (spidev/gpio device access); the dedicated
`meshtasticd` user + VFS layout are still created inside the image. For an
unprivileged run, use the in-image user *and* the device groups (device
nodes must be group-accessible — stock trixie udev gives `spidev*` / `ttyAMA0`
/ `gpiochip*` mode 0660 in groups `spi` / `dialout` / `gpio`):

**Precondition:** `--group-add` only works if the group exists **in the
image** — stock RPi OS trixie has `spi`/`gpio`; a minimal `debian:trixie`
(-slim) build may not. Create them in the image (`groupadd spi gpio`) or
run as root.

```bash
docker run --rm -it --net host \
    --user meshtasticd --group-add spi --group-add gpio \
    --device /dev/spidev0.0 --device /dev/gpiochip0 \
    -v meshtasticd-vfs:/var/lib/meshtasticd meshtasticd-multi
```

The packet log file is pre-created in the image (`0600`, owned by
`meshtasticd`), so the unprivileged user can append to it from first boot.

**TLS note:** the self-signed web key/cert is generated at build time and is
therefore *shared by every container of this image* (image contents can be
extracted, so `chmod 600` is not real protection). Fine for a single-node Pi;
mount your own cert/key over `/etc/meshtasticd/ssl` for multiple instances or
per-host identity.

## MAC requirement (both deployment paths)

The daemon refuses to start without a resolvable node MAC (`*** Blank MAC
Address not allowed!`). The shipped `config.yaml` sets
`General: MACAddress: 88:A2:9E:8B:80:5D` — the live node ID of the reference
node k4nsy-long — so **both** the host and the container start out of the
box; change the value when you deploy a different node.

Why explicit instead of derived:

- the hardware autoconf MAC source (Pi HAT+ / RAK eeprom) is skipped while
  the config.d profile pins `Lora.Module: sx1262`;
- the Bluetooth HCI fallback is dead on hosts with BT disabled — which is the
  usual setup here, since `/dev/ttyAMA0` is the default BT serial port and
  this config uses it for GPS — and inside containers with their own network
  namespace; in every such case the daemon exits at startup.

Prefer deriving it? `General: MACAddressSource: eth0` is readable by any user
in any network namespace; the explicit value is simply what keeps the node ID
stable and documented.

## Verify (concurrent clients)

With the daemon running, run the smoke test — pass `-s` **iff** the daemon
was started in sim mode (`meshtasticd -s`). Note: in sim mode the daemon
does **not** read `config.yaml` at all (stock `portduino` behavior — sim
returns before config loading), so a sim run exercises built-in defaults
(API port 4403 anyway) and does **not** test the shipped config:

```bash
pip install meshtastic   # only for the bundled protobufs
# hardware daemon:
python3 packaging/rpi5-rak6421/smoke_multiclient.py
# sim daemon (meshtasticd -s):
python3 packaging/rpi5-rak6421/smoke_multiclient.py -s
```

The script connects **two** clients simultaneously to the TCP API port
(`--host` / `--port`, default `127.0.0.1:4403`) and checks:
independent config handshakes (per-client `config_complete_id`), per-client
queue-status id remapping — including a **same-id collision** (both clients
send the same client-assigned id) and a **`want_ack` DM** whose routing
NAK must reach only the sender with the id restored — live-traffic fan-out,
owner-only response routing, and that dropping one client leaves the other
untouched.

**Which sections apply where:** sections 2 (own echo) and 3 (live-traffic
fan-out) are **sim-only** — on stock code the multi-client fan-out is only
triggered by the radio RX path, and the simulated radio is the only one that
echoes sent packets back; on hardware they would just time out. On hardware
they are skipped (not counted); the script prints the applicable section
list at startup. All other sections (1, 4-8) run on both hardware and sim.

**Exit codes:** `0` = all *applicable* checks passed, `1` = one or more
checks failed (a `FAILS` summary is printed), `2` = setup failure
(daemon unreachable / protobufs missing — a friendly message, not a
traceback).

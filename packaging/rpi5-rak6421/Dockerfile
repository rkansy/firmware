# Our multi-client meshtasticd for RPi5-class hardware (RAK6421 SX1262).
#
# Differs from the stock upstream image in that the built daemon natively
# serves ANY NUMBER of concurrent TCP API clients on port 4403 (see
# src/mesh/MultiClientHub.* and src/platform/portduino/MultiClientApiServerPort.*).
# No external multiplexer proxy is needed.
#
# Build (from the repo root):
#   docker build -f packaging/rpi5-rak6421/Dockerfile -t meshtasticd-multi .
# Run with the radio attached:
#   docker run --rm -it --net host --device /dev/spidev0.0 --device /dev/gpiochip0 \
#     -v meshtasticd-vfs:/var/lib/meshtasticd meshtasticd-multi
#
# The container runs as root by default because the SX1262 on spidev0.0 and the
# libgpiod pins need elevated access inside a container. For an unprivileged
# run, add the host's spidev/gpio device groups and use the 'meshtasticd' user
# (created below) via `--user meshtasticd`.

# trunk-ignore-all(trivy/DS002): we run as root for container device access
# trunk-ignore-all(hadolint/DL3002): device access needs root
# trunk-ignore-all(hadolint/DL3008): do not pin apt package versions
# trunk-ignore-all(hadolint/DL3013): do not pin pip package versions

FROM debian:trixie AS builder
ARG PIO_ENV=native
ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=Etc/UTC
ENV PIP_ROOT_USER_ACTION=ignore
ENV PIP_BREAK_SYSTEM_PACKAGES=1
RUN apt-get update && apt-get install --no-install-recommends -y \
        curl wget g++ zip git ca-certificates pkg-config \
        python3-pip python3-grpc-tools \
        libgpiod-dev libyaml-cpp-dev libbluetooth-dev libi2c-dev libuv1-dev \
        libusb-1.0-0-dev libulfius-dev liborcania-dev libssl-dev \
        libx11-dev libinput-dev libxkbcommon-x11-dev libsqlite3-dev libsdl2-dev \
    && apt-get clean && rm -rf /var/lib/apt/lists/* \
    && pip install --no-cache-dir -U platformio \
    && mkdir /tmp/firmware

WORKDIR /tmp/firmware
COPY . /tmp/firmware

RUN bash ./bin/build-native.sh "$PIO_ENV" && \
    cp "/tmp/firmware/release/meshtasticd_linux_$(uname -m)" /tmp/firmware/release/meshtasticd

# Web UI assets (same pinned version the stock image bundles)
RUN curl -fL "https://github.com/meshtastic/web/releases/download/v$(cat /tmp/firmware/bin/web.version)/build.tar" -o /tmp/web.tar \
    && mkdir -p /tmp/web \
    && tar -xf /tmp/web.tar -C /tmp/web/ \
    && find /tmp/web -name '*.gz' -exec gzip -d {} \; \
    && rm /tmp/web.tar

##### PRODUCTION #############
FROM debian:trixie-slim
LABEL org.opencontainers.image.title="Meshtastic (multi-client build)" \
      org.opencontainers.image.description="Our multi-client Meshtastic daemon + web UI for RPi5/RAK6421" \
      org.opencontainers.image.licenses="GPL-3.0-or-later"
ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=Etc/UTC

# nosemgrep: dockerfile.security.last-user-is-root.last-user-is-root
USER root
RUN apt-get update && apt-get --no-install-recommends -y install \
        ca-certificates openssl \
        libc-bin libc6 libgpiod3 libyaml-cpp0.8 libi2c0 libuv1t64 libusb-1.0-0 \
        liborcania2.3 libulfius2.7t64 libssl3t64 \
        libx11-6 libinput10 libxkbcommon-x11-0 libsdl2-2.0-0 \
    && apt-get clean && rm -rf /var/lib/apt/lists/* \
    # dedicated user + target layout (mirrors the host systemd deployment)
    && useradd --system --home-dir /var/lib/meshtasticd --shell /usr/sbin/nologin meshtasticd \
    && mkdir -p /var/lib/meshtasticd \
               /etc/meshtasticd/config.d \
               /etc/meshtasticd/available.d \
               /etc/meshtasticd/ssl \
               /usr/share/meshtasticd/web \
    && chown -R meshtasticd:meshtasticd /var/lib/meshtasticd \
    # one-time self-signed HTTPS cert so the webserver can start out of the box
    && CN=meshtasticd \
    && openssl req -x509 -nodes -newkey rsa:2048 -days 3650 \
         -keyout /etc/meshtasticd/ssl/private_key.pem \
         -out /etc/meshtasticd/ssl/certificate.pem \
         -subj "/CN=${CN}" \
    && chown -R meshtasticd:meshtasticd /etc/meshtasticd/ssl \
    && chmod 600 /etc/meshtasticd/ssl/private_key.pem

COPY --from=builder /tmp/firmware/release/meshtasticd /usr/bin/meshtasticd
COPY --from=builder /tmp/web /usr/share/meshtasticd/web/
COPY packaging/rpi5-rak6421/config.yaml /etc/meshtasticd/config.yaml
COPY packaging/rpi5-rak6421/config.d/lora-RAK6421-13300-slot1.yaml /etc/meshtasticd/config.d/

WORKDIR /var/lib/meshtasticd
VOLUME /var/lib/meshtasticd

# TCP API (multi-client) + HTTPS web UI
EXPOSE 4403 9443

# Root by default for spidev/gpio access; override with --user for unprivileged.
CMD [ "meshtasticd", "--config=/etc/meshtasticd/config.yaml", "--fsdir=/var/lib/meshtasticd" ]

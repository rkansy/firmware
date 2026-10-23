#!/usr/bin/env bash
#
# install-meshtasticd.sh — install OUR multi-client meshtasticd build on an
# RPi5-class Debian (trixie) host, mirroring the reference k4nsy-long layout:
#
#   /usr/bin/meshtasticd                 the daemon (multi-client API build)
#   /etc/meshtasticd/config.yaml         main config (API 4403, web 9443)
#   /etc/meshtasticd/config.d/           radio profiles (RAK6421 slot 1)
#   /etc/meshtasticd/ssl/                self-signed HTTPS cert/key
#   /var/lib/meshtasticd                 VFS (prefs), owned by user meshtasticd
#   /usr/share/meshtasticd/web           web UI assets
#   /etc/systemd/system/meshtasticd.service
#
# Usage:
#   sudo ./install-meshtasticd.sh /path/to/meshtasticd [options]
#
# Options:
#   --web-tarball FILE   local web UI build.tar (skips the download)
#   --skip-web           do not install web assets (webserver will not start)
#   --skip-ssl           do not generate a self-signed cert (bring your own)
#   --start              systemctl start meshtasticd when done
#   --force              replace existing configs with the packaged versions
#                        (each current file is backed up with a timestamp first)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

BINARY=""
WEB_TARBALL=""
SKIP_WEB=0
SKIP_SSL=0
START=0
FORCE=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --web-tarball) WEB_TARBALL="${2:?--web-tarball needs a path}"; shift 2 ;;
    --skip-web) SKIP_WEB=1; shift ;;
    --skip-ssl) SKIP_SSL=1; shift ;;
    --start) START=1; shift ;;
    --force) FORCE=1; shift ;;
    --*) echo "unknown option: $1" >&2; exit 2 ;;
    *)
      if [[ -n "$BINARY" ]]; then echo "unexpected argument: $1" >&2; exit 2; fi
      BINARY="$1"; shift ;;
  esac
done

if [[ -z "$BINARY" || ! -f "$BINARY" ]]; then
  echo "usage: sudo $0 /path/to/meshtasticd [--web-tarball FILE] [--skip-web] [--skip-ssl] [--start] [--force]" >&2
  exit 2
fi
if [[ "$(id -u)" -ne 0 ]]; then
  echo "must run as root (sudo)" >&2
  exit 1
fi

# prerequisite tools
missing=()
for tool in install useradd chown chmod; do
  command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
done
if [[ "$SKIP_SSL" -eq 0 ]]; then
  command -v openssl >/dev/null 2>&1 || missing+=("openssl")
fi
if [[ "$SKIP_WEB" -eq 0 && -z "$WEB_TARBALL" ]]; then
  command -v curl >/dev/null 2>&1 || missing+=("curl")
fi
if [[ ${#missing[@]} -gt 0 ]]; then
  echo "missing required tool(s): ${missing[*]}" >&2
  exit 1
fi

echo "== user =="
if ! id meshtasticd >/dev/null 2>&1; then
  useradd --system --home-dir /var/lib/meshtasticd --shell /usr/sbin/nologin meshtasticd
  echo "created system user 'meshtasticd'"
fi

echo "== directories =="
install -d -o meshtasticd -g meshtasticd /var/lib/meshtasticd
install -d /etc/meshtasticd/config.d /etc/meshtasticd/available.d /etc/meshtasticd/ssl
install -d -g meshtasticd /usr/share/meshtasticd/web

echo "== packet log file =="
# The shipped config sets Logging.JSONFile in the VFS dir; the file contains
# full decoded packet payloads, so it must stay 0600. Enforce ownership and
# mode on FIRST install and on EVERY re-install (an existing file with wrong
# permissions would otherwise stay broken for the service user), but never
# truncate an existing log. A symlink at this path is refused outright.
PACKETS_LOG=/var/lib/meshtasticd/packets.json
if [[ -L "$PACKETS_LOG" ]]; then
  echo "ERROR: $PACKETS_LOG is a symlink — refusing to install over it. Remove it and re-run." >&2
  exit 1
fi
if [[ ! -e "$PACKETS_LOG" ]]; then
  install -o meshtasticd -g meshtasticd -m 0600 /dev/null "$PACKETS_LOG"
fi
chown meshtasticd:meshtasticd "$PACKETS_LOG"
chmod 0600 "$PACKETS_LOG"

echo "== runtime libraries =="
# Same set the reference image installs (see packaging/rpi5-rak6421/Dockerfile
# and the root Dockerfile). On a host that already has them apt is a no-op;
# on a minimal trixie host this is what lets the service actually start.
apt-get update -qq >/dev/null 2>&1 || true
if ! apt-get install --no-install-recommends -y \
      libgpiod3 libyaml-cpp0.8 libi2c0 libuv1t64 libusb-1.0-0 \
      liborcania2.3 libulfius2.7t64 libssl3t64 \
      libx11-6 libinput10 libxkbcommon-x11-0 libsdl2-2.0-0 \
      ca-certificates openssl; then
  echo "WARNING: apt could not install the runtime library set (non-trixie host?)" >&2
  echo "         continuing — the --version/ldd check below will catch gaps" >&2
fi

echo "== binary =="
install -m 0755 "$BINARY" /usr/bin/meshtasticd
# Verify the binary actually runs — a missing shared library must fail HERE
# with an actionable message, not as a flapping service at boot.
if ! /usr/bin/meshtasticd --version; then
  echo "ERROR: /usr/bin/meshtasticd --version failed. Missing shared libraries?" >&2
  ldd /usr/bin/meshtasticd 2>/dev/null | grep -F "not found" >&2 || true
  exit 1
fi

echo "== config =="
# Idempotent and non-destructive: existing configs are NEVER overwritten on a
# re-run. Pass --force to replace them with the packaged versions — the
# current file is backed up with a timestamp before each replacement.
install_config() {
  local src="$1" dst="$2"
  if [[ -e "$dst" && "$FORCE" -ne 1 ]]; then
    echo "  keeping existing $dst (use --force to replace it)"
    return 0
  fi
  if [[ -e "$dst" ]]; then
    cp -a "$dst" "${dst}.bak-$(date +%Y%m%d-%H%M%S)"
    echo "  existing $dst backed up, replacing"
  fi
  install -m 0644 "$src" "$dst"
}
install_config "$SCRIPT_DIR/config.yaml" /etc/meshtasticd/config.yaml
install -d /etc/meshtasticd/config.d
install_config "$SCRIPT_DIR/config.d/lora-RAK6421-13300-slot1.yaml" \
  /etc/meshtasticd/config.d/lora-RAK6421-13300-slot1.yaml
# upstream radio profiles for convenience
if [[ -d "$REPO_ROOT/bin/config.d" ]]; then
  install -d /etc/meshtasticd/available.d
  cp -an "$REPO_ROOT/bin/config.d/." /etc/meshtasticd/available.d/ 2>/dev/null || true
fi

echo "== web assets =="
if [[ "$SKIP_WEB" -eq 0 ]]; then
  if [[ -n "$WEB_TARBALL" ]]; then
    tar -xf "$WEB_TARBALL" -C /usr/share/meshtasticd/web/
  else
    local_ver="$(cat "$REPO_ROOT/bin/web.version" 2>/dev/null || echo 2.6.7)"
    echo "  downloading web v${local_ver} ..."
    # Private temp dir: never a fixed /tmp path, which a local user could
    # pre-create as a symlink while we run as root.
    dl_dir="$(mktemp -d)"
    trap 'rm -rf "$dl_dir"' EXIT
    curl -fL "https://github.com/meshtastic/web/releases/download/v${local_ver}/build.tar" -o "$dl_dir/web.tar"
    tar -xf "$dl_dir/web.tar" -C /usr/share/meshtasticd/web/
  fi
  find /usr/share/meshtasticd/web -name '*.gz' -exec gzip -d {} \; 2>/dev/null || true
  chmod -R a+rX /usr/share/meshtasticd/web
else
  echo "  skipped (--skip-web)"
fi

echo "== ssl =="
SSL_KEY=/etc/meshtasticd/ssl/private_key.pem
SSL_CERT=/etc/meshtasticd/ssl/certificate.pem
# Validate the PAIR, not just the key: a half-generated pair (e.g. from an
# interrupted run) must be regenerated, not silently kept.
if [[ "$SKIP_SSL" -eq 0 && ! ( -f "$SSL_KEY" && -f "$SSL_CERT" ) ]]; then
  if [[ -f "$SSL_KEY" || -f "$SSL_CERT" ]]; then
    echo "  incomplete SSL pair found — removing orphan file(s) and regenerating"
    rm -f "$SSL_KEY" "$SSL_CERT"
  fi
  CN="$(hostname)"
  openssl req -x509 -nodes -newkey rsa:2048 -days 3650 \
    -keyout "$SSL_KEY" \
    -out "$SSL_CERT" \
    -subj "/CN=${CN}"
  echo "  generated self-signed cert for CN=${CN}"
fi
chown -R meshtasticd:meshtasticd /etc/meshtasticd/ssl
chmod 600 /etc/meshtasticd/ssl/private_key.pem 2>/dev/null || true

# systemd helpers: no-op when systemd is not managing this system (e.g. a
# container or a chroot test), so the rest of the install still completes.
have_systemd() {
  command -v systemctl >/dev/null 2>&1 && [[ -d /run/systemd/system ]]
}
run_systemctl() {
  if have_systemd; then
    systemctl "$@"
  else
    echo "  (systemd not running — skipping: systemctl $*)"
    return 0
  fi
}

echo "== systemd =="
install -m 0644 "$SCRIPT_DIR/meshtasticd.service" /etc/systemd/system/meshtasticd.service
run_systemctl daemon-reload
run_systemctl enable meshtasticd.service

echo
echo "== done =="
echo "  VFS:            /var/lib/meshtasticd (user: meshtasticd)"
echo "  API (TCP):      4403  (multi-client: N concurrent clients supported)"
echo "  Web UI (HTTPS): 9443"
if [[ "$START" -eq 1 ]]; then
  run_systemctl start meshtasticd.service
  sleep 3
  run_systemctl --no-pager status meshtasticd.service || true
  echo
  echo "concurrent-client smoke test:"
  echo "  python3 $SCRIPT_DIR/smoke_multiclient.py"
else
  echo "  systemctl start meshtasticd"
fi

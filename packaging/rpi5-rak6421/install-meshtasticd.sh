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
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

BINARY=""
WEB_TARBALL=""
SKIP_WEB=0
SKIP_SSL=0
START=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --web-tarball) WEB_TARBALL="${2:?--web-tarball needs a path}"; shift 2 ;;
    --skip-web) SKIP_WEB=1; shift ;;
    --skip-ssl) SKIP_SSL=1; shift ;;
    --start) START=1; shift ;;
    --*) echo "unknown option: $1" >&2; exit 2 ;;
    *)
      if [[ -n "$BINARY" ]]; then echo "unexpected argument: $1" >&2; exit 2; fi
      BINARY="$1"; shift ;;
  esac
done

if [[ -z "$BINARY" || ! -f "$BINARY" ]]; then
  echo "usage: sudo $0 /path/to/meshtasticd [--web-tarball FILE] [--skip-web] [--skip-ssl] [--start]" >&2
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

echo "== binary =="
install -m 0755 "$BINARY" /usr/bin/meshtasticd
/usr/bin/meshtasticd --version || true

echo "== config =="
install_config() {
  local src="$1" dst="$2"
  if [[ -e "$dst" && ! -e "${dst}.dist" ]]; then
    cp -a "$dst" "${dst}.dist"   # keep a pristine copy of the pre-existing file
    echo "  existing $dst preserved as ${dst}.dist"
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
    curl -fL "https://github.com/meshtastic/web/releases/download/v${local_ver}/build.tar" -o /tmp/meshtastic-web.tar
    tar -xf /tmp/meshtastic-web.tar -C /usr/share/meshtasticd/web/
    rm -f /tmp/meshtastic-web.tar
  fi
  find /usr/share/meshtasticd/web -name '*.gz' -exec gzip -d {} \; 2>/dev/null || true
  chmod -R a+rX /usr/share/meshtasticd/web
else
  echo "  skipped (--skip-web)"
fi

echo "== ssl =="
if [[ "$SKIP_SSL" -eq 0 && ! -f /etc/meshtasticd/ssl/private_key.pem ]]; then
  CN="$(hostname)"
  openssl req -x509 -nodes -newkey rsa:2048 -days 3650 \
    -keyout /etc/meshtasticd/ssl/private_key.pem \
    -out /etc/meshtasticd/ssl/certificate.pem \
    -subj "/CN=${CN}" 2>/dev/null
  echo "  generated self-signed cert for CN=${CN}"
fi
chown -R meshtasticd:meshtasticd /etc/meshtasticd/ssl
chmod 600 /etc/meshtasticd/ssl/private_key.pem 2>/dev/null || true

# systemd helpers: no-op when systemd is not managing this system (e.g. a
# container or a chroot test), so the rest of the install still completes.
have_systemd() {
  command -v systemctl >/dev/null 2>&1 && [[ -d /run/systemd/system ]]
}
sysctl() {
  if have_systemd; then
    systemctl "$@"
  else
    echo "  (systemd not running — skipping: systemctl $*)"
    return 0
  fi
}

echo "== systemd =="
install -m 0644 "$SCRIPT_DIR/meshtasticd.service" /etc/systemd/system/meshtasticd.service
sysctl daemon-reload
sysctl enable meshtasticd.service

echo
echo "== done =="
echo "  VFS:            /var/lib/meshtasticd (user: meshtasticd)"
echo "  API (TCP):      4403  (multi-client: N concurrent clients supported)"
echo "  Web UI (HTTPS): 9443"
if [[ "$START" -eq 1 ]]; then
  sysctl start meshtasticd.service
  sleep 3
  sysctl --no-pager status meshtasticd.service || true
  echo
  echo "concurrent-client smoke test:"
  echo "  python3 $SCRIPT_DIR/smoke_multiclient.py"
else
  echo "  systemctl start meshtasticd"
fi

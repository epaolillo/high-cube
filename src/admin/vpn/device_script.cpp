#include "vpn/device_script.hpp"

#include "vpn/vpn_topology.hpp"

namespace highcube::admin::vpn {
namespace {

constexpr const char* kEnrollTemplate = R"HCSH(#!/usr/bin/env bash
# High Cube VPN: joins this Ubuntu machine to the WireGuard network through its central node.
set -euo pipefail

HC_URL='__HC_URL__'
HC_TOKEN='__HC_TOKEN__'
IFACE='__HC_IFACE__'
STATE_DIR=/etc/hc-vpn

say() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31mError:\033[0m %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "Run this command with sudo."
command -v apt-get >/dev/null 2>&1 || die "Only Ubuntu (apt) is supported."

if ! command -v wg >/dev/null 2>&1 || ! command -v wg-quick >/dev/null 2>&1 || ! command -v curl >/dev/null 2>&1; then
  say "Installing WireGuard tools"
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq wireguard-tools curl iproute2 >/dev/null
fi

install -d -m 700 "$STATE_DIR" /etc/wireguard
if [ ! -s "$STATE_DIR/private.key" ]; then
  say "Generating the key pair (the private key never leaves this machine)"
  (umask 077 && wg genkey > "$STATE_DIR/private.key")
fi
PUBLIC_KEY=$(wg pubkey < "$STATE_DIR/private.key")
LAN_IP=$(ip -4 route get 1.1.1.1 2>/dev/null | awk '{for (i = 1; i < NF; i++) if ($i == "src") { print $(i + 1); exit }}' || true)
HOST_NAME=$(hostname | tr -cd 'A-Za-z0-9._-')

say "Registering $HOST_NAME in High Cube"
DEVICE_TOKEN=$(curl -fsS --max-time 30 -X POST "$HC_URL/api/vpn/enroll/$HC_TOKEN" \
  -H 'Content-Type: application/json' \
  --data "{\"publicKey\":\"$PUBLIC_KEY\",\"hostname\":\"$HOST_NAME\",\"lanIp\":\"$LAN_IP\"}") \
  || die "High Cube rejected the enrollment. Generate a new command from your profile."

(umask 077 && printf 'HC_URL=%s\nHC_DEVICE_TOKEN=%s\nIFACE=%s\n' "$HC_URL" "$DEVICE_TOKEN" "$IFACE" > "$STATE_DIR/device.env")

cat > /usr/local/sbin/hc-vpn-sync <<'SYNC'
#!/usr/bin/env bash
# Keeps the High Cube VPN interface in sync with the members registered in hc-admin.
set -uo pipefail
. /etc/hc-vpn/device.env
CONF="/etc/wireguard/$IFACE.conf"
VERBOSE=${1:-}

teardown() {
  systemctl disable --now hc-vpn-sync.timer >/dev/null 2>&1 || true
  wg-quick down "$IFACE" >/dev/null 2>&1 || true
  systemctl disable "wg-quick@$IFACE" >/dev/null 2>&1 || true
  rm -f "$CONF" /etc/systemd/system/hc-vpn-sync.service /etc/systemd/system/hc-vpn-sync.timer
  rm -rf /etc/hc-vpn
  rm -f /usr/local/sbin/hc-vpn-sync
  systemctl daemon-reload >/dev/null 2>&1 || true
}

# WireGuard resolves endpoint names once. When a peer behind a name has not answered for a
# while, resolve it again: the public IP behind the name may have changed.
reresolve() {
  local now pk ep host hs before after
  now=$(date +%s)
  awk '/^\[Peer\]/ {pk = ""} /^PublicKey/ {sub(/^PublicKey *= */, ""); pk = $0}
       /^Endpoint/ {sub(/^Endpoint *= */, ""); if (pk != "") print pk " " $0}' "$CONF" 2>/dev/null |
  while read -r pk ep; do
    host=${ep%:*}
    host=${host#[}
    host=${host%]}
    [[ "$host" =~ ^[0-9.]+$ || "$host" == *:* ]] && continue
    hs=$(wg show "$IFACE" latest-handshakes 2>/dev/null | awk -v k="$pk" '$1 == k {print $2}')
    if [ -z "$hs" ] || [ $((now - hs)) -gt 135 ]; then
      before=$(wg show "$IFACE" endpoints 2>/dev/null | awk -v k="$pk" '$1 == k {print $2}')
      wg set "$IFACE" peer "$pk" endpoint "$ep" 2>/dev/null || continue
      after=$(wg show "$IFACE" endpoints 2>/dev/null | awk -v k="$pk" '$1 == k {print $2}')
      [ "$before" != "$after" ] && echo "Re-resolved $ep: $before -> $after"
    fi
  done
}

# Keeps the tunnel usable even while hc-admin cannot be reached.
watchdog() {
  [ -f "$CONF" ] || return 0
  if ! ip link show "$IFACE" >/dev/null 2>&1; then
    wg-quick up "$IFACE" >/dev/null 2>&1 && echo "Brought $IFACE back up"
  fi
  local port
  port=$(awk -F' *= *' '/^ListenPort/ {print $2; exit}' "$CONF")
  if [ -n "$port" ] && command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | grep -q '^Status: active' &&
    ! ufw status 2>/dev/null | grep -qE "^$port/udp"; then
    ufw allow "$port/udp" comment 'High Cube VPN' >/dev/null 2>&1 && echo "Opened $port/udp in ufw"
  fi
  reresolve
}

UP=0
ip link show "$IFACE" >/dev/null 2>&1 && UP=1
LAN_IP=$(ip -4 route get 1.1.1.1 2>/dev/null | awk '{for (i = 1; i < NF; i++) if ($i == "src") { print $(i + 1); exit }}')
BODY=$(mktemp)
NEW=$(mktemp)
trap 'rm -f "$BODY" "$NEW"' EXIT

# Peer counters only: the first dump line holds the private key and is never sent.
# The interface does not exist before the first sync, so wg failing must not fail the pipe.
CODE=$({ wg show "$IFACE" dump 2>/dev/null | tail -n +2 || true; } | curl -sS --max-time 10 -o "$BODY" -w '%{http_code}' \
  -X POST "$HC_URL/api/vpn/device/sync" \
  -H "Authorization: Bearer $HC_DEVICE_TOKEN" \
  -H "X-Lan-Ip: $LAN_IP" \
  -H "X-Interface-Up: $UP" \
  -H 'Content-Type: text/plain' \
  --data-binary @-) || CODE=000

if [ "$CODE" = 410 ]; then
  echo "This machine was removed from the High Cube VPN"
  teardown
  exit 0
fi
if [ "$CODE" != 200 ]; then
  if [ -n "$VERBOSE" ]; then
    echo "hc-admin answered HTTP $CODE" >&2
    exit 1
  fi
  # The last configuration keeps working without hc-admin.
  watchdog
  exit 0
fi

{ printf '[Interface]\nPrivateKey = %s\n' "$(cat /etc/hc-vpn/private.key)"; cat "$BODY"; } > "$NEW"
if [ "$UP" = 0 ] || ! cmp -s "$NEW" "$CONF"; then
  # Peer changes are applied live; a different [Interface] section needs a restart.
  OLD_INTERFACE=$(sed '/^\[Peer\]/,$d' "$CONF" 2>/dev/null || true)
  NEW_INTERFACE=$(sed '/^\[Peer\]/,$d' "$NEW")
  install -m 600 "$NEW" "$CONF"
  if [ "$UP" = 1 ] && [ "$OLD_INTERFACE" = "$NEW_INTERFACE" ]; then
    wg syncconf "$IFACE" <(wg-quick strip "$IFACE") || exit 1
  else
    wg-quick down "$IFACE" >/dev/null 2>&1 || true
    wg-quick up "$IFACE" || exit 1
  fi
  systemctl enable "wg-quick@$IFACE" >/dev/null 2>&1 || true
fi
watchdog
SYNC
chmod 755 /usr/local/sbin/hc-vpn-sync

cat > /etc/systemd/system/hc-vpn-sync.service <<'UNIT'
[Unit]
Description=High Cube VPN peer sync
After=network-online.target
Wants=network-online.target

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/hc-vpn-sync
UNIT

cat > /etc/systemd/system/hc-vpn-sync.timer <<'UNIT'
[Unit]
Description=High Cube VPN peer sync every 15 seconds

[Timer]
OnBootSec=10s
OnUnitActiveSec=15s
AccuracySec=1s

[Install]
WantedBy=timers.target
UNIT

systemctl daemon-reload
say "Bringing up $IFACE"
/usr/local/sbin/hc-vpn-sync --verbose || die "Could not bring up $IFACE. Check: journalctl -u hc-vpn-sync"
systemctl enable --now hc-vpn-sync.timer >/dev/null 2>&1
ADDRESS=$(awk -F' *= *' '/^Address/ {print $2; exit}' "/etc/wireguard/$IFACE.conf")
say "Done. This machine is $ADDRESS on the High Cube VPN."
wg show "$IFACE" || true
)HCSH";

// Run once per server with sudo. hc-agent usually runs as a regular user, so WireGuard is
// driven through this root-owned helper, which the agent user may call without a password.
constexpr const char* kAgentSetupTemplate = R"HCSH(#!/usr/bin/env bash
# High Cube VPN: lets hc-agent manage WireGuard on this server.
set -euo pipefail

say() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31mError:\033[0m %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "Run this command with sudo."
command -v apt-get >/dev/null 2>&1 || die "Only Ubuntu/Debian (apt) is supported."

if ! command -v wg >/dev/null 2>&1 || ! command -v wg-quick >/dev/null 2>&1; then
  say "Installing WireGuard tools"
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq wireguard-tools iproute2 >/dev/null
fi

say "Installing /usr/local/sbin/hc-vpn-helper"
cat > /usr/local/sbin/hc-vpn-helper <<'HELPER'
#!/usr/bin/env bash
# High Cube VPN helper: the only root entry point hc-agent uses for WireGuard.
# Usage: check | apply <iface> <port> <forward 0|1> <base64 body> | status <iface>
#        | heal <iface> <port> <forward 0|1> | down <iface>
set -uo pipefail
STATE_DIR=/etc/hc-vpn
KEY="$STATE_DIR/private.key"
fail() { echo "ERROR: $*"; exit 1; }

open_firewall() {
  [ "$1" -gt 0 ] 2>/dev/null || return 0
  command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | grep -q '^Status: active' || return 0
  ufw status 2>/dev/null | grep -qE "^$1/udp" && return 0
  ufw allow "$1/udp" comment 'High Cube VPN' >/dev/null 2>&1 && echo "HEALED opened $1/udp in ufw"
}

# Central node: members reach each other through it. Docker or a firewall reload may
# drop the rule, which silently cuts member-to-member traffic.
ensure_forwarding() {
  if [ "$(sysctl -n net.ipv4.ip_forward 2>/dev/null)" != 1 ]; then
    sysctl -q -w net.ipv4.ip_forward=1 && echo "HEALED enabled ip_forward"
  fi
  if ! iptables -C FORWARD -i "$IFACE" -o "$IFACE" -j ACCEPT 2>/dev/null; then
    iptables -I FORWARD 1 -i "$IFACE" -o "$IFACE" -j ACCEPT && echo "HEALED restored the FORWARD rule"
  fi
}

# WireGuard resolves endpoint names once: resolve again when a named peer stops answering.
reresolve() {
  local now pk ep host hs before after
  now=$(date +%s)
  awk '/^\[Peer\]/ {pk = ""} /^PublicKey/ {sub(/^PublicKey *= */, ""); pk = $0}
       /^Endpoint/ {sub(/^Endpoint *= */, ""); if (pk != "") print pk " " $0}' "$CONF" 2>/dev/null |
  while read -r pk ep; do
    host=${ep%:*}
    host=${host#[}
    host=${host%]}
    [[ "$host" =~ ^[0-9.]+$ || "$host" == *:* ]] && continue
    hs=$(wg show "$IFACE" latest-handshakes 2>/dev/null | awk -v k="$pk" '$1 == k {print $2}')
    if [ -z "$hs" ] || [ $((now - hs)) -gt 135 ]; then
      before=$(wg show "$IFACE" endpoints 2>/dev/null | awk -v k="$pk" '$1 == k {print $2}')
      wg set "$IFACE" peer "$pk" endpoint "$ep" 2>/dev/null || continue
      after=$(wg show "$IFACE" endpoints 2>/dev/null | awk -v k="$pk" '$1 == k {print $2}')
      [ "$before" != "$after" ] && echo "HEALED re-resolved $ep: $before -> $after"
    fi
  done
}

ACTION=${1:-}
if [ "$ACTION" = check ]; then echo ok; exit 0; fi
IFACE=${2:-}
[[ "$IFACE" =~ ^[a-z0-9-]{1,15}$ ]] || fail "invalid interface name"
CONF="/etc/wireguard/$IFACE.conf"

case "$ACTION" in
  apply)
    PORT=${3:-}
    FORWARD=${4:-0}
    [[ "$PORT" =~ ^[0-9]{1,5}$ ]] || fail "invalid port"
    [[ "$FORWARD" =~ ^[01]$ ]] || fail "invalid forward flag"
    if ! command -v wg >/dev/null 2>&1 || ! command -v wg-quick >/dev/null 2>&1; then
      export DEBIAN_FRONTEND=noninteractive
      apt-get update -qq >/dev/null 2>&1
      apt-get install -y -qq wireguard-tools >/dev/null 2>&1 || fail "apt-get could not install wireguard-tools"
    fi
    install -d -m 700 "$STATE_DIR" /etc/wireguard
    if [ ! -s "$KEY" ]; then
      (umask 077 && wg genkey > "$KEY") || fail "wg genkey failed"
    fi
    NEW=$(mktemp)
    trap 'rm -f "$NEW"' EXIT
    {
      printf '[Interface]\nPrivateKey = %s\n' "$(cat "$KEY")"
      if [ "$FORWARD" = 1 ]; then
        # Central node: members reach each other through it. Docker sets FORWARD to DROP.
        printf 'PostUp = sysctl -q -w net.ipv4.ip_forward=1\n'
        printf 'PostUp = iptables -I FORWARD 1 -i %%i -o %%i -j ACCEPT\n'
        printf 'PostDown = iptables -D FORWARD -i %%i -o %%i -j ACCEPT\n'
      fi
      # Plain WireGuard keys only: hook lines from the caller would run as root.
      printf '%s' "${5:-}" | base64 -d 2>/dev/null | grep -E \
        '^(\[Peer\]|(Address|ListenPort|MTU|PublicKey|PresharedKey|AllowedIPs|Endpoint|PersistentKeepalive) *= *[][A-Za-z0-9.:/+=, -]*|#.*|)$' || true
    } > "$NEW"
    UP=0
    ip link show "$IFACE" >/dev/null 2>&1 && UP=1
    if [ "$UP" = 0 ] || ! cmp -s "$NEW" "$CONF"; then
      OLD_INTERFACE=$(sed '/^\[Peer\]/,$d' "$CONF" 2>/dev/null || true)
      NEW_INTERFACE=$(sed '/^\[Peer\]/,$d' "$NEW")
      install -m 600 "$NEW" "$CONF"
      if [ "$UP" = 1 ] && [ "$OLD_INTERFACE" = "$NEW_INTERFACE" ]; then
        wg syncconf "$IFACE" <(wg-quick strip "$IFACE") || fail "wg syncconf failed"
      else
        wg-quick down "$IFACE" >/dev/null 2>&1 || true
        OUT=$(wg-quick up "$IFACE" 2>&1) || fail "wg-quick up failed: $(printf '%s' "$OUT" | tail -n 1)"
      fi
    fi
    systemctl enable "wg-quick@$IFACE" >/dev/null 2>&1 || true
    open_firewall "$PORT" >/dev/null
    echo ok
    ;;
  heal)
    PORT=${3:-0}
    FORWARD=${4:-0}
    [[ "$PORT" =~ ^[0-9]{1,5}$ ]] || fail "invalid port"
    [[ "$FORWARD" =~ ^[01]$ ]] || fail "invalid forward flag"
    if [ ! -f "$CONF" ]; then echo ok; exit 0; fi
    if ! ip link show "$IFACE" >/dev/null 2>&1; then
      OUT=$(wg-quick up "$IFACE" 2>&1) || fail "wg-quick up failed: $(printf '%s' "$OUT" | tail -n 1)"
      echo "HEALED brought $IFACE back up"
    fi
    [ "$FORWARD" = 1 ] && ensure_forwarding
    open_firewall "$PORT"
    reresolve
    echo ok
    ;;
  status)
    if [ ! -f "$CONF" ]; then echo "MEMBER 0"; exit 0; fi
    echo "MEMBER 1"
    UP=0
    ip link show "$IFACE" >/dev/null 2>&1 && UP=1
    echo "UP $UP"
    [ -s "$KEY" ] && echo "PUBKEY $(wg pubkey < "$KEY")"
    # Peer lines only: the first dump line holds the private key.
    [ "$UP" = 1 ] && wg show "$IFACE" dump 2>/dev/null | tail -n +2 | sed 's/^/PEER /'
    exit 0
    ;;
  down)
    ip link show "$IFACE" >/dev/null 2>&1 && wg-quick down "$IFACE" >/dev/null 2>&1
    systemctl disable "wg-quick@$IFACE" >/dev/null 2>&1 || true
    rm -f "$CONF"
    rm -rf "$STATE_DIR"
    echo ok
    ;;
  *)
    fail "unknown action"
    ;;
esac
HELPER
chown root:root /usr/local/sbin/hc-vpn-helper
chmod 755 /usr/local/sbin/hc-vpn-helper

# hc-agent runs either as a system unit (User=...) or as a systemd --user unit.
agent_user() {
  local uid unit_file
  uid=$(ps -C hc-agent -o uid= 2>/dev/null | head -n1 | tr -d ' ')
  if [ -n "$uid" ]; then id -nu "$uid"; return; fi
  if [ "$(systemctl show -p LoadState --value hc-agent 2>/dev/null)" = loaded ]; then
    uid=$(systemctl show -p User --value hc-agent 2>/dev/null)
    echo "${uid:-root}"
    return
  fi
  for unit_file in /home/*/.config/systemd/user/hc-agent.service /root/.config/systemd/user/hc-agent.service; do
    if [ -f "$unit_file" ]; then stat -c %U "$unit_file"; return; fi
  done
}

AGENT_USER=$(agent_user || true)
[ -n "$AGENT_USER" ] || die "hc-agent was not found on this server. Install the agent first, then run this command again."

if [ "$AGENT_USER" != root ]; then
  say "Allowing $AGENT_USER (hc-agent) to run the helper"
  RULE=$(mktemp)
  # The agent polls the helper every few seconds: keep those calls out of the auth log.
  printf 'Defaults!/usr/local/sbin/hc-vpn-helper !syslog, !pam_session\n%s ALL=(root) NOPASSWD: /usr/local/sbin/hc-vpn-helper\n' \
    "$AGENT_USER" > "$RULE"
  visudo -cf "$RULE" >/dev/null || { rm -f "$RULE"; die "Invalid sudoers rule"; }
  install -m 440 -o root -g root "$RULE" /etc/sudoers.d/highcube-vpn
  rm -f "$RULE"
  runuser -u "$AGENT_USER" -- sudo -n /usr/local/sbin/hc-vpn-helper check >/dev/null 2>&1 \
    || die "$AGENT_USER still cannot run the helper without a password. Check /etc/sudoers.d/highcube-vpn."
fi

say "Done. hc-agent ($AGENT_USER) joins the VPN within a few seconds."
)HCSH";

void replaceAll(std::string& text, const std::string& from, const std::string& to) {
  for (auto pos = text.find(from); pos != std::string::npos; pos = text.find(from, pos + to.size())) {
    text.replace(pos, from.size(), to);
  }
}

}  // namespace

std::string renderEnrollScript(const std::string& baseUrl, const std::string& token) {
  std::string script = kEnrollTemplate;
  replaceAll(script, "__HC_URL__", baseUrl);
  replaceAll(script, "__HC_TOKEN__", token);
  replaceAll(script, "__HC_IFACE__", kInterfaceName);
  return script;
}

std::string renderAgentSetupScript() {
  return kAgentSetupTemplate;
}

std::string renderExpiredScript() {
  return "#!/usr/bin/env bash\n"
         "echo 'This High Cube VPN command is invalid or expired. Generate a new one from your "
         "profile.' >&2\n"
         "exit 1\n";
}

}  // namespace highcube::admin::vpn

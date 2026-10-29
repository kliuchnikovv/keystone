#!/usr/bin/env bash
# Print the label that goes on the box.
#
#   scripts/make-label.sh                       # read the code from the hub identity
#   scripts/make-label.sh --setup-code ABCD-EFGH-JKMN --name "Living room"
#   scripts/make-label.sh --out label.txt
#
# What is on the label and why:
#   * the setup code — the password of the first-time access point `Keystone-XXXX`
#     and what the app asks for there. It comes from the hub's TLS identity
#     (scripts/gen-hub-cert.sh) and has to be read off the label: a person who
#     has only the box must be able to get the hub onto their network.
#   * the Matter setup code — so the hub can be added to Apple Home.
#
# The QR code is optional: Apple Home accepts the code typed by hand, and the
# app shows it on the Hub tab. If `qrencode` is installed, --qr adds one.
set -euo pipefail

cd "$(dirname "$0")/.."

IDENTITY="firmware/brain-s3/main/hub_identity.cpp"
SETUP_CODE=""
NAME="Keystone hub"
MATTER_CODE="3497-0112-332"
AP_SSID=""
OUT=""
WANT_QR=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --setup-code) SETUP_CODE="${2:-}"; shift 2 ;;
    --name)       NAME="${2:-}"; shift 2 ;;
    --matter-code) MATTER_CODE="${2:-}"; shift 2 ;;
    --ap-ssid)    AP_SSID="${2:-}"; shift 2 ;;
    --out)        OUT="${2:-}"; shift 2 ;;
    --qr)         WANT_QR=1; shift ;;
    -h|--help)    sed -n '2,18p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

if [[ -z "$SETUP_CODE" ]]; then
  if [[ ! -f "$IDENTITY" ]]; then
    echo "No setup code given and $IDENTITY does not exist." >&2
    echo "Generate one: scripts/gen-hub-cert.sh --setup-code" >&2
    exit 2
  fi
  SETUP_CODE=$(sed -n 's/.*kHubSetupCode\[\] = "\(.*\)";.*/\1/p' "$IDENTITY" | head -1)
  if [[ -z "$SETUP_CODE" ]]; then
    echo "Could not read the setup code from $IDENTITY." >&2
    exit 2
  fi
fi

# "abcd-efgh-jkmn" or "ABCDEFGHJKMN" -> "ABCD-EFGH-JKMN"
clean=$(printf '%s' "$SETUP_CODE" | tr -d ' -' | tr '[:lower:]' '[:upper:]')
if [[ ${#clean} -ne 12 ]]; then
  echo "A setup code is 12 characters, got ${#clean}: $SETUP_CODE" >&2
  exit 2
fi
FORMATTED="${clean:0:4}-${clean:4:4}-${clean:8:4}"

if [[ -z "$AP_SSID" ]]; then
  AP_SSID="Keystone-XXXX (the last two bytes of the hub's MAC address)"
fi

render() {
  local w=54
  local rule
  rule=$(printf '─%.0s' $(seq 1 $((w - 2))))
  row() { printf '│%-*s│\n' "$w" " $1"; }
  sep() { printf '│ %s │\n' "$rule"; }
  printf '┌%s┐\n' "$(printf '─%.0s' $(seq 1 "$w"))"
  row "$NAME"
  sep
  row "Setup code"
  row "    $FORMATTED"
  row "(the setup network's password; the app asks for it)"
  sep
  row "Apple Home:  $MATTER_CODE"
  sep
  row "1. Plug the hub in."
  row "2. On the phone, join the Wi-Fi network"
  row "   $AP_SSID"
  row "   with the setup code above."
  row "3. In the app: pick your home Wi-Fi"
  row "   and enter its password."
  row "4. Add the hub to Apple Home with the code above."
  printf '└%s┘\n' "$(printf '─%.0s' $(seq 1 "$w"))"
}

if [[ "$WANT_QR" == "1" ]]; then
  if ! command -v qrencode >/dev/null 2>&1; then
    echo "qrencode is not installed (brew install qrencode) — printing the text label only." >&2
  else
    render
    echo
    echo "QR for Apple Home (the code above is the same):"
    qrencode -t ANSIUTF8 -o - "M:$MATTER_CODE"
    echo
  fi
else
  render
fi

if [[ -n "$OUT" ]]; then
  render > "$OUT"
  echo "written: $OUT" >&2
fi

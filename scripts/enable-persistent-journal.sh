#!/usr/bin/env bash
# Turn on the persistent system journal, so that the log of a boot survives
# the next reboot.
#
# Why: the journal was in /run/log/journal (a tmpfs), so every reboot threw
# the previous boot's log away. On this machine the Pi reboots by itself when
# the build makes it stall (systemd's hardware watchdog: RuntimeWatchdogSec
# 1 min, RebootWatchdogSec 2 min) — and with no journal on disk there was
# nothing to look at afterwards.
#
# Run with sudo:  sudo scripts/enable-persistent-journal.sh
#
# Why a drop-in and not just /etc/systemd/journald.conf:
#
#   journald reads the main config, then the drop-ins; for an option that
#   takes a single value the LAST entry wins. The drop-ins are sorted by
#   FILENAME in lexicographic order — the directory they live in does not
#   matter (/etc, /run, /usr/lib are all read, then sorted by name).
#   Raspberry Pi OS ships
#     /usr/lib/systemd/journald.conf.d/40-rpi-volatile-storage.conf
#   with `Storage=volatile` (package raspberrypi-sys-mods), so editing
#   /etc/systemd/journald.conf alone is silently overridden — the main
#   config has LOWER precedence than any drop-in, so the journal stays in
#   tmpfs and the log of the boot that killed the machine is lost again.
#
#   systemd's own recommendation (journald.conf.d(5)): the vendor ships
#   drop-ins in the 10-40 range, the administrator in 60-90. "60-" therefore
#   sorts after the vendor's "40-rpi-..." and wins.
#
# The script checks the RESULT (where journald actually writes), because a
# silent no-op is worse than no script at all.
set -euo pipefail

CONF=/etc/systemd/journald.conf
DROPIN_DIR=/etc/systemd/journald.conf.d
# 60- is in the range journald.conf.d(5) recommends for the administrator
# (60-90), so it sorts after the vendor's 40-rpi-volatile-storage.conf.
DROPIN="${DROPIN_DIR}/60-keystone-persistent.conf"

if [[ "${EUID}" -ne 0 ]]; then
  echo "run as root: sudo $0" >&2
  exit 1
fi

# ---- 1. the drop-in: Storage=persistent, and it must win over the distribution's ----

mkdir -p "${DROPIN_DIR}"
cat > "${DROPIN}" <<'EOF'
# Keystone: keep the journal on disk.
#
# The system journal used to live in /run/log/journal, a tmpfs, so every reboot
# threw away the log of the boot that just died — which is exactly the log
# needed when the Pi reboots itself under memory pressure (see
# scripts/enable-persistent-journal.sh and docs/roadmap.md, Track C).
#
# This drop-in exists because Raspberry Pi OS ships
# /usr/lib/systemd/journald.conf.d/40-rpi-volatile-storage.conf with
# `Storage=volatile`. The drop-ins are sorted by filename regardless of the
# directory they are in, and the last one wins, so "60-" sorts after the
# vendor's "40-rpi-..." and overrides it. Writing Storage=persistent only in
# the main config (/etc/systemd/journald.conf) has no effect on this image:
# the main config has LOWER precedence than any drop-in.
[Journal]
Storage=persistent
EOF

# The main config keeps the limits, so the journal cannot fill the card.
if [[ -f "${CONF}" ]]; then
  grep -qE '^SystemMaxUse=' "${CONF}" || echo 'SystemMaxUse=200M' >> "${CONF}"
  grep -qE '^MaxRetentionSec=' "${CONF}" || echo 'MaxRetentionSec=1month' >> "${CONF}"
fi

# The directory must exist and belong to the journal user before journald
# starts, or it falls back to the runtime journal.
install -d -m 2755 -o root -g systemd-journal /var/log/journal

# ...and so must the per-machine directory inside it.
#
# journald opens /var/log/journal/<machine-id>/system.journal and, when that
# fails, silently falls back to the runtime journal ("Failed to open system
# journal", which is lost because the log it would go to is the one that is
# missing). It does not create the directory itself: on a normal Debian image
# systemd-tmpfiles does, via
#     z /var/log/journal/%m 2755 root systemd-journal - -
# but "z" only *adjusts* the mode of an existing directory — it never creates
# one. The /run counterpart exists because tmpfiles creates the runtime
# hierarchy early at boot, so the rule there finds the directory already
# there; under /var/log there is nothing to adjust.
#
# So the directory is created here, and the tmpfiles rule keeps its mode and
# owner right afterwards.
machine_id="$(cat /etc/machine-id 2>/dev/null || true)"
if [[ -n "${machine_id}" ]]; then
  install -d -m 2755 -o root -g systemd-journal "/var/log/journal/${machine_id}"
fi

# The directory has to come back after every reboot too, and the stock tmpfiles
# rules do not create it (see above). A "d" rule does — "d" creates, "z" only
# adjusts. So the directory is declared here, in /etc/tmpfiles.d, where it
# survives an update of the systemd package.
mkdir -p /etc/tmpfiles.d
cat > /etc/tmpfiles.d/60-keystone-journal.conf <<EOF
# Keystone: the per-machine journal directory under /var/log/journal.
#
# systemd ships only
#     z /var/log/journal/%m 2755 root systemd-journal - -
# and "z" adjusts the mode of an existing directory — it does not create one.
# journald needs /var/log/journal/<machine-id>/ to open the system journal and
# falls back to the runtime journal (in tmpfs) when it is missing, so without
# this rule the journal never reaches the disk.
#
# "d" creates the directory; the mode and owner are set here and adjusted by
# systemd's own "z" rule afterwards.
d /var/log/journal/${machine_id:-%m} 2755 root systemd-journal - -
EOF

# ---- 2. apply it ----

systemctl restart systemd-journald
sleep 1  # journald needs a moment to open the new journal

# ---- 3. check the RESULT, not the intent ----
#
# `journalctl --header` prints the file the journal is actually in. If it is
# still under /run/log/journal, the setting did not take effect — say so
# instead of printing a cheerful summary.

path="$(journalctl --header 2>/dev/null | sed -n 's/^File path: //p' || true)"
storage="$(systemd-analyze cat-config systemd/journald.conf 2>/dev/null | grep -E '^Storage=' | tail -1 || true)"

echo "effective config: ${storage:-unknown}"
echo "journal file:     ${path:-unknown}"

if [[ "${path}" == /var/log/journal/* ]]; then
  echo
  echo "OK: the journal now lives on disk and survives a reboot."
  echo
  journalctl --list-boots --no-pager | tail -5
  exit 0
fi

echo >&2
echo "FAILED: journald is still writing to ${path:-an unknown place}." >&2
echo "The journal will NOT survive a reboot." >&2
echo >&2
echo "Things to check:" >&2
echo "  - systemd-analyze cat-config systemd/journald.conf | grep Storage" >&2
echo "    (the LAST Storage= wins; it must say 'persistent')" >&2
echo "  - ls -ld /var/log/journal   (must be root:systemd-journal, mode 2755)" >&2
echo "  - ls -ld /var/log/journal/\$(cat /etc/machine-id)" >&2
echo "    (journald opens the system journal HERE and falls back to the runtime" >&2
echo "     journal if it cannot; it does NOT create this directory itself)" >&2
echo "  - ls /usr/lib/systemd/journald.conf.d/   (the distribution's drop-ins)" >&2
exit 1

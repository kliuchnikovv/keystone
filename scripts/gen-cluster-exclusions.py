#!/usr/bin/env python3
"""Regenerate the Matter cluster exclusion list in thread-c6's Matter overlay.

The Matter data model compiles a cluster server for every cluster the build
supports, and the full set does not fit into the 1.92 MB OTA slot of the
ESP32-C6. esp-matter can switch each cluster off with a
`CONFIG_SUPPORT_<X>_CLUSTER=n` symbol — the list of all of them lives in
components/esp_matter/utils/cluster_select/Kconfig.in.

This tool writes "off" for every cluster except the ones the keystone bridge
actually uses, so the list is derived from the vendor's own symbol list and
cannot drift by hand.

Run it inside the esp-matter image (the symbol list lives there):

    docker run --rm -v "$PWD:/project" espressif/esp-matter:release-v1.6_idf_v5.5.5 \
        python3 /project/scripts/gen-cluster-exclusions.py

Outside the image, point ESP_MATTER_PATH at a checkout:

    ESP_MATTER_PATH=~/esp/esp-matter python3 scripts/gen-cluster-exclusions.py
"""

import os
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
OVERLAY = REPO / "firmware" / "thread-c6" / "sdkconfig.defaults.matter"

# The clusters the keystone bridge keeps (SPEC § 6.1 device types):
#   on/off switch and lamp        → On/Off, Groups, Scenes, Identify, Descriptor
#   dimmable / colour lamp        → Level Control, Color Control
#   temperature / humidity / lux  → the three measurement clusters
#   motion / contact              → Occupancy Sensing, Boolean State
#   thermostat                    → Thermostat
#   metering                      → Electrical Power + Electrical Energy
#   every bridged endpoint        → Bridged Device Basic Information
# and the ones a Matter *server* cannot do without:
#   Access Control (CASE), Operational Credentials (fabric table), General
#   Commissioning, Network Commissioning, Administrator Commissioning
#   (the brain's multi-admin flow talks to it), Network Identity Management.
KEEP = {
    "SUPPORT_ACCESS_CONTROL_CLUSTER",
    # esp_matter's client code (esp_matter_client.cpp) reaches into the Binding
    # cluster's manager unconditionally — switching it off breaks the link.
    "SUPPORT_BINDING_CLUSTER",
    "SUPPORT_ADMINISTRATOR_COMMISSIONING_CLUSTER",
    "SUPPORT_BOOLEAN_STATE_CLUSTER",
    "SUPPORT_BRIDGED_DEVICE_BASIC_INFORMATION_CLUSTER",
    "SUPPORT_COLOR_CONTROL_CLUSTER",
    "SUPPORT_DESCRIPTOR_CLUSTER",
    "SUPPORT_ELECTRICAL_ENERGY_MEASUREMENT_CLUSTER",
    "SUPPORT_ELECTRICAL_POWER_MEASUREMENT_CLUSTER",
    "SUPPORT_GENERAL_COMMISSIONING_CLUSTER",
    "SUPPORT_GROUPS_CLUSTER",
    "SUPPORT_ILLUMINANCE_MEASUREMENT_CLUSTER",
    "SUPPORT_IDENTIFY_CLUSTER",
    "SUPPORT_LEVEL_CONTROL_CLUSTER",
    "SUPPORT_NETWORK_COMMISSIONING_CLUSTER",
    "SUPPORT_NETWORK_IDENTITY_MANAGEMENT_CLUSTER",
    "SUPPORT_OCCUPANCY_SENSING_CLUSTER",
    "SUPPORT_ON_OFF_CLUSTER",
    "SUPPORT_OPERATIONAL_CREDENTIALS_CLUSTER",
    "SUPPORT_RELATIVE_HUMIDITY_MEASUREMENT_CLUSTER",
    "SUPPORT_SCENES_CLUSTER",
    "SUPPORT_SWITCH_CLUSTER",
    "SUPPORT_TEMPERATURE_MEASUREMENT_CLUSTER",
    "SUPPORT_THERMOSTAT_CLUSTER",
}

MARKER = "# --- Matter clusters: only the ones the hub needs"

HEADER = (
    MARKER + "\n"
    "# The Matter data model compiles a cluster server for every supported\n"
    "# cluster; the full set does not fit into the 1.92 MB OTA slot. Everything\n"
    "# is switched off except the clusters the hub's bridge needs (SPEC § 6.1):\n"
    "# on/off, level, colour, temperature/humidity/illuminance, occupancy,\n"
    "# boolean state, thermostat, electrical power/energy, bridged device basic\n"
    "# information — plus the clusters a Matter server cannot do without\n"
    "# (access control, operational credentials, commissioning).\n"
    "# Regenerate with scripts/gen-cluster-exclusions.py.\n"
)


def cluster_symbols() -> list[str]:
    root = pathlib.Path(
        os.environ.get("ESP_MATTER_PATH", "/opt/espressif/esp-matter")
    )
    src = root / "components" / "esp_matter" / "utils" / "cluster_select" / "Kconfig.in"
    if not src.exists():
        sys.exit(f"cannot find {src} — set ESP_MATTER_PATH to an esp-matter checkout")
    names = re.findall(r"^config (SUPPORT_[A-Z0-9_]+_CLUSTER)\s*$", src.read_text(), re.M)
    if not names:
        sys.exit(f"no cluster symbols found in {src}")
    return sorted(set(names))


def main() -> None:
    symbols = cluster_symbols()
    unknown = sorted(KEEP - set(symbols))
    if unknown:
        sys.exit(f"these keep-list entries do not exist upstream: {unknown}")
    block = [f"CONFIG_{name}=n" for name in symbols if name not in KEEP]

    text = OVERLAY.read_text()
    head, _, _ = text.partition(MARKER)
    if not head:
        sys.exit(f"{OVERLAY} has no '{MARKER}' marker")
    OVERLAY.write_text(head.rstrip() + "\n\n" + HEADER + "\n".join(block) + "\n")
    print(
        f"{len(symbols)} clusters upstream, {len(KEEP)} kept, "
        f"{len(block)} excluded -> {OVERLAY}"
    )


if __name__ == "__main__":
    main()

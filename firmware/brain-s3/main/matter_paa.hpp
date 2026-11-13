#pragma once

// Matter commissioning on the brain: the trust store for the accessory's
// attestation chain.
//
// When the commissioner takes a device into a fabric it verifies the device's
// DAC against the PAA that signed its PAI. esp-matter's default trust store
// holds only the two hardcoded Chip-Test roots ("Chip-Test-PAA-FFF1-Cert" and
// "Chip-Test-PAA-NoVID-Cert") — no real product is signed by them, so the
// commissioning of the IKEA TIMMERFLOTTE died with "Unable to find PAA" right
// after PASE.
//
// The proper route is the Device Certification List (the commissioner downloads
// the PAA over HTTPS), but that pulls an HTTP client + TLS + cJSON into the
// image and the app partition has ~7 KB left. So we ship the one production PAA
// we actually need, compiled in (487 bytes of DER), behind esp-matter's
// CUSTOM_ATTESTATION_TRUST_STORE hook.
//
// experiment/commissioner-on-s3 — see docs/matter-backlog.md §3.

#ifdef KEYSTONE_HAVE_MATTER

// Install the PAA trust store into the commissioner.
//
// MUST be called before matter_controller_client::setup_commissioner(): that
// is where esp-matter reads the trust store (and builds the attestation
// verifier around it).
void matter_install_paa_trust_store();

#endif  // KEYSTONE_HAVE_MATTER

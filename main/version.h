/**
 * @file version.h
 * @brief Firmware version — keep in sync with the repo root VERSION file.
 *
 * Update with:  python3 tools/bump_version.py X.Y.Z
 * Do not hand-edit FW_VERSION elsewhere; include this header.
 */
#ifndef EK410_VERSION_H_
#define EK410_VERSION_H_

#define EK410_VERSION_MAJOR 1
#define EK410_VERSION_MINOR 2
#define EK410_VERSION_PATCH 1

/** Semantic version string shown in SoftAP / MQTT / getDevInfo. */
#define EK410_VERSION_STRING "1.2.1"

/** Legacy alias used by older call sites. */
#define FW_VERSION EK410_VERSION_STRING

#endif /* EK410_VERSION_H_ */

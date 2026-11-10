/* License: Apache 2.0. See LICENSE file in root directory.
 *
 * Service version of realsense-usbd (DESIGN 13): an integer that grows with every release of the daemon. The daemon
 * reports it in every HELLO reply (RSUSB_KEY_SERVICE_VERSION) and in `realsense-usbd --version`; the installer compares
 * it with the installed one and never replaces a newer service with an older one.
 *
 *   1  the first generation: admitted only the programs of one build (cdhash allowlist) and had no such number
 *   2  shared service: admits every program of the user at the Mac's screen; AllowedCDHashes is an optional restriction
 */
#ifndef RSUSBD_VERSION_H
#define RSUSBD_VERSION_H

#define RSUSBD_SERVICE_VERSION 2

#endif

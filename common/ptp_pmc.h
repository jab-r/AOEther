/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Read the currently-elected PTPv2 grandmaster clock identity by shelling
 * out to linuxptp's `pmc` tool. The `pmc` binary is expected to be
 * installed alongside ptp4l (both ship in the `linuxptp` package on
 * Debian / Ubuntu / Fedora).
 *
 * On success, writes the 8-byte clock identity to `out_bytes` and
 * returns 0. Caller can render it per RFC 7273 §4.4 as
 * "hh-hh-hh-hh-hh-hh-hh-hh".
 *
 * Returns -1 on any failure (pmc not installed, ptp4l not running, no
 * master elected yet, parse failure). Callers should fall back to the
 * :traceable form of a=ts-refclk.
 *
 * This is a blocking helper — typical runtime is <50 ms — but it forks
 * so don't call it from tight loops. The talker only invokes it at
 * startup and on the SAP-refresh timer (~30 s). */
int ptp_pmc_read_gmid(uint8_t out_bytes[8]);

/* Render an 8-byte clock identity as "hh-hh-hh-hh-hh-hh-hh-hh" (uppercase
 * hex, RFC 7273 style) into `out`. `cap` must be ≥ 24. */
void ptp_gmid_to_str(const uint8_t bytes[8], char *out, size_t cap);

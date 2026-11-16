// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_rx_ppm — the one conversion from the slope estimate (ppm, a double) to the
 * published ppm_error_milli (ppm * 1000, an int). Pure, header-only, no allocation,
 * no logging: update_ppm runs on the feeder thread and a test drives this same
 * function. */
#ifndef REAC_RX_PPM_H
#define REAC_RX_PPM_H

static inline int reac_rx_ppm_to_milli(double ppm)
{
	return (int)(ppm * 1000.0);
}

#endif /* REAC_RX_PPM_H */

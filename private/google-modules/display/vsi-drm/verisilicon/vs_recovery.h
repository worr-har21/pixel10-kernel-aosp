/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _VS_RECOVERY_H_
#define _VS_RECOVERY_H_

#include <linux/types.h>
#include <linux/workqueue.h>

struct vs_crtc;

/**
 * struct vs_recovery - Information relating to ESD recovery
 * @work: reference to handler for recovery operation
 * @count: how many times recovery has triggered
 * @recovering: whether recovery operation currently active
 */
struct vs_recovery {
	struct work_struct work;
	int count;
	atomic_t recovering;
};

/**
 * vs_recovery_register() - registers recovery handler
 * @vs_crtc: crtc object to attach recovery to
 */
void vs_recovery_register(struct vs_crtc *vs_crtc);

/**
 * vs_crtc_trigger_recovery() - Triggers crtc recovery
 * @vs_crtc: crtc object to trigger recovery for
 */
void vs_crtc_trigger_recovery(struct vs_crtc *vs_crtc);

#endif // _VS_RECOVERY_H_

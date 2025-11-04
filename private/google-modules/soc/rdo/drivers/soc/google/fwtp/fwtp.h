/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2024 Google LLC.
 *
 * Google firmware tracepoint services header.
 */

#ifndef __FWTP_H
#define __FWTP_H

#include "fwtp_ipc_client.h"

#define FWTP_PRINTER_BUFFER_SIZE 128
/**
 * struct fwtp_dev - Structure representing an FWTP kernel device.
 *
 * @fwtp_ipc_client: FWTP IPC client record.
 * @dev: Kernel device record.
 * @printer_ctx: Tracepoint printer context.
 * @printer_buffer: Buffer used for printing tracepoints. It should be big
 *                  enough to hold one line of output.
 */
struct fwtp_dev {
	struct fwtp_ipc_client fwtp_ipc_client;
	struct device *dev;
	struct fwtp_printer_ctx printer_ctx;
	char printer_buffer[FWTP_PRINTER_BUFFER_SIZE];
};

int fwtp_dev_init(struct fwtp_dev *fwtp_dev);
void fwtp_dev_deinit(struct fwtp_dev *fwtp_dev);

#endif /* __FWTP_H */

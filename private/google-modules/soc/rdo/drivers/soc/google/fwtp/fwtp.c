// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2024 Google LLC.
 *
 * Google firmware tracepoint services source.
 */

#include "fwtp.h"
#define CREATE_TRACE_POINTS
#include "fwtp_ftrace.h"

/*******************************************************************************
 * Internal FWTP kernel device services.
 ******************************************************************************/

/**
 * fwtp_dev_append_output - Appends string to kernel log and ftrace.
 *
 * @printer_ctx: Printer context.
 * @str: String to append to output.
 */
static void fwtp_dev_append_output(struct fwtp_printer_ctx *printer_ctx,
				   const char *str)
{
	struct fwtp_dev *fwtp_dev = printer_ctx->append_output_ctx;

	dev_info(fwtp_dev->dev, "%s", str);
	trace_fwtp(str);
}

/*******************************************************************************
 * External FWTP kernel device services.
 ******************************************************************************/

/**
 * fwtp_dev_init - Initializes an FWTP kernel device.
 *
 * @fwtp_dev: FWTP kernel device to initialize.
 *
 * Return: 0 on success, non-zero error code on error.
 */
int fwtp_dev_init(struct fwtp_dev *fwtp_dev)
{
	/*
	 * Set up printer context. Tracepoint lines shouldn't have new-lines
	 * with ftrace.
	 */
	fwtp_dev->printer_ctx.append_output = fwtp_dev_append_output;
	fwtp_dev->printer_ctx.append_output_ctx = fwtp_dev;
	fwtp_dev->printer_ctx.output_buffer = fwtp_dev->printer_buffer;
	fwtp_dev->printer_ctx.output_buffer_size =
		sizeof(fwtp_dev->printer_buffer);
	fwtp_dev->printer_ctx.dont_append_new_line = true;

	/* Set up the FWTP interface platform. */
	fwtp_dev->fwtp_ipc_client.fwtp_if.platform.dev = fwtp_dev->dev;

	/* Register the FWTP interface. */
	return fwtp_ipc_client_register(&(fwtp_dev->fwtp_ipc_client));
}
EXPORT_SYMBOL_GPL(fwtp_dev_init);

/**
 * fwtp_dev_deinit - Deinitializes an FWTP kernel device.
 *
 * @fwtp_dev: FWTP kernel device to deinitialize.
 */
void fwtp_dev_deinit(struct fwtp_dev *fwtp_dev)
{
	/* Unregister the FWTP interface. */
	fwtp_ipc_client_unregister(&(fwtp_dev->fwtp_ipc_client));
}
EXPORT_SYMBOL_GPL(fwtp_dev_deinit);

/* Module info. */
MODULE_AUTHOR("Google LLC");
MODULE_DESCRIPTION("Google firmware tracepoint");
MODULE_LICENSE("GPL");

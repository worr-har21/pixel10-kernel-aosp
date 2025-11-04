/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Copyright 2024 Google LLC.
 *
 * Google firmware tracepoint IPC client services header.
 *
 * This file is copied from the Pixel firmware sources to the Linux kernel
 * sources, so it's written to be compiled under both Linux and the firmware,
 * and it's licensed under GPL or MIT licenses.
 */

#ifndef __FWTP_IPC_CLIENT_H
#define __FWTP_IPC_CLIENT_H

#ifdef __KERNEL__
#include "fwtp_if.h"
#include "fwtp_printer.h"
#else
#include "lib/fwtp/fwtp_if.h"
#include "lib/fwtp/fwtp_printer.h"
#endif

__BEGIN_CDECLS

/* Size of large FWTP IPC client messages. */
#define FWTP_IPC_CLIENT_LARGE_MSG_SIZE 4096
/* Maximum size of an FWTP string table. */
#define FWTP_MAX_STRING_TABLE_SIZE (1024 * 1024)

/**
 * struct fwtp_ipc_client - Structure representing an FWTP IPC client.
 *
 * @fwtp_if: FWTP IPC interface.
 * @string_table: Table of tracepoint strings.
 * @string_table_offset: Offset of start of string table.
 * @string_table_size: Size of string table.
 * @ring_num: Tracepoint ring number.
 * @tracepoints_head_offset: Current tracepoint ring head offset.
 */
struct fwtp_ipc_client {
	struct fwtp_if fwtp_if;
	char *string_table;
	uint32_t string_table_offset;
	int string_table_size;
	uint16_t ring_num;
	uint32_t tracepoints_head_offset;
};

fwtp_error_code_t
fwtp_ipc_client_register(struct fwtp_ipc_client *fwtp_ipc_client);
void fwtp_ipc_client_unregister(struct fwtp_ipc_client *fwtp_ipc_client);
fwtp_error_code_t
fwtp_ipc_client_print_tracepoints(struct fwtp_ipc_client *fwtp_ipc_client,
				  struct fwtp_printer_ctx *printer_ctx);

/**
 * FWTP_IPC_CLIENT_LOG_ERR - Logs an error message for an FWTP IPC client.
 *
 * @fwtp_ipc_client: FWTP IPC client for which to log message.
 * @msg: Message to log.
 * @...: Arguments for message.
 */
#define FWTP_IPC_CLIENT_LOG_ERR(fwtp_ipc_client, msg, ...) \
	FWTP_IF_LOG_ERR(&((fwtp_ipc_client)->fwtp_if), msg, ##__VA_ARGS__)

/**
 * FWTP_IPC_CLIENT_LOG_WARN - Logs a warning message for an FWTP IPC client.
 *
 * @fwtp_ipc_client: FWTP IPC client for which to log message.
 * @msg: Message to log.
 * @...: Arguments for message.
 */
#define FWTP_IPC_CLIENT_LOG_WARN(fwtp_ipc_client, msg, ...) \
	FWTP_IF_LOG_WARN(&((fwtp_ipc_client)->fwtp_if), msg, ##__VA_ARGS__)

__END_CDECLS

#endif /* __FWTP_IPC_CLIENT_H */

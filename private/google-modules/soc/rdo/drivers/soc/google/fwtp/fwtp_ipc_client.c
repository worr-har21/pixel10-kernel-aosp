// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Copyright 2024 Google LLC.
 *
 * Google firmware tracepoint IPC client services source.
 *
 * This file is copied from the Pixel firmware sources to the Linux kernel
 * sources, so it's written to be compiled under both Linux and the firmware,
 * and it's licensed under GPL or MIT licenses.
 */

#ifdef __KERNEL__
// clang-format off: Self include should come first.
#include "fwtp_ipc_client.h"
// clang-format on

#include "fwtp_protocol.h"
#else
// clang-format off: Self include should come first.
#include "lib/fwtp/fwtp_ipc_client.h"
// clang-format on

#include <interfaces/protocols/mba/fwtp/fwtp_protocol.h>
#include <string.h>
#endif

/*******************************************************************************
 * Internal FWTP interface services.
 ******************************************************************************/

/**
 * fwtp_ipc_client_get_string_table - Gets the FWTP string table.
 *
 * @fwtp_ipc_client: The FWTP IPC client for which to get the string table.
 *
 * Return: kFwtpOk on success, non-zero error code on error.
 */
static fwtp_error_code_t
fwtp_ipc_client_get_string_table(struct fwtp_ipc_client *fwtp_ipc_client)
{
	struct fwtp_msg_get_strings *msg_get_strings = NULL;
	char *string_table = NULL;
	uint16_t chunk_offset = 0;
	uint16_t chunk_size;
	uint32_t table_size;
	uint16_t rx_msg_data_size;
	fwtp_error_code_t err;

	/* Allocate an FWTP get strings table message buffer. */
	msg_get_strings = FWTP_MALLOC(FWTP_IPC_CLIENT_LARGE_MSG_SIZE);
	if (!msg_get_strings) {
		err = kFwtpErrMemAlloc;
		goto out;
	}

	/*
	 * Get the string table in chunks until the entire table is retrieved.
	 */
	do {
		/* Get the next chunk. */
		memset(msg_get_strings, 0, sizeof(*msg_get_strings));
		msg_get_strings->base.type = kFwtpMsgTypeGetStrings;
		msg_get_strings->chunk_offset = chunk_offset;
		msg_get_strings->chunk_size =
			FWTP_IPC_CLIENT_LARGE_MSG_SIZE -
			sizeof(struct fwtp_msg_get_strings);
		err = fwtp_ipc_client->fwtp_if
			      .send_message(&(fwtp_ipc_client->fwtp_if),
					    msg_get_strings,
					    FWTP_IPC_CLIENT_LARGE_MSG_SIZE,
					    sizeof(struct fwtp_msg_get_strings),
					    &rx_msg_data_size);
		if (err == kFwtpOk)
			err = msg_get_strings->base.error;
		if (err != kFwtpOk) {
			FWTP_IPC_CLIENT_LOG_ERR(
				fwtp_ipc_client,
				"Get string table request failed with error %d.\n",
				err);
			goto out;
		}

		/* Get the string table offset. */
		fwtp_ipc_client->string_table_offset =
			msg_get_strings->string_table_offset;

		/* Allocate the string table if needed. */
		if (!string_table) {
			table_size = msg_get_strings->table_size;
			if (table_size == 0) {
				FWTP_IPC_CLIENT_LOG_ERR(
					fwtp_ipc_client,
					"String table is empty.\n");
				err = kFwtpErrNotFound;
				goto out;
			}
			if (table_size > FWTP_MAX_STRING_TABLE_SIZE) {
				FWTP_IPC_CLIENT_LOG_ERR(fwtp_ipc_client,
							"Table size %d too big.\n",
							table_size);
				err = kFwtpErrTooBig;
				goto out;
			}
			string_table = FWTP_MALLOC(table_size);
			if (!string_table) {
				err = kFwtpErrMemAlloc;
				goto out;
			}
		}

		/* Add the next chunk to the table. */
		chunk_size = msg_get_strings->chunk_size;
		if (chunk_size > (table_size - chunk_offset)) {
			FWTP_IPC_CLIENT_LOG_WARN(
				fwtp_ipc_client,
				"Last chunk ran past end of string table.\n");
			chunk_size = table_size - chunk_offset;
		}
		memcpy(string_table + chunk_offset, msg_get_strings->chunk,
		       chunk_size);
		chunk_offset += chunk_size;
	} while (chunk_offset < table_size);

out:
	/*
	 * On success, move string table to FWTP interface, ensuring it's null
	 * terminated.
	 */
	if (err == kFwtpOk) {
		string_table[table_size - 1] = '\0';
		fwtp_ipc_client->string_table = string_table;
		fwtp_ipc_client->string_table_size = table_size;
	} else {
		FWTP_FREE(string_table);
	}

	/* Clean up. */
	FWTP_FREE(msg_get_strings);

	return err;
}

/**
 * fwtp_ipc_client_get_string - Returns the string corresponding to a string ID.
 *
 * @printer_ctx: Printer context.
 * @string_id: ID of string to get.
 *
 * Return: String corresponding to the string ID.
 */
static const char *
fwtp_ipc_client_get_string(struct fwtp_printer_ctx *printer_ctx,
			   uint32_t string_id)
{
	struct fwtp_ipc_client *fwtp_ipc_client = printer_ctx->get_string_ctx;

	return fwtp_ipc_client->string_table + string_id -
	       fwtp_ipc_client->string_table_offset;
}

/*******************************************************************************
 * External FWTP interface services.
 ******************************************************************************/

/**
 * fwtp_ipc_client_register - Registers an FWTP IPC client.
 *
 * @fwtp_ipc_client: FWTP IPC client to register.
 *
 * Return: kFwtpOk on success, non-zero error code on error.
 */
fwtp_error_code_t
fwtp_ipc_client_register(struct fwtp_ipc_client *fwtp_ipc_client)
{
	struct fwtp_msg_version msg_version = { 0 };
	uint16_t rx_msg_data_size;
	fwtp_error_code_t err;

	/* Exchange protocol versions. */
	msg_version.base.type = kFwtpMsgTypeExchangeVersion;
	msg_version.version = FWTP_PROTOCOL_VERSION;
	err = fwtp_ipc_client->fwtp_if
		      .send_message(&(fwtp_ipc_client->fwtp_if), &msg_version,
				    sizeof(struct fwtp_msg_version),
				    sizeof(struct fwtp_msg_version),
				    &rx_msg_data_size);
	if (err == kFwtpOk)
		err = msg_version.base.error;
	if (err != kFwtpOk) {
		FWTP_IPC_CLIENT_LOG_ERR(
			fwtp_ipc_client,
			"Exchange protocol versions request failed with error %d.\n",
			err);
		goto out;
	}
	fwtp_ipc_client->fwtp_if.peer_protocol_version = msg_version.version;

	/* Get the FWTP string table. */
	err = fwtp_ipc_client_get_string_table(fwtp_ipc_client);
	if (err != kFwtpOk) {
		FWTP_IPC_CLIENT_LOG_ERR(
			fwtp_ipc_client,
			"Failed to get string table with error %d.\n", err);
		goto out;
	}

out:
	/* Clean up on error. */
	if (err != kFwtpOk)
		fwtp_ipc_client_unregister(fwtp_ipc_client);

	return err;
}
EXPORT_SYMBOL_GPL(fwtp_ipc_client_register);

/**
 * fwtp_ipc_client_unregister - Unregisters an FWTP IPC client.
 *
 * @fwtp_ipc_client: FWTP IPC client to unregister.
 */
void fwtp_ipc_client_unregister(struct fwtp_ipc_client *fwtp_ipc_client)
{
	FWTP_FREE(fwtp_ipc_client->string_table);
	fwtp_ipc_client->string_table = NULL;
}
EXPORT_SYMBOL_GPL(fwtp_ipc_client_unregister);

/**
 * fwtp_ipc_client_print_tracepoints - Prints the FWTP tracepoints.
 *
 * Sets the get_string and get_string_ctx fields in the printer context and
 * updates the timestamp_hz field.
 *
 * @fwtp_ipc_client: The FWTP IPC client for which to print the tracepoints.
 * @printer_ctx: Context to use for printing.
 *
 * Return: kFwtpOk on success, non-zero error code on error.
 */
fwtp_error_code_t
fwtp_ipc_client_print_tracepoints(struct fwtp_ipc_client *fwtp_ipc_client,
				  struct fwtp_printer_ctx *printer_ctx)
{
	struct fwtp_msg_get_tracepoints *msg_get_tracepoints;
	uint16_t rx_msg_data_size;
	fwtp_error_code_t err;

	/* Set up printer context. */
	printer_ctx->get_string = fwtp_ipc_client_get_string;
	printer_ctx->get_string_ctx = fwtp_ipc_client;

	/* Allocate an FWTP get tracepoints message buffer. */
	msg_get_tracepoints = FWTP_MALLOC(FWTP_IPC_CLIENT_LARGE_MSG_SIZE);
	if (!msg_get_tracepoints) {
		err = kFwtpErrMemAlloc;
		goto out;
	}

	/* Get tracepoints until all of them are retrieved. */
	do {
		/* Get the tracepoints. */
		memset(msg_get_tracepoints, 0, sizeof(*msg_get_tracepoints));
		msg_get_tracepoints->base.type = kFwtpMsgTypeGetTracepoints;
		msg_get_tracepoints->head_offset =
			fwtp_ipc_client->tracepoints_head_offset;
		msg_get_tracepoints->ring_num = fwtp_ipc_client->ring_num;
		err = fwtp_ipc_client->fwtp_if.send_message(
			&(fwtp_ipc_client->fwtp_if), msg_get_tracepoints,
			FWTP_IPC_CLIENT_LARGE_MSG_SIZE,
			sizeof(struct fwtp_msg_get_tracepoints),
			&rx_msg_data_size);
		if (err == kFwtpOk)
			err = msg_get_tracepoints->base.error;
		if (err != kFwtpOk) {
			FWTP_IPC_CLIENT_LOG_ERR(
				fwtp_ipc_client,
				"Get tracepoints request failed with error %d.\n",
				err);
			goto out;
		}
		if (rx_msg_data_size <
		    (sizeof(struct fwtp_msg_get_tracepoints) +
		     msg_get_tracepoints->tracepoints_size)) {
			FWTP_IPC_CLIENT_LOG_ERR(
				fwtp_ipc_client,
				"Received message size %d too short for tracepoints of size %d.\n",
				rx_msg_data_size,
				msg_get_tracepoints->tracepoints_size);
			err = kFwtpErrBadMsg;
			goto out;
		}
		fwtp_ipc_client->tracepoints_head_offset =
			msg_get_tracepoints->head_offset;
		printer_ctx->timestamp_hz = msg_get_tracepoints->timestamp_hz;
		if (msg_get_tracepoints->tracepoints_size == 0)
			break;

		/* Print tracepoints. */
		fwtp_print_entries(printer_ctx,
				   (uint64_t *)msg_get_tracepoints->tracepoints,
				   msg_get_tracepoints->tracepoints_size /
					   sizeof(uint64_t));
	} while (msg_get_tracepoints->head_offset !=
		 msg_get_tracepoints->tail_offset);

out:
	/* Clean up. */
	FWTP_FREE(msg_get_tracepoints);

	return err;
}
EXPORT_SYMBOL_GPL(fwtp_ipc_client_print_tracepoints);

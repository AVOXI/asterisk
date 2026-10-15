/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2026, AVOXI
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

#ifndef ASTERISK_RECORDING_FORK_H
#define ASTERISK_RECORDING_FORK_H

/*!
 * \file
 * \brief In-dialog recording fork, provided by res_pjsip_recording_fork.
 *
 * res_ari_channels calls this through the optional API. When that module is
 * not loaded, the stub answers HTTP 501 and the rest of ARI channels still loads.
 */

#include "asterisk/ari.h"
#include "asterisk/optional_api.h"

/*! Fields already checked by the ARI handler. Strings remain valid for the call. */
struct ast_recording_fork_params {
	const char *channel_id;
	int start;
	const char *org_id;
	const char *call_id;
	const char *sent_host;
	int sent_port;
	const char *heard_host;
	int heard_port;
	const char *codec;
	int timeout_ms;
};

AST_OPTIONAL_API(void, ast_recording_fork_run,
	(const struct ast_recording_fork_params *params, struct ast_ari_response *response),
	{
		(void) params;
		ast_ari_response_error(response, 501, "Not Implemented",
			"Recording fork requires res_pjsip_recording_fork");
	});

#endif /* ASTERISK_RECORDING_FORK_H */

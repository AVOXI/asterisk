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
#include "asterisk/json.h"
#include "asterisk/optional_api.h"

/*!
 * \brief Validate and send an in-dialog recording fork.
 *
 * The JSON body is the ARI request object. On failure the response is filled
 * and no SIP message is sent. String pointers parsed from body remain valid
 * only for this call.
 */
AST_OPTIONAL_API(void, ast_recording_fork_run,
	(const char *channel_id, struct ast_json *body, struct ast_ari_response *response),
	{
		(void) channel_id;
		(void) body;
		ast_ari_response_error(response, 501, "Not Implemented",
			"Recording fork requires res_pjsip_recording_fork");
	});

#endif /* ASTERISK_RECORDING_FORK_H */

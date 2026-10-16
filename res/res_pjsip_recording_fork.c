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

/*** MODULEINFO
	<depend>pjproject</depend>
	<depend>res_pjsip</depend>
	<depend>res_pjsip_session</depend>
	<depend>res_ari</depend>
	<support_level>core</support_level>
 ***/

#include "asterisk.h"

#include <pjsip.h>
#include <pjsip_ua.h>

#include "asterisk/res_pjsip.h"
#include "asterisk/res_pjsip_session.h"
#include "asterisk/module.h"
#include "asterisk/channel.h"
#include "asterisk/logger.h"

#define AST_API_MODULE
#include "asterisk/recording_fork.h"

enum recording_fork_send_result {
	RECORDING_FORK_SEND_OK = 0,
	RECORDING_FORK_SEND_NO_DIALOG,
	RECORDING_FORK_SEND_FAILED,
};

/*!
 * \brief Shared by the ARI worker and the PJSIP transaction callback.
 *
 * The ARI worker holds one reference. A second reference is handed to the
 * transaction and released from on_tsx_state at a terminal state, including
 * when that callback runs after the ARI wait has already timed out.
 */
struct recording_fork_request {
	ast_mutex_t lock;
	ast_cond_t cond;
	int done;
	int signaled;
	int tsx_ref_released;
	int sip_status;
};

static pjsip_module recording_fork_module;

struct recording_fork_send {
	struct ast_sip_session *session;
	struct recording_fork_request *req;
	const char *body;
	enum recording_fork_send_result result;
};

static void recording_fork_request_dtor(void *obj)
{
	struct recording_fork_request *req = obj;

	ast_mutex_destroy(&req->lock);
	ast_cond_destroy(&req->cond);
}

static void recording_fork_release_tsx_ref(struct recording_fork_request *req, pjsip_transaction *tsx)
{
	int release = 0;

	ast_mutex_lock(&req->lock);
	if (!req->tsx_ref_released) {
		req->tsx_ref_released = 1;
		release = 1;
		if (tsx && recording_fork_module.id >= 0) {
			tsx->mod_data[recording_fork_module.id] = NULL;
		}
	}
	ast_mutex_unlock(&req->lock);
	if (release) {
		ao2_ref(req, -1);
	}
}

static void recording_fork_on_tsx_state(pjsip_transaction *tsx, pjsip_event *event)
{
	struct recording_fork_request *req;
	int terminal;

	(void) event;

	if (recording_fork_module.id < 0) {
		return;
	}
	req = tsx->mod_data[recording_fork_module.id];
	if (!req) {
		return;
	}

	terminal = tsx->state == PJSIP_TSX_STATE_COMPLETED
		|| tsx->state == PJSIP_TSX_STATE_TERMINATED
		|| tsx->state == PJSIP_TSX_STATE_DESTROYED;
	if (!terminal) {
		return;
	}

	ast_mutex_lock(&req->lock);
	if (!req->signaled && tsx->status_code >= 200) {
		req->sip_status = tsx->status_code;
		req->done = 1;
		req->signaled = 1;
		ast_cond_signal(&req->cond);
	} else if (!req->signaled &&
		(tsx->state == PJSIP_TSX_STATE_TERMINATED || tsx->state == PJSIP_TSX_STATE_DESTROYED)) {
		if (tsx->status_code > 0) {
			req->sip_status = tsx->status_code;
		}
		req->done = 1;
		req->signaled = 1;
		ast_cond_signal(&req->cond);
	}
	ast_mutex_unlock(&req->lock);

	if (tsx->state == PJSIP_TSX_STATE_TERMINATED || tsx->state == PJSIP_TSX_STATE_DESTROYED) {
		recording_fork_release_tsx_ref(req, tsx);
	}
}

static pjsip_module recording_fork_module = {
	.name = {"Recording Fork", 14},
	.id = -1,
	.priority = PJSIP_MOD_PRIORITY_APPLICATION,
	.on_tsx_state = recording_fork_on_tsx_state,
};

static int recording_fork_send_task(void *data)
{
	struct recording_fork_send *send = data;
	pjsip_dialog *dlg;
	pjsip_tx_data *tdata = NULL;
	pj_status_t status;

	send->result = RECORDING_FORK_SEND_FAILED;
	/* Dialog state is only read on the session serializer. A disconnected
	 * INVITE session can still hold a dialog object; that is not a live call.
	 */
	if (!send->session->inv_session || !send->session->inv_session->dlg
		|| send->session->inv_session->state == PJSIP_INV_STATE_DISCONNECTED) {
		send->result = RECORDING_FORK_SEND_NO_DIALOG;
		return 0;
	}
	if (recording_fork_module.id < 0) {
		return 0;
	}

	dlg = send->session->inv_session->dlg;
	/* pjsip only delivers on_tsx_state to the transaction user (mod_ua for
	 * dialog requests), which forwards it to the dialog's usages. Become a
	 * usage of this dialog once so the MESSAGE transaction reaches us. */
	if (!pjsip_dlg_has_usage(dlg, &recording_fork_module)
		&& pjsip_dlg_add_usage(dlg, &recording_fork_module, NULL) != PJ_SUCCESS) {
		ast_log(LOG_WARNING, "Failed to add recording fork usage to dialog\n");
		return 0;
	}
	if (ast_sip_create_request("MESSAGE", dlg, send->session->endpoint, NULL, NULL, &tdata) || !tdata) {
		return 0;
	}
	if (ast_sip_add_body(tdata, &(struct ast_sip_body){
		.type = "text",
		.subtype = "plain",
		.body_text = send->body,
	})) {
		pjsip_tx_data_dec_ref(tdata);
		return 0;
	}

	/* Opt out before send, so a 401/407/494 or a Reason header cannot
	 * resend this MESSAGE or change the call. */
	if (ast_sip_session_suppress_response_side_effects(tdata)) {
		pjsip_tx_data_dec_ref(tdata);
		return 0;
	}

	/* Transaction reference. Released from on_tsx_state, or here if no
	 * transaction was created. The flag is claimed under req->lock so this
	 * path and the callback cannot both drop it. pjsip_dlg_send_request
	 * consumes tdata. */
	ao2_ref(send->req, +1);
	status = pjsip_dlg_send_request(dlg, tdata, recording_fork_module.id, send->req);
	if (status != PJ_SUCCESS) {
		recording_fork_release_tsx_ref(send->req, NULL);
		ast_log(LOG_WARNING, "Failed to send in-dialog recording fork MESSAGE: %d\n", (int) status);
		return 0;
	}

	send->result = RECORDING_FORK_SEND_OK;
	return 0;
}

static void recording_fork_respond(struct ast_ari_response *response, int timed_out, int sip_status)
{
	struct ast_json *msg;

	if (timed_out) {
		ast_ari_response_error(response, 504, "Gateway Timeout",
			"No final response to the recording fork MESSAGE");
		return;
	}
	if (sip_status >= 200 && sip_status < 300) {
		msg = ast_json_pack("{s:i}", "sipStatus", sip_status);
		if (!msg) {
			ast_ari_response_alloc_failed(response);
			return;
		}
		ast_ari_response_ok(response, msg);
		return;
	}
	if (sip_status >= 300) {
		msg = ast_json_pack("{s:i}", "sipStatus", sip_status);
		if (!msg) {
			ast_ari_response_alloc_failed(response);
			return;
		}
		response->message = msg;
		response->response_code = 500;
		response->response_text = "Internal Server Error";
		return;
	}
	ast_ari_response_error(response, 500, "Internal Server Error",
		"Failed to send the recording fork MESSAGE");
}

static void recording_fork_deadline(struct timespec *ts, int timeout_ms)
{
	clock_gettime(CLOCK_REALTIME, ts);
	ts->tv_sec += timeout_ms / 1000;
	ts->tv_nsec += (long) (timeout_ms % 1000) * 1000000L;
	if (ts->tv_nsec >= 1000000000L) {
		ts->tv_sec += 1;
		ts->tv_nsec -= 1000000000L;
	}
}

void AST_OPTIONAL_API_NAME(ast_recording_fork_run)(
	const struct ast_recording_fork_params *params,
	struct ast_ari_response *response)
{
	RAII_VAR(struct ast_channel *, chan, NULL, ast_channel_cleanup);
	RAII_VAR(struct ast_sip_session *, session, NULL, ao2_cleanup);
	RAII_VAR(struct recording_fork_request *, req, NULL, ao2_cleanup);
	RAII_VAR(char *, body, NULL, ast_free);
	const struct ast_channel_tech *tech;
	struct ast_sip_channel_pvt *pvt;
	struct recording_fork_send send;
	struct timespec deadline;
	int done;
	int sip_status;

	if (!params || !response) {
		return;
	}

	chan = ast_channel_get_by_name(params->channel_id);
	if (!chan) {
		ast_ari_response_error(response, 404, "Not Found", "Channel not found");
		return;
	}

	ast_channel_lock(chan);
	tech = ast_channel_tech(chan);
	pvt = NULL;
	if (tech && !strcmp(tech->type, "PJSIP")) {
		pvt = ast_channel_tech_pvt(chan);
		if (pvt && pvt->session) {
			session = ao2_bump(pvt->session);
		}
	}
	ast_channel_unlock(chan);
	if (!session) {
		ast_ari_response_error(response, 409, "Conflict",
			"Channel is not a live PJSIP session");
		return;
	}

	if (params->start) {
		if (ast_asprintf(&body, "FORK_START;%s;%s;%s:%d;%s:%d;%s",
			params->org_id, params->call_id, params->sent_host, params->sent_port,
			params->heard_host, params->heard_port, params->codec) < 0) {
			ast_ari_response_alloc_failed(response);
			return;
		}
	} else if (ast_asprintf(&body, "FORK_STOP;%s;%s", params->org_id, params->call_id) < 0) {
		ast_ari_response_alloc_failed(response);
		return;
	}

	req = ao2_alloc(sizeof(*req), recording_fork_request_dtor);
	if (!req) {
		ast_ari_response_alloc_failed(response);
		return;
	}
	ast_mutex_init(&req->lock);
	ast_cond_init(&req->cond, NULL);

	send.session = session;
	send.req = req;
	send.body = body;
	send.result = RECORDING_FORK_SEND_FAILED;
	if (ast_sip_push_task_synchronous(session->serializer, recording_fork_send_task, &send)) {
		ast_ari_response_error(response, 500, "Internal Server Error",
			"Failed to send the recording fork MESSAGE");
		return;
	}
	if (send.result == RECORDING_FORK_SEND_NO_DIALOG) {
		ast_ari_response_error(response, 409, "Conflict",
			"Channel is not a live PJSIP session");
		return;
	}
	if (send.result != RECORDING_FORK_SEND_OK) {
		recording_fork_respond(response, 0, 0);
		return;
	}

	recording_fork_deadline(&deadline, params->timeout_ms);
	ast_mutex_lock(&req->lock);
	while (!req->done) {
		if (ast_cond_timedwait(&req->cond, &req->lock, &deadline) == ETIMEDOUT) {
			break;
		}
	}
	done = req->done;
	sip_status = req->sip_status;
	ast_mutex_unlock(&req->lock);

	recording_fork_respond(response, !done, sip_status);
}

static int load_module(void)
{
	if (ast_sip_register_service(&recording_fork_module)) {
		ast_log(LOG_ERROR, "Failed to register recording fork PJSIP module\n");
		return AST_MODULE_LOAD_DECLINE;
	}
	return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	if (recording_fork_module.id >= 0) {
		ast_sip_unregister_service(&recording_fork_module);
	}
	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER, "PJSIP in-dialog recording fork",
	.support_level = AST_MODULE_SUPPORT_CORE,
	.load = load_module,
	.unload = unload_module,
	.load_pri = AST_MODPRI_APP_DEPEND,
	.requires = "res_pjsip,res_pjsip_session,res_ari",
);

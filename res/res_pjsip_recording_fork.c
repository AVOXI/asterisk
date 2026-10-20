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

#include <stdint.h>

#include "asterisk/res_pjsip.h"
#include "asterisk/res_pjsip_session.h"
#include "asterisk/module.h"
#include "asterisk/channel.h"
#include "asterisk/logger.h"
#include "asterisk/test.h"

#define AST_API_MODULE
#include "asterisk/recording_fork.h"

#define RECORDING_FORK_DEFAULT_TIMEOUT_MS 3000
/* pjproject Timer F ends an unanswered UDP transaction at 32s. A longer
 * wait only pins the ARI HTTP worker after the SIP transaction is gone.
 */
#define RECORDING_FORK_MAX_TIMEOUT_MS 30000

/* Strings point into the request JSON and remain valid for the call. */
struct recording_fork_params {
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

static int recording_fork_token_ok(const char *value)
{
	if (ast_strlen_zero(value)) {
		return 0;
	}
	if (strchr(value, ';') || strchr(value, '\r') || strchr(value, '\n')) {
		return 0;
	}
	return 1;
}

static int recording_fork_port_ok(int port)
{
	return port > 0 && port <= 65535;
}

/* The wire field is host:port. A colon is the separator, so the host is an
 * IPv4 address or a hostname. An IPv6 literal cannot be encoded here.
 */
static int recording_fork_host_ok(const char *value)
{
	if (!recording_fork_token_ok(value)) {
		return 0;
	}
	return strchr(value, ':') == NULL;
}

static int recording_fork_json_string(struct ast_json *body, const char *name, const char **out)
{
	struct ast_json *field = ast_json_object_get(body, name);

	if (!field) {
		*out = NULL;
		return 0;
	}
	if (ast_json_typeof(field) != AST_JSON_STRING) {
		return -1;
	}
	*out = ast_json_string_get(field);
	return 0;
}

/* Range-check the JSON integer before storing it in an int. Assigning
 * intmax_t straight into int wraps values such as 4294967297 to 1.
 * Returns 1 when stored, 0 when the field is absent, -1 when invalid.
 */
static int recording_fork_json_bounded(struct ast_json *body, const char *name,
	intmax_t min, intmax_t max, int *out)
{
	struct ast_json *field = ast_json_object_get(body, name);
	intmax_t value;

	if (!field) {
		return 0;
	}
	if (ast_json_typeof(field) != AST_JSON_INTEGER) {
		return -1;
	}
	value = ast_json_integer_get(field);
	if (value < min || value > max) {
		return -1;
	}
	*out = (int) value;
	return 1;
}

/* Fills response and returns -1 when the body cannot be sent. */
static int recording_fork_parse(struct ast_json *body, struct recording_fork_params *params,
	struct ast_ari_response *response)
{
	const char *action = NULL;
	const char *org_id = NULL;
	const char *call_id = NULL;
	const char *sent_host = NULL;
	const char *heard_host = NULL;
	const char *codec = NULL;
	int sent_port = 0;
	int heard_port = 0;
	int timeout_ms = 0;
	int start;
	int rc;

	if (!body || ast_json_typeof(body) != AST_JSON_OBJECT) {
		ast_ari_response_error(response, 400, "Bad Request",
			"Request body is required");
		return -1;
	}
	if (recording_fork_json_string(body, "action", &action)
		|| recording_fork_json_string(body, "orgId", &org_id)
		|| recording_fork_json_string(body, "callId", &call_id)
		|| recording_fork_json_string(body, "sentHost", &sent_host)
		|| recording_fork_json_string(body, "heardHost", &heard_host)
		|| recording_fork_json_string(body, "codec", &codec)) {
		ast_ari_response_error(response, 400, "Bad Request",
			"Recording fork string fields must be strings");
		return -1;
	}
	rc = recording_fork_json_bounded(body, "sentPort", 1, 65535, &sent_port);
	if (rc < 0) {
		ast_ari_response_error(response, 400, "Bad Request",
			"sentPort must be an integer from 1 to 65535");
		return -1;
	}
	rc = recording_fork_json_bounded(body, "heardPort", 1, 65535, &heard_port);
	if (rc < 0) {
		ast_ari_response_error(response, 400, "Bad Request",
			"heardPort must be an integer from 1 to 65535");
		return -1;
	}
	rc = recording_fork_json_bounded(body, "timeoutMs", 1, RECORDING_FORK_MAX_TIMEOUT_MS, &timeout_ms);
	if (rc < 0) {
		ast_ari_response_error(response, 400, "Bad Request",
			"timeoutMs must be an integer from 1 to 30000");
		return -1;
	}
	if (!recording_fork_token_ok(action) || !recording_fork_token_ok(org_id)
		|| !recording_fork_token_ok(call_id)) {
		ast_ari_response_error(response, 400, "Bad Request",
			"action, orgId, and callId are required");
		return -1;
	}
	start = !strcasecmp(action, "start");
	if (!start && strcasecmp(action, "stop")) {
		ast_ari_response_error(response, 400, "Bad Request",
			"action must be start or stop");
		return -1;
	}
	if (start && (!recording_fork_host_ok(sent_host) || !recording_fork_host_ok(heard_host)
		|| !recording_fork_token_ok(codec)
		|| !recording_fork_port_ok(sent_port) || !recording_fork_port_ok(heard_port))) {
		ast_ari_response_error(response, 400, "Bad Request",
			"start requires IPv4 or hostname sentHost and heardHost, plus sentPort, heardPort, and codec");
		return -1;
	}

	params->start = start;
	params->org_id = org_id;
	params->call_id = call_id;
	params->sent_host = sent_host;
	params->sent_port = sent_port;
	params->heard_host = heard_host;
	params->heard_port = heard_port;
	params->codec = codec;
	params->timeout_ms = timeout_ms > 0 ? timeout_ms : RECORDING_FORK_DEFAULT_TIMEOUT_MS;
	return 0;
}

static int recording_fork_format_body(const struct recording_fork_params *params, char **body)
{
	if (params->start) {
		return ast_asprintf(body, "FORK_START;%s;%s;%s:%d;%s:%d;%s",
			params->org_id, params->call_id, params->sent_host, params->sent_port,
			params->heard_host, params->heard_port, params->codec);
	}
	return ast_asprintf(body, "FORK_STOP;%s;%s", params->org_id, params->call_id);
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
	const char *channel_id, struct ast_json *request,
	struct ast_ari_response *response)
{
	RAII_VAR(struct ast_channel *, chan, NULL, ast_channel_cleanup);
	RAII_VAR(struct ast_sip_session *, session, NULL, ao2_cleanup);
	RAII_VAR(struct recording_fork_request *, req, NULL, ao2_cleanup);
	RAII_VAR(char *, body, NULL, ast_free);
	const struct ast_channel_tech *tech;
	struct ast_sip_channel_pvt *pvt;
	struct recording_fork_params params;
	struct recording_fork_send send;
	struct timespec deadline;
	int done;
	int sip_status;

	if (!response) {
		return;
	}
	memset(&params, 0, sizeof(params));
	if (recording_fork_parse(request, &params, response)) {
		return;
	}

	chan = ast_channel_get_by_name(channel_id);
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

	if (recording_fork_format_body(&params, &body) < 0) {
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

	recording_fork_deadline(&deadline, params.timeout_ms);
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

static void recording_fork_response_reset(struct ast_ari_response *response)
{
	ast_json_unref(response->message);
	memset(response, 0, sizeof(*response));
}

static int recording_fork_response_sip_status(struct ast_ari_response *response)
{
	struct ast_json *field;

	if (!response->message) {
		return -1;
	}
	field = ast_json_object_get(response->message, "sipStatus");
	if (!field || ast_json_typeof(field) != AST_JSON_INTEGER) {
		return -1;
	}
	return (int) ast_json_integer_get(field);
}

AST_TEST_DEFINE(recording_fork_contract)
{
	struct ast_json_error error;
	struct ast_ari_response response;
	struct recording_fork_params params;
	char *body = NULL;
	enum ast_test_result_state result = AST_TEST_PASS;
	static const char start_json[] =
		"{\"action\":\"start\",\"orgId\":\"org-1\",\"callId\":\"call-1\","
		"\"sentHost\":\"10.1.2.3\",\"sentPort\":4000,"
		"\"heardHost\":\"recorder.example\",\"heardPort\":4002,\"codec\":\"ulaw\"}";
	static const char stop_json[] =
		"{\"action\":\"STOP\",\"orgId\":\"org-1\",\"callId\":\"call-1\"}";

	switch (cmd) {
	case TEST_INIT:
		info->name = "recording_fork_contract";
		info->category = "/res/res_pjsip_recording_fork/";
		info->summary = "Recording fork body, validation, and SIP status mapping";
		info->description =
			"Checks the FORK_START and FORK_STOP body, rejects fields that would\n"
			"break that body, and maps a final SIP status onto the ARI response.";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	memset(&response, 0, sizeof(response));
	memset(&params, 0, sizeof(params));

	{
		struct ast_json *json = ast_json_load_string(start_json, &error);

		if (!json || recording_fork_parse(json, &params, &response)
			|| params.timeout_ms != RECORDING_FORK_DEFAULT_TIMEOUT_MS
			|| recording_fork_format_body(&params, &body) < 0
			|| strcmp(body, "FORK_START;org-1;call-1;10.1.2.3:4000;recorder.example:4002;ulaw")) {
			ast_test_status_update(test, "start body was not byte-exact\n");
			result = AST_TEST_FAIL;
		}
		ast_free(body);
		body = NULL;
		ast_json_unref(json);
	}

	if (result == AST_TEST_PASS) {
		struct ast_json *json = ast_json_load_string(stop_json, &error);

		memset(&params, 0, sizeof(params));
		if (!json || recording_fork_parse(json, &params, &response)
			|| recording_fork_format_body(&params, &body) < 0
			|| strcmp(body, "FORK_STOP;org-1;call-1")) {
			ast_test_status_update(test, "stop body was not byte-exact\n");
			result = AST_TEST_FAIL;
		}
		ast_free(body);
		body = NULL;
		ast_json_unref(json);
	}

	if (result == AST_TEST_PASS) {
		static const char *rejected[] = {
			"{\"action\":\"start\",\"orgId\":\"org;1\",\"callId\":\"call-1\",\"sentHost\":\"10.0.0.1\",\"sentPort\":1,\"heardHost\":\"10.0.0.1\",\"heardPort\":2,\"codec\":\"ulaw\"}",
			"{\"action\":\"start\",\"orgId\":\"org-1\",\"callId\":\"call-1\",\"sentHost\":\"2001:db8::1\",\"sentPort\":1,\"heardHost\":\"10.0.0.1\",\"heardPort\":2,\"codec\":\"ulaw\"}",
			"{\"action\":\"start\",\"orgId\":\"org-1\",\"callId\":\"call-1\",\"sentHost\":\"10.0.0.1\",\"sentPort\":1,\"heardHost\":\"10.0.0.1\",\"heardPort\":2}",
			"{\"action\":\"start\",\"orgId\":\"org-1\",\"callId\":\"call-1\",\"sentHost\":\"10.0.0.1\",\"sentPort\":4294967297,\"heardHost\":\"10.0.0.1\",\"heardPort\":2,\"codec\":\"ulaw\"}",
			"{\"action\":\"start\",\"orgId\":\"org-1\",\"callId\":\"call-1\",\"sentHost\":\"10.0.0.1\",\"sentPort\":1,\"heardHost\":\"10.0.0.1\",\"heardPort\":2,\"codec\":\"ulaw\",\"timeoutMs\":30001}",
			"{\"action\":\"nope\",\"orgId\":\"org-1\",\"callId\":\"call-1\"}",
			"[]",
		};
		size_t i;

		for (i = 0; i < ARRAY_LEN(rejected); i++) {
			struct ast_json *json = ast_json_load_string(rejected[i], &error);

			recording_fork_response_reset(&response);
			memset(&params, 0, sizeof(params));
			if (!json || !recording_fork_parse(json, &params, &response)
				|| response.response_code != 400) {
				ast_test_status_update(test, "expected HTTP 400 for %s\n", rejected[i]);
				result = AST_TEST_FAIL;
				ast_json_unref(json);
				break;
			}
			ast_json_unref(json);
		}
	}

	if (result == AST_TEST_PASS) {
		recording_fork_response_reset(&response);
		recording_fork_respond(&response, 0, 200);
		if (response.response_code != 200 || recording_fork_response_sip_status(&response) != 200) {
			ast_test_status_update(test, "SIP 200 did not become HTTP 200\n");
			result = AST_TEST_FAIL;
		}
	}
	if (result == AST_TEST_PASS) {
		recording_fork_response_reset(&response);
		recording_fork_respond(&response, 0, 480);
		if (response.response_code != 500 || recording_fork_response_sip_status(&response) != 480) {
			ast_test_status_update(test, "SIP 480 did not become HTTP 500 with sipStatus 480\n");
			result = AST_TEST_FAIL;
		}
	}
	if (result == AST_TEST_PASS) {
		recording_fork_response_reset(&response);
		recording_fork_respond(&response, 1, 0);
		if (response.response_code != 504) {
			ast_test_status_update(test, "timeout did not become HTTP 504\n");
			result = AST_TEST_FAIL;
		}
	}

	recording_fork_response_reset(&response);
	return result;
}

static int load_module(void)
{
	if (ast_sip_register_service(&recording_fork_module)) {
		ast_log(LOG_ERROR, "Failed to register recording fork PJSIP module\n");
		return AST_MODULE_LOAD_DECLINE;
	}
	AST_TEST_REGISTER(recording_fork_contract);
	return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	AST_TEST_UNREGISTER(recording_fork_contract);
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

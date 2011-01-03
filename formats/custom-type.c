#include "custom-type.h"
#include "cmd.h"
#include "common.h"

#include <string.h>
#include <hiredis/hiredis.h>
#include <hiredis/async.h>

void
custom_type_reply(redisAsyncContext *c, void *r, void *privdata) {

	redisReply *reply = r;
	struct cmd *cmd = privdata;
	char *ct;
	(void)c;

	evhttp_clear_headers(&cmd->uri_params);

	if (reply == NULL) {
		evhttp_send_reply(cmd->rq, 404, "Not Found", NULL);
		return;
	}

	if(cmd->mime) { /* use the given content-type */
		if(reply->type != REDIS_REPLY_STRING) {
			goto fail;
		}
		format_send_reply(cmd, reply->str, reply->len, cmd->mime);
		return;
	}

	if(!cmd->mimeKey) { /* how did we get here? */
		goto fail;
	}

	/* we expect array(string, string) */
	if(reply->type != REDIS_REPLY_ARRAY || reply->elements != 2) {
		goto fail;
	}

	if(reply->element[0]->type != REDIS_REPLY_STRING) {
		goto fail;
	}

	if(reply->element[1]->type == REDIS_REPLY_STRING) {
		ct = reply->element[1]->str;
	} else {
		ct = "binary/octet-stream";
	}

	/* send reply */
	format_send_reply(cmd, reply->element[0]->str, reply->element[0]->len, ct);
	return;

fail:
	evhttp_send_reply(cmd->rq, 400, "Bad request", NULL);
	cmd_free(cmd);
}

/* This will change a GET command into MGET if a key is provided to get the response MIME-type from. */
void
custom_type_process_cmd(struct cmd *cmd) {
	/* MGET if mode is “custom” */
	if(cmd->argv_len[0] == 3 && strncasecmp(cmd->argv[0], "GET", 3) == 0 && cmd->mimeKey) {
		
		cmd->count++;	/* space for content-type key */

		/* replace command with MGET */
		cmd->argv[0] = "MGET";
		cmd->argv_len[0] = 4;

		/* add mime key after the key. */
		cmd->argv[2] = cmd->mimeKey;
		cmd->argv_len[2] = strlen(cmd->mimeKey);
	}
}

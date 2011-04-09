#ifndef CMD_H
#define CMD_H

#include <stdlib.h>
#include <hiredis/async.h>
#include <sys/queue.h>
#include <event.h>
#include <evhttp.h>

struct evhttp_request;
struct http_client;
struct server;
struct worker;
struct cmd;

typedef void (*formatting_fun)(redisAsyncContext *, void *, void *);
typedef enum {CMD_SENT,
	CMD_PARAM_ERROR,
	CMD_ACL_FAIL,
	CMD_REDIS_UNAVAIL} cmd_response_t;

struct cmd {
	int fd;

	int count;
	char **argv;
	size_t *argv_len;

	/* HTTP data */
	char *mime; /* forced output content-type */
	char *if_none_match; /* used with ETags */
	char *jsonp; /* jsonp wrapper */
	int keep_alive:1;
	int mime_free:1; /* need to free mime buffer */

	/* various flags */
	int started_responding:1;
	int is_websocket:1;
	int http_version:1;
};

struct subscription {
	struct server *s;
	struct cmd *cmd;
};

struct cmd *
cmd_new(int count);

void
cmd_free(struct cmd *c);

cmd_response_t
cmd_run(struct worker *w, struct http_client *client,
		const char *uri, size_t uri_len,
		const char *body, size_t body_len);

int
cmd_select_format(struct http_client *client, struct cmd *cmd,
		const char *uri, size_t uri_len, formatting_fun *f_format);

int
cmd_is_subscribe(struct cmd *cmd);

void
cmd_send(redisAsyncContext *ac, formatting_fun f_format, struct cmd *cmd);

void
cmd_setup(struct cmd *cmd, struct http_client *client);

#endif

#ifndef CMD_H
#define CMD_H

#include <stdlib.h>
#include <hiredis/async.h>
#include <sys/queue.h>
#include <evhttp.h>
#include <event.h>

struct evhttp_request;
struct server;

typedef void (*formatting_fun)(redisAsyncContext *, void *, void *);

struct cmd {

	int count;
	const char **argv;
	size_t *argv_len;
	struct evhttp_request *rq;

	struct evkeyvalq uri_params;
};

struct cmd *
cmd_new(struct evhttp_request *rq, int count);

void
cmd_free(struct cmd *c);

void
cmd_run(struct server *s, struct evhttp_request *rq,
		const char *uri, size_t uri_len);

formatting_fun
get_formatting_funtion(struct evkeyvalq *params);

#endif

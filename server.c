#include "server.h"
#include "conf.h"
#include "cmd.h"

#include <hiredis/hiredis.h>
#include <hiredis/adapters/libevent.h>
#include <jansson.h>

#include <unistd.h>
#include <signal.h>
#include <string.h>

struct server *
server_new(const char *filename) {
	struct server *s = calloc(1, sizeof(struct server));

	s->cfg = conf_read(filename);
	s->base = event_base_new();
	s->http = evhttp_new(s->base);

	return s;
}

static void
connectCallback(const redisAsyncContext *c) {
	((void)c);
}

static void
disconnectCallback(const redisAsyncContext *c, int status) {
	struct server *s = c->data;
	if (status != REDIS_OK) {
		fprintf(stderr, "Error: %s\n", c->errstr);
	}
	s->ac = NULL;

	/* wait 10 msec and reconnect */
	s->tv_reconnect.tv_sec = 0;
	s->tv_reconnect.tv_usec = 100000;
	webdis_connect(s);
}

static void
on_timer_reconnect(int fd, short event, void *ctx) {

	(void)fd;
	(void)event;
	struct server *s = ctx;

	if(s->ac) {
		redisLibeventCleanup(s->ac->data);
		redisFree((redisContext*)s->ac);
	}

	if(s->cfg->redis_host[0] == '/') { /* unix socket */
		s->ac = redisAsyncConnectUnix(s->cfg->redis_host);
	} else {
		s->ac = redisAsyncConnect(s->cfg->redis_host, s->cfg->redis_port);
	}

	s->ac->data = s;

	if(s->ac->err) {
		fprintf(stderr, "Error: %s\n", s->ac->errstr);
	}

	redisLibeventAttach(s->ac, s->base);
	redisAsyncSetConnectCallback(s->ac, connectCallback);
	redisAsyncSetDisconnectCallback(s->ac, disconnectCallback);

	if (s->cfg->redis_auth) { /* authenticate. */
		redisAsyncCommand(s->ac, NULL, NULL, "AUTH %s", s->cfg->redis_auth);
	}
}

void
webdis_connect(struct server *s) {
	/* schedule reconnect */
	evtimer_set(&s->ev_reconnect, on_timer_reconnect, s);
	event_base_set(s->base, &s->ev_reconnect);
	evtimer_add(&s->ev_reconnect, &s->tv_reconnect);
}

struct server *
server_copy(const struct server *s) {
	struct server *ret = calloc(1, sizeof(struct server));

	*ret = *s;

	/* create a new connection */
	ret->ac = NULL;
	on_timer_reconnect(0, 0, ret);

	return ret;
}

/* Adobe flash cross-domain request */
void
on_flash_request(struct evhttp_request *rq, void *ctx) {

	(void)ctx;

	char out[] = "<?xml version=\"1.0\"?>\n"
"<!DOCTYPE cross-domain-policy SYSTEM \"http://www.macromedia.com/xml/dtds/cross-domain-policy.dtd\">\n"
"<cross-domain-policy>\n"
  "<allow-access-from domain=\"*\" />\n"
"</cross-domain-policy>\n";

	struct evbuffer *body = evbuffer_new();
	evbuffer_add(body, out, sizeof(out) - 1);

	evhttp_add_header(rq->output_headers, "Content-Type", "application/xml");
	evhttp_send_reply(rq, 200, "OK", body);
	evbuffer_free(body);
}

#ifdef _EVENT2_HTTP_H_
/* reply to OPTIONS HTTP verb */
static int
on_options(struct evhttp_request *rq) {

	evhttp_add_header(rq->output_headers, "Content-Type", "text/html");
	evhttp_add_header(rq->output_headers, "Allow", "GET,POST,OPTIONS");

	/* Cross-Origin Resource Sharing, CORS. */
	evhttp_add_header(rq->output_headers, "Access-Control-Allow-Origin", "*");
	evhttp_send_reply(rq, 200, "OK", body);

	return 1;
}
#endif

void
on_request(struct evhttp_request *rq, void *ctx) {

	const char *uri = evhttp_request_uri(rq);
	struct server *s = ctx;
	int ret;

	if(!s->ac) { /* redis is unavailable */
		evhttp_send_reply(rq, 503, "Service Unavailable", NULL);
		return;
	}

	/* check that the command can be executed */
	switch(rq->type) {
		case EVHTTP_REQ_GET:
			ret = cmd_run(s, rq, 1+uri, strlen(uri)-1);
			break;

		case EVHTTP_REQ_POST:
			ret = cmd_run(s, rq,
				(const char*)EVBUFFER_DATA(rq->input_buffer),
				EVBUFFER_LENGTH(rq->input_buffer));
			break;

#ifdef _EVENT2_HTTP_H_
		case EVHTTP_REQ_OPTIONS:
			return on_options(rq);
#endif

		default:
			evhttp_send_reply(rq, 405, "Method Not Allowed", NULL);
			return;
	}

	if(ret < 0) {
		evhttp_send_reply(rq, 403, "Forbidden", NULL);
	}
}

void
server_start(struct server *s) {

	/* ignore sigpipe */
#ifdef SIGPIPE
	signal(SIGPIPE, SIG_IGN);
#endif

	/* start http server */
	evhttp_bind_socket(s->http, s->cfg->http_host, s->cfg->http_port);
	evhttp_set_cb(s->http, "/crossdomain.xml", on_flash_request, s);
	evhttp_set_gencb(s->http, on_request, s);

	/* drop privileges */
	setuid(s->cfg->user);
	setgid(s->cfg->group);

	/* attach hiredis to libevent base */
	webdis_connect(s);

	/* loop */
	event_base_dispatch(s->base);
}


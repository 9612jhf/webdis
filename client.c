#include "client.h"
#include "server.h"
#include "cmd.h"
#include "http.h"
#include "slog.h"

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include "hiredis/async.h"

struct http_client *
http_client_new(int fd, struct server *s) {

	struct http_client *c = calloc(1, sizeof(struct http_client));
	c->fd = fd;
	c->s = s;

	/* initialize HTTP parser */
	c->settings.on_path = http_on_path;
	c->settings.on_body = http_on_body;
	c->settings.on_message_complete = http_on_complete;
	c->settings.on_header_field = http_on_header_name;
	c->settings.on_header_value = http_on_header_value;
	c->settings.on_query_string = http_on_query_string;

	http_parser_init(&c->parser, HTTP_REQUEST);
	c->parser.data = c;

	return c;
}


/**
 * Called by libevent when read(2) is possible on fd without blocking.
 */
void
http_client_read(int fd, short event, void *ctx) {

	struct http_client *c = ctx;
	char buffer[64*1024];
	int ret, nparsed;

	(void)fd;
	(void)event;

	ret = read(c->fd, buffer, sizeof(buffer));
	if(ret <= 0) { /* broken connection, bye */
		http_client_free(c);
		return;
	}

	nparsed = http_parser_execute(&c->parser, &c->settings, buffer, ret);
	if(c->parser.upgrade) {
		/* TODO: upgrade parser (WebSockets & cie) */
	} else if(nparsed != ret) { /* invalid */
		http_client_free(c);
		return;
	}

	if(!c->executing) { /* if we're not waiting for Redis to reply, continue serving. */
		http_client_serve(c);
	}
}

static void
http_client_cleanup(struct http_client *c) {

	if(c->sub) {
		return; /* we need to keep those. */
	}

	free(c->path.s);
	memset(&c->path, 0, sizeof(str_t));

	free(c->body.s);
	memset(&c->body, 0, sizeof(str_t));

	free(c->input_headers.connection.s);
	memset(&c->input_headers.connection, 0, sizeof(str_t));

	free(c->input_headers.if_none_match.s);
	memset(&c->input_headers.if_none_match, 0, sizeof(str_t));

	free(c->input_headers.authorization.s);
	memset(&c->input_headers.authorization, 0, sizeof(str_t));

	free(c->output_headers.content_type.s);
	memset(&c->output_headers.content_type, 0, sizeof(str_t));

	free(c->output_headers.etag.s);
	memset(&c->output_headers.etag, 0, sizeof(str_t));

	free(c->query_string.type.s);
	memset(&c->query_string.type, 0, sizeof(str_t));

	free(c->query_string.jsonp.s);
	memset(&c->query_string.jsonp, 0, sizeof(str_t));

	memset(&c->verb, 0, sizeof(c->verb));

	c->executing = 0;
}

void
http_client_free(struct http_client *c) {

	event_del(&c->ev);
	close(c->fd);

	if(c->sub) {
		/* clean up redis object */
		redisAsyncFree(c->sub->s->ac);

		/* clean up command object */
		if(c->sub->cmd) {
			cmd_free(c->sub->cmd);
		}
		free(c->sub);
		c->sub = NULL;
	}

	http_client_cleanup(c);
	free(c);
}

int
http_client_keep_alive(struct http_client *c) {

	/* check disconnection */
	int disconnect = 0;

	if(c->parser.http_major == 0) {
		disconnect = 1; /* No version given. */
	} else if(c->parser.http_major == 1 && c->parser.http_minor == 0) {
		disconnect = 1; /* HTTP 1.0: disconnect by default */
	}
	if(c->input_headers.connection.s) {
		if(strncasecmp(c->input_headers.connection.s, "Keep-Alive", 10) == 0) {
			disconnect = 0;
		} else if(strncasecmp(c->input_headers.connection.s, "Close", 5) == 0) {
			disconnect = 1;
		}
	}
	return disconnect ? 0 : 1;
}

void
http_client_reset(struct http_client *c) {

	if(!http_client_keep_alive(c) && !c->sub) {
		http_client_free(c);
		return;
	}

	http_client_cleanup(c);
	http_parser_init(&c->parser, HTTP_REQUEST);
}

/**
 * (Re-)add read event callback
 */
void
http_client_serve(struct http_client *c) {

	event_set(&c->ev, c->fd, EV_READ, http_client_read, c);
	event_base_set(c->s->base, &c->ev);
	event_add(&c->ev, NULL);
}

/**** Parser callbacks ****/

/**
 * Called when the path has been found. This is before any `?query-string'.
 */
int
http_on_path(http_parser *p, const char *at, size_t length) {

	struct http_client *c = p->data;

	c->path.s = calloc(length+1, 1);
	memcpy(c->path.s, at, length);
	c->path.sz = length;

	/* save HTTP verb as well */
	c->verb = (enum http_method)p->method;

	return 0;
}

/**
 * Called when the whole body has been read.
 */
int
http_on_body(http_parser *p, const char *at, size_t length) {
	struct http_client *c = p->data;

	c->body.s = calloc(length+1, 1);
	memcpy(c->body.s, at, length);
	c->body.sz = length;

	return 0;
}

/**
 * Called when the query string has been completely read.
 */
int
http_on_query_string(http_parser *parser, const char *at, size_t length) {

	struct http_client *c = parser->data;
	const char *p = at;

	while(p < at + length) {

		const char *key = p, *val;
		int key_len, val_len;
		char *eq = memchr(key, '=', length - (p-at));
		if(!eq || eq > at + length) { /* last argument */
			break;
		} else { /* found an '=' */
			char *and;
			val = eq + 1;
			key_len = eq - key;
			p = eq + 1;

			and = memchr(p, '&', length - (p-at));
			if(!and || and > at + length) {
				val_len = at + length - p; /* last arg */
			} else {
				val_len = and - val; /* cur arg */
				p = and + 1;
			}

			if(key_len == 4 && strncmp(key, "type", 4) == 0) {
				http_set_header(&c->query_string.type, val, val_len);
			} else if(key_len == 5 && strncmp(key, "jsonp", 5) == 0) {
				http_set_header(&c->query_string.jsonp, val, val_len);
			}

			if(!and) {
				break;
			}
		}
	}
	return 0;
}

/**
 * Called when the whole request has been parsed.
 */
int
http_on_complete(http_parser *p) {
	struct http_client *c = p->data;
	int ret = -1;

	c->executing = 1;
	/* check that the command can be executed */
	switch(c->verb) {
		case HTTP_GET:
			if(c->path.sz == 16 && memcmp(c->path.s, "/crossdomain.xml", 16) == 0) {
				return http_crossdomain(c);
			}
			slog(c->s, WEBDIS_DEBUG, c->path.s, c->path.sz);
			ret = cmd_run(c->s, c, 1+c->path.s, c->path.sz-1, NULL, 0);
			break;

		case HTTP_POST:
			slog(c->s, WEBDIS_DEBUG, c->path.s, c->path.sz);
			ret = cmd_run(c->s, c, 1+c->body.s, c->body.sz-1, NULL, 0);
			break;

		case HTTP_PUT:
			slog(c->s, WEBDIS_DEBUG, c->path.s, c->path.sz);
			ret = cmd_run(c->s, c, 1+c->path.s, c->path.sz-1,
					c->body.s, c->body.sz);
			break;

		case HTTP_OPTIONS:
			return http_options(c);

		default:
			slog(c->s, WEBDIS_DEBUG, "405", 3);
			http_send_error(c, 405, "Method Not Allowed");
			return 0;
	}

	if(ret < 0) {
		http_send_error(c, 403, "Forbidden");
	}

	return ret;
}

/**
 * Called when a header name is read
 */
int
http_on_header_name(http_parser *p, const char *at, size_t length) {

	struct http_client *c = p->data;

	c->last_header_name.s = calloc(length+1, 1);
	memcpy(c->last_header_name.s, at, length);
	c->last_header_name.sz = length;

	return 0;
}

/**
 * Called when a header value is read
 */
int
http_on_header_value(http_parser *p, const char *at, size_t length) {

	struct http_client *c = p->data;

	if(strncmp("Connection", c->last_header_name.s, c->last_header_name.sz) == 0) {
		http_set_header(&c->input_headers.connection, at, length);
	} else if(strncmp("If-None-Match", c->last_header_name.s, c->last_header_name.sz) == 0) {
		http_set_header(&c->input_headers.if_none_match, at, length);
	} else if(strncmp("Authorization", c->last_header_name.s, c->last_header_name.sz) == 0) {
		http_set_header(&c->input_headers.authorization, at, length);
	} else if(strncmp("Expect", c->last_header_name.s, c->last_header_name.sz) == 0) {
		if(length == 12 && memcmp(at, "100-continue", length) == 0) {
			/* support HTTP file upload */
			char http100[] = "HTTP/1.1 100 Continue\r\n\r\n";
			write(c->fd, http100, sizeof(http100)-1);
		}
	}

	free(c->last_header_name.s);
	c->last_header_name.s = NULL;
	return 0;
}




/**** HTTP Responses ****/

static void
http_response_init(struct http_client *c, struct http_response *r, int code, const char *msg) {

	memset(r, 0, sizeof(struct http_response));
	r->code = code;
	r->msg = msg;

	http_response_set_header(r, "Server", "Webdis");

	if(!http_client_keep_alive(c)) {
		http_response_set_header(r, "Connection", "Close");
	} else if(code == 200) {
		http_response_set_header(r, "Connection", "Keep-Alive");
	}
}
void
http_send_reply(struct http_client *c, short code, const char *msg,
		const char *body, size_t body_len) {

	struct http_response resp;
	const char *ct = c->output_headers.content_type.s;
	if(!ct) {
		ct = "text/html";
	}

	/* respond */
	http_response_init(c, &resp, code, msg);

	if(body_len) {
		http_response_set_header(&resp, "Content-Type", ct);
	}

	if(c->sub) {
		http_response_set_header(&resp, "Transfer-Encoding", "chunked");
	}

	if(code == 200 && c->output_headers.etag.s) {
		http_response_set_header(&resp, "ETag", c->output_headers.etag.s);
	}

	http_response_set_body(&resp, body, body_len);

	/* flush response in the socket */
	if(http_response_write(&resp, c->fd)) { /* failure */
		http_client_free(c);
	} else {
		if(c->sub) { /* don't free the client, but monitor fd. */
			http_client_serve(c);
			return;
		} else if(code == 200 && http_client_keep_alive(c)) { /* reset client */
			http_client_reset(c);
			http_client_serve(c);
		} else {
			http_client_free(c); /* error or HTTP < 1.1: close */
		}
	}
}

void
http_send_error(struct http_client *c, short code, const char *msg) {

	http_send_reply(c, code, msg, NULL, 0);
}

/* Transfer-encoding: chunked */
void
http_send_reply_start(struct http_client *c, short code, const char *msg) {

	http_send_reply(c, code, msg, NULL, 0);
}

void
http_send_reply_chunk(struct http_client *c, const char *p, size_t sz) {

	char buf[64];
	int ret;

	ret = sprintf(buf, "%x\r\n", (int)sz);
	write(c->fd, buf, ret);
	write(c->fd, p, sz);
	write(c->fd, "\r\n", 2);
}

/* send nil chunk to mark the end of a stream. */
void
http_send_reply_end(struct http_client *c) {

	http_send_reply_chunk(c, "", 0);
	http_client_free(c);
}

/* Adobe flash cross-domain request */
int
http_crossdomain(struct http_client *c) {

	struct http_response resp;
	char out[] = "<?xml version=\"1.0\"?>\n"
"<!DOCTYPE cross-domain-policy SYSTEM \"http://www.macromedia.com/xml/dtds/cross-domain-policy.dtd\">\n"
"<cross-domain-policy>\n"
  "<allow-access-from domain=\"*\" />\n"
"</cross-domain-policy>\n";

	http_response_init(c, &resp, 200, "OK");
	http_response_set_header(&resp, "Content-Type", "application/xml");
	http_response_set_body(&resp, out, sizeof(out)-1);

	http_response_write(&resp, c->fd);
	http_client_reset(c);

	return 0;
}

/* reply to OPTIONS HTTP verb */
int
http_options(struct http_client *c) {

	struct http_response resp;

	http_response_init(c, &resp, 200, "OK");

	http_response_set_header(&resp, "Content-Type", "text/html");
	http_response_set_header(&resp, "Allow", "GET,POST,OPTIONS");
	http_response_set_header(&resp, "Content-Length", "0");

	/* Cross-Origin Resource Sharing, CORS. */
	http_response_set_header(&resp, "Access-Control-Allow-Origin", "*");

	http_response_write(&resp, c->fd);
	http_client_reset(c);

	return 0;
}

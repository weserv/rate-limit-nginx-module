#include "ngx_http_rate_limit_upstream.h"

/* Reference: ngx_http_upstream_finalize_request */
void
ngx_http_rate_limit_finalize_upstream_request(ngx_http_request_t *r,
                                              ngx_http_upstream_t *u,
                                              ngx_int_t rc)
{
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "finalize http rate limit request: %i", rc);

    if (u->cleanup == NULL) {
        /* the request was already finalized */
        ngx_http_finalize_request(r, NGX_DONE);
        return;
    }

    *u->cleanup = NULL;
    u->cleanup = NULL;

    if (u->resolved && u->resolved->ctx) {
        ngx_resolve_name_done(u->resolved->ctx);
        u->resolved->ctx = NULL;
    }

    if (u->state && u->state->response_time == (ngx_msec_t) -1) {
        u->state->response_time = ngx_current_msec - u->start_time;

        if (u->pipe && u->pipe->read_length) {
            u->state->bytes_received +=
                u->pipe->read_length - u->pipe->preread_size;
            u->state->response_length = u->pipe->read_length;
        }

        if (u->peer.connection) {
            u->state->bytes_sent = u->peer.connection->sent;
        }
    }

    u->finalize_request(r, rc);

    if (u->peer.free && u->peer.sockaddr) {
        u->peer.free(&u->peer, u->peer.data, 0);
        u->peer.sockaddr = NULL;
    }

    if (u->peer.connection) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "close redis connection: %d", u->peer.connection->fd);

        if (u->peer.connection->pool) {
            ngx_destroy_pool(u->peer.connection->pool);
        }

        ngx_close_connection(u->peer.connection);
    }

    u->peer.connection = NULL;

    r->read_event_handler = ngx_http_block_reading;
    r->write_event_handler = ngx_http_core_run_phases;

    if (rc == NGX_DECLINED) {
        return;
    }

    r->connection->log->action = "sending to client";

    ngx_http_finalize_request(r, rc);

    ngx_http_core_run_phases(r);
}

/* Reference: ngx_http_upstream_test_connect */
static ngx_int_t
ngx_http_rate_limit_test_connect(ngx_connection_t *c)
{
    int       err;
    socklen_t len;

#if (NGX_HAVE_KQUEUE)

    if (ngx_event_flags & NGX_USE_KQUEUE_EVENT) {
        if (c->write->pending_eof || c->read->pending_eof) {
            if (c->write->pending_eof) {
                err = c->write->kq_errno;

            } else {
                err = c->read->kq_errno;
            }

            c->log->action = "connecting to redis";
            (void) ngx_connection_error(
                c, err, "kevent() reported that connect() failed");
            return NGX_ERROR;
        }

    } else
#endif
    {
        err = 0;
        len = sizeof(int);

        /*
         * BSDs and Linux return 0 and set a pending error in err
         * Solaris returns -1 and sets errno
         */

        if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, (void *) &err, &len) ==
            -1) {
            err = ngx_socket_errno;
        }

        if (err) {
            c->log->action = "connecting to redis";
            (void) ngx_connection_error(c, err, "connect() failed");
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}

/* Reference: ngx_http_upstream_process_non_buffered_request */
static void
ngx_http_rate_limit_process_redis_response(ngx_http_request_t *r,
                                           ngx_uint_t do_write)
{
    size_t               size;
    ssize_t              n;
    ngx_buf_t           *b;
    ngx_connection_t    *upstream;
    ngx_http_upstream_t *u;

    u = r->upstream;
    upstream = u->peer.connection;

    b = &u->buffer;

    do_write = do_write || u->length == 0;

    for (;;) {

        if (do_write) {

            if (u->out_bufs || u->busy_bufs) {
                ngx_http_rate_limit_finalize_upstream_request(r, u, NGX_DONE);
            }

            if (u->busy_bufs == NULL) {

                if (u->length == 0 ||
                    (upstream->read->eof && u->length == -1)) {
                    ngx_http_rate_limit_finalize_upstream_request(r, u, 0);
                    return;
                }

                if (upstream->read->eof) {
                    ngx_log_error(NGX_LOG_ERR, upstream->log, 0,
                                  "redis prematurely closed connection");

                    ngx_http_rate_limit_finalize_upstream_request(
                        r, u, NGX_HTTP_BAD_GATEWAY);
                    return;
                }

                if (upstream->read->error) {
                    ngx_http_rate_limit_finalize_upstream_request(
                        r, u, NGX_HTTP_BAD_GATEWAY);
                    return;
                }

                b->pos = b->start;
                b->last = b->start;
            }
        }

        size = b->end - b->last;

        if (size && upstream->read->ready) {

            n = upstream->recv(upstream, b->last, size);

            if (n == NGX_AGAIN) {
                break;
            }

            if (n > 0) {
                u->state->bytes_received += n;
                u->state->response_length += n;

                if (u->input_filter(u->input_filter_ctx, n) == NGX_ERROR) {
                    ngx_http_rate_limit_finalize_upstream_request(r, u,
                                                                  NGX_ERROR);
                    return;
                }
            }

            do_write = 1;

            continue;
        }

        break;
    }

    if (ngx_handle_read_event(upstream->read, 0) != NGX_OK) {
        ngx_http_rate_limit_finalize_upstream_request(r, u, NGX_ERROR);
        return;
    }

    if (upstream->read->active && !upstream->read->ready) {
        ngx_add_timer(upstream->read, u->conf->read_timeout);

    } else if (upstream->read->timer_set) {
        ngx_del_timer(upstream->read);
    }
}

/* Reference: ngx_http_upstream_process_non_buffered_upstream */
static void
ngx_http_rate_limit_redis_rev_handler(ngx_http_request_t *r,
                                      ngx_http_upstream_t *u)
{
    ngx_connection_t *c;

    c = u->peer.connection;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0,
                   "http rate limit process redis response");

    c->log->action = "reading from redis";

    if (c->read->timedout) {
        ngx_connection_error(c, NGX_ETIMEDOUT, "redis timed out");
        ngx_http_rate_limit_finalize_upstream_request(
            r, u, NGX_HTTP_GATEWAY_TIME_OUT);
        return;
    }

    ngx_http_rate_limit_process_redis_response(r, 0);
}

/* Reference: ngx_http_upstream_dummy_handler */
static void
ngx_http_rate_limit_dummy_handler(ngx_http_request_t *r, ngx_http_upstream_t *u)
{
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                  "rate_limit: ngx_http_rate_limit_dummy_handler should not"
                  " be called by the upstream");
}

/* Reference: ngx_http_upstream_send_response */
static void
ngx_http_rate_limit_process_response(ngx_http_request_t *r,
                                     ngx_http_upstream_t *u)
{
    ssize_t                   n;
    ngx_connection_t         *c;
    ngx_http_core_loc_conf_t *clcf;

    /* Not necessary, we don't send anything to the client */
    // u->header_sent = 1;

    c = r->connection;

    clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

    /* We are always processing a non buffered response */
    // if (!u->buffering) {

    /* Input filter is always set */
    /*if (u->input_filter == NULL) {
        u->input_filter_init = ngx_http_upstream_non_buffered_filter_init;
        u->input_filter = ngx_http_upstream_non_buffered_filter;
        u->input_filter_ctx = r;
    }*/

    u->read_event_handler = ngx_http_rate_limit_redis_rev_handler;

    /* Set write_event_handler to the dummy handler
     * to make sure we don't send anything */
    u->write_event_handler = ngx_http_rate_limit_dummy_handler;

    /* Not needed */
    // r->limit_rate = 0;
    // r->limit_rate_set = 1;

    if (u->input_filter_init(u->input_filter_ctx) == NGX_ERROR) {
        ngx_http_rate_limit_finalize_upstream_request(r, u, NGX_ERROR);
        return;
    }

    if (clcf->tcp_nodelay && ngx_tcp_nodelay(c) != NGX_OK) {
        ngx_http_rate_limit_finalize_upstream_request(r, u, NGX_ERROR);
        return;
    }

    n = u->buffer.last - u->buffer.pos;

    if (n) {
        u->buffer.last = u->buffer.pos;

        u->state->response_length += n;

        if (u->input_filter(u->input_filter_ctx, n) == NGX_ERROR) {
            ngx_http_rate_limit_finalize_upstream_request(r, u, NGX_ERROR);
            return;
        }

        ngx_http_rate_limit_finalize_upstream_request(r, u, NGX_DONE);
    } else {
        u->buffer.pos = u->buffer.start;
        u->buffer.last = u->buffer.start;

        if (ngx_http_send_special(r, NGX_HTTP_FLUSH) == NGX_ERROR) {
            ngx_http_rate_limit_finalize_upstream_request(r, u, NGX_ERROR);
            return;
        }

        if (u->peer.connection->read->ready || u->length == 0) {
            ngx_http_rate_limit_redis_rev_handler(r, u);
        }
    }
}

/* Reference: ngx_http_upstream_process_header */
void
ngx_http_rate_limit_rev_handler(ngx_http_request_t *r, ngx_http_upstream_t *u)
{
    ssize_t           n;
    ngx_int_t         rc;
    ngx_connection_t *c;

    c = u->peer.connection;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0,
                   "http rate limit rev handler");

    c->log->action = "reading response header from redis";

    if (c->read->timedout) {
        /*ngx_http_upstream_next(r, u, NGX_HTTP_UPSTREAM_FT_TIMEOUT);*/
        ngx_http_rate_limit_finalize_upstream_request(
            r, u, NGX_HTTP_GATEWAY_TIME_OUT);
        return;
    }

    if (!u->request_sent && ngx_http_rate_limit_test_connect(c) != NGX_OK) {
        /* Ensure u->reinit_request always gets called for upstream_next */
        /*u->request_sent = 1;

        ngx_http_upstream_next(r, u, NGX_HTTP_UPSTREAM_FT_ERROR);*/
        ngx_http_rate_limit_finalize_upstream_request(
            r, u, NGX_HTTP_SERVICE_UNAVAILABLE);
        return;
    }

    if (u->buffer.start == NULL) {
        u->buffer.start = ngx_palloc(r->pool, u->conf->buffer_size);
        if (u->buffer.start == NULL) {
            ngx_http_rate_limit_finalize_upstream_request(
                r, u, NGX_HTTP_INTERNAL_SERVER_ERROR);
            return;
        }

        u->buffer.pos = u->buffer.start;
        u->buffer.last = u->buffer.start;
        u->buffer.end = u->buffer.start + u->conf->buffer_size;
        u->buffer.temporary = 1;

        u->buffer.tag = u->output.tag;

        /* No need to init u->headers_in.headers and u->headers_in.trailers */
    }

    for (;;) {

        n = c->recv(c, u->buffer.last, u->buffer.end - u->buffer.last);

        if (n == NGX_AGAIN) {
            if (ngx_handle_read_event(c->read, 0) != NGX_OK) {
                ngx_http_rate_limit_finalize_upstream_request(
                    r, u, NGX_HTTP_INTERNAL_SERVER_ERROR);

                return;
            }

            return;
        }

        if (n == 0) {
            ngx_log_error(NGX_LOG_ERR, c->log, 0,
                          "redis prematurely closed connection");
        }

        if (n == NGX_ERROR || n == 0) {
            /*ngx_http_upstream_next(r, u, NGX_HTTP_UPSTREAM_FT_ERROR);*/
            ngx_http_rate_limit_finalize_upstream_request(
                r, u, NGX_HTTP_INTERNAL_SERVER_ERROR);
            return;
        }

        u->state->bytes_received += n;

        u->buffer.last += n;

        rc = u->process_header(r);

        if (rc == NGX_AGAIN) {

            if (u->buffer.last == u->buffer.end) {
                ngx_log_error(NGX_LOG_ERR, c->log, 0,
                              "redis sent too big header");

                /*ngx_http_upstream_next(r, u,
                                       NGX_HTTP_UPSTREAM_FT_INVALID_HEADER);*/

                ngx_http_rate_limit_finalize_upstream_request(
                    r, u, NGX_HTTP_INTERNAL_SERVER_ERROR);
                return;
            }

            continue;
        }

        break;
    }

    if (rc == NGX_HTTP_UPSTREAM_INVALID_HEADER) {
        /*ngx_http_upstream_next(r, u, NGX_HTTP_UPSTREAM_FT_INVALID_HEADER);*/
        ngx_http_rate_limit_finalize_upstream_request(
            r, u, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }

    if (rc == NGX_ERROR) {
        ngx_http_rate_limit_finalize_upstream_request(
            r, u, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }

    /* rc == NGX_OK */

    u->state->header_time = ngx_current_msec - u->start_time;

    u->length = -1;

    ngx_http_rate_limit_process_response(r, u);
}

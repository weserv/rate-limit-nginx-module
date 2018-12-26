#include "ngx_http_rate_limit_upstream.h"


/* Partially copied from ngx_http_upstream_finalize_request */
void
ngx_http_rate_limit_finalize_upstream_request(ngx_http_request_t *r,
                                              ngx_http_upstream_t *u, ngx_int_t rc)
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

    if (u->state && u->state->response_time) {
        u->state->response_time = ngx_current_msec - u->state->response_time;

        if (u->pipe && u->pipe->read_length) {
            u->state->bytes_received += u->pipe->read_length
                                        - u->pipe->preread_size;
            u->state->response_length = u->pipe->read_length;
        }
    }

    u->finalize_request(r, rc);

    if (u->peer.free && u->peer.sockaddr) {
        u->peer.free(&u->peer, u->peer.data, 0);
        u->peer.sockaddr = NULL;
    }

    if (u->peer.connection) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "close redis connection: %d",
                       u->peer.connection->fd);

        if (u->peer.connection->pool) {
            ngx_destroy_pool(u->peer.connection->pool);
        }

        ngx_close_connection(u->peer.connection);
    }

    u->peer.connection = NULL;

#if (NGX_HTTP_CACHE)

    if (r->cache) {

        if (u->cacheable) {

            if (rc == NGX_HTTP_BAD_GATEWAY || rc == NGX_HTTP_GATEWAY_TIME_OUT) {
                time_t  valid;

                valid = ngx_http_file_cache_valid(u->conf->cache_valid, rc);

                if (valid) {
                    r->cache->valid_sec = ngx_time() + valid;
                    r->cache->error = rc;
                }
            }
        }

        ngx_http_file_cache_free(r->cache, u->pipe->temp_file);
    }

#endif

    r->read_event_handler = ngx_http_block_reading;
    r->write_event_handler = ngx_http_core_run_phases;

    if (rc == NGX_DECLINED) {
        return;
    }

    r->connection->log->action = "sending to client";

    ngx_http_finalize_request(r, rc);

    ngx_http_core_run_phases(r);
}


/* Copied from ngx_http_upstream_test_connect */
static ngx_int_t
ngx_http_rate_limit_test_connect(ngx_connection_t *c)
{
    int       err;
    socklen_t len;

#if (NGX_HAVE_KQUEUE)

    if (ngx_event_flags & NGX_USE_KQUEUE_EVENT)  {
        if (c->write->pending_eof || c->read->pending_eof) {
            if (c->write->pending_eof) {
                err = c->write->kq_errno;

            } else {
                err = c->read->kq_errno;
            }

            c->log->action = "connecting to upstream";
            (void) ngx_connection_error(c, err,
                                    "kevent() reported that connect() failed");
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

        if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, (void *) &err, &len) == -1) {
            err = ngx_socket_errno;
        }

        if (err) {
            c->log->action = "connecting to upstream";
            (void) ngx_connection_error(c, err, "connect() failed");
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}


/* Partially copied from ngx_http_upstream_process_header */
void
ngx_http_rate_limit_rev_handler(ngx_http_request_t *r, ngx_http_upstream_t *u)
{
    ssize_t          n;
    ngx_int_t        rc;
    ngx_connection_t *c;

    c = u->peer.connection;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0,
                   "http rate limit rev handler");

    c->log->action = "reading response header from redis";

    if (c->read->timedout) {
        /*ngx_http_upstream_next(r, u, NGX_HTTP_UPSTREAM_FT_TIMEOUT);*/
        ngx_http_rate_limit_finalize_upstream_request(r, u,
                                                      NGX_HTTP_GATEWAY_TIME_OUT);
        return;
    }

    if (!u->request_sent && ngx_http_rate_limit_test_connect(c) != NGX_OK) {
        /* just to ensure u->reinit_request always gets called for
         * upstream_next */
        /*u->request_sent = 1;

        ngx_http_upstream_next(r, u, NGX_HTTP_UPSTREAM_FT_ERROR);*/
        ngx_http_rate_limit_finalize_upstream_request(r, u,
                                                      NGX_HTTP_SERVICE_UNAVAILABLE);
        return;
    }

    if (u->buffer.start == NULL) {
        u->buffer.start = ngx_palloc(r->pool, u->conf->buffer_size);
        if (u->buffer.start == NULL) {
            ngx_http_rate_limit_finalize_upstream_request(r, u,
                                                          NGX_HTTP_INTERNAL_SERVER_ERROR);
            return;
        }

        u->buffer.pos = u->buffer.start;
        u->buffer.last = u->buffer.start;
        u->buffer.end = u->buffer.start + u->conf->buffer_size;
        u->buffer.temporary = 1;

        u->buffer.tag = u->output.tag;

        /* No need to init u->headers_in.headers and u->headers_in.trailers */

#if (NGX_HTTP_CACHE)

        if (r->cache) {
            u->buffer.pos += r->cache->header_start;
            u->buffer.last = u->buffer.pos;
        }
#endif
    }

    for ( ;; ) {

        n = c->recv(c, u->buffer.last, u->buffer.end - u->buffer.last);

        if (n == NGX_AGAIN) {
            if (ngx_handle_read_event(c->read, 0) != NGX_OK) {
                ngx_http_rate_limit_finalize_upstream_request(r, u,
                                                              NGX_HTTP_INTERNAL_SERVER_ERROR);

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
            ngx_http_rate_limit_finalize_upstream_request(r, u,
                                                          NGX_HTTP_INTERNAL_SERVER_ERROR);
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

                ngx_http_rate_limit_finalize_upstream_request(r, u,
                                                              NGX_HTTP_INTERNAL_SERVER_ERROR);
                return;
            }

            continue;
        }

        break;
    }

    if (rc == NGX_HTTP_UPSTREAM_INVALID_HEADER) {
        /*ngx_http_upstream_next(r, u, NGX_HTTP_UPSTREAM_FT_INVALID_HEADER);*/
        ngx_http_rate_limit_finalize_upstream_request(r, u,
                                                      NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }

    if (rc == NGX_ERROR) {
        ngx_http_rate_limit_finalize_upstream_request(r, u,
                                                      NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }

    /* rc == NGX_OK */

    u->state->header_time = ngx_current_msec - u->start_time;

    u->length = 0;
    u->keepalive = 1;

    ngx_http_rate_limit_finalize_upstream_request(r, u, NGX_DONE);
}

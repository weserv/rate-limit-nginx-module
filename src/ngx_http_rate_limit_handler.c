#include "ngx_http_rate_limit_handler.h"
#include "ngx_http_rate_limit_util.h"

static ngx_int_t ngx_http_rate_limit_create_request(ngx_http_request_t *r);
static ngx_int_t ngx_http_rate_limit_reinit_request(ngx_http_request_t *r);
static ngx_int_t ngx_http_rate_limit_process_header(ngx_http_request_t *r);
static ngx_int_t ngx_http_rate_limit_filter_init(void *data);
static ngx_int_t ngx_http_rate_limit_filter(void *data, ssize_t bytes);
static void ngx_http_rate_limit_abort_request(ngx_http_request_t *r);
static void ngx_http_rate_limit_finalize_request(ngx_http_request_t *r,
    ngx_int_t rc);


static ngx_str_t x_limit_header = ngx_string("X-RateLimit-Limit");
static ngx_str_t x_remaining_header = ngx_string("X-RateLimit-Remaining");
static ngx_str_t x_reset_header = ngx_string("X-RateLimit-Reset");
static ngx_str_t x_retry_after_header = ngx_string("Retry-After");


ngx_int_t
ngx_http_rate_limit_handler(ngx_http_request_t *r)
{
    ngx_int_t                        rc;
    ngx_http_upstream_t             *u;
    ngx_http_rate_limit_ctx_t       *ctx;
    ngx_http_rate_limit_loc_conf_t  *rlcf;
    ngx_str_t                        target;
    ngx_url_t                        url;

    rlcf = ngx_http_get_module_loc_conf(r, ngx_http_rate_limit_module);

    if (!rlcf->enable || r->main->internal) {
        return NGX_DECLINED;
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_rate_limit_module);

    if (ctx != NULL) {
        if (!ctx->done) {
            return NGX_AGAIN;
        }

        /* Return appropriate status */

        if (ctx->status == NGX_HTTP_TOO_MANY_REQUESTS) {
            return ctx->status;
        }

        if (ctx->status >= NGX_HTTP_OK
            && ctx->status < NGX_HTTP_SPECIAL_RESPONSE)
        {
            return NGX_OK;
        }

        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "rate limit unexpected status: %ui", ctx->status);

        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    if (ngx_http_upstream_create(r) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    u = r->upstream;

    if (rlcf->complex_target) {
        /* Variables used in the rate_limit_pass directive */

        if (ngx_http_complex_value(r, rlcf->complex_target, &target)
            != NGX_OK)
        {
            return NGX_ERROR;
        }

        if (target.len == 0) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "handler: empty \"rate_limit_pass\" target");
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        url.host = target;
        url.port = 0;
        url.no_resolve = 1;

        rlcf->upstream.upstream = ngx_http_rate_limit_upstream_add(r, &url);

        if (rlcf->upstream.upstream == NULL) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "rate limit: upstream \"%V\" not found", &target);

            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
    }

    ngx_str_set(&u->schema, "redis2://");
    u->output.tag = (ngx_buf_tag_t) &ngx_http_rate_limit_module;

    u->conf = &rlcf->upstream;

    u->create_request = ngx_http_rate_limit_create_request;
    u->reinit_request = ngx_http_rate_limit_reinit_request;
    u->process_header = ngx_http_rate_limit_process_header;
    u->abort_request = ngx_http_rate_limit_abort_request;
    u->finalize_request = ngx_http_rate_limit_finalize_request;

    ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_rate_limit_ctx_t));
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    ngx_http_set_ctx(r, ctx, ngx_http_rate_limit_module);

    u->input_filter_init = ngx_http_rate_limit_filter_init;
    u->input_filter = ngx_http_rate_limit_filter;

    /* The request filter context is the request object (ngx_request_t) */
    u->input_filter_ctx = r;

    /* Initiate the upstream connection by calling NGINX upstream. */
    rc = ngx_http_read_client_request_body(r, ngx_http_upstream_init);

    if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
        return rc;
    }

    return NGX_AGAIN;/*NGX_DONE;*/
}


static ngx_int_t
ngx_http_rate_limit_create_request(ngx_http_request_t *r)
{
    ngx_buf_t                       *b;
    ngx_chain_t                     *cl;
    ngx_int_t                        rc;

    rc = ngx_http_rate_limit_build_command(r, &b);
    if (rc != NGX_OK) {
        return rc;
    }

    /* Allocate a buffer chain for NGINX. */
    cl = ngx_alloc_chain_link(r->pool);
    if (cl == NULL) {
        return NGX_ERROR;
    }

    cl->buf = b;
    cl->next = NULL;

    /* We are only sending one buffer. */
    b->last_buf = 1;

    /* Attach the buffer to the request. */
    r->upstream->request_bufs = cl;

    return NGX_OK;
}


static ngx_int_t
ngx_http_rate_limit_reinit_request(ngx_http_request_t *r)
{
    return NGX_OK;
}


static ngx_int_t
ngx_http_rate_limit_process_header(ngx_http_request_t *r)
{
    ngx_http_upstream_t             *u;
    ngx_http_rate_limit_ctx_t       *ctx;
    ngx_buf_t                       *b;
    ngx_str_t                        buf;
    u_char                           *line, *crnl, *arg;
    ngx_uint_t                       lnum;

    u = r->upstream;
    b = &u->buffer;

    if (b->last - b->pos < (ssize_t) sizeof(u_char)) {
        return NGX_AGAIN;
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_rate_limit_module);
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    lnum = 0;

    for (line = b->pos; (crnl = (u_char *) ngx_strstr(line, "\r\n")) != NULL; line = crnl + 2) {
        ++lnum;
        if (lnum == 1) {
            /* the first char is the response header
               the second char is number of return args */
            if (*line  != '*' && *(line + 1) != '5') {
                buf.data = b->pos;
                buf.len = b->last - b->pos;

                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                              "rate limit: upstream sent invalid response: \"%V\"", &buf);

                return NGX_HTTP_UPSTREAM_INVALID_HEADER;
            }

            continue;
        }
        if ((arg = (u_char *) ngx_strchr(line, ':')) == NULL || ++arg > crnl) {
            /* does not start with colon? */

            buf.data = b->pos;
            buf.len = b->last - b->pos;

            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "rate limit: upstream sent invalid response: \"%V\"", &buf);

            return NGX_HTTP_UPSTREAM_INVALID_HEADER;
        }

        buf.data = arg;
        buf.len = crnl - arg;

        switch (lnum) {
            case 2:
                /* 0 indicates the action is allowed
                   1 indicates that the action was limited/blocked */
                if (*arg == '0') {
                    ctx->status = NGX_HTTP_OK;
                } else {
                    ctx->status = NGX_HTTP_TOO_MANY_REQUESTS;
                }
                break;
            case 3:
                /* X-RateLimit-Limit HTTP header */
                ngx_set_custom_header(r, &x_limit_header, &buf);
                break;
            case 4:
                /* X-RateLimit-Remaining HTTP header */
                 ngx_set_custom_header(r, &x_remaining_header, &buf);
                break;
            case 5:
                /* The number of seconds until the user should retry,
                   and always -1 if the action was allowed. */
                if (*arg != '-') {
                    ngx_set_custom_header(r, &x_retry_after_header, &buf);
                }
                break;
            case 6:
                /* X-RateLimit-Reset header */
                ngx_set_custom_header(r, &x_reset_header, &buf);
                break;
            default:
                buf.data = arg;
                buf.len = b->last - arg;

                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                              "rate limit: upstream sent extra bytes: \"%V\"", &buf);

                return NGX_HTTP_UPSTREAM_INVALID_HEADER;
        }
    }

    u->headers_in.status_n = NGX_HTTP_OK;
    u->state->status = NGX_HTTP_OK;

    u->length = 0;
    u->keepalive = 1;

    ctx->done = 1;

    return NGX_OK;
}


static ngx_int_t
ngx_http_rate_limit_filter_init(void *data)
{
    return NGX_OK;
}


static ngx_int_t
ngx_http_rate_limit_filter(void *data, ssize_t bytes)
{
    /*ngx_http_request_t   *r = data;

    ngx_buf_t            *b;
    ngx_chain_t          *cl, **ll;
    ngx_http_upstream_t  *u;

    u = r->upstream;

    for (cl = u->out_bufs, ll = &u->out_bufs; cl; cl = cl->next) {
        ll = &cl->next;
    }

    cl = ngx_chain_get_free_buf(r->pool, &u->free_bufs);
    if (cl == NULL) {
        return NGX_ERROR;
    }

    *ll = cl;

    cl->buf->flush = 1;
    cl->buf->memory = 1;

    b = &u->buffer;

    cl->buf->pos = b->last;
    b->last += bytes;
    cl->buf->last = b->last;
    cl->buf->tag = u->output.tag;*/

    return NGX_OK;
}


static void
ngx_http_rate_limit_abort_request(ngx_http_request_t *r)
{
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "abort http rate limit request");
    return;
}


static void
ngx_http_rate_limit_finalize_request(ngx_http_request_t *r, ngx_int_t rc)
{
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "finalize http rate limit request");

    if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
        r->headers_out.status = rc;
    }

    return;
}

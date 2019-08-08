#include "ngx_http_rate_limit_handler.h"
#include "ngx_http_rate_limit_reply.h"
#include "ngx_http_rate_limit_upstream.h"
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
    ngx_http_upstream_t            *u;
    ngx_http_rate_limit_ctx_t      *ctx;
    ngx_http_rate_limit_loc_conf_t *rlcf;
    size_t                          len;
    u_char                         *p, *n;
    ngx_uint_t                      status;
    ngx_str_t                       target;
    ngx_url_t                       url;

    rlcf = ngx_http_get_module_loc_conf(r, ngx_http_rate_limit_module);

    if (!rlcf->configured) {
        return NGX_DECLINED;
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_rate_limit_module);

    if (ctx != NULL) {
        if (!ctx->finalized) {
            return NGX_AGAIN;
        }

        status = r->upstream->state->status;

        /* Return appropriate status */

        if (status == NGX_HTTP_TOO_MANY_REQUESTS) {
            ngx_log_error(rlcf->limit_log_level, r->connection->log, 0,
                          "rate limit exceeded for key \"%V\"", &ctx->key);

            return rlcf->status_code;
        }

        if (status == NGX_HTTP_OK) {
            return NGX_OK;
        }

        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "rate limit unexpected status: %ui", status);

        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_rate_limit_ctx_t));
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    if (ngx_http_complex_value(r, &rlcf->key, &ctx->key) != NGX_OK) {
        return NGX_ERROR;
    }

    if (ctx->key.len == 0) {
        return NGX_DECLINED;
    }

    len = rlcf->prefix.len;

    if (len > 0) {
        n = ngx_pnalloc(r->pool, len + ctx->key.len + 2);
        if (n == NULL) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        p = ngx_cpymem(n, rlcf->prefix.data, len);
        p = ngx_cpymem(p, "_", 1);
        ngx_cpystrn(p, ctx->key.data, ctx->key.len + 2);

        ctx->key.len += len + 1;
        ctx->key.data = n;
    }

    if (ctx->key.len > 65535) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "the value of the \"%V\" key "
                      "is more than 65535 bytes: \"%V\"",
                      &rlcf->key.value, &ctx->key);
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ctx->request = r;

    if (ngx_http_upstream_create(r) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    u = r->upstream;

    if (rlcf->complex_target) {
        /* Variables used in the rate_limit_pass directive */

        if (ngx_http_complex_value(r, rlcf->complex_target, &target) !=
            NGX_OK) {
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

    ngx_http_set_ctx(r, ctx, ngx_http_rate_limit_module);

    u->input_filter_init = ngx_http_rate_limit_filter_init;
    u->input_filter = ngx_http_rate_limit_filter;
    u->input_filter_ctx = ctx;

    r->main->count++;

    /* Initiate the upstream connection by calling NGINX upstream. */
    ngx_http_upstream_init(r);

    /* Override the read event handler to our own */
    u->read_event_handler = ngx_http_rate_limit_rev_handler;

    return NGX_AGAIN;
}

static ngx_int_t
ngx_http_rate_limit_create_request(ngx_http_request_t *r)
{
    ngx_int_t    rc;
    ngx_buf_t   *b;
    ngx_chain_t *cl;

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
    ngx_http_upstream_t *u;

    u = r->upstream;

    /* Override the read event handler to our own */
    u->read_event_handler = ngx_http_rate_limit_rev_handler;

    return NGX_OK;
}

static ngx_int_t
ngx_http_rate_limit_process_header(ngx_http_request_t *r)
{
    ngx_http_upstream_t       *u;
    ngx_http_rate_limit_ctx_t *ctx;
    ngx_buf_t                 *b;
    u_char                     chr;
    ngx_str_t                  buf;

    u = r->upstream;
    b = &u->buffer;

    if (b->last - b->pos < (ssize_t) sizeof(u_char)) {
        return NGX_AGAIN;
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_rate_limit_module);
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    /* the first char is the response header */
    chr = *b->pos;

    /* we are always expecting a multi bulk reply */
    if (chr != '*') {
        buf.data = b->pos;
        buf.len = b->last - b->pos;

        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "rate limit: redis sent invalid response: \"%V\"", &buf);

        return NGX_HTTP_UPSTREAM_INVALID_HEADER;
    }

    ++b->pos;

    u->state->status = NGX_HTTP_OK;

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
    ngx_http_rate_limit_ctx_t *ctx = data;

    return ngx_http_rate_limit_process_reply(ctx, bytes);
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
    ngx_http_rate_limit_ctx_t      *ctx;
    ngx_http_rate_limit_loc_conf_t *rlcf;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "finalize http rate limit request");

    ctx = ngx_http_get_module_ctx(r, ngx_http_rate_limit_module);
    if (ctx == NULL) {
        return;
    }

    rlcf = ngx_http_get_module_loc_conf(r, ngx_http_rate_limit_module);

    if (r->upstream->state->status == NGX_HTTP_TOO_MANY_REQUESTS ||
        rlcf->enable_headers) {
        /* X-RateLimit-Limit HTTP header */
        (void) ngx_set_custom_header(r, &x_limit_header, ctx->limit);

        /* X-RateLimit-Remaining HTTP header */
        (void) ngx_set_custom_header(r, &x_remaining_header, ctx->remaining);

        /* X-RateLimit-Reset */
        (void) ngx_set_custom_header(r, &x_reset_header, ctx->reset);

        /* Retry-After (always -1 if the action was allowed) */
        if (ctx->retry_after != -1) {
            (void) ngx_set_custom_header(r, &x_retry_after_header,
                                         (ngx_uint_t) ctx->retry_after);
        }
    }

    ctx->finalized = 1;
}

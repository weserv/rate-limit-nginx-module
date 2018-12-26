#include "ngx_http_rate_limit_util.h"


static size_t ngx_get_num_size(uint64_t i);


ngx_http_upstream_srv_conf_t *
ngx_http_rate_limit_upstream_add(ngx_http_request_t *r, ngx_url_t *url)
{
    ngx_http_upstream_main_conf_t *umcf;
    ngx_http_upstream_srv_conf_t  **uscfp;
    ngx_uint_t                    i;

    umcf = ngx_http_get_module_main_conf(r, ngx_http_upstream_module);

    uscfp = umcf->upstreams.elts;

    for (i = 0; i < umcf->upstreams.nelts; i++) {

        if (uscfp[i]->host.len != url->host.len
            || ngx_strncasecmp(uscfp[i]->host.data, url->host.data,
                               url->host.len) != 0) {
            ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                           "upstream_add: host not match");
            continue;
        }

        if (uscfp[i]->port != url->port) {
            ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                           "upstream_add: port not match: %d != %d",
                           (int) uscfp[i]->port, (int) url->port);
            continue;
        }

#if defined(nginx_version) && nginx_version < 1011006
        if (uscfp[i]->default_port
            && url->default_port
            && uscfp[i]->default_port != url->default_port)
        {
            ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                           "upstream_add: default_port not match");
            continue;
        }
#endif

        return uscfp[i];
    }

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "no upstream found: %V", &url->host);

    return NULL;
}


static size_t
ngx_get_num_size(uint64_t i)
{
    size_t n = 0;

    do {
        i = i / 10;
        n++;
    } while (i > 0);

    return n;
}


ngx_int_t
ngx_http_rate_limit_build_command(ngx_http_request_t *r, ngx_buf_t **b)
{
    size_t                         len, arg_len;
    u_char                         *p;
    ngx_http_rate_limit_ctx_t      *ctx;
    ngx_http_rate_limit_loc_conf_t *rlcf;

    rlcf = ngx_http_get_module_loc_conf(r, ngx_http_rate_limit_module);

    ctx = ngx_http_get_module_ctx(r, ngx_http_rate_limit_module);
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    /* Accumulate buffer size. */
    len = 0;

    /* Example: "*5\r\n$11\r\nRATER.LIMIT\r\n$7\r\nuser123\r\n$2\r\n15\r\n$2\r\n30\r\n$2\r\n60\r\n" */

    /*The arity of the command */
    len += sizeof("*6") - 1;
    len += sizeof("\r\n") - 1;

    /* The length of the first argument in bytes */
    len += sizeof("$11") - 1;
    len += sizeof("\r\n") - 1;

    /* Command name */
    len += sizeof("RATER.LIMIT") - 1;
    len += sizeof("\r\n") - 1;

    /* <key> */
    len += sizeof("$") - 1;
    len += ngx_get_num_size(ctx->key.len);
    len += sizeof("\r\n") - 1;
    len += ctx->key.len;
    len += sizeof("\r\n") - 1;

    /* <max_burst> */
    arg_len = ngx_get_num_size(rlcf->burst);
    len += sizeof("$") - 1;
    len += ngx_get_num_size(arg_len);
    len += sizeof("\r\n") - 1;
    len += arg_len;
    len += sizeof("\r\n") - 1;

    /* <count per period> */
    arg_len = ngx_get_num_size(rlcf->rate);
    len += sizeof("$") - 1;
    len += ngx_get_num_size(arg_len);
    len += sizeof("\r\n") - 1;
    len += arg_len;
    len += sizeof("\r\n") - 1;

    /* <period> */
    arg_len = ngx_get_num_size(rlcf->period);
    len += sizeof("$") - 1;
    len += ngx_get_num_size(arg_len);
    len += sizeof("\r\n") - 1;
    len += arg_len;
    len += sizeof("\r\n") - 1;

    /* [<quantity>] */
    if (rlcf->quantity != 1) {
        arg_len = ngx_get_num_size(rlcf->quantity);
        len += sizeof("$") - 1;
        len += ngx_get_num_size(arg_len);
        len += sizeof("\r\n") - 1;
        len += arg_len;
        len += sizeof("\r\n") - 1;
    }

    *b = ngx_create_temp_buf(r->pool, len);
    if (*b == NULL) {
        return NGX_ERROR;
    }

    p = (*b)->last;

    *p++ = '*';
    *p++ = rlcf->quantity != 1 ? '6' : '5';
    *p++ = '\r';
    *p++ = '\n';

    *p++ = '$';
    *p++ = '1';
    *p++ = '1';
    *p++ = '\r';
    *p++ = '\n';
    p = ngx_cpymem(p, "RATER.LIMIT", sizeof("RATER.LIMIT") - 1);
    *p++ = '\r';
    *p++ = '\n';

    *p++ = '$';
    p = ngx_sprintf(p, "%uz", ctx->key.len);
    *p++ = '\r';
    *p++ = '\n';
    p = ngx_copy(p, ctx->key.data, ctx->key.len);
    *p++ = '\r';
    *p++ = '\n';

    *p++ = '$';
    p = ngx_sprintf(p, "%uz", ngx_get_num_size(rlcf->burst));
    *p++ = '\r';
    *p++ = '\n';
    p = ngx_sprintf(p, "%d", rlcf->burst);
    *p++ = '\r';
    *p++ = '\n';

    *p++ = '$';
    p = ngx_sprintf(p, "%uz", ngx_get_num_size(rlcf->rate));
    *p++ = '\r';
    *p++ = '\n';
    p = ngx_sprintf(p, "%d", rlcf->rate);
    *p++ = '\r';
    *p++ = '\n';

    *p++ = '$';
    p = ngx_sprintf(p, "%uz", ngx_get_num_size(rlcf->period));
    *p++ = '\r';
    *p++ = '\n';
    p = ngx_sprintf(p, "%d", rlcf->period);
    *p++ = '\r';
    *p++ = '\n';

    if (rlcf->quantity != 1) {
        *p++ = '$';
        p = ngx_sprintf(p, "%uz", ngx_get_num_size(rlcf->quantity));
        *p++ = '\r';
        *p++ = '\n';
        p = ngx_sprintf(p, "%d", rlcf->quantity);
        *p++ = '\r';
        *p++ = '\n';
    }

    if (p - (*b)->pos != (ssize_t) len) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "rate limit: buffer error %uz != %uz",
                      (size_t)(p - (*b)->pos), len);

        return NGX_ERROR;
    }

    (*b)->last = p;

    return NGX_OK;
}


ngx_int_t
ngx_set_custom_header(ngx_http_request_t *r, ngx_str_t *key, ngx_str_t *value)
{
    ngx_table_elt_t *h;

    h = ngx_list_push(&r->headers_out.headers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    h->key = *key;
    h->value = *value;

    /* Mark the header as not deleted. */
    h->hash = 1;

    h->lowcase_key = ngx_pnalloc(r->pool, h->key.len);
    if (h->lowcase_key == NULL) {
        return NGX_ERROR;
    }

    ngx_strlow(h->lowcase_key, h->key.data, h->key.len);

    return NGX_OK;
}

#ifndef NGX_HTTP_RATE_LIMIT_MODULE_H
#define NGX_HTTP_RATE_LIMIT_MODULE_H

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

extern ngx_module_t ngx_http_rate_limit_module;

typedef struct {
    ngx_flag_t               configured;
    ngx_http_complex_value_t key;

    ngx_http_upstream_conf_t  upstream;
    ngx_http_complex_value_t *complex_target; /* for rate_limit_pass */

    ngx_flag_t enable_headers;
    ngx_uint_t status_code;
    ngx_uint_t limit_log_level;

    ngx_str_t  prefix;
    ngx_uint_t requests;
    ngx_uint_t period;
    ngx_uint_t burst;
    ngx_uint_t quantity;
} ngx_http_rate_limit_loc_conf_t;

typedef struct {
    ngx_str_t key;

    ngx_http_request_t *request;

    /* used to parse the redis response */
    ngx_uint_t state;

    /* flag indicating whether the rate limit has been finalized */
    ngx_flag_t finalized;

    /* parsed variables from the redis response */
    ngx_uint_t limit;
    ngx_uint_t remaining;
    ngx_uint_t reset;
    ngx_int_t  retry_after;
} ngx_http_rate_limit_ctx_t;

#endif /* NGX_HTTP_RATE_LIMIT_MODULE_H */

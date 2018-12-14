#ifndef NGX_HTTP_RATE_LIMIT_MODULE_H
#define NGX_HTTP_RATE_LIMIT_MODULE_H


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


extern ngx_module_t  ngx_http_rate_limit_module;

typedef struct {
    ngx_flag_t                configured;
    ngx_http_complex_value_t  key;

    ngx_http_upstream_conf_t  upstream;
    ngx_http_complex_value_t *complex_target; /* for rate_limit_pass */

    ngx_flag_t                enable_headers;
    ngx_uint_t                status_code;
    ngx_uint_t                limit_log_level;

    ngx_str_t                 prefix;
    ngx_uint_t                rate;
    ngx_uint_t                period;
    ngx_uint_t                burst;
    ngx_uint_t                quantity;
} ngx_http_rate_limit_loc_conf_t;

typedef struct {
    ngx_str_t                 key;

    ngx_flag_t                done;
    ngx_uint_t                status;
} ngx_http_rate_limit_ctx_t;

#endif /* NGX_HTTP_RATE_LIMIT_MODULE_H */

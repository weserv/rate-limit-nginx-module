#ifndef NGX_HTTP_RATE_LIMIT_UTIL_H
#define NGX_HTTP_RATE_LIMIT_UTIL_H

#include "ngx_http_rate_limit_module.h"

ngx_http_upstream_srv_conf_t *ngx_http_rate_limit_upstream_add(
        ngx_http_request_t *r, ngx_url_t *url);
ngx_int_t ngx_http_rate_limit_build_command(ngx_http_request_t *r,
                                            ngx_buf_t **b);
ngx_int_t ngx_set_custom_header(ngx_http_request_t *r, ngx_str_t *key,
                                ngx_uint_t value);

#endif /* NGX_HTTP_RATE_LIMIT_UTIL_H */

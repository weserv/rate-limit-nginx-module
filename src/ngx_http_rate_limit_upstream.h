#ifndef NGX_HTTP_RATE_LIMIT_UPSTREAM_H
#define NGX_HTTP_RATE_LIMIT_UPSTREAM_H

#include <ngx_http.h>

void ngx_http_rate_limit_rev_handler(ngx_http_request_t *r,
                                     ngx_http_upstream_t *u);

#endif /* NGX_HTTP_RATE_LIMIT_UPSTREAM_H */

#ifndef NGX_HTTP_RATE_LIMIT_REPLY_H
#define NGX_HTTP_RATE_LIMIT_REPLY_H

#include "ngx_http_rate_limit_module.h"

ngx_int_t ngx_http_rate_limit_process_reply(ngx_http_rate_limit_ctx_t *ctx,
                                            ssize_t bytes);

#endif /* NGX_HTTP_RATE_LIMIT_REPLY_H */

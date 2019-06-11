#include "ngx_http_rate_limit_reply.h"

ngx_int_t
ngx_http_rate_limit_process_reply(ngx_http_rate_limit_ctx_t *ctx, ssize_t bytes)
{
    ngx_buf_t           *b;
    ngx_http_upstream_t *u;
    u_char               ch, *p;

    u = ctx->request->upstream;
    b = &u->buffer;

    enum {
        sw_start = 0,
        sw_CRLF1,
        sw_ARG1,
        sw_CRLF2,
        sw_ARG2,
        sw_LF1,
        sw_ARG3,
        sw_LF2,
        sw_ARG4,
        sw_LF3,
        sw_ARG5,
        sw_almost_done
    } state;

    state = ctx->state;

    b->pos = b->last;
    b->last += bytes;

    /* Example response:
     * "5\r\n:0\r\n:16\r\n:15\r\n:-1\r\n:2\r\n"
     * Note: the first multi bulk reply byte (`*`) is
     * checked within `u->process_header`.
     */

    for (p = b->pos; p < b->last; p++) {
        ch = *p;

        switch (state) {

        case sw_start:
            /* our bulk length must always be 5 */
            switch (ch) {
            case '5':
                state = sw_CRLF1;
                break;
            default:
                return NGX_ERROR;
            }
            break;

        case sw_CRLF1:
            switch (ch) {
            case CR:
                break;
            case LF:
                state = sw_ARG1;
                break;
            default:
                return NGX_ERROR;
            }
            break;

        case sw_ARG1:
            /* 0 indicates the action is allowed
             * 1 indicates that the action was limited/blocked */
            switch (ch) {
            case ':':
                break;
            case '0':
                u->state->status = NGX_HTTP_OK;
                state = sw_CRLF2;
                break;
            case '1':
                u->state->status = NGX_HTTP_TOO_MANY_REQUESTS;
                state = sw_CRLF2;
                break;
            default:
                return NGX_ERROR;
            }
            break;

        case sw_CRLF2:
            switch (ch) {
            case CR:
                break;
            case LF:
                state = sw_ARG2;
                break;
            default:
                return NGX_ERROR;
            }
            break;

        case sw_ARG2:
            /* X-RateLimit-Limit HTTP header */
            if (ch == ':') {
                break;
            }

            if (ch == CR) {
                state = sw_LF1;
                break;
            }

            if (ch < '0' || ch > '9') {
                return NGX_ERROR;
            }

            ctx->limit = ctx->limit * 10 + (ch - '0');

            break;

        case sw_LF1:
            switch (ch) {
            case LF:
                state = sw_ARG3;
                break;
            default:
                return NGX_ERROR;
            }
            break;

        case sw_ARG3:
            /* X-RateLimit-Remaining HTTP header */
            if (ch == ':') {
                break;
            }

            if (ch == CR) {
                state = sw_LF2;
                break;
            }

            if (ch < '0' || ch > '9') {
                return NGX_ERROR;
            }

            ctx->remaining = ctx->remaining * 10 + (ch - '0');

            break;

        case sw_LF2:
            switch (ch) {
            case CR:
                break;
            case LF:
                state = sw_ARG4;
                break;
            default:
                return NGX_ERROR;
            }
            break;

        case sw_ARG4:
            /* The number of seconds until the user should retry,
             * and always -1 if the action was allowed. */
            if (ch == ':') {
                break;
            }

            if (ch == CR) {
                state = sw_LF3;
                break;
            }

            if (u->state->status == NGX_HTTP_OK) {
                if (ch != '-' && ch != '1') {
                    return NGX_ERROR;
                }
                break;
            }

            if (ch < '0' || ch > '9') {
                return NGX_ERROR;
            }

            ctx->retry_after = ctx->retry_after * 10 + (ch - '0');

            break;

        case sw_LF3:
            switch (ch) {
            case LF:
                state = sw_ARG5;
                break;
            default:
                return NGX_ERROR;
            }
            break;

        case sw_ARG5:
            /* X-RateLimit-Reset HTTP header */
            if (ch == ':') {
                break;
            }

            if (ch == CR) {
                state = sw_almost_done;
                break;
            }

            if (ch < '0' || ch > '9') {
                return NGX_ERROR;
            }

            ctx->reset = ctx->reset * 10 + (ch - '0');

            break;

        case sw_almost_done:
            /* End of redis response */
            switch (ch) {
            case LF:
                goto done;
            default:
                return NGX_ERROR;
            }
        }
    }
    
    b->pos = p;
    ctx->state = state;

    return NGX_AGAIN;

done:

    b->pos = p + 1;

    u->keepalive = 1;
    u->length = 0;

    return NGX_OK;
}
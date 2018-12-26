#include "ngx_http_rate_limit_module.h"
#include "ngx_http_rate_limit_handler.h"
#include "ngx_http_rate_limit_util.h"

static ngx_int_t ngx_http_rate_limit_init(ngx_conf_t *cf);
static void *ngx_http_rate_limit_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_rate_limit_merge_loc_conf(ngx_conf_t *cf,
                                                void *parent, void *child);
static char *ngx_http_rate_limit(ngx_conf_t *cf, ngx_command_t *cmd,
                                 void *conf);
static char *ngx_http_rate_limit_pass(ngx_conf_t *cf, ngx_command_t *cmd,
                                      void *conf);


static ngx_conf_num_bounds_t ngx_http_rate_limit_status_bounds = {
        ngx_conf_check_num_bounds, 400, 599
};


static ngx_command_t ngx_http_rate_limit_commands[] = {

        { ngx_string("rate_limit"),
          NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE123,
          ngx_http_rate_limit,
          NGX_HTTP_LOC_CONF_OFFSET,
          0,
          NULL },

        { ngx_string("rate_limit_prefix"),
          NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
          ngx_conf_set_str_slot,
          NGX_HTTP_LOC_CONF_OFFSET,
          offsetof(ngx_http_rate_limit_loc_conf_t, prefix),
          NULL },

        { ngx_string("rate_limit_quantity"),
          NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
          ngx_conf_set_num_slot,
          NGX_HTTP_LOC_CONF_OFFSET,
          offsetof(ngx_http_rate_limit_loc_conf_t, quantity),
          NULL },

        { ngx_string("rate_limit_pass"),
          NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
          ngx_http_rate_limit_pass,
          NGX_HTTP_LOC_CONF_OFFSET,
          0,
          NULL },

        { ngx_string("rate_limit_headers"),
          NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_FLAG,
          ngx_conf_set_flag_slot,
          NGX_HTTP_LOC_CONF_OFFSET,
          offsetof(ngx_http_rate_limit_loc_conf_t, enable_headers),
          NULL },

        { ngx_string("rate_limit_status"),
          NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
          ngx_conf_set_num_slot,
          NGX_HTTP_LOC_CONF_OFFSET,
          offsetof(ngx_http_rate_limit_loc_conf_t, status_code),
          &ngx_http_rate_limit_status_bounds },

        { ngx_string("rate_limit_connect_timeout"),
          NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
          ngx_conf_set_msec_slot,
          NGX_HTTP_LOC_CONF_OFFSET,
          offsetof(ngx_http_rate_limit_loc_conf_t, upstream.connect_timeout),
          NULL },

        { ngx_string("rate_limit_send_timeout"),
          NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
          ngx_conf_set_msec_slot,
          NGX_HTTP_LOC_CONF_OFFSET,
          offsetof(ngx_http_rate_limit_loc_conf_t, upstream.send_timeout),
          NULL },

        { ngx_string("rate_limit_buffer_size"),
          NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
          ngx_conf_set_size_slot,
          NGX_HTTP_LOC_CONF_OFFSET,
          offsetof(ngx_http_rate_limit_loc_conf_t, upstream.buffer_size),
          NULL },

        { ngx_string("rate_limit_read_timeout"),
          NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
          ngx_conf_set_msec_slot,
          NGX_HTTP_LOC_CONF_OFFSET,
          offsetof(ngx_http_rate_limit_loc_conf_t, upstream.read_timeout),
          NULL },

        ngx_null_command
};


static ngx_http_module_t ngx_http_rate_limit_module_ctx = {
        NULL,                                  /* preconfiguration */
        ngx_http_rate_limit_init,              /* postconfiguration */

        NULL,                                  /* create main configuration */
        NULL,                                  /* init main configuration */

        NULL,                                  /* create server configuration */
        NULL,                                  /* merge server configuration */

        ngx_http_rate_limit_create_loc_conf,   /* create location configration */
        ngx_http_rate_limit_merge_loc_conf     /* merge location configration */
};


ngx_module_t ngx_http_rate_limit_module = {
        NGX_MODULE_V1,
        &ngx_http_rate_limit_module_ctx,       /* module context */
        ngx_http_rate_limit_commands,          /* module directives */
        NGX_HTTP_MODULE,                       /* module type */
        NULL,                                  /* init master */
        NULL,                                  /* init module */
        NULL,                                  /* init process */
        NULL,                                  /* init thread */
        NULL,                                  /* exit thread */
        NULL,                                  /* exit process */
        NULL,                                  /* exit master */
        NGX_MODULE_V1_PADDING
};


static void *
ngx_http_rate_limit_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_rate_limit_loc_conf_t *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_rate_limit_loc_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    /*
     * set by ngx_pcalloc():
     *
     *     conf->upstream.bufs.num = 0;
     *     conf->upstream.next_upstream = 0;
     *     conf->upstream.temp_path = NULL;
     *     conf->upstream.uri = { 0, NULL };
     *     conf->upstream.location = NULL;
     *
     *     conf->prefix = { 0, NULL };
     */

    conf->upstream.connect_timeout = NGX_CONF_UNSET_MSEC;
    conf->upstream.send_timeout = NGX_CONF_UNSET_MSEC;
    conf->upstream.read_timeout = NGX_CONF_UNSET_MSEC;

    conf->upstream.buffer_size = NGX_CONF_UNSET_SIZE;

    /* the hardcoded values */
    conf->upstream.cyclic_temp_file = 0;
    conf->upstream.buffering = 0;
    conf->upstream.ignore_client_abort = 1;
    conf->upstream.send_lowat = 0;
    conf->upstream.bufs.num = 0;
    conf->upstream.busy_buffers_size = 0;
    conf->upstream.max_temp_file_size = 0;
    conf->upstream.temp_file_write_size = 0;
    conf->upstream.intercept_errors = 1;
    conf->upstream.intercept_404 = 1;
    conf->upstream.pass_request_headers = 0;
    conf->upstream.pass_request_body = 0;

    conf->enable_headers = NGX_CONF_UNSET;
    conf->status_code = NGX_CONF_UNSET_UINT;

    conf->quantity = NGX_CONF_UNSET_UINT;

    return conf;
}


static char *
ngx_http_rate_limit_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_rate_limit_loc_conf_t *prev = parent;
    ngx_http_rate_limit_loc_conf_t *conf = child;

    ngx_conf_merge_msec_value(conf->upstream.connect_timeout,
                              prev->upstream.connect_timeout, 60000);

    ngx_conf_merge_msec_value(conf->upstream.send_timeout,
                              prev->upstream.send_timeout, 60000);

    ngx_conf_merge_msec_value(conf->upstream.read_timeout,
                              prev->upstream.read_timeout, 60000);

    ngx_conf_merge_size_value(conf->upstream.buffer_size,
                              prev->upstream.buffer_size,
                              (size_t) ngx_pagesize);

    if (conf->upstream.upstream == NULL) {
        conf->upstream.upstream = prev->upstream.upstream;
    }

    ngx_conf_merge_value(conf->enable_headers, prev->enable_headers, 0);
    ngx_conf_merge_uint_value(conf->status_code, prev->status_code,
                              NGX_HTTP_TOO_MANY_REQUESTS);

    ngx_conf_merge_str_value(conf->prefix, prev->prefix, "");
    ngx_conf_merge_uint_value(conf->quantity, prev->quantity, 1);

    return NGX_CONF_OK;
}


static char *
ngx_http_rate_limit(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_rate_limit_loc_conf_t *lrcf = conf;

    u_char                           *p;
    size_t                           len;
    ngx_str_t                        *value;
    ngx_int_t                        rate, period, burst;
    ngx_uint_t                       i;
    ngx_http_compile_complex_value_t ccv;

    value = cf->args->elts;

    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));

    ccv.cf = cf;
    ccv.value = &value[1];
    ccv.complex_value = &lrcf->key;

    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    rate = 1;
    period = 1;
    burst = 0;

    for (i = 2; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "rate=", 5) == 0) {

            len = value[i].len;
            p = value[i].data + len - 3;

            if (ngx_strncmp(p, "r/s", 3) == 0) {
                period = 1;
                len -= 3;
            } else if (ngx_strncmp(p, "r/m", 3) == 0) {
                period = 60;
                len -= 3;
            } else if (ngx_strncmp(p, "r/h", 3) == 0) {
                period = 3600;
                len -= 3;
            }

            rate = ngx_atoi(value[i].data + 5, len - 5);
            if (rate <= 0) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid rate \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "burst=", 6) == 0) {

            burst = ngx_atoi(value[i].data + 6, value[i].len - 6);
            if (burst <= 0) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid burst value \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            continue;
        }

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid parameter \"%V\"", &value[i]);
        return NGX_CONF_ERROR;
    }

    lrcf->rate = rate;
    lrcf->period = period;
    lrcf->burst = burst;

    return NGX_CONF_OK;
}


static char *
ngx_http_rate_limit_pass(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_rate_limit_loc_conf_t *rlcf = conf;

    ngx_str_t  *value;
    ngx_uint_t n;
    ngx_url_t  url;

    ngx_http_compile_complex_value_t ccv;

    if (rlcf->upstream.upstream) {
        return "is duplicate";
    }

    value = cf->args->elts;

    n = ngx_http_script_variables_count(&value[1]);
    if (n) {
        rlcf->complex_target = ngx_palloc(cf->pool,
                                          sizeof(ngx_http_complex_value_t));

        if (rlcf->complex_target == NULL) {
            return NGX_CONF_ERROR;
        }

        ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));
        ccv.cf = cf;
        ccv.value = &value[1];
        ccv.complex_value = rlcf->complex_target;

        if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
            return NGX_CONF_ERROR;
        }

        rlcf->configured = 1;

        return NGX_CONF_OK;
    }

    rlcf->complex_target = NULL;

    ngx_memzero(&url, sizeof(ngx_url_t));

    url.url = value[1];
    url.no_resolve = 1;

    rlcf->upstream.upstream = ngx_http_upstream_add(cf, &url, 0);
    if (rlcf->upstream.upstream == NULL) {
        return NGX_CONF_ERROR;
    }

    rlcf->configured = 1;

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_rate_limit_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt       *h;
    ngx_http_core_main_conf_t *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);
    h = ngx_array_push(&cmcf->phases[NGX_HTTP_PREACCESS_PHASE].handlers);

    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_rate_limit_handler;

    return NGX_OK;
}

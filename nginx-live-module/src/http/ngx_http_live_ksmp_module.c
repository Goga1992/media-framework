#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include "../ngx_live.h"
#include "../ngx_live_media_info.h"
#include "../ngx_live_segment_cache.h"
#include "../ngx_live_timeline.h"
#include "../ngx_live_filler.h"


#define ngx_all_set(mask, f)  (((mask) & (f)) == (f))


static ngx_int_t ngx_http_live_ksmp_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
static ngx_int_t ngx_http_live_ksmp_uint32_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);

static ngx_int_t ngx_http_live_ksmp_add_variables(ngx_conf_t *cf);

static void *ngx_http_live_ksmp_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_live_ksmp_merge_loc_conf(ngx_conf_t *cf, void *parent,
    void *child);

static char *ngx_http_live_ksmp(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);


typedef ngx_int_t (*ngx_http_live_ksmp_args_handler_pt)(void *data,
    ngx_str_t *key, ngx_str_t *value);


typedef struct {
    ngx_int_t                 level;
} ngx_http_live_ksmp_loc_conf_t;


typedef struct {
    ngx_chain_t              *out;
    ngx_chain_t             **last;
    size_t                    size;
    size_t                    padding;
    ngx_str_t                 source;
    ngx_str_t                 err_msg;
    uint32_t                  err_code;
} ngx_http_live_ksmp_ctx_t;


typedef struct {
    ngx_http_request_t       *r;

    ngx_str_t                 channel_id;
    ngx_str_t                 timeline_id;
    ngx_str_t                 variant_ids;
    uint32_t                  media_type_mask;
    int64_t                   time;
    uint32_t                  segment_index;
    size_t                    padding;
    uint32_t                  flags;

    uint32_t                  err_code;
    ngx_str_t                 err_msg;
    u_char                    err_buf[NGX_MAX_ERROR_STR];  /* last */
} ngx_http_live_ksmp_params_t;


static ngx_conf_num_bounds_t  ngx_http_live_ksmp_comp_level_bounds = {
    ngx_conf_check_num_bounds, 1, 9
};

static ngx_command_t  ngx_http_live_ksmp_commands[] = {

    { ngx_string("live_ksmp"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_live_ksmp,
      0,
      0,
      NULL },

    { ngx_string("live_ksmp_comp_level"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_live_ksmp_loc_conf_t, level),
      &ngx_http_live_ksmp_comp_level_bounds },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_live_ksmp_module_ctx = {
    ngx_http_live_ksmp_add_variables,       /* preconfiguration */
    NULL,                                   /* postconfiguration */

    NULL,                                   /* create main configuration */
    NULL,                                   /* init main configuration */

    NULL,                                   /* create server configuration */
    NULL,                                   /* merge server configuration */

    ngx_http_live_ksmp_create_loc_conf,     /* create location configuration */
    ngx_http_live_ksmp_merge_loc_conf       /* merge location configuration */
};


ngx_module_t  ngx_http_live_ksmp_module = {
    NGX_MODULE_V1,
    &ngx_http_live_ksmp_module_ctx,         /* module context */
    ngx_http_live_ksmp_commands,            /* module directives */
    NGX_HTTP_MODULE,                        /* module type */
    NULL,                                   /* init master */
    NULL,                                   /* init module */
    NULL,                                   /* init process */
    NULL,                                   /* init thread */
    NULL,                                   /* exit thread */
    NULL,                                   /* exit process */
    NULL,                                   /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_http_variable_t  ngx_http_live_ksmp_vars[] = {

    { ngx_string("live_ksmp_source"), NULL, ngx_http_live_ksmp_variable,
      offsetof(ngx_http_live_ksmp_ctx_t, source), 0, 0 },

    { ngx_string("live_ksmp_err_msg"), NULL, ngx_http_live_ksmp_variable,
      offsetof(ngx_http_live_ksmp_ctx_t, err_msg), 0, 0 },

    { ngx_string("live_ksmp_err_code"), NULL,
      ngx_http_live_ksmp_uint32_variable,
      offsetof(ngx_http_live_ksmp_ctx_t, err_code), 0, 0 },

      ngx_http_null_variable
};


static ngx_str_t  ngx_http_live_ksmp_type =
    ngx_string("application/octet-stream");

static ngx_str_t  ngx_http_live_ksmp_filler_source = ngx_string("filler");


static ngx_int_t
ngx_http_live_ksmp_args_parse(ngx_http_request_t *r,
    ngx_http_live_ksmp_args_handler_pt handler, void *data)
{
    u_char      *buf;
    u_char      *p, *start;
    u_char      *src, *dst;
    ngx_int_t    rc;
    ngx_str_t    key;
    ngx_str_t    value;
    ngx_flag_t   unescape;

    buf = ngx_pnalloc(r->pool, r->args.len + 2);
    if (buf == NULL) {
        ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
            "ngx_http_live_ksmp_args_parse: alloc failed");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    p = ngx_copy(buf, r->args.data, r->args.len);
    *p++ = '&';
    *p++ = '\0';

    p = buf;

    unescape = 0;
    key.len = 0;
    start = p;

    for ( ;; ) {

        switch (*p) {

        case '\0':
            return NGX_OK;

        case '=':
            if (key.len != 0) {
                p++;
                break;
            }

            if (unescape) {
                src = dst = start;
                ngx_unescape_uri(&dst, &src, p - start, 0);
                unescape = 0;

            } else {
                dst = p;
            }

            key.data = start;
            key.len = dst - start;

            p++;    /* skip '=' */

            start = p;
            break;

        case '&':
            if (unescape) {
                src = dst = start;
                ngx_unescape_uri(&dst, &src, p - start, 0);
                unescape = 0;

            } else {
                dst = p;
            }

            if (key.len) {
                value.data = start;
                value.len = dst - start;

            } else {
                key.data = start;
                key.len = dst - start;
                value.len = 0;
            }

            rc = handler(data, &key, &value);
            if (rc != NGX_OK) {
                return rc;
            }

            key.len = 0;

            p++;    /* skip '&' */

            start = p;
            break;

        case '+':
            *p++ = ' ';
            break;

        case '%':
            unescape = 1;
            /* fall through */

        default:
            p++;
        }
    }
}


static ngx_int_t
ngx_http_live_ksmp_args_handler(void *data, ngx_str_t *key, ngx_str_t *value)
{
    ngx_int_t                     int_val;
    ngx_http_live_ksmp_params_t  *params = data;

    if (key->len == sizeof("channel_id") - 1 &&
        ngx_memcmp(key->data, "channel_id", sizeof("channel_id") - 1) == 0)
    {
        params->channel_id = *value;

    } else if (key->len == sizeof("timeline_id") - 1 &&
        ngx_memcmp(key->data, "timeline_id", sizeof("timeline_id") - 1) == 0)
    {
        params->timeline_id = *value;

    } else if (key->len == sizeof("variant_ids") - 1 &&
        ngx_memcmp(key->data, "variant_ids", sizeof("variant_ids") - 1) == 0)
    {
        params->variant_ids = *value;

    } else if (key->len == sizeof("media_type_mask") - 1 &&
        ngx_memcmp(key->data, "media_type_mask", sizeof("media_type_mask") - 1)
        == 0)
    {
        int_val = ngx_hextoi(value->data, value->len);
        if (int_val == NGX_ERROR) {
            ngx_log_error(NGX_LOG_ERR, params->r->connection->log, 0,
                "ngx_http_live_ksmp_args_handler: "
                "invalid media_type_mask \"%V\"", value);
            return NGX_HTTP_BAD_REQUEST;
        }

        params->media_type_mask &= int_val;
        if (!params->media_type_mask) {
            ngx_log_error(NGX_LOG_ERR, params->r->connection->log, 0,
                "ngx_http_live_ksmp_args_handler: "
                "empty media_type_mask \"%V\"", value);
            return NGX_HTTP_BAD_REQUEST;
        }

    } else if (key->len == sizeof("time") - 1 &&
        ngx_memcmp(key->data, "time", sizeof("time") - 1)
        == 0)
    {
        int_val = ngx_atoi(value->data, value->len);
        if (int_val == NGX_ERROR) {
            ngx_log_error(NGX_LOG_ERR, params->r->connection->log, 0,
                "ngx_http_live_ksmp_args_handler: "
                "invalid time \"%V\"", value);
            return NGX_HTTP_BAD_REQUEST;
        }

        if (int_val >= NGX_KSMP_INVALID_TIMESTAMP) {
            ngx_log_error(NGX_LOG_ERR, params->r->connection->log, 0,
                "ngx_http_live_ksmp_args_handler: "
                "time \"%V\" too large", value);
            return NGX_HTTP_BAD_REQUEST;
        }

        params->time = int_val;


    } else if (key->len == sizeof("segment_index") - 1 &&
        ngx_memcmp(key->data, "segment_index", sizeof("segment_index") - 1)
        == 0)
    {
        int_val = ngx_atoi(value->data, value->len);
        if (int_val == NGX_ERROR) {
            ngx_log_error(NGX_LOG_ERR, params->r->connection->log, 0,
                "ngx_http_live_ksmp_args_handler: "
                "invalid segment_index \"%V\"", value);
            return NGX_HTTP_BAD_REQUEST;
        }

        if (int_val >= NGX_KSMP_INVALID_SEGMENT_INDEX) {
            ngx_log_error(NGX_LOG_ERR, params->r->connection->log, 0,
                "ngx_http_live_ksmp_args_handler: "
                "segment_index \"%V\" too large", value);
            return NGX_HTTP_BAD_REQUEST;
        }

        params->segment_index = int_val;

    } else if (key->len == sizeof("padding") - 1 &&
        ngx_memcmp(key->data, "padding", sizeof("padding") - 1) == 0)
    {
        int_val = ngx_atoi(value->data, value->len);
        if (int_val == NGX_ERROR) {
            ngx_log_error(NGX_LOG_ERR, params->r->connection->log, 0,
                "ngx_http_live_ksmp_args_handler: "
                "invalid padding \"%V\"", value);
            return NGX_HTTP_BAD_REQUEST;
        }

        params->padding = int_val;

    } else if (key->len == sizeof("flags") - 1 &&
        ngx_memcmp(key->data, "flags", sizeof("flags") - 1) == 0)
    {
        int_val = ngx_hextoi(value->data, value->len);
        if (int_val == NGX_ERROR) {
            ngx_log_error(NGX_LOG_ERR, params->r->connection->log, 0,
                "ngx_http_live_ksmp_args_handler: "
                "invalid flags \"%V\"", value);
            return NGX_HTTP_BAD_REQUEST;
        }

        params->flags = int_val;
    }

    return NGX_OK;
}


static void
ngx_http_live_ksmp_set_error(ngx_http_live_ksmp_params_t *params,
    uint32_t code, const char *fmt, ...)
{
    u_char     *p, *last;
    va_list     args;
    ngx_str_t   err_msg;
    u_char      err_buf[NGX_MAX_ERROR_STR];

    /* in case of multiple errors, keep the one with higher prio */
    if (!params->err_code || code < params->err_code) {
        err_msg.data = params->err_buf;
        last = params->err_buf + sizeof(params->err_buf);

    } else {
        err_msg.data = err_buf;
        last = err_buf + sizeof(err_buf);
    }

    va_start(args, fmt);
    p = ngx_vslprintf(err_msg.data, last, fmt, args);
    va_end(args);

    err_msg.len = p - err_msg.data;

    ngx_log_error(NGX_LOG_INFO, params->r->connection->log, 0,
        "ngx_http_live_ksmp_set_error: %V", &err_msg);

    if (err_msg.data == params->err_buf) {
        params->err_code = code;
        params->err_msg = err_msg;
    }
}


static ngx_int_t
ngx_http_live_ksmp_parse(ngx_http_request_t *r,
    ngx_http_live_ksmp_params_t *params)
{
    ngx_int_t  rc;

    ngx_memzero(params, offsetof(ngx_http_live_ksmp_params_t, err_buf));
    params->r = r;
    params->media_type_mask = KMP_MEDIA_TYPE_MASK;
    params->segment_index = NGX_KSMP_INVALID_SEGMENT_INDEX;
    params->time = NGX_KSMP_INVALID_TIMESTAMP;

    rc = ngx_http_live_ksmp_args_parse(r, ngx_http_live_ksmp_args_handler,
        params);
    if (rc != NGX_OK) {
        return rc;
    }

    if (!params->flags) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
            "ngx_http_live_ksmp_parse: missing \"flags\" arg");
        return NGX_HTTP_BAD_REQUEST;
    }

    if (!params->channel_id.data) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
            "ngx_http_live_ksmp_parse: missing \"channel_id\" arg");
        return NGX_HTTP_BAD_REQUEST;
    }

    if (!params->timeline_id.data) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
            "ngx_http_live_ksmp_parse: missing \"timeline_id\" arg");
        return NGX_HTTP_BAD_REQUEST;
    }

    if (params->segment_index != NGX_KSMP_INVALID_SEGMENT_INDEX &&
        params->time != NGX_KSMP_INVALID_TIMESTAMP)
    {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
            "ngx_http_live_ksmp_parse: "
            "request includes both \"segment_index\" and \"time\"");
        return NGX_HTTP_BAD_REQUEST;
    }

    if (ngx_all_set(params->flags,
        NGX_KSMP_FLAG_TIME_START_RELATIVE | NGX_KSMP_FLAG_TIME_END_RELATIVE))
    {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
            "ngx_http_live_ksmp_parse: "
            "request includes both start-relative and end-relative flags");
        return NGX_HTTP_BAD_REQUEST;
    }

    if (params->segment_index == NGX_KSMP_INVALID_SEGMENT_INDEX &&
        params->time == NGX_KSMP_INVALID_TIMESTAMP &&
        (params->flags & NGX_KSMP_FLAG_MEDIA))
    {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
            "ngx_http_live_ksmp_parse: "
            "missing \"segment_index\" arg when requesting media");
        return NGX_HTTP_BAD_REQUEST;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_live_ksmp_output(ngx_http_request_t *r, ngx_uint_t flags)
{
    ngx_int_t                  rc;
    ngx_http_live_ksmp_ctx_t  *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_live_ksmp_module);

    r->headers_out.content_type = ngx_http_live_ksmp_type;
    r->headers_out.content_length_n = ctx->size;
    r->headers_out.status = NGX_HTTP_OK;

    rc = ngx_http_send_header(r);
    if (rc == NGX_ERROR || rc > NGX_OK) {
        ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
            "ngx_http_live_ksmp_output: send header failed %i", rc);
        return rc;
    }

    if (r->header_only) {
        return NGX_HTTP_OK;     /* != NGX_OK to stop execution */
    }

    rc = ngx_http_output_filter(r, ctx->out);
    ctx->out = NULL;

    if (rc == NGX_ERROR || rc >= NGX_HTTP_SPECIAL_RESPONSE) {
        ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
            "ngx_http_live_ksmp_output: output filter failed %i", rc);
        return rc;
    }

    if (!flags) {
        return NGX_OK;
    }

    rc = ngx_http_send_special(r, flags);
    if (rc == NGX_ERROR || rc >= NGX_HTTP_SPECIAL_RESPONSE) {
        ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
            "ngx_http_live_ksmp_output: send special failed %i", rc);
        return rc;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_live_ksmp_write_padding(ngx_http_request_t *r)
{
    ngx_int_t                    rc;
    ngx_buf_t                   *b;
    ngx_chain_t                 *cl;
    ngx_http_live_ksmp_ctx_t    *ctx;
    ngx_persist_block_header_t  *header;

    ctx = ngx_http_get_module_ctx(r, ngx_http_live_ksmp_module);
    if (!ctx->padding) {
        return NGX_OK;
    }

    b = ngx_create_temp_buf(r->pool, ctx->padding);
    if (b == NULL) {
        ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
            "ngx_http_live_ksmp_write_padding: alloc buf failed");
        return NGX_ERROR;
    }

    cl = ngx_alloc_chain_link(r->pool);
    if (cl == NULL) {
        ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
            "ngx_http_live_ksmp_write_padding: alloc chain failed");
        return NGX_ERROR;
    }

    cl->buf = b;
    cl->next = NULL;

    header = (void *) b->pos;
    header->id = NGX_KSMP_BLOCK_PADDING;
    header->size = b->end - b->pos;
    header->header_size = sizeof(*header);
    b->last = (void *) (header + 1);

    ngx_memzero(b->last, b->end - b->last);
    b->last = b->end;

    rc = ngx_http_output_filter(r, cl);
    if (rc == NGX_ERROR || rc >= NGX_HTTP_SPECIAL_RESPONSE) {
        ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
            "ngx_http_live_ksmp_write_padding: output filter failed %i", rc);
        return rc;
    }

    return rc;
}


static ngx_int_t
ngx_http_live_ksmp_segment_set_size(void *arg, size_t size)
{
    ngx_int_t                  rc;
    ngx_http_request_t        *r = arg;
    ngx_http_live_ksmp_ctx_t  *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_live_ksmp_module);

    ctx->size += size;

    rc = ngx_http_live_ksmp_output(r, 0);
    if (rc != NGX_OK) {
        ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
            "ngx_http_live_ksmp_segment_set_size: output failed %i", rc);
        return rc;
    }

    ngx_http_run_posted_requests(r->connection);

    return NGX_OK;
}

static ngx_int_t
ngx_http_live_ksmp_segment_write(void *arg, ngx_chain_t *cl)
{
    ngx_int_t            rc;
    ngx_http_request_t  *r = arg;

    rc = ngx_http_output_filter(r, cl);
    if (rc == NGX_ERROR || rc >= NGX_HTTP_SPECIAL_RESPONSE) {
        ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
            "ngx_http_live_ksmp_segment_write: output filter failed %i", rc);
        return rc;
    }

    ngx_http_run_posted_requests(r->connection);

    return NGX_OK;
}

static void
ngx_http_live_ksmp_segment_close(void *arg, ngx_int_t rc)
{
    ngx_connection_t    *c;
    ngx_http_request_t  *r = arg;

    c = r->connection;

    if (rc != NGX_OK) {
        if (r->header_sent) {
            rc = NGX_ERROR;
        }
        goto done;
    }

    rc = ngx_http_live_ksmp_write_padding(r);
    if (rc != NGX_OK) {
        rc = NGX_ERROR;
        goto done;
    }

    rc = ngx_http_send_special(r, NGX_HTTP_LAST);
    if (rc == NGX_ERROR || rc >= NGX_HTTP_SPECIAL_RESPONSE) {
        ngx_log_error(NGX_LOG_NOTICE, c->log, 0,
            "ngx_http_live_ksmp_segment_close: send special failed %i",
            rc);
    }

done:

    ngx_http_finalize_request(r, rc);

    ngx_http_run_posted_requests(c);
}


static ngx_int_t
ngx_http_live_ksmp_output_error_str(ngx_http_request_t *r, uint32_t code,
    ngx_str_t *message)
{
    ngx_wstream_t             *ws;
    ngx_persist_write_ctx_t   *write_ctx;
    ngx_http_live_ksmp_ctx_t  *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_live_ksmp_module);

    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
        "ngx_http_live_ksmp_output_error_str: %V, code: %uD", message, code);

    ctx->err_msg.data = ngx_pnalloc(r->pool, message->len);
    if (ctx->err_msg.data != NULL) {
        ngx_memcpy(ctx->err_msg.data, message->data, message->len);
        ctx->err_msg.len = message->len;
    }
    ctx->err_code = code;

    write_ctx = ngx_persist_write_init(r->pool, NGX_KSMP_PERSIST_TYPE, 0);
    if (write_ctx == NULL) {
        ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
            "ngx_http_live_ksmp_output_error_str: write init failed");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ws = ngx_persist_write_stream(write_ctx);

    if (ngx_persist_write_block_open(write_ctx,
            NGX_KSMP_BLOCK_ERROR) != NGX_OK ||
        ngx_persist_write(write_ctx, &code, sizeof(code)) != NGX_OK ||
        ngx_wstream_str(ws, message) != NGX_OK)
    {
        ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
            "ngx_http_live_ksmp_output_error_str: write failed");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ngx_persist_write_block_close(write_ctx);      /* error */

    ctx->out = ngx_persist_write_close(write_ctx, &ctx->size, NULL);
    if (ctx->out == NULL) {
        ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
            "ngx_http_live_ksmp_output_error_str: close failed");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    return ngx_http_live_ksmp_output(r, NGX_HTTP_LAST);
}

static ngx_int_t
ngx_http_live_ksmp_output_error(ngx_http_request_t *r, uint32_t code,
    const char *fmt, ...)
{
    u_char     *p;
    va_list     args;
    ngx_str_t   message;
    u_char      buf[NGX_MAX_ERROR_STR];

    va_start(args, fmt);
    p = ngx_vslprintf(buf, buf + sizeof(buf), fmt, args);
    va_end(args);

    message.data = buf;
    message.len = p - buf;

    return ngx_http_live_ksmp_output_error_str(r, code, &message);
}


static ngx_int_t
ngx_http_live_ksmp_add_track_ref(ngx_live_persist_serve_scope_t *scope,
    uint32_t track_id, ngx_live_track_t *track)
{
    ngx_uint_t             i, n;
    ngx_array_t           *refs = scope->track_refs;
    ngx_live_track_ref_t  *ref;

    ref = refs->elts;
    n = refs->nelts;
    for (i = 0; i < n; i++) {
        if (ref[i].id == track_id) {
            return NGX_OK;
        }
    }

    ref = ngx_array_push(refs);
    if (ref == NULL) {
        ngx_log_error(NGX_LOG_NOTICE, refs->pool->log, 0,
            "ngx_http_live_ksmp_add_track_ref: push failed");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ref->id = track_id;
    if (track_id == track->in.key) {
        ref->track = track;

    } else {
        ref->track = ngx_live_track_get_by_int(scope->channel, track_id);
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_live_ksmp_output_variant(ngx_http_live_ksmp_params_t *params,
    ngx_live_variant_t *variant, ngx_live_persist_serve_scope_t *scope)
{
    uint32_t           track_id;
    uint32_t           req_media_types;
    uint32_t           res_media_types;
    ngx_int_t          rc;
    ngx_uint_t         i;
    ngx_live_track_t  *cur_track;

    req_media_types = params->media_type_mask;

    if ((params->flags & NGX_KSMP_FLAG_ACTIVE_LAST) &&
        !ngx_live_variant_is_active_last(variant, scope->timeline,
            req_media_types))
    {
        ngx_http_live_ksmp_set_error(params,
            NGX_KSMP_ERR_VARIANT_INACTIVE,
            "variant inactive (last), mask: 0x%uxD, variant: %V, channel: %V",
            params->media_type_mask, &variant->sn.str,
            &variant->channel->sn.str);
        return NGX_ABORT;
    }

    if (params->flags & NGX_KSMP_FLAG_ACTIVE_ANY) {
        req_media_types = ngx_live_variant_is_active_any(variant,
            scope->timeline, req_media_types);
        if (!req_media_types) {
            ngx_http_live_ksmp_set_error(params,
                NGX_KSMP_ERR_VARIANT_INACTIVE,
                "variant inactive (any), mask: 0x%uxD, variant: %V"
                ", channel: %V",
                params->media_type_mask, &variant->sn.str,
                &variant->channel->sn.str);
            return NGX_ABORT;
        }
    }

    res_media_types = 0;
    for (i = 0; i < KMP_MEDIA_COUNT; i++) {
        cur_track = variant->tracks[i];
        if (cur_track == NULL) {
            continue;
        }

        if (!(req_media_types & (1 << i))) {
            cur_track->output = 0;
            continue;
        }

        if (params->segment_index != NGX_KSMP_INVALID_SEGMENT_INDEX) {
            cur_track->media_info_node = ngx_live_media_info_queue_get_node(
                cur_track, params->segment_index, &track_id);
            if (cur_track->media_info_node == NULL) {
                ngx_http_live_ksmp_set_error(params,
                    NGX_KSMP_ERR_MEDIA_INFO_NOT_FOUND,
                    "no media info, index: %uD, track: %V, variant: %V"
                    ", channel: %V",
                    params->segment_index, &cur_track->sn.str,
                    &variant->sn.str, &variant->channel->sn.str);
                cur_track->output = 0;
                continue;
            }

            if (scope->track_refs) {
                rc = ngx_http_live_ksmp_add_track_ref(scope, track_id,
                    cur_track);
                if (rc != NGX_OK) {
                    return rc;
                }
            }

        } else {
            if (ngx_live_media_info_queue_get_last(cur_track) == NULL) {
                ngx_http_live_ksmp_set_error(params,
                    NGX_KSMP_ERR_MEDIA_INFO_NOT_FOUND,
                    "no media info, track: %V, variant: %V, channel: %V",
                    &cur_track->sn.str, &variant->sn.str,
                    &variant->channel->sn.str);
                cur_track->output = 0;
                continue;
            }
        }

        cur_track->output = 1;
        cur_track->written = 0;
        res_media_types |= 1 << cur_track->media_type;
    }

    if (!res_media_types) {
        ngx_http_live_ksmp_set_error(params,
            NGX_KSMP_ERR_TRACK_NOT_FOUND,
            "no tracks found, mask: 0x%uxD, variant: %V, channel: %V",
            params->media_type_mask, &variant->sn.str,
            &variant->channel->sn.str);
        return NGX_ABORT;
    }

    scope->header.res_media_types |= res_media_types;

    return NGX_OK;
}


static ngx_int_t
ngx_http_live_ksmp_add_variants(ngx_http_request_t *r,
    ngx_http_live_ksmp_params_t *params, ngx_live_persist_serve_scope_t *scope)
{
    ngx_int_t             rc;
    ngx_queue_t          *q;
    ngx_live_channel_t   *channel = scope->channel;
    ngx_live_variant_t   *cur_variant;
    ngx_live_variant_t  **dst;

    dst = ngx_palloc(r->pool, sizeof(dst[0]) * channel->variants.count);
    if (dst == NULL) {
        ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
            "ngx_http_live_ksmp_add_variants: alloc failed");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    scope->variants = dst;

    for (q = ngx_queue_head(&channel->variants.queue);
        q != ngx_queue_sentinel(&channel->variants.queue);
        q = ngx_queue_next(q))
    {
        cur_variant = ngx_queue_data(q, ngx_live_variant_t, queue);

        rc = ngx_http_live_ksmp_output_variant(params, cur_variant, scope);
        if (rc == NGX_OK) {
            *dst++ = cur_variant;

        } else if (rc != NGX_ABORT) {
            return rc;
        }
    }

    scope->header.variant_count = dst - scope->variants;

    return NGX_OK;
}


static ngx_uint_t
ngx_http_live_ksmp_value_count(ngx_str_t *value)
{
    u_char      *p, *last;
    ngx_uint_t   n;

    p = value->data;
    last = p + value->len;

    for (n = 1; p < last; p++) {
        if (*p == ',') {
            n++;
        }
    }

    return n;
}

static ngx_int_t
ngx_http_live_ksmp_parse_variant_ids(ngx_http_request_t *r,
    ngx_http_live_ksmp_params_t *params, ngx_live_persist_serve_scope_t *scope)
{
    u_char               *p, *last, *next;
    ngx_int_t             rc;
    ngx_str_t             cur;
    ngx_uint_t            variant_count;
    ngx_live_channel_t   *channel;
    ngx_live_variant_t   *variant;
    ngx_live_variant_t  **dst;

    variant_count = ngx_http_live_ksmp_value_count(&params->variant_ids);

    dst = ngx_palloc(r->pool, sizeof(dst[0]) * variant_count);
    if (dst == NULL) {
        ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
            "ngx_http_live_ksmp_parse_variant_ids: alloc failed");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    scope->variants = dst;
    channel = scope->channel;

    p = params->variant_ids.data;
    last = p + params->variant_ids.len;

    for ( ;; ) {
        next = ngx_strlchr(p, last, ',');

        cur.data = p;
        cur.len = next != NULL ? next - p : last - p;

        variant = ngx_live_variant_get(channel, &cur);
        if (variant != NULL) {
            rc = ngx_http_live_ksmp_output_variant(params, variant, scope);
            if (rc == NGX_OK) {
                *dst++ = variant;

            } else if (rc != NGX_ABORT) {
                return rc;
            }

        } else {
            ngx_http_live_ksmp_set_error(params,
                NGX_KSMP_ERR_VARIANT_NOT_FOUND,
                "unknown variant \"%V\", channel: %V", &cur, &channel->sn.str);
        }

        if (next == NULL) {
            break;
        }

        p = next + 1;
    }

    scope->header.variant_count = dst - scope->variants;

    return NGX_OK;
}


static void
ngx_http_live_ksmp_cleanup(void *data)
{
    ngx_connection_t    *c;
    ngx_http_request_t  *r = data;

    c = r->connection;

    ngx_log_error(NGX_LOG_ERR, c->log, 0,
        "ngx_http_live_ksmp_cleanup: request cleaned up");

    ngx_http_finalize_request(r, NGX_ERROR);

    ngx_http_run_posted_requests(c);
}


static ngx_int_t
ngx_http_live_ksmp_init_scope(ngx_http_live_ksmp_params_t *params,
    ngx_live_persist_serve_scope_t *scope)
{
    uint32_t                      code;
    ngx_int_t                     rc;
    ngx_http_request_t           *r = params->r;
    ngx_live_channel_t           *channel;
    ngx_live_timeline_t          *timeline;
    ngx_live_core_preset_conf_t  *cpcf;

    ngx_memzero(&scope->header, sizeof(scope->header));

    /* channel */
    channel = ngx_live_channel_get(&params->channel_id);
    if (channel == NULL) {
        return ngx_http_live_ksmp_output_error(r,
            NGX_KSMP_ERR_CHANNEL_NOT_FOUND,
            "unknown channel \"%V\"", &params->channel_id);
    }

    channel->last_accessed = ngx_time();

    if (channel->blocked) {
        return ngx_http_live_ksmp_output_error(r,
            NGX_KSMP_ERR_CHANNEL_BLOCKED,
            "channel \"%V\" is blocked", &params->channel_id);
    }

    scope->channel = channel;

    /* timeline */
    timeline = ngx_live_timeline_get(channel, &params->timeline_id);
    if (timeline == NULL) {
        return ngx_http_live_ksmp_output_error(r,
            NGX_KSMP_ERR_TIMELINE_NOT_FOUND,
            "unknown timeline \"%V\", channel: %V",
            &params->timeline_id, &params->channel_id);
    }

    timeline->last_accessed = ngx_time();

    if (timeline->manifest.segment_count <= 0) {
        if (timeline->manifest.target_duration_segments) {
            return ngx_http_live_ksmp_output_error(r,
                NGX_KSMP_ERR_TIMELINE_EMPTIED,
                "timeline \"%V\" no longer has segments, channel: %V",
                &params->timeline_id, &params->channel_id);
        }

        return ngx_http_live_ksmp_output_error(r,
            NGX_KSMP_ERR_TIMELINE_EMPTY,
            "no segments in timeline \"%V\", channel: %V",
            &params->timeline_id, &params->channel_id);
    }

    if ((params->flags & NGX_KSMP_FLAG_CHECK_EXPIRY) &&
        ngx_live_timeline_is_expired(timeline))
    {
        return ngx_http_live_ksmp_output_error(r,
            NGX_KSMP_ERR_TIMELINE_EXPIRED,
            "timeline \"%V\" is expired, channel: %V",
            &params->timeline_id, &params->channel_id);
    }

    if (params->time != NGX_KSMP_INVALID_TIMESTAMP) {
        cpcf = ngx_live_get_module_preset_conf(channel, ngx_live_core_module);

        params->time = ngx_live_rescale_time(params->time, 1000,
            cpcf->timescale);

        if (ngx_live_timeline_get_time(timeline, params->flags,
            r->connection->log, &params->time))
        {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        if (ngx_live_timelines_get_segment_index(channel, params->time,
            &params->segment_index) != NGX_OK)
        {
            return ngx_http_live_ksmp_output_error(r,
                NGX_KSMP_ERR_SEGMENT_TIME_NOT_FOUND,
                "time %L not found in any segment, timeline: %V, channel: %V",
                params->time, &params->timeline_id, &params->channel_id);
        }
    }

    if (params->flags & NGX_KSMP_FLAG_MEDIA) {
        code = ngx_live_timeline_get_segment_info(timeline,
            params->segment_index, params->flags, &scope->si.correction);
        if (code != NGX_KSMP_ERR_SUCCESS) {
            return ngx_http_live_ksmp_output_error(r, code,
                "segment %uD does not exist, timeline: %V, channel: %V",
                params->segment_index, &params->timeline_id,
                &params->channel_id);
        }

    } else {
        scope->si.correction = 0;
    }

    scope->si.index = params->segment_index;
    scope->si.time = params->time;
    scope->si.reserved = 0;

    scope->timeline = timeline;

    /* scope */
    if (params->flags & NGX_KSMP_FLAG_MEDIA) {
        scope->track_refs = ngx_array_create(r->pool, KMP_MEDIA_COUNT,
            sizeof(ngx_live_track_ref_t));
        if (scope->track_refs == NULL) {
            ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                "ngx_http_live_ksmp_init_scope: create array failed");
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

    } else {
        scope->track_refs = NULL;
    }

    scope->flags = params->flags;

    if (params->variant_ids.data != NULL) {
        rc = ngx_http_live_ksmp_parse_variant_ids(r, params, scope);
        if (rc != NGX_OK) {
            return rc;
        }

    } else {
        rc = ngx_http_live_ksmp_add_variants(r, params, scope);
        if (rc != NGX_OK) {
            return rc;
        }
    }

    if (scope->header.variant_count <= 0) {
        if (params->err_code) {
            return ngx_http_live_ksmp_output_error(r,
                params->err_code, "%V", &params->err_msg);

        } else {
            return ngx_http_live_ksmp_output_error(r,
                NGX_KSMP_ERR_VARIANT_NO_MATCH,
                "no variant matches the request, channel: %V",
                &params->channel_id);
        }
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_live_ksmp_write(ngx_http_live_ksmp_params_t *params,
    ngx_live_persist_serve_scope_t *scope)
{
    ngx_int_t                       rc;
    ngx_wstream_t                  *ws;
    ngx_http_request_t             *r = params->r;
    ngx_live_channel_t             *channel = scope->channel;
    ngx_persist_write_ctx_t        *write_ctx;
    ngx_http_live_ksmp_ctx_t       *ctx;
    ngx_persist_write_marker_t      marker;
    ngx_live_segment_copy_req_t     req;
    ngx_live_core_preset_conf_t    *cpcf;
    ngx_http_live_ksmp_loc_conf_t  *klcf;

    klcf = ngx_http_get_module_loc_conf(r, ngx_http_live_ksmp_module);

    write_ctx = ngx_persist_write_init(r->pool, NGX_KSMP_PERSIST_TYPE,
        klcf->level);
    if (write_ctx == NULL) {
        ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
            "ngx_http_live_ksmp_write: write init failed");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ngx_persist_write_ctx(write_ctx) = scope;

    ws = ngx_persist_write_stream(write_ctx);

    channel = scope->channel;

    if (ngx_persist_write_block_open(write_ctx,
            NGX_KSMP_BLOCK_CHANNEL) != NGX_OK ||
        ngx_wstream_str(ws, &channel->sn.str) != NGX_OK ||
        ngx_persist_write_reserve(write_ctx, sizeof(scope->header), &marker)
            != NGX_OK ||
        ngx_live_persist_write_blocks(channel, write_ctx,
            NGX_LIVE_PERSIST_CTX_SERVE_CHANNEL, channel) != NGX_OK)
    {
        ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
            "ngx_http_live_ksmp_write: write failed (1)");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    cpcf = ngx_live_get_module_preset_conf(channel, ngx_live_core_module);

    scope->header.timescale = cpcf->timescale;
    scope->header.req_media_types = params->media_type_mask;
    scope->header.last_modified = channel->last_modified;
    scope->header.now = ngx_time();
    ngx_persist_write_marker_write(&marker, &scope->header,
        sizeof(scope->header));

    ngx_persist_write_block_close(write_ctx);      /* channel */

    ctx = ngx_http_get_module_ctx(r, ngx_http_live_ksmp_module);

    ctx->out = ngx_persist_write_close(write_ctx, &ctx->size, &ctx->last);
    if (ctx->out == NULL) {
        ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
            "ngx_http_live_ksmp_write: close failed");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    if (!(params->flags & NGX_KSMP_FLAG_MEDIA)) {
        return ngx_http_live_ksmp_output(r, NGX_HTTP_LAST);
    }

    if (params->padding) {
        if (params->padding < NGX_KSMP_MIN_PADDING) {
            ctx->padding = NGX_KSMP_MIN_PADDING;

        } else if (params->padding > NGX_KSMP_MAX_PADDING) {
            ctx->padding = NGX_KSMP_MAX_PADDING;

        } else {
            ctx->padding = params->padding;
        }

        ctx->size += ctx->padding;
    }

    rc = ngx_live_filler_serve_segments(r->pool, scope->track_refs,
        scope->si.index, &ctx->last, &ctx->size);
    switch (rc) {

    case NGX_OK:
        req.source = ngx_http_live_ksmp_filler_source;
        break;

    case NGX_DONE:
        req.source.len = 0;
        break;

    default:
        ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
            "ngx_http_live_ksmp_write: serve filler failed");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    req.size = 0;
    req.chain = NULL;

    if (scope->track_refs->nelts > 0) {
        req.pool = r->pool;
        req.channel = channel;
        req.tracks = scope->track_refs->elts;
        req.track_count = scope->track_refs->nelts;
        req.flags = params->flags;
        req.segment_index = params->segment_index;
        req.time = params->time;

        req.writer.set_size = ngx_http_live_ksmp_segment_set_size;
        req.writer.write = ngx_http_live_ksmp_segment_write;
        req.writer.close = ngx_http_live_ksmp_segment_close;
        req.writer.cleanup = ngx_http_live_ksmp_cleanup;
        req.writer.arg = r;

        rc = ngx_live_copy_segment(&req);

    } else {
        rc = NGX_OK;
    }

    switch (rc) {

    case NGX_OK:
        break;

    case NGX_DONE:
        ctx->source = req.source;
        r->main->count++;
        return NGX_DONE;

    default:
        ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
            "ngx_http_live_ksmp_write: read failed %i", rc);
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ctx->source = req.source;
    ctx->size += req.size;

    rc = ngx_http_live_ksmp_output(r, 0);
    if (rc != NGX_OK) {
        return rc;
    }

    if (req.chain != NULL) {
        rc = ngx_http_output_filter(r, req.chain);
        if (rc == NGX_ERROR || rc >= NGX_HTTP_SPECIAL_RESPONSE) {
            ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                "ngx_http_live_ksmp_write: output filter failed %i", rc);
            return rc;
        }
    }

    rc = ngx_http_live_ksmp_write_padding(r);
    if (rc != NGX_OK) {
        return rc;
    }

    rc = ngx_http_send_special(r, NGX_HTTP_LAST);
    if (rc == NGX_ERROR || rc >= NGX_HTTP_SPECIAL_RESPONSE) {
        ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
            "ngx_http_live_ksmp_write: send special failed %i", rc);
        return rc;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_live_ksmp_handler(ngx_http_request_t *r)
{
    ngx_int_t                       rc;
    ngx_http_live_ksmp_ctx_t       *ctx;
    ngx_http_live_ksmp_params_t     params;
    ngx_live_persist_serve_scope_t  scope;

    if (r->method != NGX_HTTP_GET) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
            "ngx_http_live_ksmp_handler: unsupported method %ui", r->method);
        return NGX_HTTP_NOT_ALLOWED;
    }

    rc = ngx_http_discard_request_body(r);
    if (rc != NGX_OK) {
        ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
            "ngx_http_live_ksmp_handler: discard request body failed %i", rc);
        return rc;
    }

    rc = ngx_http_live_ksmp_parse(r, &params);
    if (rc != NGX_OK) {
        return rc;
    }

    ctx = ngx_pcalloc(r->pool, sizeof(*ctx));
    if (ctx == NULL) {
        ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
            "ngx_http_live_ksmp_handler: alloc ctx failed");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ngx_http_set_ctx(r, ctx, ngx_http_live_ksmp_module);

    rc = ngx_http_live_ksmp_init_scope(&params, &scope);
    if (rc != NGX_OK || r->header_sent) {
        return rc;
    }

    return ngx_http_live_ksmp_write(&params, &scope);
}


static ngx_int_t
ngx_http_live_ksmp_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    ngx_str_t                 *s;
    ngx_http_live_ksmp_ctx_t  *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_live_ksmp_module);
    if (ctx == NULL) {
        v->not_found = 1;
        return NGX_OK;
    }

    s = (ngx_str_t *) ((char *) ctx + data);

    if (s->len) {
        v->len = s->len;
        v->valid = 1;
        v->no_cacheable = 0;
        v->not_found = 0;
        v->data = s->data;

    } else {
        v->not_found = 1;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_live_ksmp_uint32_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    uint32_t                   n;
    ngx_http_live_ksmp_ctx_t  *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_live_ksmp_module);
    if (ctx == NULL) {
        v->not_found = 1;
        return NGX_OK;
    }

    v->data = ngx_pnalloc(r->pool, NGX_INT32_LEN);
    if (v->data == NULL) {
        return NGX_ERROR;
    }

    n = *(uint32_t *) ((char *) ctx + data);
    v->len = ngx_sprintf(v->data, "%uD", n) - v->data;

    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;

    return NGX_OK;
}


static ngx_int_t
ngx_http_live_ksmp_add_variables(ngx_conf_t *cf)
{
    ngx_http_variable_t  *var, *v;

    for (v = ngx_http_live_ksmp_vars; v->name.len; v++) {
        var = ngx_http_add_variable(cf, &v->name, v->flags);
        if (var == NULL) {
            return NGX_ERROR;
        }

        var->get_handler = v->get_handler;
        var->data = v->data;
    }

    return NGX_OK;
}


static void *
ngx_http_live_ksmp_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_live_ksmp_loc_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_live_ksmp_loc_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    conf->level = NGX_CONF_UNSET;

    return conf;
}


static char *
ngx_http_live_ksmp_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_live_ksmp_loc_conf_t  *prev = parent;
    ngx_http_live_ksmp_loc_conf_t  *conf = child;

    ngx_conf_merge_value(conf->level, prev->level, 6);

    return NGX_CONF_OK;
}


static char *
ngx_http_live_ksmp(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_core_loc_conf_t  *clcf;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_live_ksmp_handler;

    return NGX_CONF_OK;
}

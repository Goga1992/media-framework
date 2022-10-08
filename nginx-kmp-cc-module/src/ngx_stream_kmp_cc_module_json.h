/* auto-generated by generate_json_header.py */

#ifndef ngx_copy_fix
#define ngx_copy_fix(dst, src)   ngx_copy(dst, (src), sizeof(src) - 1)
#endif

#ifndef ngx_copy_str
#define ngx_copy_str(dst, src)   ngx_copy(dst, (src).data, (src).len)
#endif

/* ngx_stream_kmp_cc_session_json writer */

static size_t
ngx_stream_kmp_cc_session_json_get_size(ngx_stream_kmp_cc_ctx_t *obj)
{
    size_t  result;

    result =
        sizeof("{\"mem_left\":") - 1 + NGX_SIZE_T_LEN +
        sizeof(",\"mem_limit\":") - 1 + NGX_SIZE_T_LEN +
        sizeof(",\"input\":") - 1 + ngx_kmp_in_json_get_size(obj->input) +
        sizeof(",\"cc\":") - 1 + ngx_kmp_cc_json_get_size(obj->cc) +
        sizeof("}") - 1;

    return result;
}


static u_char *
ngx_stream_kmp_cc_session_json_write(u_char *p, ngx_stream_kmp_cc_ctx_t *obj)
{
    p = ngx_copy_fix(p, "{\"mem_left\":");
    p = ngx_sprintf(p, "%uz", (size_t) obj->mem_left);
    p = ngx_copy_fix(p, ",\"mem_limit\":");
    p = ngx_sprintf(p, "%uz", (size_t) obj->mem_limit);
    p = ngx_copy_fix(p, ",\"input\":");
    p = ngx_kmp_in_json_write(p, obj->input);
    p = ngx_copy_fix(p, ",\"cc\":");
    p = ngx_kmp_cc_json_write(p, obj->cc);
    *p++ = '}';

    return p;
}


/* ngx_stream_kmp_cc_server_json writer */

static size_t
ngx_stream_kmp_cc_server_json_get_size(ngx_stream_core_srv_conf_t *obj)
{
    size_t                         result;
    ngx_queue_t                   *q;
    ngx_stream_kmp_cc_ctx_t       *cur;
    ngx_stream_kmp_cc_srv_conf_t  *kscf;

    kscf = ngx_stream_get_module_srv_conf(obj->ctx, ngx_stream_kmp_cc_module);
    result =
        sizeof("{\"sessions\":[") - 1 +
        sizeof("]}") - 1;

    for (q = ngx_queue_head(&kscf->sessions);
        q != ngx_queue_sentinel(&kscf->sessions);
        q = ngx_queue_next(q))
    {
        cur = ngx_queue_data(q, ngx_stream_kmp_cc_ctx_t, queue);
        result += ngx_stream_kmp_cc_session_json_get_size(cur) + sizeof(",") -
            1;
    }

    return result;
}


static u_char *
ngx_stream_kmp_cc_server_json_write(u_char *p, ngx_stream_core_srv_conf_t *obj)
{
    ngx_queue_t                   *q;
    ngx_stream_kmp_cc_ctx_t       *cur;
    ngx_stream_kmp_cc_srv_conf_t  *kscf;

    kscf = ngx_stream_get_module_srv_conf(obj->ctx, ngx_stream_kmp_cc_module);
    p = ngx_copy_fix(p, "{\"sessions\":[");

    for (q = ngx_queue_head(&kscf->sessions);
        q != ngx_queue_sentinel(&kscf->sessions);
        q = ngx_queue_next(q))
    {
        cur = ngx_queue_data(q, ngx_stream_kmp_cc_ctx_t, queue);

        if (p[-1] != '[') {
            *p++ = ',';
        }

        p = ngx_stream_kmp_cc_session_json_write(p, cur);
    }

    p = ngx_copy_fix(p, "]}");

    return p;
}


/* ngx_stream_kmp_cc_stream_json writer */

size_t
ngx_stream_kmp_cc_stream_json_get_size(void *obj)
{
    size_t                        result;
    ngx_uint_t                    n;
    ngx_stream_core_srv_conf_t   *cur;
    ngx_stream_core_main_conf_t  *cmcf;

    cmcf = ngx_stream_kmp_cc_get_core_main_conf();
    if (!cmcf) {
        return sizeof("null") - 1;
    }

    result =
        sizeof("{\"servers\":[") - 1 +
        sizeof("]}") - 1;

    for (n = 0; n < cmcf->servers.nelts; n++) {
        cur = ((ngx_stream_core_srv_conf_t **) cmcf->servers.elts)[n];

        if (cur->handler != ngx_stream_kmp_cc_handler) {
            continue;
        }

        result += ngx_stream_kmp_cc_server_json_get_size(cur) + sizeof(",") -
            1;
    }

    return result;
}


u_char *
ngx_stream_kmp_cc_stream_json_write(u_char *p, void *obj)
{
    ngx_uint_t                    n;
    ngx_stream_core_srv_conf_t   *cur;
    ngx_stream_core_main_conf_t  *cmcf;

    cmcf = ngx_stream_kmp_cc_get_core_main_conf();
    if (!cmcf) {
        p = ngx_copy_fix(p, "null");
        return p;
    }

    p = ngx_copy_fix(p, "{\"servers\":[");

    for (n = 0; n < cmcf->servers.nelts; n++) {
        cur = ((ngx_stream_core_srv_conf_t **) cmcf->servers.elts)[n];

        if (cur->handler != ngx_stream_kmp_cc_handler) {
            continue;
        }

        if (p[-1] != '[') {
            *p++ = ',';
        }

        p = ngx_stream_kmp_cc_server_json_write(p, cur);
    }

    p = ngx_copy_fix(p, "]}");

    return p;
}

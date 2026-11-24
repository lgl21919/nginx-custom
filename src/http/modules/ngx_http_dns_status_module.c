
/*
 * Copyright (C) Nginx, Inc.
 *
 * dns_status — view DNS resolver cache, upstream "server ... resolve"
 * peers, and recent request-time dynamic resolve results.
 *
 * Usage:
 *   location /dns_status {
 *       dns_status;
 *   }
 *   curl http://127.0.0.1/dns_status
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_http_upstream_round_robin.h>


#if (NGX_HTTP_DNS_STATUS)

void ngx_http_dns_status_record_dynamic(ngx_str_t *name,
    ngx_resolver_addr_t *addrs, ngx_uint_t naddrs, ngx_int_t state);


#define NGX_HTTP_DNS_STATUS_DYNAMIC_MAX  128
#define NGX_HTTP_DNS_STATUS_NAME_MAX     256
#define NGX_HTTP_DNS_STATUS_ADDRS_MAX    16
#define NGX_HTTP_DNS_STATUS_BUFSIZE      (256 * 1024)


typedef struct {
    time_t        at;
    ngx_int_t     state;
    ngx_uint_t    naddrs;
    size_t        name_len;
    u_char        name[NGX_HTTP_DNS_STATUS_NAME_MAX];
    size_t        addr_lens[NGX_HTTP_DNS_STATUS_ADDRS_MAX];
    u_char        addrs[NGX_HTTP_DNS_STATUS_ADDRS_MAX][NGX_SOCKADDR_STRLEN];
} ngx_http_dns_status_dynamic_t;


static ngx_http_dns_status_dynamic_t
    ngx_http_dns_status_dynamics[NGX_HTTP_DNS_STATUS_DYNAMIC_MAX];
static ngx_uint_t  ngx_http_dns_status_dynamic_pos;
static ngx_uint_t  ngx_http_dns_status_dynamic_count;


static ngx_int_t ngx_http_dns_status_handler(ngx_http_request_t *r);
static char *ngx_http_dns_status(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static u_char *ngx_http_dns_status_dump_upstream_resolve(
    ngx_http_request_t *r, u_char *buf, u_char *last);
static u_char *ngx_http_dns_status_dump_dynamic(u_char *buf, u_char *last);
static u_char *ngx_http_dns_status_dump_resolvers(ngx_http_request_t *r,
    u_char *buf, u_char *last);


static ngx_command_t  ngx_http_dns_status_commands[] = {

    { ngx_string("dns_status"),
      NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_dns_status,
      0,
      0,
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_dns_status_module_ctx = {
    NULL,                                  /* preconfiguration */
    NULL,                                  /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    NULL,                                  /* create location configuration */
    NULL                                   /* merge location configuration */
};


ngx_module_t  ngx_http_dns_status_module = {
    NGX_MODULE_V1,
    &ngx_http_dns_status_module_ctx,       /* module context */
    ngx_http_dns_status_commands,          /* module directives */
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


void
ngx_http_dns_status_record_dynamic(ngx_str_t *name,
    ngx_resolver_addr_t *addrs, ngx_uint_t naddrs, ngx_int_t state)
{
    ngx_uint_t                     i, n, pos;
    ngx_http_dns_status_dynamic_t *e;

    pos = ngx_http_dns_status_dynamic_pos;
    e = &ngx_http_dns_status_dynamics[pos];

    ngx_memzero(e, sizeof(ngx_http_dns_status_dynamic_t));

    e->at = ngx_time();
    e->state = state;

    e->name_len = ngx_min(name->len, (size_t) (NGX_HTTP_DNS_STATUS_NAME_MAX - 1));
    ngx_memcpy(e->name, name->data, e->name_len);

    n = ngx_min(naddrs, (ngx_uint_t) NGX_HTTP_DNS_STATUS_ADDRS_MAX);
    e->naddrs = n;

    for (i = 0; i < n; i++) {
        e->addr_lens[i] = ngx_sock_ntop(addrs[i].sockaddr, addrs[i].socklen,
                                        e->addrs[i], NGX_SOCKADDR_STRLEN, 1);
    }

    ngx_http_dns_status_dynamic_pos =
        (pos + 1) % NGX_HTTP_DNS_STATUS_DYNAMIC_MAX;

    if (ngx_http_dns_status_dynamic_count < NGX_HTTP_DNS_STATUS_DYNAMIC_MAX) {
        ngx_http_dns_status_dynamic_count++;
    }
}


static u_char *
ngx_http_dns_status_dump_dynamic(u_char *buf, u_char *last)
{
    ngx_uint_t                     i, j, idx, count, pos;
    ngx_http_dns_status_dynamic_t *e;

    count = ngx_http_dns_status_dynamic_count;
    pos = ngx_http_dns_status_dynamic_pos;

    buf = ngx_slprintf(buf, last,
                       "\n# section: recent resolve events "
                       "(request-time dynamic + upstream zone resolve; "
                       "recent %ui, ring %ui)\n",
                       count, (ngx_uint_t) NGX_HTTP_DNS_STATUS_DYNAMIC_MAX);

    if (count == 0) {
        return ngx_slprintf(buf, last,
                            "(empty — trigger variable proxy_pass "
                            "or wait for server ... resolve timer)\n");
    }

    for (i = 0; i < count; i++) {
        idx = (pos + NGX_HTTP_DNS_STATUS_DYNAMIC_MAX - count + i)
              % NGX_HTTP_DNS_STATUS_DYNAMIC_MAX;
        e = &ngx_http_dns_status_dynamics[idx];

        if (e->state) {
            buf = ngx_slprintf(buf, last,
                               "  time=%T name=\"%*s\" state=%i (%s)\n",
                               e->at, e->name_len, e->name, e->state,
                               ngx_resolver_strerror(e->state));
            continue;
        }

        buf = ngx_slprintf(buf, last, "  time=%T name=\"%*s\" naddrs=%ui",
                           e->at, e->name_len, e->name, e->naddrs);

        for (j = 0; j < e->naddrs; j++) {
            buf = ngx_slprintf(buf, last, " addr=%*s",
                               e->addr_lens[j], e->addrs[j]);
        }

        if (buf < last) {
            *buf++ = '\n';
        }
    }

    return buf;
}


#if (NGX_HTTP_UPSTREAM_ZONE)

static u_char *
ngx_http_dns_status_dump_peers(u_char *buf, u_char *last,
    ngx_http_upstream_rr_peers_t *peers, const char *title)
{
    u_char                       text[NGX_SOCKADDR_STRLEN];
    size_t                       len;
    ngx_http_upstream_rr_peer_t *peer;

    if (peers == NULL) {
        return buf;
    }

    ngx_http_upstream_rr_peers_rlock(peers);

    buf = ngx_slprintf(buf, last, "  %s peers=%ui\n", title, peers->number);

    for (peer = peers->peer; peer; peer = peer->next) {
        len = ngx_sock_ntop(peer->sockaddr, peer->socklen,
                            text, NGX_SOCKADDR_STRLEN, 1);

        buf = ngx_slprintf(buf, last,
                           "    peer=%*s server=\"%V\" weight=%i "
                           "max_fails=%ui fail_timeout=%T down=%ui",
                           len, text, &peer->server, peer->weight,
                           peer->max_fails, peer->fail_timeout,
                           (ngx_uint_t) peer->down);

        if (peer->host) {
            buf = ngx_slprintf(buf, last, " host=\"%V\"", &peer->host->name);
            if (peer->host->service.len) {
                buf = ngx_slprintf(buf, last, " service=\"%V\"",
                                   &peer->host->service);
            }
            buf = ngx_slprintf(buf, last, " host_valid=%T", peer->host->valid);
        }

        if (buf < last) {
            *buf++ = '\n';
        }
    }

    for (peer = peers->resolve; peer; peer = peer->next) {
        buf = ngx_slprintf(buf, last,
                           "    template server=\"%V\" resolve=1",
                           &peer->server);
        if (peer->host) {
            buf = ngx_slprintf(buf, last, " host=\"%V\"", &peer->host->name);
            if (peer->host->service.len) {
                buf = ngx_slprintf(buf, last, " service=\"%V\"",
                                   &peer->host->service);
            }
        }
        if (buf < last) {
            *buf++ = '\n';
        }
    }

    ngx_http_upstream_rr_peers_unlock(peers);

    if (peers->next) {
        buf = ngx_http_dns_status_dump_peers(buf, last, peers->next, "backup");
    }

    return buf;
}

#endif


static u_char *
ngx_http_dns_status_dump_upstream_resolve(ngx_http_request_t *r,
    u_char *buf, u_char *last)
{
    ngx_uint_t                      i;
    ngx_http_upstream_srv_conf_t  **uscfp;
    ngx_http_upstream_main_conf_t  *umcf;
#if (NGX_HTTP_UPSTREAM_ZONE)
    ngx_http_upstream_rr_peers_t   *peers;
    ngx_uint_t                      n;
#endif

    umcf = ngx_http_get_module_main_conf(r, ngx_http_upstream_module);
    if (umcf == NULL) {
        return ngx_slprintf(buf, last, "\n# section: upstream resolve\n"
                            "(no upstream main conf)\n");
    }

    buf = ngx_slprintf(buf, last,
                       "\n# section: upstream server ... resolve "
                       "(zone peers)\n");

    uscfp = umcf->upstreams.elts;

#if !(NGX_HTTP_UPSTREAM_ZONE)
    buf = ngx_slprintf(buf, last,
                       "(NGX_HTTP_UPSTREAM_ZONE disabled at build time)\n");
    return buf;
#else
    n = 0;

    for (i = 0; i < umcf->upstreams.nelts; i++) {
        if (uscfp[i]->shm_zone == NULL) {
            continue;
        }

        peers = uscfp[i]->peer.data;
        if (peers == NULL) {
            continue;
        }

        n++;
        buf = ngx_slprintf(buf, last, "upstream \"%V\" zone=\"%V\"\n",
                           &uscfp[i]->host, &uscfp[i]->shm_zone->shm.name);

        if (uscfp[i]->resolver) {
            buf = ngx_slprintf(buf, last, "  upstream_resolver:\n");
            buf = ngx_resolver_dump(uscfp[i]->resolver, buf, last, NULL);
        } else {
            buf = ngx_slprintf(buf, last, "  upstream_resolver: (none)\n");
        }

        buf = ngx_http_dns_status_dump_peers(buf, last, peers, "primary");
    }

    if (n == 0) {
        buf = ngx_slprintf(buf, last,
                           "(no upstream with zone — "
                           "need: zone + server name resolve)\n");
    }

    return buf;
#endif
}


static u_char *
ngx_http_dns_status_dump_resolvers(ngx_http_request_t *r, u_char *buf,
    u_char *last)
{
    ngx_http_core_loc_conf_t  *clcf;

    clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

    buf = ngx_slprintf(buf, last,
                       "\n# section: http/server/location resolver cache "
                       "(shared by dynamic resolve)\n");

    if (clcf == NULL || clcf->resolver == NULL) {
        return ngx_slprintf(buf, last, "(no resolver in this location)\n");
    }

    return ngx_resolver_dump(clcf->resolver, buf, last, NULL);
}


static ngx_int_t
ngx_http_dns_status_handler(ngx_http_request_t *r)
{
    size_t       size;
    ngx_int_t    rc;
    ngx_buf_t   *b;
    ngx_chain_t  out;

    if (!(r->method & (NGX_HTTP_GET|NGX_HTTP_HEAD))) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    rc = ngx_http_discard_request_body(r);
    if (rc != NGX_OK) {
        return rc;
    }

    r->headers_out.content_type_len = sizeof("text/plain") - 1;
    ngx_str_set(&r->headers_out.content_type, "text/plain");
    r->headers_out.content_type_lowcase = NULL;

    size = NGX_HTTP_DNS_STATUS_BUFSIZE;
    b = ngx_create_temp_buf(r->pool, size);
    if (b == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    out.buf = b;
    out.next = NULL;

    b->last = ngx_slprintf(b->last, b->end,
                           "nginx dns_status\n"
                           "worker_pid=%P\n",
                           ngx_pid);

    b->last = ngx_http_dns_status_dump_resolvers(r, b->last, b->end);
    b->last = ngx_http_dns_status_dump_upstream_resolve(r, b->last, b->end);
    b->last = ngx_http_dns_status_dump_dynamic(b->last, b->end);

    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = b->last - b->pos;

    b->last_buf = (r == r->main) ? 1 : 0;
    b->last_in_chain = 1;

    rc = ngx_http_send_header(r);
    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    return ngx_http_output_filter(r, &out);
}


static char *
ngx_http_dns_status(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_core_loc_conf_t  *clcf;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_dns_status_handler;

    return NGX_CONF_OK;
}

#endif /* NGX_HTTP_DNS_STATUS */

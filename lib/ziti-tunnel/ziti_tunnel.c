/*
 Copyright NetFoundry Inc.

 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at

 https://www.apache.org/licenses/LICENSE-2.0

 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.
 */

// something wrong with lwip_xxxx byteorder functions
#ifdef _WIN32
#define LWIP_DONT_PROVIDE_BYTEORDER_FUNCTIONS 1
#endif

#if defined(__mips) || defined(__mips__)
#define LWIP_DONT_PROVIDE_BYTEORDER_FUNCTIONS 1
#endif

#include "uv.h"

#include "lwip/init.h"
#include "lwip/raw.h"
#include "lwip/timeouts.h"
#include "netif_shim.h"
#include "ziti/ziti_tunnel.h"
#include "ziti_tunnel_priv.h"
#include "tunnel_tcp.h"
#include "tunnel_udp.h"

#include <string.h>

const char *DST_PROTO_KEY = "dst_protocol";
const char *DST_IP_KEY = "dst_ip";
const char *DST_PORT_KEY = "dst_port";
const char *DST_HOST_KEY = "dst_hostname";
const char *SRC_PROTO_KEY = "src_protocol";
const char *SRC_IP_KEY = "src_ip";
const char *SRC_PORT_KEY = "src_port";
const char *SOURCE_IP_KEY = "source_ip";

static void run_packet_loop(uv_loop_t *loop, tunneler_context tnlr_ctx);

STAILQ_HEAD(tlnr_ctx_list_s, tunneler_ctx_s) tnlr_ctx_list_head = STAILQ_HEAD_INITIALIZER(tnlr_ctx_list_head);

// todo expose tunneler_context to modules that need `ziti_tunnel_async_send` (e.g. windows-scripts) so these can be removed
static uv_once_t default_loop_sem_init_once = UV_ONCE_INIT;
static uv_sem_t default_loop_sem;
static void default_loop_sem_init(void) {
    uv_sem_init(&default_loop_sem, 1);
}

static tunneler_context create_tunneler_ctx(tunneler_sdk_options *opts, uv_loop_t *loop) {
    TNL_LOG(INFO, "Ziti Tunneler SDK (%s)", ziti_tunneler_version());

    if (opts == NULL) {
        TNL_LOG(ERR, "invalid tunneler options");
        return NULL;
    }

    struct tunneler_ctx_s *ctx = calloc(1, sizeof(struct tunneler_ctx_s));
    if (ctx == NULL) {
        TNL_LOG(ERR, "failed to allocate tunneler context");
        return NULL;
    }
    ctx->loop = loop;
    uv_sem_init(&ctx->sem, 1);
    uv_once(&default_loop_sem_init_once, default_loop_sem_init);
    memcpy(&ctx->opts, opts, sizeof(ctx->opts));
    return ctx;
}

tunneler_context ziti_tunneler_init_host_only(tunneler_sdk_options *opts, uv_loop_t *loop) {
    return create_tunneler_ctx(opts, loop);
}

tunneler_context ziti_tunneler_init(tunneler_sdk_options *opts, uv_loop_t *loop) {
    struct tunneler_ctx_s *ctx = create_tunneler_ctx(opts, loop);
    if (ctx == NULL) {
        return NULL;
    }

    LIST_INIT(&ctx->intercepts);
    ctx->intercepts_cache.impl = NULL;

    run_packet_loop(loop, ctx);

    return ctx;
}

void ziti_tunnel_commit_routes(tunneler_context tnlr_ctx) {
    if (tnlr_ctx->opts.netif_driver == NULL) {
        TNL_LOG(DEBUG, "No netif_driver found tun is running in host only mode and intercepts are disabled");
        return;
    }

    if (tnlr_ctx->opts.netif_driver->commit_routes != NULL) {
        tnlr_ctx->opts.netif_driver->commit_routes(tnlr_ctx->opts.netif_driver->handle, tnlr_ctx->loop);
    }
}

void ziti_tunneler_exclude_route(tunneler_context tnlr_ctx, const char *dst) {
    if (tnlr_ctx->opts.netif_driver == NULL) {
        TNL_LOG(DEBUG, "No netif_driver found tun is running in host only mode and intercepts are disabled");
        return;
    }

    if (tnlr_ctx->opts.netif_driver->exclude_rt == NULL) {
        TNL_LOG(WARN, "netif_driver->exclude_rt function is not implemented");
        return;
    }

    uv_getaddrinfo_t resolve_req = {0};
    uv_interface_address_t *if_addrs;
    int err, num_if_addrs;
    if ((err = uv_interface_addresses(&if_addrs, &num_if_addrs)) != 0) {
        TNL_LOG(ERR, "uv_interface_addresses failed: %s", uv_strerror(err));
        return;
    }

    TNL_LOG(DEBUG, "excluding %s from tunneler intercept", dst);

    uv_getaddrinfo(tnlr_ctx->loop, &resolve_req, NULL, dst, NULL, NULL);
    struct addrinfo *addrinfo = resolve_req.addrinfo;
    while (addrinfo != NULL) {
        struct excluded_route_s exrt = {0};
        uv_ip4_name((const struct sockaddr_in *) addrinfo->ai_addr, exrt.route, MAX_ROUTE_LEN);
        // make sure the address isn't local
        for (int i = 0; i < num_if_addrs; i++) {
            struct sockaddr *a = (struct sockaddr *) &if_addrs[i].address;
            if (a->sa_family == AF_INET && addrinfo->ai_family == AF_INET) {
                struct sockaddr_in *if_addr = (struct sockaddr_in *) a;
                struct sockaddr_in *if_mask = (struct sockaddr_in *) &if_addrs[i].netmask;
                struct sockaddr_in *ex_addr = (struct sockaddr_in *) addrinfo->ai_addr;
                if ((if_addr->sin_addr.s_addr & if_mask->sin_addr.s_addr) ==
                    (ex_addr->sin_addr.s_addr & if_mask->sin_addr.s_addr)) {
                    TNL_LOG(DEBUG, "%s is a local address on %s; not excluding route", exrt.route, if_addrs[i].name);
                    goto done;
                }
            } else if (a->sa_family == AF_INET6) {
                TNL_LOG(TRACE, "ipv6 address compare not implemented");
            }
        }
        tnlr_ctx->opts.netif_driver->exclude_rt(tnlr_ctx->opts.netif_driver->handle, tnlr_ctx->loop, exrt.route);
        addrinfo = addrinfo->ai_next;
    }

    done:
    uv_freeaddrinfo(resolve_req.addrinfo);
    uv_free_interface_addresses(if_addrs, num_if_addrs);
}


static void tunneler_kill_active(const void *ztx);

void ziti_tunneler_shutdown(tunneler_context tnlr_ctx) {
    TNL_LOG(DEBUG, "tnlr_ctx %p", tnlr_ctx);

    while (!LIST_EMPTY(&tnlr_ctx->intercepts)) {
        intercept_ctx_t *i = LIST_FIRST(&tnlr_ctx->intercepts);
        tunneler_kill_active(i->app_intercept_ctx);
        LIST_REMOVE(i, entries);
    }
}

/** called by tunneler application when data has been successfully written to ziti */
void ziti_tunneler_ack(struct write_ctx_s *write_ctx) {
    write_ctx->ack(write_ctx);
    free(write_ctx);
}

const char *get_intercepted_address(const struct tunneler_io_ctx_s * tnlr_io) {
    if (tnlr_io == NULL) {
        return NULL;
    }
    return tnlr_io->intercepted;
}

const char *get_client_address(const struct tunneler_io_ctx_s * tnlr_io) {
    if (tnlr_io == NULL) {
        return NULL;
    }
    return tnlr_io->client;
}

void free_tunneler_io_context(tunneler_io_context *tnlr_io_ctx_p) {
    if (tnlr_io_ctx_p == NULL) {
        return;
    }

    if (*tnlr_io_ctx_p != NULL) {
        tunneler_io_context io = *tnlr_io_ctx_p;
        if (io->service_name != NULL) free((char*)io->service_name);
        free(io);
        *tnlr_io_ctx_p = NULL;
    }
}

void ziti_tunneler_set_idle_timeout(struct io_ctx_s *io_context, unsigned int timeout) {
    io_context->tnlr_io->idle_timeout = timeout;
}
/**
 * called by tunneler application when a service dial has completed
 * - let the client know that we have a connection (e.g. send SYN/ACK)
 */
void ziti_tunneler_dial_completed(struct io_ctx_s *io, bool ok) {
    if (io == NULL) {
        TNL_LOG(ERR, "null io");
        return;
    }
    if (io->ziti_io == NULL || io->tnlr_io == NULL) {
        TNL_LOG(ERR, "null ziti_io or tnlr_io");
    }
    const char *status = ok ? "succeeded" : "failed";
    TNL_LOG(DEBUG, "ziti dial %s: client[%s] service[%s]", status, io->tnlr_io->client, io->tnlr_io->service_name);

    switch (io->tnlr_io->proto) {
        case tun_tcp:
            tunneler_tcp_dial_completed(io, ok);
            break;
        case tun_udp:
            tunneler_udp_dial_completed(io, ok);
            break;
        default:
            TNL_LOG(ERR, "unknown proto %d", io->tnlr_io->proto);
            break;
    }
}

host_ctx_t *ziti_tunneler_host(tunneler_context tnlr_ctx, const void *ziti_ctx, const char *service_name, cfg_type_e cfg_type, void *config) {
    return tnlr_ctx->opts.ziti_host((void *) ziti_ctx, tnlr_ctx->loop, service_name, cfg_type, config);
}

intercept_ctx_t* intercept_ctx_new(tunneler_context tnlr_ctx, const char *app_id, void *app_intercept_ctx) {
    intercept_ctx_t *ictx = calloc(1, sizeof(intercept_ctx_t));
    ictx->tnlr_ctx = tnlr_ctx;
    ictx->service_name = strdup(app_id);
    ictx->app_intercept_ctx = app_intercept_ctx;
    STAILQ_INIT(&ictx->protocols);
    STAILQ_INIT(&ictx->addresses);
    STAILQ_INIT(&ictx->port_ranges);
    STAILQ_INIT(&ictx->allowed_source_addresses);

    return ictx;
}

void intercept_ctx_set_match_addr(intercept_ctx_t *intercept, intercept_match_addr_fn pred) {
    intercept->match_addr = pred;
}

void intercept_ctx_add_protocol(intercept_ctx_t *ctx, const char *protocol) {
    protocol_t *proto = calloc(1, sizeof(protocol_t));
    proto->protocol = strdup(protocol);
    STAILQ_INSERT_TAIL(&ctx->protocols, proto, entries);
}

void intercept_ctx_add_address(intercept_ctx_t *i_ctx, const ziti_address *za) {
    if (!i_ctx || !za) {
        return;
    }
    address_t *a = calloc(1, sizeof(address_t));
    memcpy(&a->za, za, sizeof(ziti_address));
    ziti_address_print(a->str, sizeof(a->str), za);
    STAILQ_INSERT_TAIL(&i_ctx->addresses, a, entries);
}

void intercept_ctx_add_allowed_source_address(intercept_ctx_t *i_ctx, const ziti_address *za) {
    if (!i_ctx || !za) {
        return;
    }
    address_t *a = calloc(1, sizeof(address_t));
    memcpy(&a->za, za, sizeof(ziti_address));
    ziti_address_print(a->str, sizeof(a->str), za);
    STAILQ_INSERT_TAIL(&i_ctx->allowed_source_addresses, a, entries);
}

port_range_t *parse_port_range(uint16_t low, uint16_t high) {
    port_range_t *pr = calloc(1, sizeof(port_range_t));
    if (low <= high) {
        pr->low = low;
        pr->high = high;
    } else {
        pr->low = high;
        pr->high = low;
    }

    if (low == high) {
        snprintf(pr->str, sizeof(pr->str), "%d", low);
    } else {
        snprintf(pr->str, sizeof(pr->str), "[%d-%d]", low, high);
    }
    return pr;
}

port_range_t *intercept_ctx_add_port_range(intercept_ctx_t *i_ctx, uint16_t low, uint16_t high) {
    port_range_t *pr = parse_port_range(low, high);
    STAILQ_INSERT_TAIL(&i_ctx->port_ranges, pr, entries);
    return pr;
}

void intercept_ctx_override_cbs(intercept_ctx_t *i_ctx, ziti_sdk_dial_cb dial, ziti_sdk_write_cb write, ziti_sdk_close_cb close_write, ziti_sdk_close_cb close) {
    i_ctx->dial_fn = dial;
    i_ctx->write_fn = write;
    i_ctx->close_write_fn = close_write;
    i_ctx->close_fn = close;
}

/** intercept a service as described by the intercept_ctx */
int ziti_tunneler_intercept(tunneler_context tnlr_ctx, intercept_ctx_t *i_ctx) {
    if (tnlr_ctx == NULL) {
        TNL_LOG(ERR, "null tnlr_ctx");
        return -1;
    }

    model_map_clear(&tnlr_ctx->intercepts_cache, NULL);
    address_t *address;
    STAILQ_FOREACH(address, &i_ctx->addresses, entries) {
        protocol_t *proto;
        STAILQ_FOREACH(proto, &i_ctx->protocols, entries) {
            port_range_t *pr;
            STAILQ_FOREACH(pr, &i_ctx->port_ranges, entries) {
                // todo find conflicts with services
                // intercept_ctx_t *match;
                // match = lookup_intercept_by_address(tnlr_ctx, proto->protocol, &address->ip, pr->low, pr->high);
                TNL_LOG(DEBUG, "intercepting address[%s:%s:%s] service[%s]",
                        proto->protocol, address->str, pr->str, i_ctx->service_name);
            }
        }
    }

    STAILQ_FOREACH(address, &i_ctx->addresses, entries) {
         add_route(tnlr_ctx->opts.netif_driver, address);
    }

    LIST_INSERT_HEAD(&tnlr_ctx->intercepts, (struct intercept_ctx_s *)i_ctx, entries);

    return 0;
}

static void tunneler_kill_active(const void *zi_ctx) {
    struct io_ctx_list_s *l;
    ziti_sdk_close_cb zclose;

    l = tunneler_tcp_active(zi_ctx);
    while (!SLIST_EMPTY(l)) {
        struct io_ctx_list_entry_s *n = SLIST_FIRST(l);
        TNL_LOG(DEBUG, "service_ctx[%p] client[%s] killing active connection", zi_ctx, n->io->tnlr_io->client);
        // close the ziti connection, which also closes the underlay
        zclose = n->io->close_fn;
        if (zclose) zclose(n->io->ziti_io);
        SLIST_REMOVE_HEAD(l, entries);
        free(n);
    }
    free(l);

    // todo be selective about protocols when merging newer config types
    l = tunneler_udp_active(zi_ctx);
    while (!SLIST_EMPTY(l)) {
        struct io_ctx_list_entry_s *n = SLIST_FIRST(l);
        TNL_LOG(DEBUG, "service[%p] client[%s] killing active connection", zi_ctx, n->io->tnlr_io->client);
        // close the ziti connection, which also closes the underlay
        zclose = n->io->close_fn;
        if (zclose) zclose(n->io->ziti_io);
        SLIST_REMOVE_HEAD(l, entries);
        free(n);
    }
    free(l);
}

intercept_ctx_t * ziti_tunnel_find_intercept(tunneler_context tnlr_ctx, void *zi_ctx) {
    struct intercept_ctx_s *intercept;
    if (tnlr_ctx == NULL) {
        TNL_LOG(WARN, "null tnlr_ctx");
        return NULL;
    }

    LIST_FOREACH(intercept, &tnlr_ctx->intercepts, entries) {
        if (intercept->app_intercept_ctx == zi_ctx) {
            return intercept;
        }
    }

    return NULL;
}


// when called due to service unavailable we want to remove from tnlr_ctx.
// when called due to conflict we want to mark as disabled
void ziti_tunneler_stop_intercepting(tunneler_context tnlr_ctx, void *zi_ctx) {
    TNL_LOG(DEBUG, "removing intercept for service_ctx[%p]", zi_ctx);
    model_map_clear(&tnlr_ctx->intercepts_cache, NULL);
    struct intercept_ctx_s *intercept = ziti_tunnel_find_intercept(tnlr_ctx, zi_ctx);

    if (intercept != NULL) {
        TNL_LOG(DEBUG, "removing routes for service[%s] service_ctx[%p]", intercept->service_name, zi_ctx);
        tunneler_kill_active(zi_ctx);

        LIST_REMOVE(intercept, entries);

        struct address_s *address;
        STAILQ_FOREACH(address, &intercept->addresses, entries) {
            delete_route(tnlr_ctx->opts.netif_driver, address);
        }

        free_intercept(intercept);
    }


    tunneler_kill_active(zi_ctx);

}

/** called by tunneler application when data is read from a ziti connection */
ssize_t ziti_tunneler_write(tunneler_io_context tnlr_io_ctx, const void *data, size_t len) {
    if (tnlr_io_ctx == NULL) {
        TNL_LOG(WARN, "null tunneler io context");
        return -1;
    }

    ssize_t r;
    switch (tnlr_io_ctx->proto) {
        case tun_tcp:
            r = tunneler_tcp_write(tnlr_io_ctx->tcp, data, len);
            break;
        case tun_udp:
            r = tunneler_udp_write(tnlr_io_ctx->udp, data, len);
            break;
    }

    return r;
}

/** called by tunneler application when a ziti connection closes */
int ziti_tunneler_close(tunneler_io_context tnlr_io_ctx) {
    if (tnlr_io_ctx == NULL) {
        TNL_LOG(DEBUG, "null tnlr_io_ctx");
        return 0;
    }
    TNL_LOG(DEBUG, "closing connection: client[%s] service[%s]",
            tnlr_io_ctx->client, tnlr_io_ctx->service_name);
    switch (tnlr_io_ctx->proto) {
        case tun_tcp:
            tunneler_tcp_close(tnlr_io_ctx->tcp);
            tnlr_io_ctx->tcp = NULL;
            break;
        case tun_udp:
            tunneler_udp_close(tnlr_io_ctx->udp);
            tnlr_io_ctx->udp = NULL;
            break;
        default:
            TNL_LOG(ERR, "unknown proto %d", tnlr_io_ctx->proto);
            break;
    }

    if (tnlr_io_ctx->conn_timer) {
        uv_close((uv_handle_t *) tnlr_io_ctx->conn_timer, (uv_close_cb) free);
        tnlr_io_ctx->conn_timer = NULL;
    }

    free_tunneler_io_context(&tnlr_io_ctx);
    return 0;
}

/** called by tunneler application when an EOF is received from ziti */
int ziti_tunneler_close_write(tunneler_io_context tnlr_io_ctx) {
    if (tnlr_io_ctx == NULL) {
        TNL_LOG(DEBUG, "null tnlr_io_ctx");
        return 0;
    }
    TNL_LOG(DEBUG, "closing write connection: client[%s] service[%s]",
            tnlr_io_ctx->client, tnlr_io_ctx->service_name);
    switch (tnlr_io_ctx->proto) {
        case tun_tcp:
            tunneler_tcp_close_write(tnlr_io_ctx->tcp);
            break;
        default:
            TNL_LOG(DEBUG, "not sending FIN on %d connection", tnlr_io_ctx->proto);
            break;
    }
    return 0;
}

static void on_tun_data(uv_poll_t * req, int status, int events) {
    if (status != 0) {
        TNL_LOG(WARN, "not sure why status is %d", status);
        return;
    }

    if (events & UV_READABLE) {
        netif_shim_input(netif_default);
    }
}

static void check_lwip_timeouts(uv_timer_t * timer) {
    // if timer is not active it may have been a while since
    // we run timers, let LWIP adjust timeouts
    if (!uv_is_active((const uv_handle_t *) timer)) {
        sys_restart_timeouts();
    }

    // run timers before potentially pausing
    sys_check_timeouts();

    if (tcp_active_pcbs == NULL && tcp_tw_pcbs == NULL) {
        uv_timer_stop(timer);
        return;
    }

    u32_t sleep = sys_timeouts_sleeptime();
    TNL_LOG(TRACE, "next wake in %d millis", sleep);
    // second sleep arg is only need to make timer `active` next time
    // we hit this function to avoid calling sys_restart_timeouts()
    uv_timer_start(timer, check_lwip_timeouts, sleep, sleep);
}

void check_tnlr_timer(tunneler_context tnlr_ctx) {
    check_lwip_timeouts(&tnlr_ctx->lwip_timer_req);
}

/**
 * set up a protocol handler. lwip will call recv_fn with arg for each
 * packet that matches the protocol.
 */
static struct raw_pcb * init_protocol_handler(u8_t proto, raw_recv_fn recv_fn, void *arg) {
    struct raw_pcb *pcb;
    err_t err;

    if ((pcb = raw_new_ip_type(IPADDR_TYPE_ANY, proto)) == NULL) {
        TNL_LOG(ERR, "failed to allocate raw pcb for protocol %d", proto);
        return NULL;
    }

    if ((err = raw_bind(pcb, IP_ANY_TYPE)) != ERR_OK) {
        TNL_LOG(ERR, "failed to bind for protocol %d: error %d", proto, err);
        raw_remove(pcb);
        return NULL;
    }

    raw_bind_netif(pcb, netif_default);
    raw_recv(pcb, recv_fn, arg);

    return pcb;
}

static void run_packet_loop(uv_loop_t *loop, tunneler_context tnlr_ctx) {
    tunneler_sdk_options opts = tnlr_ctx->opts;
    if (opts.ziti_close == NULL || opts.ziti_close_write == NULL ||  opts.ziti_write == NULL ||
        opts.ziti_dial == NULL || opts.ziti_host == NULL) {
        TNL_LOG(ERR, "ziti_sdk_* callback options cannot be null");
        exit(1);
    }

    lwip_init();

    netif_driver netif_driver = opts.netif_driver;
    if (netif_add_noaddr(&tnlr_ctx->netif, netif_driver, netif_shim_init, ip_input) == NULL) {
        TNL_LOG(ERR, "netif_add failed");
        exit(1);
    }

    netif_set_default(&tnlr_ctx->netif);
    netif_set_link_up(&tnlr_ctx->netif);
    netif_set_up(&tnlr_ctx->netif);

    if (netif_driver->setup) {
        netif_driver->setup(netif_driver->handle, loop, on_packet, netif_default);
    } else if (netif_driver->uv_poll_init) {
        netif_driver->uv_poll_init(netif_driver->handle, loop, &tnlr_ctx->netif_poll_req);
        if (uv_poll_start(&tnlr_ctx->netif_poll_req, UV_READABLE, on_tun_data) != 0) {
            TNL_LOG(ERR, "failed to start tun poll handle");
            exit(1);
        }
    } else {
        TNL_LOG(WARN, "no method to initiate tunnel reader, maybe it's ok");
    }

    if ((tnlr_ctx->tcp = init_protocol_handler(IP_PROTO_TCP, recv_tcp, tnlr_ctx)) == NULL) {
        TNL_LOG(ERR, "tcp setup failed");
        exit(1);
    }
    if ((tnlr_ctx->udp = init_protocol_handler(IP_PROTO_UDP, recv_udp, tnlr_ctx)) == NULL) {
        TNL_LOG(ERR, "udp setup failed");
        exit(1);
    }

    // don't run LWIP timers until we have active TCP connections
    uv_timer_init(loop, &tnlr_ctx->lwip_timer_req);
    uv_unref((uv_handle_t *) &tnlr_ctx->lwip_timer_req);
}

typedef struct ziti_tunnel_async_call_s {
    ziti_tunnel_async_fn f;
    void *               arg;
} ziti_tunnel_async_call_t;

static void async_close_cb(uv_handle_t *async) {
    if (async->data != NULL) {
        free(async->data);
    }
    free(async);
}

/** invoke a caller-supplied function with argument. this is called by libuv on the loop thread */
static void ziti_tunnel_async_wrapper(uv_async_t *async) {
    ziti_tunnel_async_call_t *call = async->data;
    if (call != NULL && call->f != NULL) {
        call->f(async->loop, call->arg);
    }
    uv_close((uv_handle_t *)async, async_close_cb);
}

/** sets up a function call on the specified loop */
void ziti_tunnel_async_send(tunneler_context tctx, ziti_tunnel_async_fn f, void *arg) {
    uv_loop_t *loop = uv_default_loop();
    if (tctx != NULL) {
        loop = tctx->loop;
    }

    ziti_tunnel_async_call_t *call = calloc(1, sizeof(ziti_tunnel_async_call_t));
    call->f = f;
    call->arg = arg;

    uv_async_t *async = calloc(1, sizeof(uv_async_t));
    async->data = call;

    if (tctx != NULL) {
        uv_sem_wait(&tctx->sem);
    } else {
        uv_sem_wait(&default_loop_sem);
    }
    int e = uv_async_init(loop, async, ziti_tunnel_async_wrapper);
    if (tctx != NULL) {
        uv_sem_post(&tctx->sem);
    } else {
        uv_sem_post(&default_loop_sem);
    }
    if (e != 0) {
        TNL_LOG(ERR, "uv_async_init error: %s", uv_err_name(e));
        free(call);
        free(async);
        return;
    }

    uv_async_send(async);
}

#define _str(x) #x
#define str(x) _str(x)

IMPL_MODEL(tunnel_ip_mem_pool, TNL_IP_MEM_POOL)
IMPL_MODEL(tunnel_ip_conn, TNL_IP_CONN)
IMPL_MODEL(tunnel_ip_stats, TNL_IP_STATS)

static void ziti_tunnel_get_ip_mem_pool(tunnel_ip_mem_pool *pool, int pool_id, const char *pool_name) {
    if (!pool) return;
    TNL_LOG(VERBOSE, "getting IP mem pool %s", pool_name);
    pool->name = strdup(pool_name);
    pool->used = memp_pools[pool_id]->stats->used;
    pool->max = memp_pools[pool_id]->stats->max;
    pool->avail = memp_pools[pool_id]->stats->avail;
}

void ziti_tunnel_get_ip_stats(tunnel_ip_stats *stats) {
    if (!stats) return;
    TNL_LOG(DEBUG, "collecting ip statistics");
    if (stats->pools) free(stats->pools);
    stats->pools = calloc(4, sizeof(tunnel_ip_mem_pool *));
    stats->pools[0] = calloc(1, sizeof(tunnel_ip_mem_pool));
    ziti_tunnel_get_ip_mem_pool(stats->pools[0], MEMP_PBUF_POOL, _str(MEMP_PBUF_POOL));
    stats->pools[1] = calloc(1, sizeof(tunnel_ip_mem_pool));
    ziti_tunnel_get_ip_mem_pool(stats->pools[1], MEMP_TCP_PCB, _str(MEMP_TCP_PCB));
    stats->pools[2] = calloc(1, sizeof(tunnel_ip_mem_pool));
    ziti_tunnel_get_ip_mem_pool(stats->pools[2], MEMP_UDP_PCB, _str(MEMP_UDP_PCB));

    int max_conns = MEMP_NUM_TCP_PCB + MEMP_NUM_UDP_PCB + 1;
    stats->connections = calloc(max_conns, sizeof(tunnel_ip_conn *));

    int i= 0;
    for (struct tcp_pcb *tpcb = tcp_tw_pcbs; tpcb != NULL; tpcb = tpcb->next) {
        stats->connections[i] = calloc(1, sizeof(tunnel_ip_conn));
        tunneler_tcp_get_conn(stats->connections[i++], tpcb);
    }

    for (struct tcp_pcb *tpcb = tcp_active_pcbs; tpcb != NULL; tpcb = tpcb->next) {
        stats->connections[i] = calloc(1, sizeof(tunnel_ip_conn));
        tunneler_tcp_get_conn(stats->connections[i++], tpcb);
    }
    for (struct udp_pcb *upcb = udp_pcbs; upcb != NULL; upcb = upcb->next) {
        stats->connections[i] = calloc(1, sizeof(tunnel_ip_conn));
        tunneler_udp_get_conn(stats->connections[i++], upcb);
    }
}


const char* ziti_tunneler_version() {
    return str(GIT_VERSION);
}

const char* ziti_tunneler_build_date() {
    return str(BUILD_DATE);
}
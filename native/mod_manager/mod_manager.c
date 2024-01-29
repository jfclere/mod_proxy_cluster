/*
 * Copyright The mod_cluster Project Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * @author Jean-Frederic Clere
 */

#include "apr_strings.h"
#include "apr_lib.h"
#include "apr_uuid.h"

#define CORE_PRIVATE
#include "httpd.h"
#include "http_config.h"
#include "http_log.h"
#include "http_main.h"
#include "http_request.h"
#include "http_protocol.h"
#include "http_core.h"
#include "scoreboard.h"
#include "mod_proxy.h"
#include "ap_mpm.h"

#include "ap_slotmem.h"

#include "node.h"
#include "host.h"
#include "context.h"
#include "balancer.h"
#include "sessionid.h"
#include "domain.h"

#include "common.h"

#include "mod_proxy_cluster.h"

#define DEFMAXCONTEXT    100
#define DEFMAXNODE       20
#define DEFMAXHOST       20
#define DEFMAXSESSIONID  0 /* it has performance/security impact */
#define MAXMESSSIZE      1024

/* Warning messages */
#define SBALBAD          "Balancer name contained an upper case character. We will use \"%s\" instead."

/* Error messages */
#define TYPESYNTAX       1
#define SMESPAR          "SYNTAX: Can't parse MCMP message. It might have contained illegal symbols or unknown elements."
#define SBALBIG          "SYNTAX: Balancer field too big"
#define SBAFBIG          "SYNTAX: A field is too big"
#define SROUBIG          "SYNTAX: JVMRoute field too big"
#define SROUBAD          "SYNTAX: JVMRoute can't be empty"
#define SDOMBIG          "SYNTAX: LBGroup field too big"
#define SHOSBIG          "SYNTAX: Host field too big"
#define SPORBIG          "SYNTAX: Port field too big"
#define STYPBIG          "SYNTAX: Type field too big"
#define SALIBAD          "SYNTAX: Alias without Context"
#define SCONBAD          "SYNTAX: Context without Alias"
#define SBADFLD          "SYNTAX: Invalid field \"%s\" in message"
#define SMISFLD          "SYNTAX: Mandatory field(s) missing in message"
#define SCMDUNS          "SYNTAX: Command is not supported"
#define SMULALB          "SYNTAX: Only one Alias in APP command"
#define SMULCTB          "SYNTAX: Only one Context in APP command"
#define SREADER          "SYNTAX: %s can't read POST data"

#define SJIDBIG          "SYNTAX: JGroupUuid field too big"
#define SJDDBIG          "SYNTAX: JGroupData field too big"
#define SJIDBAD          "SYNTAX: JGroupUuid can't be empty"

#define TYPEMEM          2
#define MNODEUI          "MEM: Can't update or insert node with \"%s\" JVMRoute"
#define MNODERM          "MEM: Old node with \"%s\" JVMRoute still exists"
#define MBALAUI          "MEM: Can't update or insert balancer for node with \"%s\" JVMRoute"
#define MNODERD          "MEM: Can't read node with \"%s\" JVMRoute"
#define MHOSTRD          "MEM: Can't read host alias for node with \"%s\" JVMRoute"
#define MHOSTUI          "MEM: Can't update or insert host alias for node with \"%s\" JVMRoute"
#define MCONTUI          "MEM: Can't update or insert context for node with \"%s\" JVMRoute"
#define MJBIDRD          "MEM: Can't read JGroupId"
#define MJBIDUI          "MEM: Can't update or insert JGroupId"
#define MNODEET          "MEM: Another for the same worker already exist"

/* Protocol version supported */
#define VERSION_PROTOCOL "0.2.1"

/* Internal substitution for node commands */
#define NODE_COMMAND     "/NODE_COMMAND"

/* range of the commands */
#define RANGECONTEXT     0
#define RANGENODE        1
#define RANGEDOMAIN      2

/* define content-type */
#define TEXT_PLAIN       1
#define TEXT_XML         2

/* Data structure for shared memory block */
typedef struct version_data
{
    apr_uint64_t counter;
} version_data;

/* mutex and lock for tables/slotmen */
static apr_global_mutex_t *node_mutex;
static apr_global_mutex_t *context_mutex;
static const char *node_mutex_type = "node-shm";
static const char *context_mutex_type = "context-shm";

/* counter for the version (nodes) */
static ap_slotmem_instance_t *version_node_mem = NULL;

/* shared memory */
static mem_t *contextstatsmem = NULL;
static mem_t *nodestatsmem = NULL;
static mem_t *hoststatsmem = NULL;
static mem_t *balancerstatsmem = NULL;
static mem_t *sessionidstatsmem = NULL;
static mem_t *domainstatsmem = NULL;

static slotmem_storage_method *storage = NULL;
static balancer_method *balancerhandler = NULL;
static void (*advertise_info)(request_rec *) = NULL;

static APR_OPTIONAL_FN_TYPE(balancer_manage) *balancer_manage = NULL;

module AP_MODULE_DECLARE_DATA manager_module;

static char balancer_nonce[APR_UUID_FORMATTED_LENGTH + 1];

typedef struct mod_manager_config
{
    /* base name for the shared memory */
    char *basefilename;
    /* max number of context supported */
    unsigned maxcontext;
    /* max number of node supported */
    unsigned maxnode;
    /* max number of host supported */
    unsigned maxhost;
    /* max number of session supported */
    unsigned maxsessionid;

    /* version, the version is increased each time the node update logic is called */
    unsigned tableversion;

    /* Should be the slotmem persisted (1) or not (0) */
    int persistent;

    /* check for nonce in the command logic */
    int nonce;

    /* default name for balancer */
    char *balancername;

    /* allow aditional display */
    int allow_display;
    /* allow command logic */
    int allow_cmd;
    /* don't context in first status page */
    int reduce_display;
    /* maximum message size */
    int maxmesssize;
    /* Enable MCPM receiver */
    int enable_mcpm_receive;
    /* Enable WebSocket Proxy */
    int enable_ws_tunnel;
    /* WebSocket upgrade header */
    char *ws_upgrade_header;
    /* AJP secret */
    char *ajp_secret;
    /* size of the proxy response field buffer */
    long response_field_size;

} mod_manager_config;

/* routines for the node_storage_method */
static apr_status_t loc_read_node(int ids, nodeinfo_t **node)
{
    return get_node(nodestatsmem, node, ids);
}

static int loc_get_ids_used_node(int *ids)
{
    return get_ids_used_node(nodestatsmem, ids);
}

static int loc_get_max_size_node(void)
{
    return nodestatsmem ? get_max_size_node(nodestatsmem) : 0;
}

static apr_status_t loc_remove_node(int id)
{
    return remove_node(nodestatsmem, id);
}

static apr_status_t loc_find_node(nodeinfo_t **node, const char *route)
{
    return find_node(nodestatsmem, node, route);
}

/**
 * Increase the version of the nodes table
 */
static void inc_version_node(void)
{
    version_data *base;
    if (storage->dptr(version_node_mem, 0, (void **)&base) == APR_SUCCESS)
        base->counter++;
}
static apr_uint64_t get_version_node(void)
{
    version_data *base;
    if (storage->dptr(version_node_mem, 0, (void **)&base) == APR_SUCCESS)
        return base->counter;
    else
        return 0;
}
static void set_version_node(apr_uint64_t val)
{
    version_data *base;
    if (storage->dptr(version_node_mem, 0, (void **)&base) == APR_SUCCESS)
        base->counter = val;
}

/**
 * Check is the nodes (in shared memory) were modified since last
 * call to worker_nodes_are_updated().
 *
 * @param data server_rec
 * @param pool unused argument
 * @return 0 (no update) or X (the version has changed, the local table needs to be updated)
 */
static unsigned loc_worker_nodes_need_update(void *data, apr_pool_t *pool)
{
    int size;
    server_rec *s = (server_rec *)data;
    unsigned last = 0;
    mod_manager_config *mconf = ap_get_module_config(s->module_config, &manager_module);
    (void)pool;

    size = loc_get_max_size_node();
    if (size == 0) {
        return 0; /* broken */
    }

    last = get_version_node();

    if (last != mconf->tableversion) {
        return last;
    }
    return 0;
}

/**
 * Store the last version update in the proccess config
 */
static int loc_worker_nodes_are_updated(void *data, unsigned last)
{
    server_rec *s = (server_rec *)data;
    mod_manager_config *mconf = ap_get_module_config(s->module_config, &manager_module);
    mconf->tableversion = last;
    return 0;
}

static apr_status_t loc_lock_nodes(void)
{
    return apr_global_mutex_lock(node_mutex);
}

static apr_status_t loc_unlock_nodes(void)
{
    return apr_global_mutex_unlock(node_mutex);
}

static int loc_get_max_size_context(void)
{
    return contextstatsmem ? get_max_size_context(contextstatsmem) : 0;
}

static int loc_get_max_size_host(void)
{
    return hoststatsmem ? get_max_size_host(hoststatsmem) : 0;
}

/**
 * Remove the virtual hosts and contexts corresponding the node
 */
static void loc_remove_host_context(int node, apr_pool_t *pool)
{
    /* for read the hosts */
    int i;
    int size = loc_get_max_size_host();
    int *id;
    int sizecontext = loc_get_max_size_context();
    int *idcontext;

    if (size == 0) {
        return;
    }
    id = apr_palloc(pool, sizeof(int) * size);
    idcontext = apr_palloc(pool, sizeof(int) * sizecontext);
    size = get_ids_used_host(hoststatsmem, id);
    for (i = 0; i < size; i++) {
        hostinfo_t *ou;

        if (get_host(hoststatsmem, &ou, id[i]) != APR_SUCCESS) {
            continue;
        }
        if (ou->node == node) {
            remove_host(hoststatsmem, ou->id);
        }
    }

    sizecontext = get_ids_used_context(contextstatsmem, idcontext);
    for (i = 0; i < sizecontext; i++) {
        contextinfo_t *context;
        if (get_context(contextstatsmem, &context, idcontext[i]) != APR_SUCCESS) {
            continue;
        }
        if (context->node == node) {
            remove_context(contextstatsmem, context->id);
        }
    }
}

static const struct node_storage_method node_storage = {
    loc_read_node,
    loc_get_ids_used_node,
    loc_get_max_size_node,
    loc_worker_nodes_need_update,
    loc_worker_nodes_are_updated,
    loc_remove_node,
    loc_find_node,
    loc_remove_host_context,
    loc_lock_nodes,
    loc_unlock_nodes,
};

/*
 * routines for the context_storage_method
 */
static apr_status_t loc_read_context(int ids, contextinfo_t **context)
{
    return get_context(contextstatsmem, context, ids);
}

static int loc_get_ids_used_context(int *ids)
{
    return get_ids_used_context(contextstatsmem, ids);
}

static apr_status_t loc_lock_contexts(void)
{
    return apr_global_mutex_lock(context_mutex);
}

static apr_status_t loc_unlock_contexts(void)
{
    return apr_global_mutex_unlock(context_mutex);
}

/* clang-format off */
static const struct context_storage_method context_storage = {
    loc_read_context,
    loc_get_ids_used_context,
    loc_get_max_size_context,
    loc_lock_contexts,
    loc_unlock_contexts
};
/* clang-format on */

/*
 * routines for the host_storage_method
 */
static apr_status_t loc_read_host(int ids, hostinfo_t **host)
{
    return get_host(hoststatsmem, host, ids);
}

static int loc_get_ids_used_host(int *ids)
{
    return get_ids_used_host(hoststatsmem, ids);
}

static const struct host_storage_method host_storage = {
    loc_read_host,
    loc_get_ids_used_host,
    loc_get_max_size_host,
};

/*
 * routines for the balancer_storage_method
 */
static apr_status_t loc_read_balancer(int ids, balancerinfo_t **balancer)
{
    return get_balancer(balancerstatsmem, balancer, ids);
}

static int loc_get_ids_used_balancer(int *ids)
{
    return get_ids_used_balancer(balancerstatsmem, ids);
}

static int loc_get_max_size_balancer(void)
{
    return balancerstatsmem ? get_max_size_balancer(balancerstatsmem) : 0;
}

static const struct balancer_storage_method balancer_storage = {
    loc_read_balancer,
    loc_get_ids_used_balancer,
    loc_get_max_size_balancer,
};

/*
 * routines for the sessionid_storage_method
 */
static apr_status_t loc_read_sessionid(int ids, sessionidinfo_t **sessionid)
{
    return get_sessionid(sessionidstatsmem, sessionid, ids);
}

static int loc_get_ids_used_sessionid(int *ids)
{
    return get_ids_used_sessionid(sessionidstatsmem, ids);
}

static int loc_get_max_size_sessionid(void)
{
    return sessionidstatsmem ? get_max_size_sessionid(sessionidstatsmem) : 0;
}

static apr_status_t loc_remove_sessionid(sessionidinfo_t *sessionid)
{
    return remove_sessionid(sessionidstatsmem, sessionid);
}

static apr_status_t loc_insert_update_sessionid(sessionidinfo_t *sessionid)
{
    return insert_update_sessionid(sessionidstatsmem, sessionid);
}

static const struct sessionid_storage_method sessionid_storage = {
    loc_read_sessionid,   loc_get_ids_used_sessionid,  loc_get_max_size_sessionid,
    loc_remove_sessionid, loc_insert_update_sessionid,
};

/*
 * routines for the domain_storage_method
 */
static apr_status_t loc_read_domain(int ids, domaininfo_t **domain)
{
    return get_domain(domainstatsmem, domain, ids);
}

static int loc_get_ids_used_domain(int *ids)
{
    return get_ids_used_domain(domainstatsmem, ids);
}

static int loc_get_max_size_domain(void)
{
    return domainstatsmem ? get_max_size_domain(domainstatsmem) : 0;
}

static apr_status_t loc_remove_domain(domaininfo_t *domain)
{
    return remove_domain(domainstatsmem, domain);
}

static apr_status_t loc_insert_update_domain(domaininfo_t *domain)
{
    return insert_update_domain(domainstatsmem, domain);
}

static apr_status_t loc_find_domain(domaininfo_t **domain, const char *route, const char *balancer)
{
    return find_domain(domainstatsmem, domain, route, balancer);
}

/* clang-format off */
static const struct domain_storage_method domain_storage = {
    loc_read_domain,
    loc_get_ids_used_domain,
    loc_get_max_size_domain,
    loc_remove_domain,
    loc_insert_update_domain,
    loc_find_domain,
};
/* clang-format on */

/* helper for the handling of the Alias: host1,... Context: context1,... */
struct cluster_host
{
    char *host;
    char *context;
    struct cluster_host *next;
};

/*
 * cleanup logic
 */
static apr_status_t cleanup_manager(void *param)
{
    /* shared memory */
    contextstatsmem = NULL;
    nodestatsmem = NULL;
    hoststatsmem = NULL;
    balancerstatsmem = NULL;
    sessionidstatsmem = NULL;
    domainstatsmem = NULL;
    version_node_mem = NULL;
    (void)param;
    return APR_SUCCESS;
}

static void mc_initialize_cleanup(apr_pool_t *p)
{
    apr_pool_cleanup_register(p, NULL, cleanup_manager, apr_pool_cleanup_null);
}

static void normalize_balancer_name(char *balancer_name, const server_rec *s)
{
    int upper_case_char_found = 0;
    char *balancer_name_start = balancer_name;
    for (; *balancer_name; ++balancer_name) {
        if (!upper_case_char_found) {
            upper_case_char_found = apr_isupper(*balancer_name);
        }
        *balancer_name = apr_tolower(*balancer_name);
    }
    balancer_name = balancer_name_start;
    if (upper_case_char_found) {
        ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, s, SBALBAD, balancer_name);
    }
}

/*
 * This routine is called in the parent; we must register our
 * mutex type before the config is processed so that users can
 * adjust the mutex settings using the Mutex directive.
 */
static int manager_pre_config(apr_pool_t *pconf, apr_pool_t *plog, apr_pool_t *ptemp)
{
    (void)plog;
    (void)ptemp;
    ap_mutex_register(pconf, node_mutex_type, NULL, APR_LOCK_DEFAULT, 0);
    ap_mutex_register(pconf, context_mutex_type, NULL, APR_LOCK_DEFAULT, 0);
    return OK;
}

/*
 * Call after parser the configuration.
 * Creates the shared memory.
 */
static int manager_init(apr_pool_t *p, apr_pool_t *plog, apr_pool_t *ptemp, server_rec *s)
{
    char *node;
    char *context;
    char *host;
    char *balancer;
    char *sessionid;
    char *domain;
    char *version;
    apr_uuid_t uuid;
    mod_manager_config *mconf = ap_get_module_config(s->module_config, &manager_module);
    apr_status_t rv;
    (void)plog; /* unused variable */

    if (ap_state_query(AP_SQ_MAIN_STATE) == AP_SQ_MS_CREATE_PRE_CONFIG) {
        return OK;
    }

    if (mconf->basefilename) {
        node = apr_pstrcat(ptemp, mconf->basefilename, "/manager.node", NULL);
        context = apr_pstrcat(ptemp, mconf->basefilename, "/manager.context", NULL);
        host = apr_pstrcat(ptemp, mconf->basefilename, "/manager.host", NULL);
        balancer = apr_pstrcat(ptemp, mconf->basefilename, "/manager.balancer", NULL);
        sessionid = apr_pstrcat(ptemp, mconf->basefilename, "/manager.sessionid", NULL);
        domain = apr_pstrcat(ptemp, mconf->basefilename, "/manager.domain", NULL);
        version = apr_pstrcat(ptemp, mconf->basefilename, "/manager.version", NULL);
    } else {
        node = ap_server_root_relative(ptemp, "logs/manager.node");
        context = ap_server_root_relative(ptemp, "logs/manager.context");
        host = ap_server_root_relative(ptemp, "logs/manager.host");
        balancer = ap_server_root_relative(ptemp, "logs/manager.balancer");
        sessionid = ap_server_root_relative(ptemp, "logs/manager.sessionid");
        domain = ap_server_root_relative(ptemp, "logs/manager.domain");
        version = ap_server_root_relative(ptemp, "logs/manager.version");
    }

    /* Do some sanity checks */
    if (mconf->maxhost < mconf->maxnode) {
        ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, s, "manager_init: Maxhost value increased to Maxnode (%d)",
                     mconf->maxnode);
        mconf->maxhost = mconf->maxnode;
    }
    if (mconf->maxcontext < mconf->maxhost) {
        ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, s, "manager_init: Maxcontext value increased to Maxhost (%d)",
                     mconf->maxhost);
        mconf->maxcontext = mconf->maxhost;
    }

    /* Get a provider to handle the shared memory */
    storage = ap_lookup_provider(AP_SLOTMEM_PROVIDER_GROUP, "shm", AP_SLOTMEM_PROVIDER_VERSION);
    if (storage == NULL) {
        ap_log_error(APLOG_MARK, APLOG_EMERG, 0, s, "manager_init: ap_lookup_provider %s failed",
                     AP_SLOTMEM_PROVIDER_GROUP);
        return !OK;
    }
    nodestatsmem = create_mem_node(node, &mconf->maxnode, mconf->persistent + AP_SLOTMEM_TYPE_PREGRAB, p, storage);
    if (nodestatsmem == NULL) {
        ap_log_error(APLOG_MARK, APLOG_EMERG, 0, s, "manager_init: create_mem_node %s failed", node);
        return !OK;
    }
    if (get_last_mem_error(nodestatsmem) != APR_SUCCESS) {
        char buf[120];
        ap_log_error(APLOG_MARK, APLOG_EMERG, 0, s, "manager_init: create_mem_node %s failed: %s", node,
                     apr_strerror(get_last_mem_error(nodestatsmem), buf, sizeof(buf)));
        return !OK;
    }

    contextstatsmem =
        create_mem_context(context, &mconf->maxcontext, mconf->persistent + AP_SLOTMEM_TYPE_PREGRAB, p, storage);
    if (contextstatsmem == NULL) {
        ap_log_error(APLOG_MARK, APLOG_EMERG, 0, s, "manager_init: create_mem_context failed");
        return !OK;
    }

    hoststatsmem = create_mem_host(host, &mconf->maxhost, mconf->persistent + AP_SLOTMEM_TYPE_PREGRAB, p, storage);
    if (hoststatsmem == NULL) {
        ap_log_error(APLOG_MARK, APLOG_EMERG, 0, s, "manager_init: create_mem_host failed");
        return !OK;
    }

    balancerstatsmem =
        create_mem_balancer(balancer, &mconf->maxhost, mconf->persistent + AP_SLOTMEM_TYPE_PREGRAB, p, storage);
    if (balancerstatsmem == NULL) {
        ap_log_error(APLOG_MARK, APLOG_EMERG, 0, s, "manager_init: create_mem_balancer failed");
        return !OK;
    }

    if (mconf->maxsessionid) {
        /* Only create sessionid stuff if required */
        sessionidstatsmem = create_mem_sessionid(sessionid, &mconf->maxsessionid,
                                                 mconf->persistent + AP_SLOTMEM_TYPE_PREGRAB, p, storage);
        if (sessionidstatsmem == NULL) {
            ap_log_error(APLOG_MARK, APLOG_EMERG, 0, s, "manager_init: create_mem_sessionid failed");
            return !OK;
        }
    }

    domainstatsmem =
        create_mem_domain(domain, &mconf->maxnode, mconf->persistent + AP_SLOTMEM_TYPE_PREGRAB, p, storage);
    if (domainstatsmem == NULL) {
        ap_log_error(APLOG_MARK, APLOG_EMERG, 0, s, "manager_init: create_mem_domain failed");
        return !OK;
    }

    /* For the version node we just need a apr_uint64_t in shared memory */
    rv = storage->create(&version_node_mem, version, sizeof(apr_uint64_t), 1, AP_SLOTMEM_TYPE_PREGRAB, p);
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_EMERG, rv, s, "manager_init: create_share_version failed");
        return !OK;
    }
    set_version_node(0);

    /* Get a provider to ping/pong logics */
    balancerhandler = ap_lookup_provider("proxy_cluster", "balancer", "0");
    if (balancerhandler == NULL) {
        ap_log_error(APLOG_MARK, APLOG_WARNING, 0, s, "manager_init: can't find a ping/pong logic");
    }

    advertise_info = ap_lookup_provider("advertise", "info", "0");
    balancer_manage = APR_RETRIEVE_OPTIONAL_FN(balancer_manage);

    /* Retrieve a UUID and store the nonce. */
    apr_uuid_get(&uuid);
    apr_uuid_format(balancer_nonce, &uuid);

    /* Clean up to prevent backgroup thread (proxy_cluster_watchdog_func) to crash */
    mc_initialize_cleanup(p);

    /* Create global mutex */
    if (ap_global_mutex_create(&node_mutex, NULL, node_mutex_type, NULL, s, p, 0) != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_EMERG, 0, s, "manager_init: ap_global_mutex_create %s failed", node_mutex_type);
        return !OK;
    }
    if (ap_global_mutex_create(&context_mutex, NULL, context_mutex_type, NULL, s, p, 0) != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_EMERG, 0, s, "manager_init: ap_global_mutex_create %s failed", node_mutex_type);
        return !OK;
    }

    return OK;
}

static apr_status_t decodeenc(char **ptr);
static char **process_buff(request_rec *r, char *buff)
{
    int i = 0;
    char *s = buff;
    char **ptr = NULL;
    for (; *s != '\0'; s++) {
        if (*s == '&' || *s == '=') {
            i++;
        }
    }
    ptr = apr_palloc(r->pool, sizeof(char *) * (i + 2));
    if (ptr == NULL) {
        return NULL;
    }

    s = buff;
    ptr[0] = s;
    ptr[i + 1] = NULL;
    i = 1;
    for (; *s != '\0'; s++) {
        /* our separators */
        if (*s == '&' || *s == '=') {
            *s = '\0';
            ptr[i] = s + 1;
            i++;
        }
    }

    if (decodeenc(ptr) != APR_SUCCESS) {
        return NULL;
    }

    return ptr;
}

/**
 * Insert the hosts from Alias information
 */
static apr_status_t insert_update_hosts(mem_t *mem, char *str, int node, int vhost)
{
    char *ptr = str;
    char *previous = str;
    hostinfo_t info;
    char empty[1] = {'\0'};
    apr_status_t status;

    info.node = node;
    info.vhost = vhost;
    if (ptr == NULL) {
        ptr = empty;
        previous = ptr;
    }
    while (*ptr) {
        if (*ptr == ',') {
            *ptr = '\0';
            strncpy(info.host, previous, HOSTALIASZ);
            status = insert_update_host(mem, &info);
            if (status != APR_SUCCESS) {
                return status;
            }
            previous = ptr + 1;
        }
        ptr++;
    }
    strncpy(info.host, previous, sizeof(info.host));
    return insert_update_host(mem, &info);
}

/**
 * Remove the context using the contextinfo_t information
 * we read it first then remove it
 */
static void read_remove_context(mem_t *mem, contextinfo_t *context)
{
    contextinfo_t *info;
    info = read_context(mem, context);
    if (info != NULL) {
        remove_context(mem, info->id);
    }
}

/**
 * Insert the context from Context information
 * Note:
 *     1 - if status is REMOVE remove_context will be called
 *     2 - return codes of REMOVE are ignored (always success)
 */
static apr_status_t insert_update_contexts(mem_t *mem, char *str, int node, int vhost, int status)
{
    char *ptr = str;
    char *previous = str;
    apr_status_t ret = APR_SUCCESS;
    contextinfo_t info;
    char empty[2] = {'/', '\0'};

    info.node = node;
    info.vhost = vhost;
    info.status = status;
    if (ptr == NULL) {
        ptr = empty;
        previous = ptr;
    }
    while (*ptr) {
        if (*ptr == ',') {
            *ptr = '\0';
            info.id = 0;
            strncpy(info.context, previous, sizeof(info.context));
            if (status != REMOVE) {
                ret = insert_update_context(mem, &info);
                if (ret != APR_SUCCESS) {
                    return ret;
                }
            } else {
                read_remove_context(mem, &info);
            }

            previous = ptr + 1;
        }
        ptr++;
    }
    info.id = 0;
    strncpy(info.context, previous, sizeof(info.context));
    if (status != REMOVE) {
        ret = insert_update_context(mem, &info);
    } else {
        read_remove_context(mem, &info);
    }
    return ret;
}

/**
 * Check that the node could be handle as is there were the same
 */
static int is_same_node(const nodeinfo_t *nodeinfo, const nodeinfo_t *node)
{
    if (strcmp(nodeinfo->mess.balancer, node->mess.balancer)) {
        return 0;
    }
    if (strcmp(nodeinfo->mess.Host, node->mess.Host)) {
        return 0;
    }
    if (strcmp(nodeinfo->mess.Port, node->mess.Port)) {
        return 0;
    }
    if (strcmp(nodeinfo->mess.Type, node->mess.Type)) {
        return 0;
    }
    if (nodeinfo->mess.reversed != node->mess.reversed) {
        return 0;
    }

    /* Those means the reslist has to be changed */
    if (nodeinfo->mess.smax != node->mess.smax) {
        return 0;
    }
    if (nodeinfo->mess.ttl != node->mess.ttl) {
        return 0;
    }

    /* All other fields can be modified without causing problems */
    return -1;
}

/**
 * Check if another node has the same worker
 */
static int is_same_worker_existing(const request_rec *r, const nodeinfo_t *node)
{
    int size, i;
    int *id;
    size = loc_get_max_size_node();
    if (size == 0) {
        return 0;
    }
    id = apr_palloc(r->pool, sizeof(int) * size);
    size = get_ids_used_node(nodestatsmem, id);
    for (i = 0; i < size; i++) {
        nodeinfo_t *ou;
        if (get_node(nodestatsmem, &ou, id[i]) != APR_SUCCESS) {
            continue;
        }
        if (is_same_node(ou, node)) {
            /* we have a node that corresponds to the same worker */
            if (!strcmp(ou->mess.JVMRoute, node->mess.JVMRoute)) {
                return 0; /* well it is the same */
            }
            if (ou->mess.remove) {
                if (strcmp(ou->mess.JVMRoute, "REMOVED") == 0) {
                    /* Look in remove_removed_node, only "REMOVED" have cleaned the contexts/hosts */
                    return 0; /* well it marked removed */
                }
            }
            ap_log_error(APLOG_MARK, APLOG_WARNING, 0, r->server,
                         "process_config: nodes %s and %s correspond to the same worker", node->mess.JVMRoute,
                         ou->mess.JVMRoute);
            return -1;
        }
    }
    return 0;
}

/**
 * Builds the parameter for mod_balancer
 */
static apr_status_t mod_manager_manage_worker(request_rec *r, const nodeinfo_t *node, const balancerinfo_t *bal)
{
    apr_table_t *params;
    params = apr_table_make(r->pool, 10);
    /* balancer */
    apr_table_set(params, "b", node->mess.balancer);
    apr_table_set(params, "b_lbm", "cluster");
    apr_table_set(params, "b_tmo", apr_psprintf(r->pool, "%d", bal->Timeout));
    apr_table_set(params, "b_max", apr_psprintf(r->pool, "%d", bal->Maxattempts));
    apr_table_set(params, "b_ss", apr_pstrcat(r->pool, bal->StickySessionCookie, "|", bal->StickySessionPath, NULL));

    /* and new worker */
    apr_table_set(params, "b_wyes", "1");
    apr_table_set(params, "b_nwrkr",
                  apr_pstrcat(r->pool, node->mess.Type, "://", node->mess.Host, ":", node->mess.Port, NULL));
    balancer_manage(r, params);
    apr_table_clear(params);

    /* now process the worker */
    apr_table_set(params, "b", node->mess.balancer);
    apr_table_set(params, "w",
                  apr_pstrcat(r->pool, node->mess.Type, "://", node->mess.Host, ":", node->mess.Port, NULL));
    apr_table_set(params, "w_wr", node->mess.JVMRoute);
    apr_table_set(params, "w_status_D", "0"); /* Not Dissabled */

    /* set the health check (requires mod_proxy_hcheck) */
    /* CPING for AJP and OPTIONS for HTTP/1.1 */
    if (strcmp(node->mess.Type, "ajp")) {
        apr_table_set(params, "w_hm", "OPTIONS");
    } else {
        apr_table_set(params, "w_hm", "CPING");
    }
    /* Use 10 sec for the moment, the idea is to adjust it with the STATUS frequency */
    apr_table_set(params, "w_hi", "10000");
    return balancer_manage(r, params);
}

/**
 * Check if the proxy balancer module already has a worker and return the id
 */
static proxy_worker *proxy_node_getid(request_rec *r, const nodeinfo_t *nodeinfo, int *id,
                                      const proxy_server_conf **the_conf)
{
    if (balancerhandler != NULL) {
        return balancerhandler->proxy_node_getid(r, nodeinfo->mess.balancer, nodeinfo->mess.Type, nodeinfo->mess.Host,
                                                 nodeinfo->mess.Port, id, the_conf);
    }
    return NULL;
}

static void reenable_proxy_worker(request_rec *r, nodeinfo_t *node, proxy_worker *worker, nodeinfo_t *nodeinfo,
                                  const proxy_server_conf *the_conf)
{
    if (balancerhandler != NULL) {
        balancerhandler->reenable_proxy_worker(r->server, node, worker, nodeinfo, the_conf);
    }
}

static int proxy_node_get_free_id(request_rec *r, int node_table_size)
{
    if (balancerhandler != NULL) {
        return balancerhandler->proxy_node_get_free_id(r, node_table_size);
    }
    return -1;
}

static void process_config_balancer_defaults(request_rec *r, balancerinfo_t *balancerinfo, mod_manager_config *mconf)
{
    memset(balancerinfo, '\0', sizeof(*balancerinfo));
    if (mconf->balancername != NULL) {
        normalize_balancer_name(mconf->balancername, r->server);
        strncpy(balancerinfo->balancer, mconf->balancername, sizeof(balancerinfo->balancer));
        balancerinfo->balancer[sizeof(balancerinfo->balancer) - 1] = '\0';
    } else {
        strcpy(balancerinfo->balancer, "mycluster");
    }
    balancerinfo->StickySession = 1;
    balancerinfo->StickySessionForce = 1;
    strcpy(balancerinfo->StickySessionCookie, "JSESSIONID");
    strcpy(balancerinfo->StickySessionPath, "jsessionid");
    balancerinfo->Maxattempts = 1;
    balancerinfo->Timeout = 0;
}

static void process_config_node_defaults(request_rec *r, nodeinfo_t *nodeinfo, mod_manager_config *mconf)
{
    memset(&nodeinfo->mess, '\0', sizeof(nodeinfo->mess));
    if (mconf->balancername != NULL) {
        normalize_balancer_name(mconf->balancername, r->server);
        strncpy(nodeinfo->mess.balancer, mconf->balancername, sizeof(nodeinfo->mess.balancer));
        nodeinfo->mess.balancer[sizeof(nodeinfo->mess.balancer) - 1] = '\0';
    } else {
        strcpy(nodeinfo->mess.balancer, "mycluster");
    }
    strcpy(nodeinfo->mess.Host, "localhost");
    strcpy(nodeinfo->mess.Port, "8009");
    strcpy(nodeinfo->mess.Type, "ajp");
    nodeinfo->mess.Upgrade[0] = '\0';
    nodeinfo->mess.AJPSecret[0] = '\0';
    nodeinfo->mess.reversed = 0;
    nodeinfo->mess.remove = 0;               /* not marked as removed */
    nodeinfo->mess.flushpackets = flush_off; /* FLUSH_OFF; See enum flush_packets in proxy.h flush_off */
    nodeinfo->mess.flushwait = PROXY_FLUSH_WAIT;
    nodeinfo->mess.ping = apr_time_from_sec(10);
    nodeinfo->mess.smax = -1; /* let mod_proxy logic get the right one */
    nodeinfo->mess.ttl = apr_time_from_sec(60);
    nodeinfo->mess.timeout = 0;
    nodeinfo->mess.id = -1;
    nodeinfo->mess.lastcleantry = 0;
}

static char *process_config_balancer(const request_rec *r, const char *key, char *val, balancerinfo_t *balancerinfo,
                                     nodeinfo_t *nodeinfo, int *errtype)
{
    if (strcasecmp(key, "Balancer") == 0) {
        if (strlen(val) >= sizeof(nodeinfo->mess.balancer)) {
            *errtype = TYPESYNTAX;
            return SBALBIG;
        }
        normalize_balancer_name(val, r->server);
        strncpy(nodeinfo->mess.balancer, val, sizeof(nodeinfo->mess.balancer));
        nodeinfo->mess.balancer[sizeof(nodeinfo->mess.balancer) - 1] = '\0';
        strncpy(balancerinfo->balancer, val, sizeof(balancerinfo->balancer));
        balancerinfo->balancer[sizeof(balancerinfo->balancer) - 1] = '\0';
    }
    if (strcasecmp(key, "StickySession") == 0) {
        if (strcasecmp(val, "no") == 0) {
            balancerinfo->StickySession = 0;
        }
    }
    if (strcasecmp(key, "StickySessionCookie") == 0) {
        if (strlen(val) >= sizeof(balancerinfo->StickySessionCookie)) {
            *errtype = TYPESYNTAX;
            return SBAFBIG;
        }
        strcpy(balancerinfo->StickySessionCookie, val);
    }
    if (strcasecmp(key, "StickySessionPath") == 0) {
        if (strlen(val) >= sizeof(balancerinfo->StickySessionPath)) {
            *errtype = TYPESYNTAX;
            return SBAFBIG;
        }
        strcpy(balancerinfo->StickySessionPath, val);
    }
    if (strcasecmp(key, "StickySessionRemove") == 0) {
        if (strcasecmp(val, "yes") == 0) {
            balancerinfo->StickySessionRemove = 1;
        }
    }
    /* The java part assumes default = yes and sents only StickySessionForce=No */
    if (strcasecmp(key, "StickySessionForce") == 0) {
        if (strcasecmp(val, "no") == 0) {
            balancerinfo->StickySessionForce = 0;
        }
    }
    /* Note that it is workerTimeout (set/getWorkerTimeout in java code) */
    if (strcasecmp(key, "WaitWorker") == 0) {
        balancerinfo->Timeout = apr_time_from_sec(atoi(val));
    }
    if (strcasecmp(key, "Maxattempts") == 0) {
        balancerinfo->Maxattempts = atoi(val);
    }

    return NULL;
}

static char *process_config_node(const char *key, char *val, nodeinfo_t *nodeinfo, int *errtype)
{
    if (strcasecmp(key, "JVMRoute") == 0) {
        if (strlen(val) >= sizeof(nodeinfo->mess.JVMRoute)) {
            *errtype = TYPESYNTAX;
            return SROUBIG;
        }
        strcpy(nodeinfo->mess.JVMRoute, val);
    }
    /* We renamed it LBGroup */
    if (strcasecmp(key, "Domain") == 0) {
        if (strlen(val) >= sizeof(nodeinfo->mess.Domain)) {
            *errtype = TYPESYNTAX;
            return SDOMBIG;
        }
        strcpy(nodeinfo->mess.Domain, val);
    }
    if (strcasecmp(key, "Host") == 0) {
        char *p_read = val, *p_write = val;
        int flag = 0;
        if (strlen(val) >= sizeof(nodeinfo->mess.Host)) {
            *errtype = TYPESYNTAX;
            return SHOSBIG;
        }
        /* Removes %zone from an address */
        if (*p_read == '[') {
            while (*p_read) {
                *p_write = *p_read++;
                if ((*p_write == '%' || flag) && *p_write != ']') {
                    flag = 1;
                } else {
                    p_write++;
                }
            }
            *p_write = '\0';
        }
        strcpy(nodeinfo->mess.Host, val);
    }
    if (strcasecmp(key, "Port") == 0) {
        if (strlen(val) >= sizeof(nodeinfo->mess.Port)) {
            *errtype = TYPESYNTAX;
            return SPORBIG;
        }
        strcpy(nodeinfo->mess.Port, val);
    }
    if (strcasecmp(key, "Type") == 0) {
        if (strlen(val) >= sizeof(nodeinfo->mess.Type)) {
            *errtype = TYPESYNTAX;
            return STYPBIG;
        }
        strcpy(nodeinfo->mess.Type, val);
    }
    if (strcasecmp(key, "Reversed") == 0) {
        if (strcasecmp(val, "yes") == 0) {
            nodeinfo->mess.reversed = 1;
        }
    }
    if (strcasecmp(key, "flushpackets") == 0) {
        if (strcasecmp(val, "on") == 0) {
            nodeinfo->mess.flushpackets = flush_on;
        } else if (strcasecmp(val, "auto") == 0) {
            nodeinfo->mess.flushpackets = flush_auto;
        }
    }
    if (strcasecmp(key, "flushwait") == 0) {
        nodeinfo->mess.flushwait = atoi(val) * 1000;
    }
    if (strcasecmp(key, "ping") == 0) {
        nodeinfo->mess.ping = apr_time_from_sec(atoi(val));
    }
    if (strcasecmp(key, "smax") == 0) {
        nodeinfo->mess.smax = atoi(val);
    }
    if (strcasecmp(key, "ttl") == 0) {
        nodeinfo->mess.ttl = apr_time_from_sec(atoi(val));
    }
    if (strcasecmp(key, "Timeout") == 0) {
        nodeinfo->mess.timeout = apr_time_from_sec(atoi(val));
    }

    return NULL;
}

static nodeinfo_t *read_node_by_id(mem_t *mem, int id)
{
    nodeinfo_t workernodeinfo;
    workernodeinfo.mess.id = id;
    (void)mem;
    return read_node(nodestatsmem, &workernodeinfo);
}

static void mark_node_removed(nodeinfo_t *node)
{
    if (node) {
        strcpy(node->mess.JVMRoute, "REMOVED");
        node->mess.remove = 1;
        node->updatetime = apr_time_now();
        node->mess.num_remove_check = 0;
    }
}
static const proxy_worker_shared *read_shared_by_node(request_rec *r, nodeinfo_t *node)
{
    void *sconf = r->server->module_config;
    int i, port;
    char *name = apr_pstrcat(r->pool, "balancer://", node->mess.balancer, NULL);
    proxy_server_conf *conf = (proxy_server_conf *)ap_get_module_config(sconf, &proxy_module);
    proxy_balancer *balancer = (proxy_balancer *)conf->balancers->elts;
    if (sscanf(node->mess.Port, "%u", &port) != 1) {
        return NULL; /* something is wrong */
    }
    for (i = 0; i < conf->balancers->nelts; i++, balancer++) {
        int j;
        proxy_worker **workers;
        if (strcmp(balancer->s->name, name)) {
            continue;
        }
        workers = (proxy_worker **)balancer->workers->elts;
        for (j = 0; j < balancer->workers->nelts; j++, workers++) {
            proxy_worker *worker = *workers;
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "read_shared_by_node: Balancer %s worker %s, %s, %d",
                         balancer->s->name, worker->s->route, worker->s->hostname, worker->s->port);
            if (worker->s->port == port && strcmp(worker->s->hostname, node->mess.Host) == 0 &&
                strcmp(worker->s->route, node->mess.JVMRoute) == 0) {
                return worker->s;
            }
        }
    }

    return NULL;
}

/*
 * Process a CONFIG message
 * Balancer: <Balancer name>
 * <balancer configuration>
 * StickySession	StickySessionCookie	StickySessionPath	StickySessionRemove
 * StickySessionForce	Timeout	Maxattempts
 * JvmRoute?: <JvmRoute>
 * Domain: <Domain>
 * <Host: <Node IP>
 * Port: <Connector Port>
 * Type: <Type of the connector>
 * Reserved: <Use connection pool initiated by Tomcat *.>
 * <node conf>
 * flushpackets	flushwait	ping	smax	ttl
 * Virtual hosts in JBossAS
 * Alias: <vhost list>
 * Context corresponding to the applications.
 * Context: <context list>
 */
static char *process_config(request_rec *r, char **ptr, int *errtype)
{
    /* Process the node/balancer description */
    nodeinfo_t nodeinfo;
    nodeinfo_t *node;
    balancerinfo_t balancerinfo;

    struct cluster_host *vhost;
    struct cluster_host *phost;

    int i = 0;
    int id = -1;
    int vid = 1; /* zero and "" is empty */
    int removed = 0;
    void *sconf = r->server->module_config;
    mod_manager_config *mconf = ap_get_module_config(sconf, &manager_module);
    int clean = 1;
    proxy_worker *worker;
    const proxy_server_conf *the_conf = NULL;
    apr_status_t rv;

    vhost = apr_palloc(r->pool, sizeof(struct cluster_host));

    /* Map nothing by default */
    vhost->host = NULL;
    vhost->context = NULL;
    vhost->next = NULL;
    phost = vhost;

    /* Fill default node values */
    process_config_node_defaults(r, &nodeinfo, mconf);
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "process_config: Start");

    /* Fill default balancer values */
    process_config_balancer_defaults(r, &balancerinfo, mconf);

    while (ptr[i]) {
        char *msg = NULL;
        /* XXX: balancer part */
        msg = process_config_balancer(r, ptr[i], ptr[i + 1], &balancerinfo, &nodeinfo, errtype);
        if (msg != NULL) {
            return msg;
        }

        /* XXX: Node part */
        msg = process_config_node(ptr[i], ptr[i + 1], &nodeinfo, errtype);
        if (msg != NULL) {
            return msg;
        }

        /* Hosts and contexts (optional parameters) */
        if (strcasecmp(ptr[i], "Alias") == 0) {
            if (phost->host && !phost->context) {
                *errtype = TYPESYNTAX;
                return SALIBAD;
            }
            if (phost->host) {
                phost->next = apr_palloc(r->pool, sizeof(struct cluster_host));
                phost = phost->next;
                phost->next = NULL;
                phost->context = NULL;
            }
            phost->host = ptr[i + 1];
        }
        if (strcasecmp(ptr[i], "Context") == 0) {
            if (phost->context) {
                *errtype = TYPESYNTAX;
                return SCONBAD;
            }
            phost->context = ptr[i + 1];
        }
        i++;
        i++;
    }

    /* Check for JVMRoute */
    if (nodeinfo.mess.JVMRoute[0] == '\0') {
        *errtype = TYPESYNTAX;
        return SROUBAD;
    }

    if (mconf->enable_ws_tunnel && strcmp(nodeinfo.mess.Type, "ajp")) {
        if (!strcmp(nodeinfo.mess.Type, "http")) {
            strcpy(nodeinfo.mess.Type, "ws");
        }
        if (!strcmp(nodeinfo.mess.Type, "https")) {
            strcpy(nodeinfo.mess.Type, "wss");
        }
        if (mconf->ws_upgrade_header) {
            strncpy(nodeinfo.mess.Upgrade, mconf->ws_upgrade_header, sizeof(nodeinfo.mess.Upgrade));
            nodeinfo.mess.Upgrade[sizeof(nodeinfo.mess.Upgrade) - 1] = '\0';
        } else {
            strcpy(nodeinfo.mess.Upgrade, "websocket");
        }
    }

    if (strcmp(nodeinfo.mess.Type, "ajp") == 0) {
        if (mconf->ajp_secret) {
            strncpy(nodeinfo.mess.AJPSecret, mconf->ajp_secret, sizeof(nodeinfo.mess.AJPSecret));
            nodeinfo.mess.AJPSecret[sizeof(nodeinfo.mess.AJPSecret) - 1] = '\0';
        }
    }

    if (mconf->response_field_size && strcmp(nodeinfo.mess.Type, "ajp")) {
        nodeinfo.mess.ResponseFieldSize = mconf->response_field_size;
    }
    /* Insert or update balancer description */
    rv = loc_lock_nodes();
    ap_assert(rv == APR_SUCCESS);
    if (insert_update_balancer(balancerstatsmem, &balancerinfo) != APR_SUCCESS) {
        loc_unlock_nodes();
        *errtype = TYPEMEM;
        return apr_psprintf(r->pool, MBALAUI, nodeinfo.mess.JVMRoute);
    }

    /* check for removed node */
    node = read_node(nodestatsmem, &nodeinfo);
    if (node != NULL) {
        /* If the node is removed (or kill and restarted) and recreated unchanged that is ok: network problems */
        if (!is_same_node(node, &nodeinfo)) {
            /* Here we can't update it because the old one is still in */
            char *mess = apr_psprintf(r->pool, MNODERM, node->mess.JVMRoute);
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
                         "process_config: node %s %d %s : %s  %s already exists, removing...", node->mess.JVMRoute,
                         node->mess.id, node->mess.Port, nodeinfo.mess.JVMRoute, nodeinfo.mess.Port);
            mark_node_removed(node);
            loc_remove_host_context(node->mess.id, r->pool);
            inc_version_node();
            loc_unlock_nodes();
            *errtype = TYPEMEM;
            return mess;
        }
    }
    /* check if a node corresponding to the same worker already exists */
    if (is_same_worker_existing(r, &nodeinfo)) {
        loc_unlock_nodes();
        *errtype = TYPEMEM;
        return MNODEET;
    }

    /* Check for corresponding proxy_worker */
    worker = proxy_node_getid(r, &nodeinfo, &id, &the_conf);
    if (id != -1) {
        /* Same node should be OK, different nodes will bring problems */
        if (node != NULL && node->mess.id == id) {
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                         "process_config: worker %d (%s) exists and should be OK", id, nodeinfo.mess.JVMRoute);
        } else {
            /* Here that is the tricky part, we will insert_update the whole node including proxy_worker_shared */
            char *pptr;
            unsigned long offset;

            ap_log_error(APLOG_MARK, APLOG_WARNING, 0, r->server,
                         "process_config: worker %d (%s) exists and IS NOT OK!!!", id, nodeinfo.mess.JVMRoute);
            if (node == NULL) {
                /* try to read the node */
                nodeinfo_t *workernode = read_node_by_id(nodestatsmem, id);
                if (workernode != NULL) {
                    if (strcmp(workernode->mess.JVMRoute, "REMOVED") == 0) {
                        /* We are in the remove process */
                        /* Something to clean ? */
                        strcpy(workernode->mess.JVMRoute, nodeinfo.mess.JVMRoute);
                        /* if the workernode->mess is zeroed we are going to reinsert it */
                    } else if ((workernode->mess.JVMRoute[0] != '\0') &&
                               (strcmp(workernode->mess.JVMRoute, nodeinfo.mess.JVMRoute) != 0)) {
                        ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
                                     "process_config: worker %d (%s) exists and does NOT correspond to %s", id,
                                     workernode->mess.JVMRoute, nodeinfo.mess.JVMRoute);
                        loc_unlock_nodes();
                        *errtype = TYPEMEM;
                        return MNODEET;
                    }
                    removed = id; /* we save the id of the worknode in case insert/update fails */
                }
                ap_assert(the_conf);
            }
            clean = 0;
            ap_assert(worker->s->port != 0);
            /* XXX: really needed? offset logic OK here, we save the worker information (see mod_proxy_cluster) */
            pptr = (char *)&nodeinfo;
            offset = sizeof(nodemess_t) + sizeof(apr_time_t) +
                     sizeof(int); /* nodeinfo.offset doesn't contain the information */
            offset = APR_ALIGN_DEFAULT(offset);
            pptr = pptr + offset;
            memcpy(pptr, worker->s, sizeof(proxy_worker_shared)); /* restore the information we are going to reuse */
            ap_assert(the_conf);
        }
    } else {
        nodeinfo_t *workernode;
        rv = find_node_byhostport(nodestatsmem, &workernode, nodeinfo.mess.Host, nodeinfo.mess.Port);
        if (rv == APR_SUCCESS) {
            /* Normally the node is just being removed, so no host/context but some other child might have a worker */
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "process_config: NOT NEW (%d %s) %s %s (%s)",
                         workernode->mess.id, workernode->mess.JVMRoute, workernode->mess.Host, workernode->mess.Port,
                         nodeinfo.mess.JVMRoute);
            if (strcmp(workernode->mess.JVMRoute, "REMOVED") == 0) {
                id = workernode->mess.id; /* we are reusing it */
                strcpy(workernode->mess.JVMRoute, nodeinfo.mess.JVMRoute);
                workernode->mess.remove = 0;
                workernode->mess.num_remove_check = 0;
            } else {
                id = workernode->mess.id;
            }
        } else {
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "process_config: NEW (%s) %s", nodeinfo.mess.JVMRoute,
                         nodeinfo.mess.Port);
        }
    }
    if (id == -1) {
        /* make sure we insert in a "free" node according to the worker logic */
        id = proxy_node_get_free_id(r, node_storage.get_max_size_node());
        if (id == -1 && balancerhandler != NULL) {
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                         "process_config: NEW (%s) %s %s will not be added (Maxnode reached)", nodeinfo.mess.JVMRoute,
                         nodeinfo.mess.Host, nodeinfo.mess.Port);
        } else if (id != -1) {
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "process_config: NEW (%s) %s %s in %d",
                         nodeinfo.mess.JVMRoute, nodeinfo.mess.Host, nodeinfo.mess.Port, id);
        }
    }

    /* Insert or update node description */
    if (insert_update_node(nodestatsmem, &nodeinfo, &id, clean) != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                     "process_config: insert_update_node failed for %s clean: %d", nodeinfo.mess.JVMRoute, clean);
        loc_unlock_nodes();
        if (removed) {
            nodeinfo_t *workernode = read_node_by_id(nodestatsmem, removed);
            mark_node_removed(workernode);
        }
        loc_unlock_nodes();
        *errtype = TYPEMEM;
        return apr_psprintf(r->pool, MNODEUI, nodeinfo.mess.JVMRoute);
    }

    if (clean == 0) {
        /* need to read the node */
        nodeinfo_t *workernode = read_node_by_id(nodestatsmem, id);
        ap_assert(workernode != NULL);
        ap_assert(the_conf);
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "process_config: worker %d (%s) inserted", id,
                     nodeinfo.mess.JVMRoute);
        /* make sure we can use it */
        ap_assert(worker->context != NULL);
        ap_assert(workernode->mess.id == id);
        ap_assert(the_conf);

        /* so the scheme, hostname and port correspond to worker which was removed and readded */
        reenable_proxy_worker(r, workernode, worker, &nodeinfo, the_conf);
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server,
                     "process_config: reenable_proxy_worker... scheme %s hostname %s port %d route %s name %s id: %d",
                     worker->s->scheme, worker->s->hostname_ex, worker->s->port, worker->s->route,
#ifdef PROXY_WORKER_EXT_NAME_SIZE
                     worker->s->name_ex,
#else
                     worker->s->name,
#endif
                     worker->s->index);
    } else {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "process_config: (%s) %s inserted/updated in worker %d",
                     nodeinfo.mess.JVMRoute, nodeinfo.mess.Port, id);
    }
    inc_version_node();

    /* Insert the Alias and corresponding Context */
    phost = vhost;
    if (phost->host == NULL && phost->context == NULL) {
        loc_unlock_nodes();
        /* if using mod_balancer create or update the worker */
        if (balancer_manage) {
            apr_status_t rv = mod_manager_manage_worker(r, &nodeinfo, &balancerinfo);
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "process_config: balancer-manager returned %d", rv);
        } else {
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "process_config: NO balancer-manager");
        }
        return NULL; /* Alias and Context missing */
    }
    while (phost) {
        if (insert_update_hosts(hoststatsmem, phost->host, id, vid) != APR_SUCCESS) {
            loc_unlock_nodes();
            return apr_psprintf(r->pool, MHOSTUI, nodeinfo.mess.JVMRoute);
        }
        if (insert_update_contexts(contextstatsmem, phost->context, id, vid, STOPPED) != APR_SUCCESS) {
            loc_unlock_nodes();
            return apr_psprintf(r->pool, MCONTUI, nodeinfo.mess.JVMRoute);
        }
        phost = phost->next;
        vid++;
    }
    loc_unlock_nodes();

    /* if using mod_balancer create or update the worker */
    if (balancer_manage) {
        apr_status_t rv = mod_manager_manage_worker(r, &nodeinfo, &balancerinfo);
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "process_config: balancer-manager returned %d", rv);
    } else {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "process_config: NO balancer-manager");
    }

    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "process_config: Done");

    return NULL;
}

/*
 * Hepler function to convert status to string. The individual statuses are macros defined in context.h; if
 * there is an unknown status (integer value other than 1-4, it will be interpreted as REMOVE -> "REMOVED").
 */
static char *context_status_to_string(int status)
{
    switch (status) {
    case ENABLED:
        return "ENABLED";
    case DISABLED:
        return "DISABLED";
    case STOPPED:
        return "STOPPED";
    default:
        return "REMOVED";
    }
}

/**
 * Process a DUMP command.
 */
static char *process_dump(request_rec *r, int *errtype)
{
    int size, i;
    int *id;
    unsigned char type;
    const char *accept_header = apr_table_get(r->headers_in, "Accept");
    (void)errtype;

    if (accept_header && strstr((char *)accept_header, "text/xml") != NULL) {
        ap_set_content_type(r, "text/xml");
        type = TEXT_XML;
        ap_rprintf(r, "<?xml version=\"1.0\" standalone=\"yes\" ?>\n");
    } else {
        ap_set_content_type(r, "text/plain");
        type = TEXT_PLAIN;
    }

    size = loc_get_max_size_balancer();
    if (size == 0) {
        return NULL;
    }

    if (type == TEXT_XML) {
        ap_rprintf(r, "<Dump><Balancers>");
    }

    id = apr_palloc(r->pool, sizeof(int) * size);
    size = get_ids_used_balancer(balancerstatsmem, id);
    for (i = 0; i < size; i++) {
        balancerinfo_t *ou;
        if (get_balancer(balancerstatsmem, &ou, id[i]) != APR_SUCCESS) {
            continue;
        }

        switch (type) {
        case TEXT_XML:
            ap_rprintf(r, "<Balancer id=\"%d\" name=\"%.*s\">\
                           <StickySession>\
                               <Enabled>%d</Enabled>\
                               <Cookie>%.*s</Cookie>\
                               <Path>%.*s</Path>\
                               <Remove>%d</Remove>\
                               <Force>%d</Force>\
                           </StickySession>\
                           <Timeout>%d</Timeout>\
                           <MaxAttempts>%d</MaxAttempts>\
                           </Balancer>",
                       id[i], (int)sizeof(ou->balancer), ou->balancer, ou->StickySession,
                       (int)sizeof(ou->StickySessionCookie), ou->StickySessionCookie,
                       (int)sizeof(ou->StickySessionPath), ou->StickySessionPath, ou->StickySessionRemove,
                       ou->StickySessionForce, (int)apr_time_sec(ou->Timeout), ou->Maxattempts);
            break;
        case TEXT_PLAIN:
        default:
            ap_rprintf(
                r,
                "balancer: [%d] Name: %.*s Sticky: %d [%.*s]/[%.*s] remove: %d force: %d Timeout: %d maxAttempts: %d\n",
                id[i], (int)sizeof(ou->balancer), ou->balancer, ou->StickySession, (int)sizeof(ou->StickySessionCookie),
                ou->StickySessionCookie, (int)sizeof(ou->StickySessionPath), ou->StickySessionPath,
                ou->StickySessionRemove, ou->StickySessionForce, (int)apr_time_sec(ou->Timeout), ou->Maxattempts);
            break;
        }
    }
    if (type == TEXT_XML) {
        ap_rprintf(r, "</Balancers>");
    }

    size = loc_get_max_size_node();
    id = apr_palloc(r->pool, sizeof(int) * size);
    size = get_ids_used_node(nodestatsmem, id);

    if (type == TEXT_XML) {
        ap_rprintf(r, "<Nodes>");
    }
    for (i = 0; i < size; i++) {
        nodeinfo_t *ou;
        if (get_node(nodestatsmem, &ou, id[i]) != APR_SUCCESS) {
            continue;
        }

        switch (type) {
        case TEXT_XML:
            ap_rprintf(r, "<Node id=\"%d\">\
                               <Balancer>%.*s</Balancer>\
                               <JVMRoute>%.*s</JVMRoute>\
                               <LBGroup>%.*s</LBGroup>\
                               <Host>%.*s</Host>\
                               <Port>%.*s</Port>\
                               <Type>%.*s</Type>\
                               <FlushPackets>%d</FlushPackets>\
                               <FlushWait>%d</FlushWait>\
                               <Ping>%d</Ping>\
                               <Smax>%d</Smax>\
                               <Ttl>%d</Ttl>\
                               <Timeout>%d</Timeout>\
                           </Node>",
                       ou->mess.id, (int)sizeof(ou->mess.balancer), ou->mess.balancer, (int)sizeof(ou->mess.JVMRoute),
                       ou->mess.JVMRoute, (int)sizeof(ou->mess.Domain), ou->mess.Domain, (int)sizeof(ou->mess.Host),
                       ou->mess.Host, (int)sizeof(ou->mess.Port), ou->mess.Port, (int)sizeof(ou->mess.Type),
                       ou->mess.Type, ou->mess.flushpackets, ou->mess.flushwait / 1000,
                       (int)apr_time_sec(ou->mess.ping), ou->mess.smax, (int)apr_time_sec(ou->mess.ttl),
                       (int)apr_time_sec(ou->mess.timeout));
            break;
        case TEXT_PLAIN:
        default:
            ap_rprintf(r, "node: [%d:%d],Balancer: %.*s,JVMRoute: %.*s,LBGroup: [%.*s],Host: %.*s,Port: %.*s,\
                           Type: %.*s,flushpackets: %d,flushwait: %d,ping: %d,smax: %d,ttl: %d,timeout: %d\n",
                       id[i], ou->mess.id, (int)sizeof(ou->mess.balancer), ou->mess.balancer,
                       (int)sizeof(ou->mess.JVMRoute), ou->mess.JVMRoute, (int)sizeof(ou->mess.Domain), ou->mess.Domain,
                       (int)sizeof(ou->mess.Host), ou->mess.Host, (int)sizeof(ou->mess.Port), ou->mess.Port,
                       (int)sizeof(ou->mess.Type), ou->mess.Type, ou->mess.flushpackets, ou->mess.flushwait / 1000,
                       (int)apr_time_sec(ou->mess.ping), ou->mess.smax, (int)apr_time_sec(ou->mess.ttl),
                       (int)apr_time_sec(ou->mess.timeout));
            break;
        }
    }

    if (type == TEXT_XML) {
        ap_rprintf(r, "</Nodes><Hosts>");
    }

    size = loc_get_max_size_host();
    id = apr_palloc(r->pool, sizeof(int) * size);
    size = get_ids_used_host(hoststatsmem, id);
    for (i = 0; i < size; i++) {
        hostinfo_t *ou;
        if (get_host(hoststatsmem, &ou, id[i]) != APR_SUCCESS) {
            continue;
        }

        switch (type) {
        case TEXT_XML:
            ap_rprintf(r, "<Host id=\"%d\" alias=\"%.*s\">\
                               <Vhost>%d</Vhost>\
                               <Node>%d</Node>\
                           </Host>",
                       id[i], (int)sizeof(ou->host), ou->host, ou->vhost, ou->node);
            break;
        case TEXT_PLAIN:
        default:
            ap_rprintf(r, "host: %d [%.*s] vhost: %d node: %d\n", id[i], (int)sizeof(ou->host), ou->host, ou->vhost,
                       ou->node);
            break;
        }
    }
    if (type == TEXT_XML) {
        ap_rprintf(r, "</Hosts><Contexts>");
    }

    size = loc_get_max_size_context();
    id = apr_palloc(r->pool, sizeof(int) * size);
    size = get_ids_used_context(contextstatsmem, id);
    for (i = 0; i < size; i++) {
        contextinfo_t *ou;
        if (get_context(contextstatsmem, &ou, id[i]) != APR_SUCCESS) {
            continue;
        }

        switch (type) {
        case TEXT_XML:
            ap_rprintf(r, "<Context id=\"%d\" path=\"%.*s\">\
                            <Vhost>%d</Vhost>\
                            <Node>%d</Node>\
                            <Status id=\"%d\">%s</Status>\
                           </Context>",
                       id[i], (int)sizeof(ou->context), ou->context, ou->vhost, ou->node, ou->status,
                       context_status_to_string(ou->status));
            break;
        case TEXT_PLAIN:
        default:
            ap_rprintf(r, "context: %d [%.*s] vhost: %d node: %d status: %d\n", id[i], (int)sizeof(ou->context),
                       ou->context, ou->vhost, ou->node, ou->status);
            break;
        }
    }

    if (type == TEXT_XML) {
        ap_rprintf(r, "</Contexts></Dump>");
    }
    return NULL;
}

/*
 * Process a INFO command.
 * Statics informations ;-)
 */
static char *process_info(request_rec *r, int *errtype)
{
    int size, i;
    int *id;
    unsigned char type;
    const char *accept_header = apr_table_get(r->headers_in, "Accept");
    (void)errtype;

    if (accept_header && strstr((char *)accept_header, "text/xml") != NULL) {
        ap_set_content_type(r, "text/xml");
        type = TEXT_XML;
        ap_rprintf(r, "<?xml version=\"1.0\" standalone=\"yes\" ?>\n");
    } else {
        ap_set_content_type(r, "text/plain");
        type = TEXT_PLAIN;
    }

    size = loc_get_max_size_node();
    if (size == 0) {
        return NULL;
    }
    id = apr_palloc(r->pool, sizeof(int) * size);
    size = get_ids_used_node(nodestatsmem, id);

    if (type == TEXT_XML) {
        ap_rprintf(r, "<Info><Nodes>");
    }

    for (i = 0; i < size; i++) {
        nodeinfo_t *ou;
        const proxy_worker_shared *proxystat;
        char *flushpackets;
        if (get_node(nodestatsmem, &ou, id[i]) != APR_SUCCESS) {
            continue;
        }

        switch (type) {
        case TEXT_XML:
            ap_rprintf(r, "<Node id=\"%d\" name=\"%.*s\">\
                           <Balancer>%.*s</Balancer>\
                           <LBGroup>%.*s</LBGroup>\
                           <Host>%.*s</Host>\
                           <Port>%.*s</Port>\
                           <Type>%.*s</Type>",
                       id[i], (int)sizeof(ou->mess.JVMRoute), ou->mess.JVMRoute, (int)sizeof(ou->mess.balancer),
                       ou->mess.balancer, (int)sizeof(ou->mess.Domain), ou->mess.Domain, (int)sizeof(ou->mess.Host),
                       ou->mess.Host, (int)sizeof(ou->mess.Port), ou->mess.Port, (int)sizeof(ou->mess.Type),
                       ou->mess.Type);
            break;
        case TEXT_PLAIN:
        default:
            ap_rprintf(r, "Node: [%d],Name: %.*s,Balancer: %.*s,LBGroup: %.*s,Host: %.*s,Port: %.*s,Type: %.*s", id[i],
                       (int)sizeof(ou->mess.JVMRoute), ou->mess.JVMRoute, (int)sizeof(ou->mess.balancer),
                       ou->mess.balancer, (int)sizeof(ou->mess.Domain), ou->mess.Domain, (int)sizeof(ou->mess.Host),
                       ou->mess.Host, (int)sizeof(ou->mess.Port), ou->mess.Port, (int)sizeof(ou->mess.Type),
                       ou->mess.Type);
            break;
        }

        flushpackets = "Off";
        switch (ou->mess.flushpackets) {
        case flush_on:
            flushpackets = "On";
            break;
        case flush_auto:
            flushpackets = "Auto";
        }

        switch (type) {
        case TEXT_XML:
            ap_rprintf(r, "<Flushpackets>%s</Flushpackets>\
                           <Flushwait>%d</Flushwait>\
                           <Ping>%d</Ping>\
                           <Smax>%d</Smax>\
                           <Ttl>%d</Ttl>",
                       flushpackets, ou->mess.flushwait / 1000, (int)apr_time_sec(ou->mess.ping), ou->mess.smax,
                       (int)apr_time_sec(ou->mess.ttl));
            break;
        case TEXT_PLAIN:
        default:
            ap_rprintf(r, ",Flushpackets: %s,Flushwait: %d,Ping: %d,Smax: %d,Ttl: %d", flushpackets,
                       ou->mess.flushwait / 1000, (int)apr_time_sec(ou->mess.ping), ou->mess.smax,
                       (int)apr_time_sec(ou->mess.ttl));
            break;
        }

        proxystat = read_shared_by_node(r, ou);
        if (!proxystat) {
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "process_config: No proxystat, assum zeros");
            proxystat = apr_pcalloc(r->pool, sizeof(proxy_worker_shared));
        }

        switch (type) {
        case TEXT_XML:
            ap_rprintf(r, "<Elected>%d</Elected>\
                           <Read>%d</Read>\
                           <Transfered>%d</Transfered>\
                           <Connected>%d</Connected>\
                           <Load>%d</Load>\
                           </Node>",
                       (int)proxystat->elected, (int)proxystat->read, (int)proxystat->transferred, (int)proxystat->busy,
                       proxystat->lbfactor);
            break;
        case TEXT_PLAIN:
        default:
            ap_rprintf(r, ",Elected: %d,Read: %d,Transfered: %d,Connected: %d,Load: %d\n", (int)proxystat->elected,
                       (int)proxystat->read, (int)proxystat->transferred, (int)proxystat->busy, proxystat->lbfactor);
            break;
        }
    }

    if (type == TEXT_XML) {
        ap_rprintf(r, "</Nodes>");
    }

    /* Process the Vhosts */
    size = loc_get_max_size_host();
    id = apr_palloc(r->pool, sizeof(int) * size);
    size = get_ids_used_host(hoststatsmem, id);
    if (type == TEXT_XML) {
        ap_rprintf(r, "<Vhosts>");
    }
    for (i = 0; i < size; i++) {
        hostinfo_t *ou;
        if (get_host(hoststatsmem, &ou, id[i]) != APR_SUCCESS) {
            continue;
        }

        switch (type) {
        case TEXT_XML:
            ap_rprintf(r, "<Vhost id=\"%d\" alias=\"%.*s\">\
                           <Node id=\"%d\"/>\
                           </Vhost>",
                       ou->vhost, (int)sizeof(ou->host), ou->host, ou->node);
            break;
        case TEXT_PLAIN:
        default:
            ap_rprintf(r, "Vhost: [%d:%d:%d], Alias: %.*s\n", ou->node, ou->vhost, id[i], (int)sizeof(ou->host),
                       ou->host);
            break;
        }
    }

    if (type == TEXT_XML) {
        ap_rprintf(r, "</Vhosts>");
    }

    /* Process the Contexts */
    size = loc_get_max_size_context();
    id = apr_palloc(r->pool, sizeof(int) * size);
    size = get_ids_used_context(contextstatsmem, id);

    if (type == TEXT_XML) {
        ap_rprintf(r, "<Contexts>");
    }

    for (i = 0; i < size; i++) {
        contextinfo_t *ou;
        if (get_context(contextstatsmem, &ou, id[i]) != APR_SUCCESS) {
            continue;
        }

        switch (type) {
        case TEXT_XML:
            ap_rprintf(r, "<Context id=\"%d\">\
                           <Status id=\"%d\">%s</Status>\
                           <Context>%.*s</Context>\
                           <Node id=\"%d\"/>\
                           <Vhost id=\"%d\"/>\
                           </Context>",
                       id[i], ou->status, context_status_to_string(ou->status), (int)sizeof(ou->context), ou->context,
                       ou->node, ou->vhost);
            break;
        case TEXT_PLAIN:
        default:
            ap_rprintf(r, "Context: [%d:%d:%d], Context: %.*s, Status: %s\n", ou->node, ou->vhost, id[i],
                       (int)sizeof(ou->context), ou->context, context_status_to_string(ou->status));
            break;
        }
    }

    if (type == TEXT_XML) {
        ap_rprintf(r, "</Contexts></Info>");
    }
    return NULL;
}

/**
 * Process a *-APP command that applies to the node NOTE: the node is locked
 */
static char *process_node_cmd(request_rec *r, int status, int *errtype, nodeinfo_t *node)
{
    /* for read the hosts */
    int i, j;
    int size = loc_get_max_size_host();
    int *id;
    (void)errtype;

    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "process_node_cmd: status %s processing node: %d",
                 context_status_to_string(status), node->mess.id);
    if (size == 0) {
        return NULL;
    }
    id = apr_palloc(r->pool, sizeof(int) * size);
    size = get_ids_used_host(hoststatsmem, id);
    for (i = 0; i < size; i++) {
        hostinfo_t *ou;
        int sizecontext;
        int *idcontext;

        if (get_host(hoststatsmem, &ou, id[i]) != APR_SUCCESS) {
            continue;
        }
        if (ou->node != node->mess.id) {
            continue;
        }
        /* If the host corresponds to a node process all contextes */
        sizecontext = get_max_size_context(contextstatsmem);
        idcontext = apr_palloc(r->pool, sizeof(int) * sizecontext);
        sizecontext = get_ids_used_context(contextstatsmem, idcontext);
        for (j = 0; j < sizecontext; j++) {
            contextinfo_t *context;
            if (get_context(contextstatsmem, &context, idcontext[j]) != APR_SUCCESS) {
                continue;
            }
            if (context->vhost == ou->vhost && context->node == ou->node) {
                /* Process the context */
                if (status != REMOVE) {
                    context->status = status;
                    insert_update_context(contextstatsmem, context);
                } else {
                    remove_context(contextstatsmem, context->id);
                }
            }
        }
        if (status == REMOVE) {
            remove_host(hoststatsmem, ou->id);
        }
    }

    /* The REMOVE-APP * removes the node (well mark it removed) */
    if (status == REMOVE) {
        int id;
        node->mess.remove = 1;
        insert_update_node(nodestatsmem, node, &id, 0);
    }
    return NULL;
}

/**
 * Process an enable/disable/stop/remove application message
 */
static char *process_appl_cmd(request_rec *r, char **ptr, int status, int *errtype, int global, int fromnode)
{
    nodeinfo_t nodeinfo;
    nodeinfo_t *node;
    struct cluster_host *vhost;

    int i = 0;
    hostinfo_t hostinfo;
    hostinfo_t *host;
    char *p_tmp;

    memset(&nodeinfo.mess, '\0', sizeof(nodeinfo.mess));
    /* Map nothing by default */
    vhost = apr_palloc(r->pool, sizeof(struct cluster_host));
    vhost->host = NULL;
    vhost->context = NULL;
    vhost->next = NULL;

    while (ptr[i]) {
        if (strcasecmp(ptr[i], "JVMRoute") == 0) {
            if (strlen(ptr[i + 1]) >= sizeof(nodeinfo.mess.JVMRoute)) {
                *errtype = TYPESYNTAX;
                return SROUBIG;
            }
            strcpy(nodeinfo.mess.JVMRoute, ptr[i + 1]);
            nodeinfo.mess.id = -1;
        }
        if (strcasecmp(ptr[i], "Alias") == 0) {
            if (vhost->host) {
                *errtype = TYPESYNTAX;
                return SMULALB;
            }
            p_tmp = ptr[i + 1];
            /* Aliases to lower case for further case-insensitive treatment, IETF RFC 1035 Section 2.3.3. */
            while (*p_tmp) {
                *p_tmp = apr_tolower(*p_tmp);
                ++p_tmp;
            }
            vhost->host = ptr[i + 1];
        }
        if (strcasecmp(ptr[i], "Context") == 0) {
            if (vhost->context) {
                *errtype = TYPESYNTAX;
                return SMULCTB;
            }
            vhost->context = ptr[i + 1];
        }
        i++;
        i++;
    }

    /* Check for JVMRoute, Alias and Context */
    if (nodeinfo.mess.JVMRoute[0] == '\0') {
        *errtype = TYPESYNTAX;
        return SROUBAD;
    }
    if (vhost->context == NULL && vhost->host != NULL) {
        *errtype = TYPESYNTAX;
        return SALIBAD;
    }
    if (vhost->host == NULL && vhost->context != NULL) {
        *errtype = TYPESYNTAX;
        return SCONBAD;
    }

    /* Read the node */
    loc_lock_nodes();
    node = read_node(nodestatsmem, &nodeinfo);
    if (node == NULL) {
        loc_unlock_nodes();
        if (status == REMOVE) {
            return NULL; /* Already done */
        }
        *errtype = TYPEMEM;
        return apr_psprintf(r->pool, MNODERD, nodeinfo.mess.JVMRoute);
    }

    /* If the node is marked removed check what to do */
    if (node->mess.remove) {
        loc_unlock_nodes();
        if (status == REMOVE) {
            return NULL; /* Already done */
        } else {
            /* Act has if the node wasn't found */
            *errtype = TYPEMEM;
            return apr_psprintf(r->pool, MNODERD, node->mess.JVMRoute);
        }
    }
    inc_version_node();

    /* Process the * APP commands */
    if (global) {
        char *ret;
        ret = process_node_cmd(r, status, errtype, node);
        loc_unlock_nodes();
        return ret;
    }

    /* Read the ID of the virtual host corresponding to the first Alias */
    hostinfo.node = node->mess.id;
    if (vhost->host != NULL) {
        char *s = hostinfo.host;
        unsigned j = 1;
        strncpy(hostinfo.host, vhost->host, HOSTALIASZ);
        while (*s != ',' && j < sizeof(hostinfo.host)) {
            j++;
            s++;
        }
        *s = '\0';
    } else {
        hostinfo.host[0] = '\0';
    }
    hostinfo.id = 0;
    host = read_host(hoststatsmem, &hostinfo);
    if (host == NULL) {
        /* If REMOVE ignores it */
        if (status == REMOVE) {
            loc_unlock_nodes();
            return NULL;
        } else {
            int vid, size, *id;
            /* Find the first available vhost id */
            vid = 0;
            size = loc_get_max_size_host();
            id = apr_palloc(r->pool, sizeof(int) * size);
            size = get_ids_used_host(hoststatsmem, id);
            for (i = 0; i < size; i++) {
                hostinfo_t *ou;
                if (get_host(hoststatsmem, &ou, id[i]) != APR_SUCCESS) {
                    continue;
                }

                if (ou->node == node->mess.id && ou->vhost > vid) {
                    vid = ou->vhost;
                }
            }
            vid++; /* Use next one. */
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "process_appl_cmd: adding vhost: %d node: %d route: %s",
                         vid, node->mess.id, nodeinfo.mess.JVMRoute);

            /* If the Host doesn't exist yet create it */
            if (insert_update_hosts(hoststatsmem, vhost->host, node->mess.id, vid) != APR_SUCCESS) {
                loc_unlock_nodes();
                *errtype = TYPEMEM;
                return apr_psprintf(r->pool, MHOSTUI, nodeinfo.mess.JVMRoute);
            }
            hostinfo.id = 0;
            hostinfo.node = node->mess.id;
            if (vhost->host != NULL) {
                strncpy(hostinfo.host, vhost->host, sizeof(hostinfo.host));
                hostinfo.host[sizeof(hostinfo.host) - 1] = '\0';
            } else {
                hostinfo.host[0] = '\0';
            }
            host = read_host(hoststatsmem, &hostinfo);
            if (host == NULL) {
                loc_unlock_nodes();
                *errtype = TYPEMEM;
                return apr_psprintf(r->pool, MHOSTRD, node->mess.JVMRoute);
            }
        }
    }

    if (status == ENABLED) {
        /* There is no load balancing between balancers */
        int size = loc_get_max_size_context();
        int *id = apr_palloc(r->pool, sizeof(int) * size);
        size = get_ids_used_context(contextstatsmem, id);
        for (i = 0; i < size; i++) {
            contextinfo_t *ou;
            if (get_context(contextstatsmem, &ou, id[i]) != APR_SUCCESS) {
                continue;
            }
            if (strcmp(ou->context, vhost->context) == 0) {
                /* There is the same context somewhere else */
                nodeinfo_t *hisnode;
                if (get_node(nodestatsmem, &hisnode, ou->node) != APR_SUCCESS) {
                    continue;
                }
                if (strcmp(hisnode->mess.balancer, node->mess.balancer)) {
                    /* the same context would be on 2 different balancer */
                    ap_log_error(APLOG_MARK, APLOG_WARNING, 0, r->server,
                                 "process_appl_cmd: ENABLE: context %s is in balancer %s and %s", vhost->context,
                                 node->mess.balancer, hisnode->mess.balancer);
                }
            }
        }
    }

    /* Now update each context from Context: part */
    if (insert_update_contexts(contextstatsmem, vhost->context, node->mess.id, host->vhost, status) != APR_SUCCESS) {
        loc_unlock_nodes();
        *errtype = TYPEMEM;
        return apr_psprintf(r->pool, MCONTUI, node->mess.JVMRoute);
    }

    /* Remove the host if all the contextes have been removed */
    if (status == REMOVE) {
        int size = loc_get_max_size_context();
        int *id = apr_palloc(r->pool, sizeof(int) * size);
        size = get_ids_used_context(contextstatsmem, id);
        for (i = 0; i < size; i++) {
            contextinfo_t *ou;
            if (get_context(contextstatsmem, &ou, id[i]) != APR_SUCCESS) {
                continue;
            }
            if (ou->vhost == host->vhost && ou->node == node->mess.id) {
                break;
            }
        }
        if (i == size) {
            int size = loc_get_max_size_host();
            int *id = apr_palloc(r->pool, sizeof(int) * size);
            size = get_ids_used_host(hoststatsmem, id);
            for (i = 0; i < size; i++) {
                hostinfo_t *ou;

                if (get_host(hoststatsmem, &ou, id[i]) != APR_SUCCESS) {
                    continue;
                }
                if (ou->vhost == host->vhost && ou->node == node->mess.id) {
                    remove_host(hoststatsmem, ou->id);
                }
            }
        }
    } else if (status == STOPPED) {
        /* insert_update_contexts in fact makes that vhost->context corresponds only to the first context... */
        contextinfo_t in;
        contextinfo_t *ou;
        in.id = 0;
        strncpy(in.context, vhost->context, CONTEXTSZ);
        in.vhost = host->vhost;
        in.node = node->mess.id;
        ou = read_context(contextstatsmem, &in);
        if (ou != NULL) {
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "process_appl_cmd: STOP-APP nbrequests %d",
                         ou->nbrequests);
            if (fromnode) {
                ap_set_content_type(r, "text/plain");
                ap_rprintf(r, "Type=STOP-APP-RSP&JvmRoute=%.*s&Alias=%.*s&Context=%.*s&Requests=%d",
                           (int)sizeof(nodeinfo.mess.JVMRoute), nodeinfo.mess.JVMRoute, (int)sizeof(vhost->host),
                           vhost->host, (int)sizeof(vhost->context), vhost->context, ou->nbrequests);
                ap_rprintf(r, "\n");
            }
        } else {
            ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "process_appl_cmd: STOP-APP can't read_context");
        }
    }
    loc_unlock_nodes();
    return NULL;
}

static char *process_enable(request_rec *r, char **ptr, int *errtype, int global)
{
    return process_appl_cmd(r, ptr, ENABLED, errtype, global, 0);
}

static char *process_disable(request_rec *r, char **ptr, int *errtype, int global)
{
    return process_appl_cmd(r, ptr, DISABLED, errtype, global, 0);
}

static char *process_stop(request_rec *r, char **ptr, int *errtype, int global, int fromnode)
{
    return process_appl_cmd(r, ptr, STOPPED, errtype, global, fromnode);
}

static char *process_remove(request_rec *r, char **ptr, int *errtype, int global)
{
    return process_appl_cmd(r, ptr, REMOVE, errtype, global, 0);
}

/*
 * Call the ping/pong logic
 * Do a ping/png request to the node and set the load factor.
 */
static int isnode_up(request_rec *r, int id, int Load)
{
    return balancerhandler != NULL ? balancerhandler->proxy_node_isup(r, id, Load) : OK;
}

/*
 * Call the ping/pong logic using scheme://host:port
 * Do a ping/png request to the node and set the load factor.
 */
static int ishost_up(request_rec *r, char *scheme, char *host, char *port)
{
    return balancerhandler != NULL ? balancerhandler->proxy_host_isup(r, scheme, host, port) : OK;
}

/*
 * Process the STATUS command
 * Load -1 : Broken
 * Load 0  : Standby.
 * Load 1-100 : Load factor.
 */
static char *process_status(request_rec *r, const char *const *ptr, int *errtype)
{
    int Load = -1;
    nodeinfo_t nodeinfo;
    nodeinfo_t *node;

    int i = 0;

    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "Processing STATUS");
    while (ptr[i]) {
        if (strcasecmp(ptr[i], "JVMRoute") == 0) {
            if (strlen(ptr[i + 1]) >= sizeof(nodeinfo.mess.JVMRoute)) {
                *errtype = TYPESYNTAX;
                return SROUBIG;
            }
            strcpy(nodeinfo.mess.JVMRoute, ptr[i + 1]);
            nodeinfo.mess.id = -1;
        } else if (strcasecmp(ptr[i], "Load") == 0) {
            Load = atoi(ptr[i + 1]);
        } else {
            *errtype = TYPESYNTAX;
            return apr_psprintf(r->pool, SBADFLD, ptr[i]);
        }
        i++;
        i++;
    }

    /* Read the node */
    loc_lock_nodes();
    node = read_node(nodestatsmem, &nodeinfo);
    loc_unlock_nodes();
    if (node == NULL) {
        *errtype = TYPEMEM;
        return apr_psprintf(r->pool, MNODERD, nodeinfo.mess.JVMRoute);
    }

    /*
     * If the node is usualable do a ping/pong to prevent Split-Brain Syndrome
     * and update the worker status and load factor acccording to the test result.
     */
    ap_set_content_type(r, "text/plain");
    ap_rprintf(r, "Type=STATUS-RSP&JVMRoute=%.*s", (int)sizeof(nodeinfo.mess.JVMRoute), nodeinfo.mess.JVMRoute);

    ap_rprintf(r, isnode_up(r, node->mess.id, Load) != OK ? "&State=NOTOK" : "&State=OK");

    ap_rprintf(r, "&id=%d", (int)ap_scoreboard_image->global->restart_time);

    ap_rprintf(r, "\n");
    return NULL;
}

/*
 * Process the VERSION command
 */
static char *process_version(request_rec *r, const char *const *const ptr, int *errtype)
{
    const char *accept_header = apr_table_get(r->headers_in, "Accept");
    (void)ptr;
    (void)errtype;

    if (accept_header && strstr((char *)accept_header, "text/xml") != NULL) {
        ap_set_content_type(r, "text/xml");
        ap_rprintf(r, "<?xml version=\"1.0\" standalone=\"yes\" ?>\n");
        ap_rprintf(r, "<version><release>%s</release><protocol>%s</protocol></version>", MOD_CLUSTER_EXPOSED_VERSION,
                   VERSION_PROTOCOL);
    } else {
        ap_set_content_type(r, "text/plain");
        ap_rprintf(r, "release: %s, protocol: %s", MOD_CLUSTER_EXPOSED_VERSION, VERSION_PROTOCOL);
    }
    ap_rprintf(r, "\n");
    return NULL;
}

/*
 * Process the PING command
 * With a JVMRoute does a cping/cpong in the node.
 * Without just answers ok.
 * NOTE: It is hard to cping/cpong a host + port but CONFIG + PING + REMOVE_APP *
 *       would do the same.
 */
static char *process_ping(request_rec *r, const char *const *ptr, int *errtype)
{
    nodeinfo_t nodeinfo;
    nodeinfo_t *node;
    char *scheme = NULL;
    char *host = NULL;
    char *port = NULL;

    int i = 0;

    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "Processing PING");
    nodeinfo.mess.id = -1;
    while (ptr[i] && ptr[i][0] != '\0') {
        if (strcasecmp(ptr[i], "JVMRoute") == 0) {
            if (strlen(ptr[i + 1]) >= sizeof(nodeinfo.mess.JVMRoute)) {
                *errtype = TYPESYNTAX;
                return SROUBIG;
            }
            strcpy(nodeinfo.mess.JVMRoute, ptr[i + 1]);
            nodeinfo.mess.id = 0;
        } else if (strcasecmp(ptr[i], "Scheme") == 0) {
            scheme = apr_pstrdup(r->pool, ptr[i + 1]);
        } else if (strcasecmp(ptr[i], "Host") == 0) {
            host = apr_pstrdup(r->pool, ptr[i + 1]);
        } else if (strcasecmp(ptr[i], "Port") == 0) {
            port = apr_pstrdup(r->pool, ptr[i + 1]);
        } else {
            *errtype = TYPESYNTAX;
            return apr_psprintf(r->pool, SBADFLD, ptr[i]);
        }
        i++;
        i++;
    }
    if (nodeinfo.mess.id == -1) {
        /* PING scheme, host, port or just httpd */
        if (scheme == NULL && host == NULL && port == NULL) {
            ap_set_content_type(r, "text/plain");
            ap_rprintf(r, "Type=PING-RSP&State=OK");
        } else {
            if (scheme == NULL || host == NULL || port == NULL) {
                *errtype = TYPESYNTAX;
                return apr_psprintf(r->pool, SMISFLD);
            }
            ap_set_content_type(r, "text/plain");
            ap_rprintf(r, "Type=PING-RSP");
            ap_rprintf(r, ishost_up(r, scheme, host, port) != OK ? "&State=NOTOK" : "&State=OK");
        }
    } else {

        /* Read the node */
        loc_lock_nodes();
        node = read_node(nodestatsmem, &nodeinfo);
        loc_unlock_nodes();
        if (node == NULL) {
            *errtype = TYPEMEM;
            return apr_psprintf(r->pool, MNODERD, nodeinfo.mess.JVMRoute);
        }

        /*
         * If the node is usualable do a ping/pong to prevent Split-Brain Syndrome
         * and update the worker status and load factor acccording to the test result.
         */
        ap_set_content_type(r, "text/plain");
        ap_rprintf(r, "Type=PING-RSP&JVMRoute=%.*s", (int)sizeof(nodeinfo.mess.JVMRoute), nodeinfo.mess.JVMRoute);

        ap_rprintf(r, isnode_up(r, node->mess.id, -2) != OK ? "&State=NOTOK" : "&State=OK");
    }
    ap_rprintf(r, "&id=%d", (int)ap_scoreboard_image->global->restart_time);

    ap_rprintf(r, "\n");
    return NULL;
}

/*
 * Decodes a '%' escaped string, and returns the number of characters
 * (From mod_proxy_ftp.c).
 */

/* already called in the knowledge that the characters are hex digits */
/* Copied from modules/proxy/proxy_util.c */
static int mod_manager_hex2c(const char *x)
{
    int i, ch;

#if !APR_CHARSET_EBCDIC
    ch = x[0];
    if (apr_isdigit(ch)) {
        i = ch - '0';
    } else if (apr_isupper(ch)) {
        i = ch - ('A' - 10);
    } else {
        i = ch - ('a' - 10);
    }
    i <<= 4;

    ch = x[1];
    if (apr_isdigit(ch)) {
        i += ch - '0';
    } else if (apr_isupper(ch)) {
        i += ch - ('A' - 10);
    } else {
        i += ch - ('a' - 10);
    }
    return i;
#else  /*APR_CHARSET_EBCDIC */
    /*
     * we assume that the hex value refers to an ASCII character
     * so convert to EBCDIC so that it makes sense locally;
     *
     * example:
     *
     * client specifies %20 in URL to refer to a space char;
     * at this point we're called with EBCDIC "20"; after turning
     * EBCDIC "20" into binary 0x20, we then need to assume that 0x20
     * represents an ASCII char and convert 0x20 to EBCDIC, yielding
     * 0x40
     */
    char buf[1];

    if (1 == sscanf(x, "%2x", &i)) {
        buf[0] = i & 0xFF;
        ap_xlate_proto_from_ascii(buf, 1);
        return buf[0];
    } else {
        return 0;
    }
#endif /*APR_CHARSET_EBCDIC */
}

/*
 * Processing of decoded characters
 */
static apr_status_t decodeenc(char **ptr)
{
    int val, i, j;
    char ch;
    val = 0;
    while (NULL != ptr[val]) {
        if (ptr[val][0] == '\0') {
            return APR_SUCCESS; /* special case for no characters */
        }
        for (i = 0, j = 0; ptr[val][i] != '\0'; i++, j++) {
            /* decode it if not already done */
            ch = ptr[val][i];
            if (ch == '%' && apr_isxdigit(ptr[val][i + 1]) && apr_isxdigit(ptr[val][i + 2])) {
                ch = (char)mod_manager_hex2c(&(ptr[val][i + 1]));
                i += 2;
            }

            /* process decoded, = and & are legit characters */
            /* from apr_escape_entity() */
            if (ch == '<' || ch == '>' || ch == '\"' || ch == '\'') {
                return TYPESYNTAX;
            }
            /* from apr_escape_shell() */
            if (ch == '\r' || ch == '\n') {
                return TYPESYNTAX;
            }

            ptr[val][j] = ch;
        }
        ptr[val][j] = '\0';
        val++;
    }
    return APR_SUCCESS;
}

/*
 * Check that the method is one of ours
 */
static int check_method(const request_rec *r)
{
    int ours = 0;
    if (strcasecmp(r->method, "CONFIG") == 0) {
        ours = 1;
    } else if (strcasecmp(r->method, "ENABLE-APP") == 0) {
        ours = 1;
    } else if (strcasecmp(r->method, "DISABLE-APP") == 0) {
        ours = 1;
    } else if (strcasecmp(r->method, "STOP-APP") == 0) {
        ours = 1;
    } else if (strcasecmp(r->method, "REMOVE-APP") == 0) {
        ours = 1;
    } else if (strcasecmp(r->method, "STATUS") == 0) {
        ours = 1;
    } else if (strcasecmp(r->method, "DUMP") == 0) {
        ours = 1;
    } else if (strcasecmp(r->method, "ERROR") == 0) {
        ours = 1;
    } else if (strcasecmp(r->method, "INFO") == 0) {
        ours = 1;
    } else if (strcasecmp(r->method, "PING") == 0) {
        ours = 1;
    } else if (strcasecmp(r->method, "ADDID") == 0) {
        ours = 1;
    } else if (strcasecmp(r->method, "REMOVEID") == 0) {
        ours = 1;
    } else if (strcasecmp(r->method, "QUERY") == 0) {
        ours = 1;
    } else if (strcasecmp(r->method, "VERSION") == 0) {
        ours = 1;
    }
    return ours;
}

/*
 * This routine is called before mod_proxy translate name.
 * This allows us to make decisions before mod_proxy
 * to be able to fill tables even with ProxyPass / balancer...
 */
static int manager_trans(request_rec *r)
{
    int ours = 0;
    core_dir_config *conf = (core_dir_config *)ap_get_module_config(r->per_dir_config, &core_module);
    mod_manager_config *mconf = ap_get_module_config(r->server->module_config, &manager_module);

    if (conf && conf->handler && r->method_number == M_GET && strcmp(conf->handler, "mod_cluster-manager") == 0) {
        r->handler = "mod_cluster-manager";
        r->filename = apr_pstrdup(r->pool, r->uri);
        return OK;
    }
    if (r->method_number != M_INVALID) {
        return DECLINED;
    }
    if (!mconf->enable_mcpm_receive) {
        return DECLINED; /* Not allowed to receive MCMP */
    }

    ours = check_method(r);
    if (ours) {
        int i;
        /* The method one of ours */
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "manager_trans: %s (%s)", r->method, r->uri);
        r->handler = "mod-cluster"; /* that hack doesn't work on httpd-2.4.x */
        i = strlen(r->uri);
        if (strcmp(r->uri, "*") == 0 || (i >= 2 && r->uri[i - 1] == '*' && r->uri[i - 2] == '/')) {
            r->filename = apr_pstrdup(r->pool, NODE_COMMAND);
        } else {
            r->filename = apr_pstrdup(r->pool, r->uri);
        }
        return OK;
    }

    return DECLINED;
}

/*
 * fixup logic to prevent subrequest to our methods
 */
static int manager_map_to_storage(request_rec *r)
{
    int ours = 0;
    mod_manager_config *mconf = ap_get_module_config(r->server->module_config, &manager_module);
    if (r->method_number != M_INVALID) {
        return DECLINED;
    }
    if (!mconf->enable_mcpm_receive) {
        return DECLINED; /* Not allowed to receive MCMP */
    }

    ours = check_method(r);
    if (ours) {
        /* The method one of ours */
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "manager_map_to_storage: %s (%s)", r->method, r->uri);
        return OK;
    }

    return DECLINED;
}

/*
 * Create the commands that are possible on the context
 */
static char *context_string(request_rec *r, contextinfo_t *ou, const char *Alias, const char *JVMRoute)
{
    char context[CONTEXTSZ + 1];
    strncpy(context, ou->context, CONTEXTSZ + 1);
    return apr_pstrcat(r->pool, "JVMRoute=", JVMRoute, "&Alias=", Alias, "&Context=", context, NULL);
}

static char *balancer_nonce_string(request_rec *r)
{
    char *ret = "";
    void *sconf = r->server->module_config;
    mod_manager_config *mconf = ap_get_module_config(sconf, &manager_module);
    if (mconf->nonce) {
        ret = apr_psprintf(r->pool, "nonce=%s&", balancer_nonce);
    }
    return ret;
}

static void context_command_string(request_rec *r, contextinfo_t *ou, const char *Alias, const char *JVMRoute)
{
    if (ou->status == DISABLED) {
        ap_rprintf(r, "<a href=\"%s?%sCmd=ENABLE-APP&Range=CONTEXT&%s\">Enable</a> ", r->uri, balancer_nonce_string(r),
                   context_string(r, ou, Alias, JVMRoute));
        ap_rprintf(r, " <a href=\"%s?%sCmd=STOP-APP&Range=CONTEXT&%s\">Stop</a>", r->uri, balancer_nonce_string(r),
                   context_string(r, ou, Alias, JVMRoute));
    }
    if (ou->status == ENABLED) {
        ap_rprintf(r, "<a href=\"%s?%sCmd=DISABLE-APP&Range=CONTEXT&%s\">Disable</a>", r->uri, balancer_nonce_string(r),
                   context_string(r, ou, Alias, JVMRoute));
        ap_rprintf(r, " <a href=\"%s?%sCmd=STOP-APP&Range=CONTEXT&%s\">Stop</a>", r->uri, balancer_nonce_string(r),
                   context_string(r, ou, Alias, JVMRoute));
    }
    if (ou->status == STOPPED) {
        ap_rprintf(r, "<a href=\"%s?%sCmd=ENABLE-APP&Range=CONTEXT&%s\">Enable</a> ", r->uri, balancer_nonce_string(r),
                   context_string(r, ou, Alias, JVMRoute));
        ap_rprintf(r, "<a href=\"%s?%sCmd=DISABLE-APP&Range=CONTEXT&%s\">Disable</a>", r->uri, balancer_nonce_string(r),
                   context_string(r, ou, Alias, JVMRoute));
    }
}

/*
 * Create the commands that are possible on the node
 */
static char *node_string(request_rec *r, const char *JVMRoute)
{
    return apr_pstrcat(r->pool, "JVMRoute=", JVMRoute, NULL);
}

static void node_command_string(request_rec *r, const char *JVMRoute)
{
    ap_rprintf(r, "<a href=\"%s?%sCmd=ENABLE-APP&Range=NODE&%s\">Enable Contexts</a> ", r->uri,
               balancer_nonce_string(r), node_string(r, JVMRoute));
    ap_rprintf(r, "<a href=\"%s?%sCmd=DISABLE-APP&Range=NODE&%s\">Disable Contexts</a> ", r->uri,
               balancer_nonce_string(r), node_string(r, JVMRoute));
    ap_rprintf(r, "<a href=\"%s?%sCmd=STOP-APP&Range=NODE&%s\">Stop Contexts</a>", r->uri, balancer_nonce_string(r),
               node_string(r, JVMRoute));
}

static void domain_command_string(request_rec *r, const char *Domain)
{
    ap_rprintf(r, "<a href=\"%s?%sCmd=ENABLE-APP&Range=DOMAIN&Domain=%s\">Enable Nodes</a> ", r->uri,
               balancer_nonce_string(r), Domain);
    ap_rprintf(r, "<a href=\"%s?%sCmd=DISABLE-APP&Range=DOMAIN&Domain=%s\">Disable Nodes</a> ", r->uri,
               balancer_nonce_string(r), Domain);
    ap_rprintf(r, "<a href=\"%s?%sCmd=STOP-APP&Range=DOMAIN&Domain=%s\">Stop Nodes</a>", r->uri,
               balancer_nonce_string(r), Domain);
}

/*
 * Process the parameters and display corresponding informations
 */
static void manager_info_contexts(request_rec *r, int reduce_display, int allow_cmd, int node, int host,
                                  const char *Alias, const char *JVMRoute)
{
    int size, i;
    int *id;
    /* Process the Contexts */
    if (!reduce_display) {
        ap_rprintf(r, "<h3>Contexts:</h3>");
    }
    ap_rprintf(r, "<pre>");
    size = loc_get_max_size_context();
    if (size == 0) {
        return;
    }
    id = apr_palloc(r->pool, sizeof(int) * size);
    size = get_ids_used_context(contextstatsmem, id);
    for (i = 0; i < size; i++) {
        contextinfo_t *ou;
        if (get_context(contextstatsmem, &ou, id[i]) != APR_SUCCESS) {
            continue;
        }
        if (ou->node != node || ou->vhost != host) {
            continue;
        }
        ap_rprintf(r, "%.*s, Status: %s Request: %d ", (int)sizeof(ou->context), ou->context,
                   context_status_to_string(ou->status), ou->nbrequests);
        if (allow_cmd) {
            context_command_string(r, ou, Alias, JVMRoute);
        }
        ap_rprintf(r, "\n");
    }
    ap_rprintf(r, "</pre>");
}

static void manager_info_hosts(request_rec *r, int reduce_display, int allow_cmd, int node, const char *JVMRoute)
{
    int size, i, j;
    int *id, *idChecker;
    int vhost = 0;

    /* Process the Vhosts */
    size = loc_get_max_size_host();
    if (size == 0) {
        return;
    }
    id = apr_palloc(r->pool, sizeof(int) * size);
    size = get_ids_used_host(hoststatsmem, id);
    idChecker = apr_pcalloc(r->pool, sizeof(int) * size);
    for (i = 0; i < size; i++) {
        hostinfo_t *ou;
        if (get_host(hoststatsmem, &ou, id[i]) != APR_SUCCESS) {
            continue;
        }
        if (ou->node != node) {
            continue;
        }
        if (ou->vhost != vhost) {
            /* if we've logged this already, contine */
            if (idChecker[i] == 1) {
                continue;
            }
            if (vhost && !reduce_display) {
                ap_rprintf(r, "</pre>");
            }
            if (!reduce_display) {
                ap_rprintf(r, "<h2> Virtual Host %d:</h2>", ou->vhost);
            }
            manager_info_contexts(r, reduce_display, allow_cmd, ou->node, ou->vhost, ou->host, JVMRoute);
            if (reduce_display) {
                ap_rprintf(r, "Aliases: ");
            } else {
                ap_rprintf(r, "<h3>Aliases:</h3>");
                ap_rprintf(r, "<pre>");
            }
            vhost = ou->vhost;

            ap_rprintf(r, "%.*s", (int)sizeof(ou->host), ou->host);
            ap_rprintf(r, reduce_display ? " " : "\n");

            /* Go ahead and check for any other later alias entries for this vhost and print them now */
            for (j = i + 1; j < size; j++) {
                hostinfo_t *pv;
                if (get_host(hoststatsmem, &pv, id[j]) != APR_SUCCESS) {
                    continue;
                }
                if (pv->node != node) {
                    continue;
                }
                if (pv->vhost != vhost) {
                    continue;
                }

                /* mark this entry as logged */
                idChecker[j] = 1;
                /* step the outer loop forward if we can */
                if (i == j - 1) {
                    i++;
                }
                ap_rprintf(r, "%.*s", (int)sizeof(pv->host), pv->host);
                ap_rprintf(r, reduce_display ? " " : "\n");
            }
        }
    }
    if (size && !reduce_display) {
        ap_rprintf(r, "</pre>");
    }
}

static void manager_sessionid(request_rec *r)
{
    int size, i;
    int *id;

    /* Process the Sessionids */
    size = loc_get_max_size_sessionid();
    if (size == 0) {
        return;
    }
    id = apr_palloc(r->pool, sizeof(int) * size);
    size = get_ids_used_sessionid(sessionidstatsmem, id);
    if (!size) {
        return;
    }
    ap_rprintf(r, "<h1>SessionIDs:</h1>");
    ap_rprintf(r, "<pre>");
    for (i = 0; i < size; i++) {
        sessionidinfo_t *ou;
        if (get_sessionid(sessionidstatsmem, &ou, id[i]) != APR_SUCCESS) {
            continue;
        }
        ap_rprintf(r, "id: %.*s route: %.*s\n", (int)sizeof(ou->sessionid), ou->sessionid, (int)sizeof(ou->JVMRoute),
                   ou->JVMRoute);
    }
    ap_rprintf(r, "</pre>");
}

static void manager_domain(request_rec *r, int reduce_display)
{
#if HAVE_CLUSTER_EX_DEBUG
    int size, i;
    int *id;

    /* Process the domain information: the removed node belonging to a domain are stored there */
    if (reduce_display) {
        ap_rprintf(r, "<br/>LBGroup:");
    } else {
        ap_rprintf(r, "<h1>LBGroup:</h1>");
    }
    ap_rprintf(r, "<pre>");
    size = loc_get_max_size_domain();
    if (size == 0) {
        return;
    }
    id = apr_palloc(r->pool, sizeof(int) * size);
    size = get_ids_used_domain(domainstatsmem, id);
    for (i = 0; i < size; i++) {
        domaininfo_t *ou;
        if (get_domain(domainstatsmem, &ou, id[i]) != APR_SUCCESS) {
            continue;
        }
        ap_rprintf(r, "dom: %.*s route: %.*s balancer: %.*s\n", DOMAINNDSZ, ou->domain, JVMROUTESZ, ou->JVMRoute,
                   BALANCERSZ, ou->balancer);
    }
    ap_rprintf(r, "</pre>");
#endif
}

static int count_sessionid(request_rec *r, const char *route)
{
    int size, i;
    int *id;
    int count = 0;

    /* Count the sessionid corresponding to the route */
    size = loc_get_max_size_sessionid();
    if (size == 0) {
        return 0;
    }
    id = apr_palloc(r->pool, sizeof(int) * size);
    size = get_ids_used_sessionid(sessionidstatsmem, id);
    for (i = 0; i < size; i++) {
        sessionidinfo_t *ou;
        if (get_sessionid(sessionidstatsmem, &ou, id[i]) != APR_SUCCESS) {
            continue;
        }
        if (strcmp(route, ou->JVMRoute) == 0) {
            count++;
        }
    }
    return count;
}

static void process_error(request_rec *r, char *errstring, int errtype)
{
    r->status_line = apr_psprintf(r->pool, "ERROR");
    apr_table_setn(r->err_headers_out, "Version", VERSION_PROTOCOL);
    switch (errtype) {
    case TYPESYNTAX:
        apr_table_setn(r->err_headers_out, "Type", "SYNTAX");
        break;
    case TYPEMEM:
        apr_table_setn(r->err_headers_out, "Type", "MEM");
        break;
    default:
        apr_table_setn(r->err_headers_out, "Type", "GENERAL");
        break;
    }
    apr_table_setn(r->err_headers_out, "Mess", errstring);
    ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server, "manager_handler %s error: %s", r->method, errstring);
}

static void sort_nodes(nodeinfo_t *nodes, int nbnodes)
{
    int i;
    int changed = -1;
    if (nbnodes <= 1) {
        return;
    }
    while (changed) {
        changed = 0;
        for (i = 0; i < nbnodes - 1; i++) {
            if (strcmp(nodes[i].mess.Domain, nodes[i + 1].mess.Domain) > 0) {
                nodeinfo_t node;
                node = nodes[i + 1];
                nodes[i + 1] = nodes[i];
                nodes[i] = node;
                changed = -1;
            }
        }
    }
}

static char *process_domain(request_rec *r, char **ptr, int *errtype, const char *cmd, const char *domain)
{
    int size, i;
    int *id;
    int pos;
    char *errstring = NULL;
    size = loc_get_max_size_node();
    if (size == 0) {
        return NULL;
    }
    id = apr_palloc(r->pool, sizeof(int) * size);
    size = get_ids_used_node(nodestatsmem, id);

    for (pos = 0; ptr[pos] != NULL && ptr[pos + 1] != NULL; pos = pos + 2) {
    }

    ptr[pos] = apr_pstrdup(r->pool, "JVMRoute");
    ptr[pos + 2] = NULL;
    ptr[pos + 3] = NULL;
    ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server, "process_domain");
    for (i = 0; i < size; i++) {
        nodeinfo_t *ou;
        if (get_node(nodestatsmem, &ou, id[i]) != APR_SUCCESS) {
            continue;
        }
        if (strcmp(ou->mess.Domain, domain) != 0) {
            continue;
        }
        /* add the JVMRoute */
        ptr[pos + 1] = apr_pstrdup(r->pool, ou->mess.JVMRoute);
        if (strcasecmp(cmd, "ENABLE-APP") == 0) {
            errstring = process_enable(r, ptr, errtype, RANGENODE);
        } else if (strcasecmp(cmd, "DISABLE-APP") == 0) {
            errstring = process_disable(r, ptr, errtype, RANGENODE);
        } else if (strcasecmp(cmd, "STOP-APP") == 0) {
            errstring = process_stop(r, ptr, errtype, RANGENODE, 0);
        } else if (strcasecmp(cmd, "REMOVE-APP") == 0) {
            errstring = process_remove(r, ptr, errtype, RANGENODE);
        }
    }
    return errstring;
}

/* XXX: move to mod_proxy_cluster as a provider ? */
static void printproxy_stat(request_rec *r, int reduce_display, nodeinfo_t *node)
{
    char *status;
    const proxy_worker_shared *proxystat = read_shared_by_node(r, node);
    if (!proxystat) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server, "printproxy_stat: can't find worker");
        status = "NOTOK";
        proxystat = apr_pcalloc(r->pool, sizeof(proxy_worker_shared));
    } else {
        status = proxystat->status & PROXY_WORKER_NOT_USABLE_BITMAP ? "NOTOK" : "OK";
    }


    if (reduce_display) {
        ap_rprintf(r, " %s ", status);
    } else {
        ap_rprintf(r, ",Status: %s,Elected: %d,Read: %d,Transferred: %d,Connected: %d,Load: %d", status,
                   (int)proxystat->elected, (int)proxystat->read, (int)proxystat->transferred, (int)proxystat->busy,
                   proxystat->lbfactor);
    }
}

/*
 * Display module information
 */
static void modules_info(request_rec *r)
{
    if (ap_find_linked_module("mod_proxy_cluster.c") != NULL) {
        ap_rputs("mod_proxy_cluster.c: OK<br/>", r);
    } else {
        ap_rputs("mod_proxy_cluster.c: missing<br/>", r);
    }

    if (ap_find_linked_module("mod_sharedmem.c") != NULL) {
        ap_rputs("mod_sharedmem.c: OK<br/>", r);
    } else {
        ap_rputs("mod_sharedmem.c: missing<br/>", r);
    }

    ap_rputs("Protocol supported: ", r);
    if (ap_find_linked_module("mod_proxy_http.c") != NULL) {
        ap_rputs("http ", r);
    }
    if (ap_find_linked_module("mod_proxy_ajp.c") != NULL) {
        ap_rputs("AJP ", r);
    }
    if (ap_find_linked_module("mod_ssl.c") != NULL) {
        ap_rputs("https", r);
    }
    ap_rputs("<br/>", r);

    if (ap_find_linked_module("mod_advertise.c") != NULL) {
        ap_rputs("mod_advertise.c: OK<br/>", r);
    } else {
        ap_rputs("mod_advertise.c: not loaded<br/>", r);
    }
}

/*
 * Process INFO message and mod_cluster_manager pages generation
 */
static int manager_info(request_rec *r)
{
    int size, i, sizesessionid;
    int *id;
    apr_table_t *params = apr_table_make(r->pool, 10);
    int access_status;
    const char *name;
    nodeinfo_t *nodes;
    int nbnodes = 0;
    char *domain = "";
    char *errstring = NULL;
    void *sconf = r->server->module_config;
    mod_manager_config *mconf = ap_get_module_config(sconf, &manager_module);

    if (r->args) {
        char *args = apr_pstrdup(r->pool, r->args);
        char *tok, *val;
        while (args && *args) {
            if ((val = ap_strchr(args, '='))) {
                *val++ = '\0';
                if ((tok = ap_strchr(val, '&'))) {
                    *tok++ = '\0';
                }
                /*
                 * Special case: contexts contain path information
                 */
                if ((access_status = ap_unescape_url(val)) != OK) {
                    if (strcmp(args, "Context") || (access_status != HTTP_NOT_FOUND)) {
                        return access_status;
                    }
                }
                apr_table_setn(params, args, val);
                args = tok;
            } else {
                return HTTP_BAD_REQUEST;
            }
        }
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "manager_info: request: %s", r->args);
    }

    /*
     * Check that the supplied nonce matches this server's nonce;
     * otherwise ignore all parameters, to prevent a CSRF attack.
     */
    if (mconf->nonce && ((name = apr_table_get(params, "nonce")) == NULL || strcmp(balancer_nonce, name) != 0)) {
        apr_table_clear(params);
    }

    /* process the parameters */
    if (r->args) {
        const char *val = apr_table_get(params, "Refresh");
        const char *cmd = apr_table_get(params, "Cmd");
        const char *typ = apr_table_get(params, "Range");
        const char *domain = apr_table_get(params, "Domain");
        /* Process the Refresh parameter */
        if (val) {
            long t = atol(val);
            apr_table_set(r->headers_out, "Refresh", apr_ltoa(r->pool, t < 1 ? 10 : t));
        }
        /* Process INFO and DUMP */
        if (cmd != NULL) {
            int errtype = 0;
            if (strcasecmp(cmd, "DUMP") == 0) {
                errstring = process_dump(r, &errtype);
                if (!errstring) {
                    return OK;
                }
            } else if (strcasecmp(cmd, "INFO") == 0) {
                errstring = process_info(r, &errtype);
                if (!errstring) {
                    return OK;
                }
            }
            if (errstring) {
                process_error(r, errstring, errtype);
            }
        }
        /* Process other command if any */
        if (cmd != NULL && typ != NULL && mconf->allow_cmd && errstring == NULL) {
            int global = RANGECONTEXT;
            int errtype = 0;
            int i;
            char **ptr;
            const apr_array_header_t *arr = apr_table_elts(params);
            const apr_table_entry_t *elts = (const apr_table_entry_t *)arr->elts;

            if (strcasecmp(typ, "NODE") == 0) {
                global = RANGENODE;
            } else if (strcasecmp(typ, "DOMAIN") == 0) {
                global = RANGEDOMAIN;
            }

            if (global == RANGEDOMAIN) {
                ptr = apr_palloc(r->pool, sizeof(char *) * (arr->nelts + 2) * 2);
            } else {
                ptr = apr_palloc(r->pool, sizeof(char *) * (arr->nelts + 1) * 2);
            }
            for (i = 0; i < arr->nelts; i++) {
                ptr[i * 2] = elts[i].key;
                ptr[i * 2 + 1] = elts[i].val;
            }
            ptr[arr->nelts * 2] = NULL;
            ptr[arr->nelts * 2 + 1] = NULL;

            if (global == RANGEDOMAIN) {
                errstring = process_domain(r, ptr, &errtype, cmd, domain);
            } else if (strcasecmp(cmd, "ENABLE-APP") == 0) {
                errstring = process_enable(r, ptr, &errtype, global);
            } else if (strcasecmp(cmd, "DISABLE-APP") == 0) {
                errstring = process_disable(r, ptr, &errtype, global);
            } else if (strcasecmp(cmd, "STOP-APP") == 0) {
                errstring = process_stop(r, ptr, &errtype, global, 0);
            } else if (strcasecmp(cmd, "REMOVE-APP") == 0) {
                errstring = process_remove(r, ptr, &errtype, global);
            } else {
                errstring = SCMDUNS;
                errtype = TYPESYNTAX;
            }
            if (errstring) {
                process_error(r, errstring, errtype);
            }
        }
    }

    ap_set_content_type(r, "text/html; charset=ISO-8859-1");
    ap_rputs(DOCTYPE_HTML_3_2 "<html><head>\n<title>Mod_cluster Status</title>\n</head><body>\n", r);
    ap_rvputs(r, "<h1>", MOD_CLUSTER_EXPOSED_VERSION, "</h1>", NULL);

    if (errstring) {
        ap_rvputs(r, "<h1> Command failed: ", errstring, "</h1>\n", NULL);
        ap_rvputs(r, " <a href=\"", r->uri, "\">Continue</a>\n", NULL);
        ap_rputs("</body></html>\n", r);
        return OK;
    }

    /* Advertise information */
    if (mconf->allow_display) {
        ap_rputs("start of \"httpd.conf\" configuration<br/>", r);
        modules_info(r);
        if (advertise_info != NULL) {
            advertise_info(r);
        }
        ap_rputs("end of \"httpd.conf\" configuration<br/><br/>", r);
    }

    ap_rvputs(r, "<a href=\"", r->uri, "?", balancer_nonce_string(r), "refresh=10", "\">Auto Refresh</a>", NULL);

    ap_rvputs(r, " <a href=\"", r->uri, "?", balancer_nonce_string(r), "Cmd=DUMP&Range=ALL", "\">show DUMP output</a>",
              NULL);

    ap_rvputs(r, " <a href=\"", r->uri, "?", balancer_nonce_string(r), "Cmd=INFO&Range=ALL", "\">show INFO output</a>",
              NULL);

    ap_rputs("\n", r);

    sizesessionid = loc_get_max_size_sessionid();

    size = loc_get_max_size_node();
    if (size == 0) {
        return OK;
    }
    id = apr_palloc(r->pool, sizeof(int) * size);
    size = get_ids_used_node(nodestatsmem, id);


    /* read the node to sort them by domain */
    nodes = apr_palloc(r->pool, sizeof(nodeinfo_t) * size);
    for (i = 0; i < size; i++) {
        nodeinfo_t *ou;
        if (get_node(nodestatsmem, &ou, id[i]) != APR_SUCCESS) {
            continue;
        }
        memcpy(&nodes[nbnodes], ou, sizeof(nodeinfo_t));
        nbnodes++;
    }
    sort_nodes(nodes, nbnodes);

    /* display the ordered nodes */
    for (i = 0; i < size; i++) {
        char *flushpackets;
        nodeinfo_t *ou = &nodes[i];

        if (strcmp(domain, ou->mess.Domain) != 0) {
            if (mconf->reduce_display) {
                ap_rprintf(r, "<br/><br/>LBGroup %.*s: ", (int)sizeof(ou->mess.Domain), ou->mess.Domain);
            } else {
                ap_rprintf(r, "<h1> LBGroup %.*s: ", (int)sizeof(ou->mess.Domain), ou->mess.Domain);
            }
            domain = ou->mess.Domain;
            if (mconf->allow_cmd) {
                domain_command_string(r, domain);
            }
            if (!mconf->reduce_display) {
                ap_rprintf(r, "</h1>\n");
            }
        }
        if (mconf->reduce_display) {
            ap_rprintf(r, "<br/><br/>Node %.*s ", (int)sizeof(ou->mess.JVMRoute), ou->mess.JVMRoute);
        } else {
            ap_rprintf(r, "<h1> Node %.*s (%.*s://%.*s:%.*s): </h1>\n", (int)sizeof(ou->mess.JVMRoute),
                       ou->mess.JVMRoute, (int)sizeof(ou->mess.Type), ou->mess.Type, (int)sizeof(ou->mess.Host),
                       ou->mess.Host, (int)sizeof(ou->mess.Port), ou->mess.Port);
        }
        if (mconf->reduce_display) {
            printproxy_stat(r, mconf->reduce_display, ou);
        }

        if (mconf->allow_cmd) {
            node_command_string(r, ou->mess.JVMRoute);
        }

        if (!mconf->reduce_display) {
            ap_rprintf(r, "<br/>\n");
            ap_rprintf(r, "Balancer: %.*s,LBGroup: %.*s", (int)sizeof(ou->mess.balancer), ou->mess.balancer,
                       (int)sizeof(ou->mess.Domain), ou->mess.Domain);

            flushpackets = "Off";
            switch (ou->mess.flushpackets) {
            case flush_on:
                flushpackets = "On";
                break;
            case flush_auto:
                flushpackets = "Auto";
            }
            ap_rprintf(r, ",Flushpackets: %s,Flushwait: %d,Ping: %d,Smax: %d,Ttl: %d", flushpackets, ou->mess.flushwait,
                       (int)ou->mess.ping, ou->mess.smax, (int)ou->mess.ttl);
        }

        if (mconf->reduce_display) {
            ap_rprintf(r, "<br/>\n");
        } else {
            printproxy_stat(r, mconf->reduce_display, ou);
        }

        if (sizesessionid) {
            ap_rprintf(r, ",Num sessions: %d", count_sessionid(r, ou->mess.JVMRoute));
        }
        ap_rprintf(r, "\n");

        /* Process the Vhosts */
        manager_info_hosts(r, mconf->reduce_display, mconf->allow_cmd, ou->mess.id, ou->mess.JVMRoute);
    }
    /* Display the sessions */
    if (sizesessionid) {
        manager_sessionid(r);
    }

    manager_domain(r, mconf->reduce_display);

    ap_rputs("</body></html>\n", r);
    return OK;
}

/* Process the requests from the ModClusterService */
static int manager_handler(request_rec *r)
{
    apr_bucket_brigade *input_brigade;
    char *errstring = NULL;
    int errtype = 0;
    char *buff;
    apr_size_t bufsiz = 0, maxbufsiz, len;
    apr_status_t status;
    int global = 0;
    int ours = 0;
    char **ptr;
    void *sconf = r->server->module_config;
    mod_manager_config *mconf;

    if (strcmp(r->handler, "mod_cluster-manager") == 0) {
        /* Display the nodes information */
        if (r->method_number != M_GET) {
            return DECLINED;
        }
        return manager_info(r);
    }

    mconf = ap_get_module_config(sconf, &manager_module);
    if (!mconf->enable_mcpm_receive) {
        return DECLINED; /* Not allowed to receive MCMP */
    }

    ours = check_method(r);
    if (!ours) {
        return DECLINED;
    }

    /* Use a buffer to read the message */
    if (mconf->maxmesssize) {
        maxbufsiz = mconf->maxmesssize;
    } else {
        /* we calculate it */
        maxbufsiz = 9 + JVMROUTESZ;
        maxbufsiz = maxbufsiz + (mconf->maxhost * HOSTALIASZ) + 7;
        maxbufsiz = maxbufsiz + (mconf->maxcontext * CONTEXTSZ) + 8;
    }
    if (maxbufsiz < MAXMESSSIZE) {
        maxbufsiz = MAXMESSSIZE;
    }
    buff = apr_pcalloc(r->pool, maxbufsiz);
    input_brigade = apr_brigade_create(r->pool, r->connection->bucket_alloc);
    len = maxbufsiz;
    while ((status = ap_get_brigade(r->input_filters, input_brigade, AP_MODE_READBYTES, APR_BLOCK_READ, len)) ==
           APR_SUCCESS) {
        apr_brigade_flatten(input_brigade, buff + bufsiz, &len);
        apr_brigade_cleanup(input_brigade);
        bufsiz += len;
        if (bufsiz >= maxbufsiz || len == 0) {
            break;
        }
        len = maxbufsiz - bufsiz;
    }

    if (status != APR_SUCCESS) {
        errstring = apr_psprintf(r->pool, SREADER, r->method);
        r->status_line = apr_psprintf(r->pool, "ERROR");
        apr_table_setn(r->err_headers_out, "Version", VERSION_PROTOCOL);
        apr_table_setn(r->err_headers_out, "Type", "SYNTAX");
        apr_table_setn(r->err_headers_out, "Mess", errstring);
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server, "manager_handler: %s error: %s", r->method, errstring);
        return 500;
    }
    buff[bufsiz] = '\0';

    /* XXX: Size limit it? */
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "manager_handler: %s (%s) processing: \"%s\"", r->method,
                 r->filename, buff);

    ptr = process_buff(r, buff);
    if (ptr == NULL) {
        process_error(r, SMESPAR, TYPESYNTAX);
        return 500;
    }
    if (strstr(r->filename, NODE_COMMAND)) {
        global = 1;
    }

    if (strcasecmp(r->method, "CONFIG") == 0) {
        errstring = process_config(r, ptr, &errtype);
    }
    /* Application handling */
    else if (strcasecmp(r->method, "ENABLE-APP") == 0) {
        errstring = process_enable(r, ptr, &errtype, global);
    } else if (strcasecmp(r->method, "DISABLE-APP") == 0) {
        errstring = process_disable(r, ptr, &errtype, global);
    } else if (strcasecmp(r->method, "STOP-APP") == 0) {
        errstring = process_stop(r, ptr, &errtype, global, 1);
    } else if (strcasecmp(r->method, "REMOVE-APP") == 0) {
        errstring = process_remove(r, ptr, &errtype, global);
    }
    /* Status handling */
    else if (strcasecmp(r->method, "STATUS") == 0) {
        errstring = process_status(r, (const char *const *)ptr, &errtype);
    } else if (strcasecmp(r->method, "DUMP") == 0) {
        errstring = process_dump(r, &errtype);
    } else if (strcasecmp(r->method, "INFO") == 0) {
        errstring = process_info(r, &errtype);
    } else if (strcasecmp(r->method, "PING") == 0) {
        errstring = process_ping(r, (const char *const *)ptr, &errtype);
    } else if (strcasecmp(r->method, "VERSION") == 0) {
        errstring = process_version(r, (const char *const *)ptr, &errtype);
    } else {
        errstring = SCMDUNS;
        errtype = TYPESYNTAX;
    }

    /* Check error string and build the error message */
    if (errstring) {
        process_error(r, errstring, errtype);
        return 500;
    }

    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, "manager_handler: %s  OK", r->method);

    ap_rflush(r);
    return OK;
}

/*
 * Attach to the shared memory when the child is created
 */
static void manager_child_init(apr_pool_t *p, server_rec *s)
{
    char *node;
    char *context;
    char *host;
    char *balancer;
    char *sessionid;
    mod_manager_config *mconf = ap_get_module_config(s->module_config, &manager_module);

    if (storage == NULL) {
        /* that happens when doing a gracefull restart for example after additing/changing the storage provider */
        ap_log_error(APLOG_MARK, APLOG_EMERG, 0, s, "Fatal storage provider not initialized");
        return;
    }

    if (apr_global_mutex_child_init(&node_mutex, apr_global_mutex_lockfile(node_mutex), p) != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_CRIT, 0, s, APLOGNO(02994) "Failed to reopen mutex %s in child",
                     node_mutex_type);
        exit(EXIT_FAILURE);
    }
    if (apr_global_mutex_child_init(&context_mutex, apr_global_mutex_lockfile(context_mutex), p) != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_CRIT, 0, s, APLOGNO(02994) "Failed to reopen mutex %s in child",
                     context_mutex_type);
        exit(EXIT_FAILURE);
    }

    mconf->tableversion = 0;

    if (mconf->basefilename) {
        node = apr_pstrcat(p, mconf->basefilename, "/manager.node", NULL);
        context = apr_pstrcat(p, mconf->basefilename, "/manager.context", NULL);
        host = apr_pstrcat(p, mconf->basefilename, "/manager.host", NULL);
        balancer = apr_pstrcat(p, mconf->basefilename, "/manager.balancer", NULL);
        sessionid = apr_pstrcat(p, mconf->basefilename, "/manager.sessionid", NULL);
    } else {
        node = ap_server_root_relative(p, "logs/manager.node");
        context = ap_server_root_relative(p, "logs/manager.context");
        host = ap_server_root_relative(p, "logs/manager.host");
        balancer = ap_server_root_relative(p, "logs/manager.balancer");
        sessionid = ap_server_root_relative(p, "logs/manager.sessionid");
    }

    nodestatsmem = get_mem_node(node, &mconf->maxnode, p, storage);
    if (nodestatsmem == NULL) {
        ap_log_error(APLOG_MARK, APLOG_EMERG, 0, s, "manager_child_init: get_mem_node %s failed", node);
        return;
    }
    if (get_last_mem_error(nodestatsmem) != APR_SUCCESS) {
        char buf[120];
        ap_log_error(APLOG_MARK, APLOG_EMERG, 0, s, "manager_child_init: get_mem_node %s failed: %s", node,
                     apr_strerror(get_last_mem_error(nodestatsmem), buf, sizeof(buf)));
        return;
    }

    contextstatsmem = get_mem_context(context, &mconf->maxcontext, p, storage);
    if (contextstatsmem == NULL) {
        ap_log_error(APLOG_MARK, APLOG_EMERG, 0, s, "manager_child_init: get_mem_context failed");
        return;
    }

    hoststatsmem = get_mem_host(host, &mconf->maxhost, p, storage);
    if (hoststatsmem == NULL) {
        ap_log_error(APLOG_MARK, APLOG_EMERG, 0, s, "manager_child_init: get_mem_host failed");
        return;
    }

    balancerstatsmem = get_mem_balancer(balancer, &mconf->maxhost, p, storage);
    if (balancerstatsmem == NULL) {
        ap_log_error(APLOG_MARK, APLOG_EMERG, 0, s, "manager_child_init: get_mem_balancer failed");
        return;
    }

    if (mconf->maxsessionid) {
        /*  Try to get sessionid stuff only if required */
        sessionidstatsmem = get_mem_sessionid(sessionid, &mconf->maxsessionid, p, storage);
        if (sessionidstatsmem == NULL) {
            ap_log_error(APLOG_MARK, APLOG_EMERG, 0, s, "manager_child_init: get_mem_sessionid failed");
            return;
        }
    }
}

/*
 * Supported directives
 */
static const char *cmd_manager_maxcontext(cmd_parms *cmd, void *mconfig, const char *word)
{
    mod_manager_config *mconf = ap_get_module_config(cmd->server->module_config, &manager_module);
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    (void)mconfig;

    if (err != NULL) {
        return err;
    }
    mconf->maxcontext = atoi(word);
    return NULL;
}

static const char *cmd_manager_maxnode(cmd_parms *cmd, void *mconfig, const char *word)
{
    mod_manager_config *mconf = ap_get_module_config(cmd->server->module_config, &manager_module);
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    (void)mconfig;

    if (err != NULL) {
        return err;
    }
    mconf->maxnode = atoi(word);
    return NULL;
}

static const char *cmd_manager_maxhost(cmd_parms *cmd, void *mconfig, const char *word)
{
    mod_manager_config *mconf = ap_get_module_config(cmd->server->module_config, &manager_module);
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    (void)mconfig;

    if (err != NULL) {
        return err;
    }
    mconf->maxhost = atoi(word);
    return NULL;
}

static const char *cmd_manager_maxsessionid(cmd_parms *cmd, void *mconfig, const char *word)
{
    mod_manager_config *mconf = ap_get_module_config(cmd->server->module_config, &manager_module);
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    (void)mconfig;

    if (err != NULL) {
        return err;
    }
    mconf->maxsessionid = atoi(word);
    return NULL;
}

static const char *cmd_manager_memmanagerfile(cmd_parms *cmd, void *mconfig, const char *word)
{
    mod_manager_config *mconf = ap_get_module_config(cmd->server->module_config, &manager_module);
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    (void)mconfig;

    if (err != NULL) {
        return err;
    }
    mconf->basefilename = ap_server_root_relative(cmd->pool, word);
    if (apr_dir_make_recursive(mconf->basefilename, APR_UREAD | APR_UWRITE | APR_UEXECUTE, cmd->pool) != APR_SUCCESS) {
        return "Can't create directory corresponding to MemManagerFile";
    }
    return NULL;
}

static const char *cmd_manager_balancername(cmd_parms *cmd, void *mconfig, const char *word)
{
    mod_manager_config *mconf = ap_get_module_config(cmd->server->module_config, &manager_module);
    mconf->balancername = apr_pstrdup(cmd->pool, word);
    normalize_balancer_name(mconf->balancername, cmd->server);
    (void)mconfig; /* unused variable */
    return NULL;
}

static const char *cmd_manager_pers(cmd_parms *cmd, void *dummy, const char *arg)
{
    mod_manager_config *mconf = ap_get_module_config(cmd->server->module_config, &manager_module);
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    (void)dummy;

    if (err != NULL) {
        return err;
    }
    if (strcasecmp(arg, "Off") == 0) {
        mconf->persistent = 0;
    } else if (strcasecmp(arg, "On") == 0) {
        mconf->persistent = AP_SLOTMEM_TYPE_PERSIST;
    } else {
        return "PersistSlots must be one of: "
               "off | on";
    }

    return NULL;
}

static const char *cmd_manager_nonce(cmd_parms *cmd, void *dummy, const char *arg)
{
    mod_manager_config *mconf = ap_get_module_config(cmd->server->module_config, &manager_module);
    (void)dummy;

    if (strcasecmp(arg, "Off") == 0) {
        mconf->nonce = 0;
    } else if (strcasecmp(arg, "On") == 0) {
        mconf->nonce = -1;
    } else {
        return "CheckNonce must be one of: "
               "off | on";
    }

    return NULL;
}

static const char *cmd_manager_allow_display(cmd_parms *cmd, void *dummy, const char *arg)
{
    mod_manager_config *mconf = ap_get_module_config(cmd->server->module_config, &manager_module);
    (void)dummy;

    if (strcasecmp(arg, "Off") == 0) {
        mconf->allow_display = 0;
    } else if (strcasecmp(arg, "On") == 0) {
        mconf->allow_display = -1;
    } else {
        return "AllowDisplay must be one of: "
               "off | on";
    }

    return NULL;
}

static const char *cmd_manager_allow_cmd(cmd_parms *cmd, void *dummy, const char *arg)
{
    mod_manager_config *mconf = ap_get_module_config(cmd->server->module_config, &manager_module);
    (void)dummy;

    if (strcasecmp(arg, "Off") == 0) {
        mconf->allow_cmd = 0;
    } else if (strcasecmp(arg, "On") == 0) {
        mconf->allow_cmd = -1;
    } else {
        return "AllowCmd must be one of: "
               "off | on";
    }

    return NULL;
}

static const char *cmd_manager_reduce_display(cmd_parms *cmd, void *dummy, const char *arg)
{
    mod_manager_config *mconf = ap_get_module_config(cmd->server->module_config, &manager_module);
    (void)dummy;

    if (strcasecmp(arg, "Off") == 0) {
        mconf->reduce_display = 0;
    } else if (strcasecmp(arg, "On") == 0) {
        mconf->reduce_display = 1;
    } else {
        return "ReduceDisplay must be one of: "
               "off | on";
    }

    return NULL;
}

static const char *cmd_manager_maxmesssize(cmd_parms *cmd, void *mconfig, const char *word)
{
    mod_manager_config *mconf = ap_get_module_config(cmd->server->module_config, &manager_module);
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    (void)mconfig;

    if (err != NULL) {
        return err;
    }
    mconf->maxmesssize = atoi(word);
    if (mconf->maxmesssize < MAXMESSSIZE) {
        return "MaxMCMPMessSize must bigger than 1024";
    }
    return NULL;
}

static const char *cmd_manager_enable_mcpm_receive(cmd_parms *cmd, void *dummy)
{
    mod_manager_config *mconf = ap_get_module_config(cmd->server->module_config, &manager_module);
    (void)dummy;

    if (!cmd->server->is_virtual) {
        return "EnableMCPMReceive must be in a VirtualHost";
    }
    mconf->enable_mcpm_receive = 1;
    return NULL;
}

static const char *cmd_manager_enable_ws_tunnel(cmd_parms *cmd, void *dummy)
{
    mod_manager_config *mconf = ap_get_module_config(cmd->server->module_config, &manager_module);
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    (void)dummy;

    if (err != NULL) {
        return err;
    }
    if (ap_find_linked_module("mod_proxy_http.c") != NULL) {
        mconf->enable_ws_tunnel = 1;
        return NULL;
    }

    return "EnableWsTunnel requires mod_proxy_http.c";
}

static const char *cmd_manager_ws_upgrade_header(cmd_parms *cmd, void *mconfig, const char *word)
{
    mod_manager_config *mconf = ap_get_module_config(cmd->server->module_config, &manager_module);
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    (void)mconfig;

    if (err != NULL) {
        return err;
    }
    if (strlen(word) >= PROXY_WORKER_MAX_SCHEME_SIZE) {
        return apr_psprintf(cmd->temp_pool, "upgrade protocol length must be < %d characters",
                            PROXY_WORKER_MAX_SCHEME_SIZE);
    }
    if (ap_find_linked_module("mod_proxy_http.c") != NULL) {
        mconf->enable_ws_tunnel = 1;
        mconf->ws_upgrade_header = apr_pstrdup(cmd->pool, word);
        return NULL;
    }

    return "WSUpgradeHeader requires mod_proxy_http.c";
}

static const char *cmd_manager_ajp_secret(cmd_parms *cmd, void *mconfig, const char *word)
{
    mod_manager_config *mconf = ap_get_module_config(cmd->server->module_config, &manager_module);
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    (void)mconfig;

    if (err != NULL) {
        return err;
    }
    if (strlen(word) >= PROXY_WORKER_MAX_SECRET_SIZE) {
        return apr_psprintf(cmd->temp_pool, "AJP secret length must be < %d characters", PROXY_WORKER_MAX_SECRET_SIZE);
    }
    if (ap_find_linked_module("mod_proxy_ajp.c") != NULL) {
        mconf->ajp_secret = apr_pstrdup(cmd->pool, word);
        return NULL;
    }

    return "AJPsecret requires mod_proxy_ajp.c";
}

static const char *cmd_manager_responsefieldsize(cmd_parms *cmd, void *mconfig, const char *word)
{
    mod_manager_config *mconf = ap_get_module_config(cmd->server->module_config, &manager_module);
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    long s = atol(word);
    (void)mconfig;

    if (err != NULL) {
        return err;
    }
    if (s < 0) {
        return "ResponseFieldSize must be greater than 0 bytes, or 0 for system default.";
    }
    if (ap_find_linked_module("mod_proxy_http.c") != NULL) {
        mconf->response_field_size = (s ? s : HUGE_STRING_LEN);
        return NULL;
    }

    return "ResponseFieldSize requires mod_proxy_http.c";
}


static const command_rec manager_cmds[] = {
    AP_INIT_TAKE1("Maxcontext", cmd_manager_maxcontext, NULL, OR_ALL,
                  "Maxcontext - number max context supported by mod_cluster"),
    AP_INIT_TAKE1("Maxnode", cmd_manager_maxnode, NULL, OR_ALL, "Maxnode - number max node supported by mod_cluster"),
    AP_INIT_TAKE1("Maxhost", cmd_manager_maxhost, NULL, OR_ALL,
                  "Maxhost - number max host (Alias in virtual hosts) supported by mod_cluster"),
    AP_INIT_TAKE1(
        "Maxsessionid", cmd_manager_maxsessionid, NULL, OR_ALL,
        "Maxsessionid - number session (Used to track number of sessions per nodes) supported by mod_cluster"),
    AP_INIT_TAKE1("MemManagerFile", cmd_manager_memmanagerfile, NULL, OR_ALL,
                  "MemManagerFile - base name of the files used to create/attach to shared memory"),
    AP_INIT_TAKE1("ManagerBalancerName", cmd_manager_balancername, NULL, OR_ALL,
                  "ManagerBalancerName - name of a balancer corresponding to the manager"),
    AP_INIT_TAKE1("PersistSlots", cmd_manager_pers, NULL, OR_ALL,
                  "PersistSlots - Persist the slot mem elements on | off (Default: off No persistence)"),
    AP_INIT_TAKE1("CheckNonce", cmd_manager_nonce, NULL, OR_ALL,
                  "CheckNonce - Switch check of nonce when using mod_cluster-manager handler on | off (Default: on "
                  "Nonce checked)"),
    AP_INIT_TAKE1("AllowDisplay", cmd_manager_allow_display, NULL, OR_ALL,
                  "AllowDisplay - Display additional information in the mod_cluster-manager page on | off (Default: "
                  "off Only version displayed)"),
    AP_INIT_TAKE1("AllowCmd", cmd_manager_allow_cmd, NULL, OR_ALL,
                  "AllowCmd - Allow commands using mod_cluster-manager URL on | off (Default: on Commmands allowed)"),
    AP_INIT_TAKE1("ReduceDisplay", cmd_manager_reduce_display, NULL, OR_ALL,
                  "ReduceDisplay - Don't contexts in the main mod_cluster-manager page. on | off (Default: off Context "
                  "displayed)"),
    AP_INIT_TAKE1("MaxMCMPMessSize", cmd_manager_maxmesssize, NULL, OR_ALL,
                  "MaxMCMPMaxMessSize - Maximum size of MCMP messages. (Default: calculated min value: 1024)"),
    AP_INIT_NO_ARGS("EnableMCPMReceive", cmd_manager_enable_mcpm_receive, NULL, OR_ALL,
                    "EnableMCPMReceive - Allow the VirtualHost to receive MCPM."),
    AP_INIT_NO_ARGS(
        "EnableWsTunnel", cmd_manager_enable_ws_tunnel, NULL, OR_ALL,
        "EnableWsTunnel - Use ws or wss instead of http or https when creating nodes (allows WebSocket proxying)."),
    AP_INIT_TAKE1(
        "WSUpgradeHeader", cmd_manager_ws_upgrade_header, NULL, OR_ALL,
        "WSUpgradeHeader - Accept http upgrade headers. Values: WebSocket or * to use any supplied by a client."),
    AP_INIT_TAKE1("AJPSecret", cmd_manager_ajp_secret, NULL, OR_ALL,
                  "AJPSecret - secret for all mod_cluster node, not configued no secret."),
    AP_INIT_TAKE1("ResponseFieldSize", cmd_manager_responsefieldsize, NULL, OR_ALL,
                  "ResponseFieldSize - Adjust the size of the proxy response field buffer."),
    {NULL},
};

/* hooks declaration */

static void manager_hooks(apr_pool_t *p)
{
    static const char *const aszSucc[] = {"mod_proxy.c", NULL};

    /* For the lock */
    ap_hook_pre_config(manager_pre_config, NULL, NULL, APR_HOOK_MIDDLE);

    /* Create the shared tables for mod_proxy_cluster */
    ap_hook_post_config(manager_init, NULL, NULL, APR_HOOK_MIDDLE);

    /* Attach to the shared tables with create the child */
    ap_hook_child_init(manager_child_init, NULL, NULL, APR_HOOK_FIRST);

    /* post read_request handling: to be handle to use ProxyPass / */
    ap_hook_translate_name(manager_trans, NULL, aszSucc, APR_HOOK_FIRST);

    /* Process the request from the ModClusterService */
    ap_hook_handler(manager_handler, NULL, NULL, APR_HOOK_REALLY_FIRST);

    /* prevent subrequest to map our / or what ever is send with our methods. */
    ap_hook_map_to_storage(manager_map_to_storage, NULL, NULL, APR_HOOK_REALLY_FIRST);

    /* Register nodes/hosts/contexts table provider */
    ap_register_provider(p, "manager", "shared", "0", &node_storage);
    ap_register_provider(p, "manager", "shared", "1", &host_storage);
    ap_register_provider(p, "manager", "shared", "2", &context_storage);
    ap_register_provider(p, "manager", "shared", "3", &balancer_storage);
    ap_register_provider(p, "manager", "shared", "4", &sessionid_storage);
    ap_register_provider(p, "manager", "shared", "5", &domain_storage);
}

/*
 * Config creation stuff
 */
static void *create_manager_config(apr_pool_t *p)
{
    mod_manager_config *mconf = apr_pcalloc(p, sizeof(*mconf));

    mconf->basefilename = NULL;
    mconf->maxcontext = DEFMAXCONTEXT;
    mconf->maxnode = DEFMAXNODE;
    mconf->maxhost = DEFMAXHOST;
    mconf->maxsessionid = DEFMAXSESSIONID;
    mconf->tableversion = 0;
    mconf->persistent = 0;
    mconf->nonce = -1;
    mconf->balancername = NULL;
    mconf->allow_display = 0;
    mconf->allow_cmd = -1;
    mconf->reduce_display = 0;
    mconf->enable_mcpm_receive = 0;
    mconf->enable_ws_tunnel = 0;
    mconf->ws_upgrade_header = NULL;
    mconf->ajp_secret = NULL;
    mconf->response_field_size = 0;
    return mconf;
}

static void *create_manager_server_config(apr_pool_t *p, server_rec *s)
{
    (void)s;
    return create_manager_config(p);
}

static void *merge_manager_server_config(apr_pool_t *p, void *server1_conf, void *server2_conf)
{
    mod_manager_config *mconf1 = (mod_manager_config *)server1_conf;
    mod_manager_config *mconf2 = (mod_manager_config *)server2_conf;
    mod_manager_config *mconf = (mod_manager_config *)create_manager_config(p);

    if (mconf2->basefilename) {
        mconf->basefilename = apr_pstrdup(p, mconf2->basefilename);
    } else if (mconf1->basefilename) {
        mconf->basefilename = apr_pstrdup(p, mconf1->basefilename);
    }

    if (mconf2->maxcontext != DEFMAXCONTEXT) {
        mconf->maxcontext = mconf2->maxcontext;
    } else if (mconf1->maxcontext != DEFMAXCONTEXT) {
        mconf->maxcontext = mconf1->maxcontext;
    }

    if (mconf2->maxnode != DEFMAXNODE) {
        mconf->maxnode = mconf2->maxnode;
    } else if (mconf1->maxnode != DEFMAXNODE) {
        mconf->maxnode = mconf1->maxnode;
    }

    if (mconf2->maxhost != DEFMAXHOST) {
        mconf->maxhost = mconf2->maxhost;
    } else if (mconf1->maxhost != DEFMAXHOST) {
        mconf->maxhost = mconf1->maxhost;
    }

    if (mconf2->maxsessionid != DEFMAXSESSIONID) {
        mconf->maxsessionid = mconf2->maxsessionid;
    } else if (mconf1->maxsessionid != DEFMAXSESSIONID) {
        mconf->maxsessionid = mconf1->maxsessionid;
    }

    if (mconf2->persistent != 0) {
        mconf->persistent = mconf2->persistent;
    } else if (mconf1->persistent != 0) {
        mconf->persistent = mconf1->persistent;
    }

    if (mconf2->nonce != -1) {
        mconf->nonce = mconf2->nonce;
    } else if (mconf1->nonce != -1) {
        mconf->nonce = mconf1->nonce;
    }

    if (mconf2->balancername) {
        mconf->balancername = apr_pstrdup(p, mconf2->balancername);
    } else if (mconf1->balancername) {
        mconf->balancername = apr_pstrdup(p, mconf1->balancername);
    }

    if (mconf2->allow_display != 0) {
        mconf->allow_display = mconf2->allow_display;
    } else if (mconf1->allow_display != 0) {
        mconf->allow_display = mconf1->allow_display;
    }

    if (mconf2->allow_cmd != -1) {
        mconf->allow_cmd = mconf2->allow_cmd;
    } else if (mconf1->allow_cmd != -1) {
        mconf->allow_cmd = mconf1->allow_cmd;
    }

    if (mconf2->reduce_display != 0) {
        mconf->reduce_display = mconf2->reduce_display;
    } else if (mconf1->reduce_display != 0) {
        mconf->reduce_display = mconf1->reduce_display;
    }

    if (mconf2->enable_mcpm_receive != 0) {
        mconf->enable_mcpm_receive = mconf2->enable_mcpm_receive;
    } else if (mconf1->enable_mcpm_receive != 0) {
        mconf->enable_mcpm_receive = mconf1->enable_mcpm_receive;
    }

    if (mconf2->enable_ws_tunnel != 0) {
        mconf->enable_ws_tunnel = mconf2->enable_ws_tunnel;
    } else if (mconf1->enable_ws_tunnel != 0) {
        mconf->enable_ws_tunnel = mconf1->enable_ws_tunnel;
    }

    if (mconf2->ws_upgrade_header) {
        mconf->ws_upgrade_header = apr_pstrdup(p, mconf2->ws_upgrade_header);
    } else if (mconf1->ws_upgrade_header) {
        mconf->ws_upgrade_header = apr_pstrdup(p, mconf1->ws_upgrade_header);
    }

    if (mconf2->ajp_secret) {
        mconf->ajp_secret = apr_pstrdup(p, mconf2->ajp_secret);
    } else if (mconf1->ajp_secret) {
        mconf->ajp_secret = apr_pstrdup(p, mconf1->ajp_secret);
    }

    if (mconf2->response_field_size) {
        mconf->response_field_size = mconf2->response_field_size;
    } else if (mconf1->response_field_size) {
        mconf->response_field_size = mconf1->response_field_size;
    }

    return mconf;
}

/* Module declaration */

module AP_MODULE_DECLARE_DATA manager_module = {
    STANDARD20_MODULE_STUFF,
    NULL,
    NULL,
    create_manager_server_config,
    merge_manager_server_config,
    manager_cmds,        /* command table */
    manager_hooks,       /* register hooks */
    AP_MODULE_FLAG_NONE, /* flags */
};

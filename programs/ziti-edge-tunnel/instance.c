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

#include "model/dtos.h"
#include <ziti/ziti_log.h>
#include <time.h>
#include "lwip/ip_addr.h"
#include "ziti/ziti_tunnel.h"
#include "identity-utils.h"

#define MIN_API_PAGESIZE 10
#define DEFAULT_API_PAGESIZE 25
#if _WIN32
    #ifndef PATH_MAX //normalize to PATH_MAX even on vs 2022 and arm
        #ifdef _MAX_PATH
            #define PATH_MAX _MAX_PATH // https://learn.microsoft.com/en-us/cpp/c-runtime-library/path-field-limits
        #else
            #define PATH_MAX 260 // this should not be necessary...
        #endif
    #endif
#endif

model_map tnl_identity_map = {0};
static const char* CFG_INTERCEPT_V1 = "intercept.v1";
static const char* CFG_HOST_V1 = "host.v1";
static const char* CFG_ZITI_TUNNELER_CLIENT_V1 = "ziti-tunneler-client.v1";
static tunnel_status tnl_status = {0};
extern char *config_dir;

tunnel_identity *find_tunnel_identity(const char* identifier) {
    char* normalized_identifier = strdup(identifier);
    normalize_identifier(normalized_identifier);
    tunnel_identity *tnl_id = model_map_get(&tnl_identity_map, normalized_identifier);
    free(normalized_identifier);
    if (tnl_id != NULL) {
        return tnl_id;
    } else {
        ZITI_LOG(WARN, "Identity ztx[%s] is not loaded yet or already removed.", identifier);
        return NULL;
    }
}

/**
 * file name will be passed to this function, if this called by the load identities function.
 * file name will be null, if this is called any other time
 */
tunnel_identity *create_or_get_tunnel_identity(const char* identifier, const char* filename) {
    tunnel_identity *id = find_tunnel_identity(identifier);

    if (id != NULL) {
        return id;
    } else {
        tunnel_identity *tnl_id = calloc(1, sizeof(struct tunnel_identity_s));
        char* normalized_identifier = strdup(identifier);
        normalize_identifier(normalized_identifier);
        tnl_id->Identifier = normalized_identifier;
        if (filename != NULL) {
            char* extension = strstr(filename, ".json");

            size_t length;
            if (extension != NULL) {
                length = extension - filename;
            } else {
                length = strlen(filename);
            }
            char *fingerprint = calloc(length + 1, sizeof(char));
            snprintf(fingerprint, length+1, "%s", filename);
            tnl_id->FingerPrint = fingerprint;

            char *name = calloc(length + 1, sizeof(char));
            snprintf(name, length+1, "%s", fingerprint);
            tnl_id->Name = name;

            tnl_id->Active = true;
        }
        model_map_set(&tnl_identity_map, normalized_identifier, tnl_id);
        return tnl_id;
    }
}

void set_mfa_timeout(tunnel_identity *tnl_id) {
    if (tnl_id->Services != NULL) {
        int mfa_min_timeout = -1;
        int mfa_min_timeout_rem = -1;
        int mfa_max_timeout = -1;
        int mfa_max_timeout_rem = -1;
        bool no_timeout_svc = false;
        bool no_timeout_svc_rem = false;
        for (int svc_idx = 0; tnl_id->Services[svc_idx] != 0; svc_idx++) {
            tunnel_service *tnl_svc = tnl_id->Services[svc_idx];

            if (tnl_svc->Timeout > -1) {
                if (mfa_min_timeout == -1 || mfa_min_timeout > tnl_svc->Timeout) {
                    mfa_min_timeout = (int)tnl_svc->Timeout;
                }
                if (mfa_max_timeout == -1 || mfa_max_timeout < tnl_svc->Timeout) {
                    mfa_max_timeout = (int)tnl_svc->Timeout;
                }
            } else {
                no_timeout_svc = true;
            }
            if (tnl_svc->TimeoutRemaining > -1) {
                if (mfa_min_timeout_rem == -1 || mfa_min_timeout_rem > tnl_svc->TimeoutRemaining) {
                    mfa_min_timeout_rem = (int)tnl_svc->TimeoutRemaining;
                }
                if (mfa_max_timeout_rem == -1 || mfa_max_timeout_rem < tnl_svc->TimeoutRemaining) {
                    mfa_max_timeout_rem = (int)tnl_svc->TimeoutRemaining;
                }
            } else {
                no_timeout_svc_rem = true;
            }

        }
        if (no_timeout_svc) {
            mfa_max_timeout = -1;
        }
        if (no_timeout_svc_rem) {
            mfa_max_timeout_rem = -1;
        }

        tnl_id->MfaMaxTimeout = mfa_max_timeout;
        tnl_id->MfaMaxTimeoutRem = mfa_max_timeout_rem;
        tnl_id->MaxTimeoutRemInSvcEvent = mfa_max_timeout_rem;
        tnl_id->MfaMinTimeout = mfa_min_timeout;
        tnl_id->MfaMinTimeoutRem = mfa_min_timeout_rem;
        tnl_id->MinTimeoutRemInSvcEvent = mfa_min_timeout_rem;
    }

}

void add_or_remove_services_from_tunnel(tunnel_identity *id, tunnel_service_array added_services, tunnel_service_array removed_services) {

    int idx;
    model_map updates = {0};

    // add services from tunnel id to map
    if (id->Services != NULL) {
        for (idx =0; id->Services[idx]; idx++) {
            tunnel_service *svc = id->Services[idx];
            model_map_set(&updates, svc->Name, svc);
        }
    }

    // remove services from map
    if (removed_services != NULL) {
        for(idx=0; removed_services[idx]; idx++){
            tunnel_service *svc = removed_services[idx];
            tunnel_service *rem_svc = model_map_get(&updates, svc->Name);
            if (rem_svc != NULL) {
                model_map_remove(&updates, rem_svc->Name);
            }
        }
    }

    //add services to map
    if (added_services != NULL) {
        for(idx=0; added_services[idx]; idx++){
            tunnel_service *svc = added_services[idx];
            model_map_set(&updates, svc->Name, svc);
        }
    }

    // reallocate when new event comes, we need to maintain the whole list of services in tunnel_identity
    if (id->Services == NULL) {
        id->Services = calloc(model_map_size(&updates) + 1, sizeof(struct tunnel_service_s));
    } else {
        free(id->Services);
        id->Services = calloc(model_map_size(&updates) + 1, sizeof(struct tunnel_service_s));
    }
    model_map_iter it = model_map_iterator(&updates);
    idx=0;
    while(it != NULL) {
        id->Services[idx++] = model_map_it_value(it);
        it = model_map_it_remove(it);
    }

    model_map_clear(&updates, NULL);
    set_mfa_timeout(id);
    uv_timeval64_t now;
    uv_gettimeofday(&now);
    if (id->ServiceUpdatedTime == NULL) {
        id->ServiceUpdatedTime = malloc(sizeof(timestamp));
    }
    id->ServiceUpdatedTime->tv_sec = now.tv_sec;
    id->ServiceUpdatedTime->tv_usec = now.tv_usec;

}

static tunnel_posture_check *getTunnelPostureCheck(ziti_posture_query *pq){
    tunnel_posture_check *pc = calloc(1, sizeof(struct tunnel_posture_check_s));
    pc->Id = strdup(pq->id);
    pc->IsPassing = pq->is_passing;
    pc->QueryType = strdup(ziti_posture_query_types.name(pq->query_type));
    pc->Timeout = pq->timeout;
    pc->TimeoutRemaining = *pq->timeoutRemaining;
    return pc;
}

static void setTunnelPostureDataTimeout(tunnel_service *tnl_svc, ziti_service *service) {
    int minTimeoutRemaining = -1;
    int minTimeout = -1;
    bool hasAccess = false;
    model_map postureCheckMap = {0};

    ziti_posture_query_set *pqs;
    const char *key;
    MODEL_MAP_FOREACH(key, pqs, &service->posture_query_map) {

        if (strcmp(pqs->policy_type, "Bind") == 0) {
            ZITI_LOG(TRACE, "Posture Query set returned a Bind policy: %s [ignored]", pqs->policy_id);
            continue;
        } else {
            ZITI_LOG(TRACE, "Posture Query set returned a %s policy: %s, is_passing %d", pqs->policy_type, pqs->policy_id, pqs->is_passing);
        }

        if (pqs->is_passing) {
            hasAccess = true;
        }

        for (int posture_query_idx = 0; pqs->posture_queries[posture_query_idx]; posture_query_idx++) {
            ziti_posture_query *pq = pqs->posture_queries[posture_query_idx];
            ziti_posture_query *tmp = model_map_get(&postureCheckMap, pq->id);
            if (tmp == NULL) {
                model_map_set(&postureCheckMap, pq->id, pq);
            }

            int timeoutRemaining = (int)*pqs->posture_queries[posture_query_idx]->timeoutRemaining;
            if ((minTimeoutRemaining == -1) || (timeoutRemaining < minTimeoutRemaining)) {
                minTimeoutRemaining = timeoutRemaining;
            }

            int timeout = (int)pqs->posture_queries[posture_query_idx]->timeout;
            if ((minTimeout == -1) || (timeout < minTimeout)) {
                minTimeout = timeout;
            }
        }
    }

    if (model_map_size(&postureCheckMap) > 0) {
        int idx = 0;
        tnl_svc->PostureChecks = calloc(model_map_size(&postureCheckMap) + 1, sizeof(struct tunnel_posture_check_s));
        model_map_iter itr = model_map_iterator(&postureCheckMap);
        while (itr != NULL){
            ziti_posture_query *pq = model_map_it_value(itr);
            tunnel_posture_check *pc = getTunnelPostureCheck(pq);
            tnl_svc->PostureChecks[idx++] = pc;
            itr = model_map_it_remove(itr);
        }
    }
    model_map_clear(&postureCheckMap, NULL);

    tnl_svc->IsAccessible = hasAccess;
    tnl_svc->Timeout = minTimeout;
    tnl_svc->TimeoutRemaining = minTimeoutRemaining;
    ZITI_LOG(DEBUG, "service[%s] timeout=%d timeoutRemaining=%d", service->name, minTimeout, minTimeoutRemaining);
}

static tunnel_address *to_address(const ziti_address *za) {
    tunnel_address *tnl_address = calloc(1, sizeof(struct tunnel_address_s));

    if (za->type == ziti_address_cidr) {
        tnl_address->IsHost = false;
        tnl_address->HostName = NULL;
        char *ip = calloc(INET_ADDRSTRLEN + 1, sizeof(char));
        tnl_address->IP = ip;
        uv_inet_ntop(za->addr.cidr.af, &za->addr.cidr.ip, ip, INET_ADDRSTRLEN);
        tnl_address->Prefix = (int) za->addr.cidr.bits;
        ZITI_LOG(TRACE, "IP address: %s", tnl_address->IP);
    } else {
        tnl_address->IsHost = true;
        tnl_address->IP = NULL;
        tnl_address->HostName = strdup(za->addr.hostname);
        ZITI_LOG(TRACE, "Hostname: %s", tnl_address->HostName);
    }
    // find CIDR
    return tnl_address;
}

tunnel_port_range *getTunnelPortRange(ziti_port_range *zpr){
    tunnel_port_range *tpr = calloc(1, sizeof(struct tunnel_port_range_s));
    tpr->High = zpr->high;
    tpr->Low = zpr->low;
    return tpr;
}

static void setTunnelAllowedSourceAddress(tunnel_service *tnl_svc, ziti_service *service) {
    const char* cfg_json = ziti_service_get_raw_config(service, CFG_HOST_V1);
    tunnel_address_array allowed_src_addr_arr = NULL;
    if (cfg_json != NULL && strlen(cfg_json) > 0) {
        ZITI_LOG(TRACE, "host.v1: %s", cfg_json);
        ziti_host_cfg_v1 cfg_v1 = {0};
        parse_ziti_host_cfg_v1(&cfg_v1, cfg_json, strlen(cfg_json));
        size_t n = 0;
        int j = 0;
        ziti_address_array allowed_src_addrs = cfg_v1.allowed_source_addresses;
        for (int x = 0; allowed_src_addrs != NULL && allowed_src_addrs[x] != NULL; x++) {
            if (allowed_src_addrs[x]->type == ziti_address_cidr) {
                n++;
            }
        }
        allowed_src_addr_arr = calloc(n + 1, sizeof(tunnel_address *));
        for (int i = 0; allowed_src_addrs != NULL && allowed_src_addrs[i] != NULL; i++) {
            if (allowed_src_addrs[i]->type != ziti_address_cidr) {
                    if (allowed_src_addrs[i]->type == ziti_address_hostname) {
                        ZITI_LOG(ERROR, "hosted_service[%s] cannot use hostname '%s' as `allowed_source_address`",
                                 tnl_svc->Name, allowed_src_addrs[i]->addr.hostname);
                    } else {
                        ZITI_LOG(ERROR, "unknown ziti_address type %d", allowed_src_addrs[i]->type);
                    }
                    continue;
            }
            else{
                allowed_src_addr_arr[j] = to_address(allowed_src_addrs[i]);
                j++;
            }
        }
        free_ziti_host_cfg_v1(&cfg_v1);
        if (allowed_src_addr_arr != NULL) {
            tnl_svc->AllowedSourceAddresses = allowed_src_addr_arr;
        }
    }
}

static void setTunnelServiceAddress(tunnel_service *tnl_svc, ziti_service *service) {
    const char* cfg_json = ziti_service_get_raw_config(service, CFG_INTERCEPT_V1);
    tunnel_address_array tnl_addr_arr = NULL;
    model_string_array protocols = NULL;
    tunnel_port_range_array tnl_port_range_arr;
    if (cfg_json != NULL && strlen(cfg_json) > 0) {
        ZITI_LOG(TRACE, "intercept.v1: %s", cfg_json);
        ziti_intercept_cfg_v1 cfg_v1 = {0};
        parse_ziti_intercept_cfg_v1(&cfg_v1, cfg_json, strlen(cfg_json));
        // set address
        size_t n = model_list_size(&cfg_v1.addresses);
        tnl_addr_arr = calloc(n+1, sizeof(tunnel_address *));
        const ziti_address *zaddr;
        int i = 0;
        MODEL_LIST_FOREACH(zaddr, cfg_v1.addresses) {
            tnl_addr_arr[i++] = to_address(zaddr);
        }

        // set protocols
        n = model_list_size(&cfg_v1.protocols);
        protocols = calloc(n+1, sizeof(char*));
        ziti_protocol *proto;
        i = 0;
        MODEL_LIST_FOREACH(proto, cfg_v1.protocols) {
            protocols[i++] = strdup(ziti_protocols.name(*proto));
        }

        // set ports
        n = model_list_size(&cfg_v1.port_ranges);
        tnl_port_range_arr = calloc(n+1, sizeof(tunnel_port_range *));
        ziti_port_range *pr;
        i = 0;
        MODEL_LIST_FOREACH(pr, cfg_v1.port_ranges) {
            tnl_port_range_arr[i++] = getTunnelPortRange(pr);
        }

        free_ziti_intercept_cfg_v1(&cfg_v1);
    }  else if ((cfg_json = ziti_service_get_raw_config(service, CFG_ZITI_TUNNELER_CLIENT_V1)) != NULL) {
        ZITI_LOG(TRACE, "ziti-tunneler-client.v1: %s", cfg_json);
        ziti_client_cfg_v1 zt_client_cfg_v1;
        parse_ziti_client_cfg_v1(&zt_client_cfg_v1, cfg_json, strlen(cfg_json));

        // set tunnel address
        tnl_addr_arr = calloc(2, sizeof(tunnel_address *));
        tnl_addr_arr[0] = to_address(&zt_client_cfg_v1.hostname);

        // set protocols
        protocols = calloc(3, sizeof(char *));
        int idx = 0;
        protocols[idx++] = strdup("tcp");
        protocols[idx] = strdup("udp");

        // set port range
        // set ports
        tnl_port_range_arr = calloc(2, sizeof(tunnel_port_range *));
        tunnel_port_range *tpr = calloc(1, sizeof(tunnel_port_range));
        tpr->Low = zt_client_cfg_v1.port;
        tpr->High = zt_client_cfg_v1.port;
        tnl_port_range_arr[0] = tpr;

        free_ziti_client_cfg_v1(&zt_client_cfg_v1);
    }
    if (tnl_addr_arr != NULL) {
        tnl_svc->Addresses = tnl_addr_arr;
        tnl_svc->Ports = tnl_port_range_arr;
    }

    tnl_svc->Protocols = protocols;
}

tunnel_service *find_tunnel_service(tunnel_identity* id, const char* svc_id) {
    int idx = 0;
    tunnel_service *svc = NULL;
    if (id->Services != NULL) {
        for (idx =0; id->Services[idx]; idx++) {
            svc = id->Services[idx];
            if (strcmp(svc->Id, svc_id) == 0) {
                return svc;
            }
        }
    }
    return NULL;
}

tunnel_service *get_tunnel_service(tunnel_identity* id, ziti_service* zs) {
    struct tunnel_service_s *svc = calloc(1, sizeof(struct tunnel_service_s));
    svc->Id = strdup(zs->id);
    svc->Name = strdup(zs->name);
    svc->PostureChecks = NULL;
    svc->OwnsIntercept = true;
    svc->Permissions.Bind = ziti_service_has_permission(zs, ziti_session_type_Bind);
    svc->Permissions.Dial = ziti_service_has_permission(zs, ziti_session_type_Dial);
    setTunnelPostureDataTimeout(svc, zs);
    setTunnelServiceAddress(svc, zs);
    setTunnelAllowedSourceAddress(svc, zs);
    return svc;
}

tunnel_identity_array get_tunnel_identities() {
    const char *id;
    tunnel_identity *tnl_id;
    tunnel_identity_array tnl_id_arr = calloc(model_map_size(&tnl_identity_map) + 1, sizeof(tunnel_identity*));

    int idx = 0;
    MODEL_MAP_FOREACH(id, tnl_id, &tnl_identity_map) {
        tnl_id_arr[idx++] = tnl_id;
    }

    return tnl_id_arr;
}

tunnel_identity_array get_tunnel_identities_for_metrics() {

    tunnel_identity_array arr = get_tunnel_identities();
    tunnel_identity_array tnl_id_arr = NULL;

    for (int i =0; arr[i]; i++) {
        if (i == 0) {
            tnl_id_arr = calloc(model_map_size(&tnl_identity_map) + 1, sizeof(tunnel_identity*));
        }

        tunnel_identity* id = arr[i];
        tunnel_identity *id_new = calloc(1, sizeof(tunnel_identity));
        id_new->Identifier = id->Identifier;
        id_new->FingerPrint = id->FingerPrint;
        id_new->Name = id->Name;
        id_new->Active = id->Active;
        id_new->Loaded = id->Loaded;
        id_new->Metrics = id->Metrics;
        tnl_id_arr[i] = id_new;
    }
    free(arr);

    return tnl_id_arr;

}

int get_remaining_timeout(int timeout, int timeout_rem, tunnel_identity *tnl_id) {

    if (timeout <= 0 || timeout_rem <= 0 || tnl_id->MfaLastUpdatedTime == NULL || tnl_id->ServiceUpdatedTime == NULL) {
        return timeout_rem;
    }

    int timeout_remaining = 0;
    uv_timeval64_t now;
    uv_gettimeofday(&now);

    // calculate effective timeout remaining from last mfa or service update time
    if (tnl_id->MfaLastUpdatedTime->tv_sec > tnl_id->ServiceUpdatedTime->tv_sec) {
        //calculate svc remaining timeout
        int elapsed_time = now.tv_sec - tnl_id->MfaLastUpdatedTime->tv_sec;
        if ((timeout - elapsed_time) < 0) {
            timeout_remaining = 0;
        } else {
            timeout_remaining = timeout - elapsed_time;
        }
    } else {
        //calculate svc remaining timeout
        int elapsed_time = now.tv_sec - tnl_id->ServiceUpdatedTime->tv_sec;
        if ((timeout_rem - elapsed_time) < 0) {
            timeout_remaining = 0;
        } else {
            timeout_remaining = timeout_rem - elapsed_time;
        }
    }
    return timeout_remaining;
}

void set_mfa_timeout_rem(tunnel_identity *tnl_id) {

    if ((tnl_id->MfaMinTimeoutRem > -1 || tnl_id->MfaMaxTimeoutRem > -1) && tnl_id->Services != NULL ) {
        for (int svc_idx = 0 ; tnl_id->Services[svc_idx]; svc_idx++ ) {
            tunnel_service *tnl_svc = tnl_id->Services[svc_idx];
            int svc_timeout = -1;
            int svc_timeout_rem = -1;
            if (tnl_svc->TimeoutRemaining > -1 && tnl_svc->PostureChecks != NULL ) {
                // fetch service timeout and timeout remaining from the posture checks
                for (int pc_idx = 0; tnl_svc->PostureChecks[pc_idx]; pc_idx++) {
                    tunnel_posture_check *pc = tnl_svc->PostureChecks[pc_idx];
                    if (svc_timeout == -1 || svc_timeout > pc->Timeout) {
                        svc_timeout = pc->Timeout;
                    }
                    if (svc_timeout_rem == -1 || svc_timeout_rem > pc->TimeoutRemaining) {
                        svc_timeout_rem = pc->TimeoutRemaining;
                    }
                }

                tnl_svc->TimeoutRemaining = get_remaining_timeout(svc_timeout, svc_timeout_rem, tnl_id);
            }
        }

        if (tnl_id->MfaMinTimeoutRem > -1) {
            tnl_id->MfaMinTimeoutRem = get_remaining_timeout(tnl_id->MfaMinTimeout, tnl_id->MinTimeoutRemInSvcEvent, tnl_id);
        }
        if (tnl_id->MfaMaxTimeoutRem > -1) {
            tnl_id->MfaMaxTimeoutRem = get_remaining_timeout(tnl_id->MfaMaxTimeout, tnl_id->MaxTimeoutRemInSvcEvent, tnl_id);
        }
        if (tnl_id->MfaMaxTimeoutRem == 0 && tnl_id->MfaEnabled ) {
            tnl_id->MfaNeeded = true;
        }
    }

}

void remove_duplicate_path_separators(char *str, char target) {
    if (str == NULL || *str == '\0') return;

    char *write = str; // where to write the next char to
    char *next = str; // the next char in the string to check

    while (*next) {
        *write = *next; //copy the next char to the write position
        if (*next == target) {
            while (*(next + 1) == target) { // keep reading until no longer matching
                next++;
            }
        }
        write++;
        next++;
    }
    *write = '\0';
}

void normalize_identifier(char *str) {
    char* init_pos = str;
#if _WIN32
    // this is only fine because windows doesn't allow slashes in file/directory names so any `/` should be converted to `\`
    char find = '/';
    char replace = PATH_SEP;
    if (str == NULL) return;

    for (; *str != '\0'; str++) {
        if (*str == find) {
            *str = replace;
        } else {
            *str = (char)tolower((unsigned char)*str); // Convert to lowercase when on windows
        }
    }
#else
    // nothing to normalize at this time, fall through to remove duplicate path separators
#endif
    remove_duplicate_path_separators(init_pos, PATH_SEP);
}

/*
 * while loading data from the config file that is generated by WDE, Identifier will be empty and Fingerprint will be present
 */
void set_identifier_from_identities() {
    if (tnl_status.Identities == NULL) {
        return;
    }
    for(int idx = 0; tnl_status.Identities[idx]; idx++) {
        tunnel_identity *tnl_id = tnl_status.Identities[idx];
        if (tnl_id->Identifier == NULL && tnl_id->FingerPrint != NULL) {
            char identifier[PATH_MAX];
            snprintf(identifier, sizeof(identifier), "%s%c%s.json", config_dir, PATH_SEP, tnl_id->FingerPrint);
            tnl_id->Identifier = strdup(identifier);
        }
        if (tnl_id->Identifier != NULL) {
            // set this field to false during initialization
            normalize_identifier((char*)tnl_id->Identifier);
            // verify the identity file is still there before adding to the map. This handles the case when the file is removed manually

            struct stat buffer;
            if (stat(tnl_id->Identifier, &buffer) == 0) {
                model_map_set(&tnl_identity_map, tnl_id->Identifier, tnl_id);
            } else {
                ZITI_LOG(WARN, "identity was in config, but file no longer exists. identifier=%s", tnl_id->Identifier);
            }
        }
        //on startup - set mfa needed to false to correctly reflect tunnel status. After the identity is loaded these
        //are set to true __if necessary__
        tnl_id->MfaNeeded = false;
    }
}

void initialize_tunnel_status() {
    tnl_status.Duration = 0;
    uv_timeval64_t now;
    uv_gettimeofday(&now);
    tnl_status.StartTime.tv_sec = (long)now.tv_sec;
    tnl_status.StartTime.tv_usec = now.tv_usec;
    tnl_status.ApiPageSize = DEFAULT_API_PAGESIZE;

}

bool load_tunnel_status(const char* config_data) {
    if (parse_tunnel_status(&tnl_status, config_data, strlen(config_data)) < 0) {
        ZITI_LOG(ERROR, "Could not read tunnel status from config data");
        initialize_tunnel_status();
        // clean up the data fields, because the tunnel status may be partially filled during parsing
        if (tnl_status.Identities) {
            for (int id_idx = 0; tnl_status.Identities[id_idx] != 0; id_idx++) {
                free_tunnel_identity(tnl_status.Identities[id_idx]);
            }
            free(tnl_status.Identities);
            tnl_status.Identities = NULL;
        }
        if (tnl_status.IpInfo) {
            free_ip_info(tnl_status.IpInfo);
            tnl_status.IpInfo = NULL;
        }
        if (tnl_status.TunIpv4) {
            free((char*)tnl_status.TunIpv4);
            tnl_status.TunIpv4 = NULL;
        }
        if (tnl_status.ServiceVersion) {
            free_service_version(tnl_status.ServiceVersion);
            tnl_status.ServiceVersion = NULL;
        }
        return false;
    }
    initialize_tunnel_status();
    set_identifier_from_identities();
    return true;
}

tunnel_status *get_tunnel_status() {
    if (tnl_status.StartTime.tv_sec == 0) {
        initialize_tunnel_status();
    } else {
        uv_timeval64_t now;
        uv_gettimeofday(&now);
        uint64_t start_time_in_millis = (tnl_status.StartTime.tv_sec * 1000) + (tnl_status.StartTime.tv_usec / 1000);
        uint64_t current_time_in_millis = (now.tv_sec * 1000) + (now.tv_usec / 1000);
        tnl_status.Duration = (int)(current_time_in_millis - start_time_in_millis);
    }

    if (tnl_status.Identities) free(tnl_status.Identities);
    tnl_status.Identities = get_tunnel_identities();

    if (tnl_status.Identities != NULL) {
        for (int id_idx = 0; tnl_status.Identities[id_idx] != 0; id_idx++) {
            set_mfa_timeout_rem(tnl_status.Identities[id_idx]);
            tnl_status.Identities[id_idx]->Notified = false;
        }
    }

    return &tnl_status;
}

char *get_tunnel_config(size_t *json_len) {
    tunnel_status tnl_config = {0};
    tunnel_status *tnl_sts = get_tunnel_status();
    tnl_config.Duration = tnl_sts->Duration;
    tnl_config.StartTime = tnl_sts->StartTime;

    tunnel_identity_array tnl_id_arr_config = NULL;

    for (int i =0; tnl_sts->Identities[i]; i++) {
        if (i == 0) {
            tnl_id_arr_config = calloc(model_map_size(&tnl_identity_map) + 1, sizeof(tunnel_identity*));
        }

        tunnel_identity* id = tnl_sts->Identities[i];
        tunnel_identity *id_new = calloc(1, sizeof(tunnel_identity));
        id_new->Identifier = id->Identifier;
        id_new->FingerPrint = id->FingerPrint;
        id_new->Name = id->Name;
        id_new->MfaEnabled = id->MfaEnabled;
        id_new->MfaNeeded = id->MfaNeeded;
        id_new->Active = id->Active;
        id_new->Loaded = id->Loaded;
        id_new->Config = id-> Config;
        id_new->ControllerVersion = id->ControllerVersion;
        tnl_id_arr_config[i] = id_new;
    }

    if (tnl_id_arr_config != NULL) {
        tnl_config.Identities = tnl_id_arr_config;
    }
    tnl_config.IpInfo = tnl_sts->IpInfo;
    tnl_config.ServiceVersion = tnl_sts->ServiceVersion;
    tnl_config.TunIpv4 = tnl_sts->TunIpv4;
    tnl_config.TunPrefixLength = tnl_sts->TunPrefixLength;
    tnl_config.LogLevel = strdup(tnl_sts->LogLevel);
    tnl_config.AddDns = tnl_sts->AddDns;
    tnl_config.ApiPageSize = tnl_sts->ApiPageSize;

    char* tunnel_config_json = tunnel_status_to_json(&tnl_config, 0, json_len);

    //free up space
    if (tnl_id_arr_config != NULL) {
        for(int i=0; tnl_config.Identities[i]; i++){
            free(tnl_config.Identities[i]);
        }
        free(tnl_config.Identities);
        tnl_config.Identities = NULL;
    }
    tnl_config.IpInfo = NULL;
    tnl_config.ServiceVersion = NULL;
    tnl_config.TunIpv4 = NULL;
    free_tunnel_status(&tnl_config);

    return tunnel_config_json;
}

void set_mfa_status(const char* identifier, bool mfa_enabled, bool mfa_needed) {
    tunnel_identity *tnl_id = find_tunnel_identity(identifier);
    if (tnl_id != NULL) {
        tnl_id->MfaEnabled = mfa_enabled;
        tnl_id->MfaNeeded = mfa_needed;
        tnl_id->Notified = false;
        ZITI_LOG(DEBUG, "ztx[%s] mfa enabled : %d, mfa needed : %d ", identifier, mfa_enabled, mfa_needed);
    }
}

void update_mfa_time(const char* identifier) {
    tunnel_identity *tnl_id = find_tunnel_identity(identifier);
    if (tnl_id != NULL) {
        uv_timeval64_t now;
        uv_gettimeofday(&now);

        if (tnl_id->MfaLastUpdatedTime == NULL) {
            tnl_id->MfaLastUpdatedTime = malloc(sizeof(timestamp));
        }
        tnl_id->MfaLastUpdatedTime->tv_sec = now.tv_sec;
        tnl_id->MfaLastUpdatedTime->tv_usec = now.tv_usec;
    }
}

void set_ip_info(uint32_t dns_ip, uint32_t tun_ip, int bits) {
    tnl_status.TunPrefixLength = bits;

    if (tnl_status.TunIpv4) free((char*)tnl_status.TunIpv4);
    ip_addr_t tun_ip4 = IPADDR4_INIT(tun_ip);
    tnl_status.TunIpv4 = strdup(ipaddr_ntoa(&tun_ip4));

    if (tnl_status.IpInfo) {
        free_ip_info(tnl_status.IpInfo);
        free(tnl_status.IpInfo);
    }
    ip_addr_t dns_ip4 = IPADDR4_INIT(dns_ip);
    tnl_status.IpInfo = calloc(1, sizeof(ip_info));
    tnl_status.IpInfo->Ip = strdup(ipaddr_ntoa(&tun_ip4));
    tnl_status.IpInfo->DNS = strdup(ipaddr_ntoa(&dns_ip4));
    tnl_status.IpInfo->MTU = 65535;

    uint32_t netmask = (0xFFFFFFFFUL << (32 - bits)) & 0xFFFFFFFFUL;
    netmask = htonl(netmask);
    ip_addr_t netmask_ipv4 = IPADDR4_INIT(netmask);
    tnl_status.IpInfo->Subnet = strdup(ipaddr_ntoa(&netmask_ipv4));
}

void set_log_level(const char* log_level) {
    if (log_level == NULL) {
        return;
    }
    if (tnl_status.LogLevel) {
        free((char*)tnl_status.LogLevel);
        tnl_status.LogLevel = NULL;
    }
    tnl_status.LogLevel = strdup(log_level);
    for (int i = 0; tnl_status.LogLevel[i] != '\0'; i++) {
        ((char*)tnl_status.LogLevel)[i] = (char)tolower(tnl_status.LogLevel[i]);
    }
}

const char* get_log_level_label() {
    if (tnl_status.LogLevel) {
        return tnl_status.LogLevel;
    } else {
        return NULL;
    }
}


#define LEVEL_LBL(lvl) #lvl,
static const char *const level_labels[] = {
        DEBUG_LEVELS(LEVEL_LBL)
};

int get_log_level(const char* log_level) {
    if(!log_level) {
        char* loglvl = getenv("ZITI_LOG");
        if(!loglvl) {
            return (int) INFO; //no log level supplied - use INFO as default
        } else {
            return (int) strtol(loglvl, NULL, 10);
        }
    }
    if (isdigit(log_level[0])) {
        return (int) strtol(log_level, NULL, 10);
    }
    int lvl = 0;
    int num_levels = sizeof(level_labels) / sizeof(const char *);
    for (int i = 0;i < num_levels; i++) {
        if (strcasecmp(log_level, level_labels[i]) == 0) {
            lvl = i;
            break;
        }
    }
    return lvl;
}

void set_service_version() {
    if (tnl_status.ServiceVersion) {
        free_service_version(tnl_status.ServiceVersion);
        free(tnl_status.ServiceVersion);
    }
    tnl_status.ServiceVersion = calloc(1, sizeof(service_version));

    const char *version = ziti_tunneler_version();
    if (version != NULL) {
        tnl_status.ServiceVersion->Version = strdup(version);
        char *revision_idx = strstr(version, "-");
        if (revision_idx != NULL) {
            ((char*)tnl_status.ServiceVersion->Version)[revision_idx - version] = '\0';
            tnl_status.ServiceVersion->Revision = strdup(revision_idx + 1);
        }
    }

    tnl_status.ServiceVersion->BuildDate = strdup(ziti_tunneler_build_date());
}

void delete_identity_from_instance(const char* identifier) {
    tunnel_identity *id = model_map_get(&tnl_identity_map, identifier);
    if (id == NULL) {
        return;
    }
    model_map_remove(&tnl_identity_map, identifier);
    ZITI_LOG(DEBUG, "ztx[%s] is removed from the tunnel identity list", identifier);

    // delete identity file
    remove(identifier);
    ZITI_LOG(INFO, "Identity file %s is deleted",identifier);

    free_tunnel_identity(id);
    free(id);
}

void set_tun_ipv4_into_instance(const char* tun_ip, int prefixLength, bool addDns) {
    if (tnl_status.TunIpv4 != NULL) free((char*)tnl_status.TunIpv4);
    tnl_status.TunIpv4 = strdup(tun_ip);

    tnl_status.TunPrefixLength = prefixLength;

    tnl_status.AddDns = addDns;
}

char* get_ip_range_from_config() {
    char* ip_range = NULL;
    if (tnl_status.TunIpv4 != NULL && tnl_status.TunPrefixLength > 0) {
        ip_range = calloc(30, sizeof(char));
        snprintf(ip_range, 30 * sizeof(char), "%s/%d",tnl_status.TunIpv4, (int)tnl_status.TunPrefixLength);
    }
    return ip_range;
}

const char* get_dns_ip() {
    return tnl_status.IpInfo->DNS;
}

bool get_add_dns_flag() {
    return tnl_status.AddDns;
}

void set_ziti_status(bool enabled, const char* identifier) {
    tunnel_identity *id = model_map_get(&tnl_identity_map, identifier);
    if (id == NULL) {
        return;
    }
    id->Active = enabled;
}

int get_api_page_size() {
    return tnl_status.ApiPageSize;
}
void set_config_dir(const char *path) {
    tnl_status.ConfigDir = strdup(path);
}

void set_tun_name(const char *name) {
    tnl_status.TunName = strdup(name);
}

char* get_zet_instance_id(const char* discriminator) {
    char *zet_instance_id = NULL;
    if (discriminator) {
        zet_instance_id = calloc(strlen(DEFAULT_EXECUTABLE_NAME) + strlen(discriminator) + 2 /* separator + nul */,sizeof(char));
        sprintf(zet_instance_id, "%s.%s", DEFAULT_EXECUTABLE_NAME, discriminator);
    } else {
        zet_instance_id = strdup(DEFAULT_EXECUTABLE_NAME);
    }
    return zet_instance_id;
}

// ************** TUNNEL BROADCAST MESSAGES
IMPL_MODEL(tunnel_identity, TUNNEL_IDENTITY)
IMPL_MODEL(tunnel_config, TUNNEL_CONFIG)
IMPL_MODEL(tunnel_metrics, TUNNEL_METRICS)
IMPL_MODEL(tunnel_address, TUNNEL_ADDRESS)
IMPL_MODEL(tunnel_port_range, TUNNEL_PORT_RANGE)
IMPL_MODEL(tunnel_posture_check, TUNNEL_POSTURE_CHECK)
IMPL_MODEL(tunnel_service_permissions, TUNNEL_SERVICE_PERMISSIONS)
IMPL_MODEL(tunnel_service, TUNNEL_SERVICE)
IMPL_MODEL(tunnel_status, TUNNEL_STATUS)
IMPL_MODEL(ip_info, IP_INFO)
IMPL_MODEL(service_version, SERVICE_VERSION)

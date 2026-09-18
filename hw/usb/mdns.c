/*
 * Multicast DNS service advertising.
 *
 * Copyright (c) 2023-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "mdns.h"

#ifdef CONFIG_DARWIN

    #include <dns_sd.h>

struct MDNSService
{
    DNSServiceRef ref;
    int           fd;
};

static void DNSSD_API mdns_registered(DNSServiceRef ref, DNSServiceFlags flags, DNSServiceErrorType err,
                                      const char* name, const char* type, const char* domain, void* opaque)
{
    if (err != kDNSServiceErr_NoError) {
        warn_report("mdns: the responder rejected %s: error %d", type, (int)err);
        return;
    }

    info_report("mdns: advertising `%s%s%s'", name, type, domain);
}

static void mdns_readable(void* opaque)
{
    MDNSService*        svc = opaque;
    DNSServiceErrorType err;

    err = DNSServiceProcessResult(svc->ref);
    if (err != kDNSServiceErr_NoError) { warn_report("mdns: the responder connection failed: error %d", (int)err); }
}

MDNSService* mdns_service_register(const char* name, const char* type, uint16_t port, const char* txt, Error** errp)
{
    g_autofree uint8_t* record     = NULL;
    uint16_t            record_len = 0;
    DNSServiceRef       ref;
    DNSServiceErrorType err;
    MDNSService*        svc;

    if (txt != NULL) {
        size_t len = strlen(txt);

        if (len > 0xFF) {
            error_setg(errp, "TXT string of %zu bytes exceeds the 255 a single one holds", len);
            return NULL;
        }

        record    = g_malloc(len + 1);
        record[0] = len;
        memcpy(record + 1, txt, len);
        record_len = len + 1;
    }

    err = DNSServiceRegister(&ref, 0, kDNSServiceInterfaceIndexAny, name, type, NULL, NULL, htons(port), record_len,
                             record, mdns_registered, NULL);
    if (err != kDNSServiceErr_NoError) {
        error_setg(errp, "DNSServiceRegister failed: error %d", (int)err);
        return NULL;
    }

    svc      = g_new0(MDNSService, 1);
    svc->ref = ref;
    svc->fd  = DNSServiceRefSockFD(ref);
    qemu_set_fd_handler(svc->fd, mdns_readable, NULL, svc);

    return svc;
}

void mdns_service_unregister(MDNSService* svc)
{
    if (svc == NULL) { return; }

    qemu_set_fd_handler(svc->fd, NULL, NULL, NULL);
    DNSServiceRefDeallocate(svc->ref);
    g_free(svc);
}

#elif defined(CONFIG_WIN32)

    #include <windns.h>

struct MDNSService
{
    DNS_SERVICE_INSTANCE* instance;
    DNS_SERVICE_CANCEL    cancel;
};

static void WINAPI mdns_registered(DWORD status, void* opaque, DNS_SERVICE_INSTANCE* instance)
{
    if (status != ERROR_SUCCESS) { warn_report("mdns: the DNS Client rejected the service: error %lu", status); }

    if (instance != NULL) { DnsServiceFreeInstance(instance); }
}

MDNSService* mdns_service_register(const char* name, const char* type, uint16_t port, const char* txt, Error** errp)
{
    g_autofree char*      instance_name = g_strdup_printf("%s.%s.local", name, type);
    g_autofree char*      host_name     = g_strdup_printf("%s.local", g_get_host_name());
    g_autofree gunichar2* winstance     = g_utf8_to_utf16(instance_name, -1, NULL, NULL, NULL);
    g_autofree gunichar2* whost         = g_utf8_to_utf16(host_name, -1, NULL, NULL, NULL);
    g_autofree gunichar2* wtxt          = txt != NULL ? g_utf8_to_utf16(txt, -1, NULL, NULL, NULL) : NULL;
    PWSTR                 keys[1]       = {(PWSTR)wtxt};
    PWSTR                        values[1] = {NULL};
    DNS_SERVICE_REGISTER_REQUEST req       = {0};
    MDNSService*                 svc;
    DWORD                        status;

    if (winstance == NULL || whost == NULL || (txt != NULL && wtxt == NULL)) {
        error_setg(errp, "Service name is not valid UTF-8");
        return NULL;
    }

    svc           = g_new0(MDNSService, 1);
    svc->instance = DnsServiceConstructInstance((PCWSTR)winstance, (PCWSTR)whost, NULL, NULL, port, 0, 0,
                                                txt != NULL ? 1 : 0, keys, values);
    if (svc->instance == NULL) {
        error_setg(errp, "DnsServiceConstructInstance failed: error %lu", GetLastError());
        g_free(svc);
        return NULL;
    }

    req.Version                     = DNS_QUERY_REQUEST_VERSION1;
    req.pServiceInstance            = svc->instance;
    req.pRegisterCompletionCallback = mdns_registered;
    req.pQueryContext               = svc;
    req.unicastEnabled              = FALSE;

    status = DnsServiceRegister(&req, &svc->cancel);
    if (status != DNS_REQUEST_PENDING && status != ERROR_SUCCESS) {
        error_setg(errp, "DnsServiceRegister failed: error %lu", status);
        DnsServiceFreeInstance(svc->instance);
        g_free(svc);
        return NULL;
    }

    return svc;
}

void mdns_service_unregister(MDNSService* svc)
{
    DNS_SERVICE_REGISTER_REQUEST req = {0};

    if (svc == NULL) { return; }

    req.Version          = DNS_QUERY_REQUEST_VERSION1;
    req.pServiceInstance = svc->instance;
    DnsServiceDeRegister(&req, NULL);

    DnsServiceFreeInstance(svc->instance);
    g_free(svc);
}

#elif defined(CONFIG_AVAHI)

    #include <avahi-client/client.h>
    #include <avahi-client/publish.h>
    #include <avahi-common/alternative.h>
    #include <avahi-common/error.h>
    #include <avahi-common/strlst.h>
    #include <avahi-glib/glib-watch.h>
    #include <avahi-glib/glib-common.h>

struct MDNSService
{
    AvahiGLibPoll*   poll;
    AvahiClient*     client;
    AvahiEntryGroup* group;
    char*            name;
    char*            type;
    char*            txt;
    uint16_t         port;
};

static void mdns_publish(MDNSService* svc)
{
    AvahiStringList* txt = NULL;
    int              ret;

    if (svc->group == NULL) { return; }

    if (svc->txt != NULL) { txt = avahi_string_list_add(NULL, svc->txt); }

    ret = avahi_entry_group_add_service_strlst(svc->group, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, 0, svc->name, svc->type,
                                               NULL, NULL, svc->port, txt);
    avahi_string_list_free(txt);

    if (ret < 0) {
        warn_report("mdns: avahi rejected `%s': %s", svc->name, avahi_strerror(ret));
        return;
    }

    ret = avahi_entry_group_commit(svc->group);
    if (ret < 0) { warn_report("mdns: avahi failed to commit `%s': %s", svc->name, avahi_strerror(ret)); }
}

static void mdns_group_event(AvahiEntryGroup* group, AvahiEntryGroupState state, void* opaque)
{
    MDNSService* svc = opaque;
    char*        alt;

    switch (state) {
        case AVAHI_ENTRY_GROUP_ESTABLISHED: info_report("mdns: advertising `%s' as `%s'", svc->type, svc->name); break;
        case AVAHI_ENTRY_GROUP_COLLISION:
            alt = avahi_alternative_service_name(svc->name);
            g_free(svc->name);
            svc->name = g_strdup(alt);
            avahi_free(alt);
            avahi_entry_group_reset(group);
            mdns_publish(svc);
            break;
        case AVAHI_ENTRY_GROUP_FAILURE:
            warn_report("mdns: avahi dropped `%s': %s", svc->name,
                        avahi_strerror(avahi_client_errno(avahi_entry_group_get_client(group))));
            break;
        default: break;
    }
}

static void mdns_client_event(AvahiClient* client, AvahiClientState state, void* opaque)
{
    MDNSService* svc = opaque;

    switch (state) {
        case AVAHI_CLIENT_S_RUNNING:
            if (svc->group == NULL) { svc->group = avahi_entry_group_new(client, mdns_group_event, svc); }
            if (svc->group == NULL) {
                warn_report("mdns: avahi_entry_group_new failed: %s", avahi_strerror(avahi_client_errno(client)));
                return;
            }
            mdns_publish(svc);
            break;
        case AVAHI_CLIENT_S_COLLISION:
        case AVAHI_CLIENT_S_REGISTERING:
            if (svc->group != NULL) { avahi_entry_group_reset(svc->group); }
            break;
        case AVAHI_CLIENT_FAILURE:
            warn_report("mdns: avahi connection failed: %s", avahi_strerror(avahi_client_errno(client)));
            break;
        default: break;
    }
}

MDNSService* mdns_service_register(const char* name, const char* type, uint16_t port, const char* txt, Error** errp)
{
    MDNSService* svc;
    int          err = 0;

    svc       = g_new0(MDNSService, 1);
    svc->name = g_strdup(name);
    svc->type = g_strdup(type);
    svc->txt  = g_strdup(txt);
    svc->port = port;

    svc->poll = avahi_glib_poll_new(NULL, G_PRIORITY_DEFAULT);
    if (svc->poll == NULL) {
        error_setg(errp, "avahi_glib_poll_new failed");
        mdns_service_unregister(svc);
        return NULL;
    }

    svc->client = avahi_client_new(avahi_glib_poll_get(svc->poll), 0, mdns_client_event, svc, &err);
    if (svc->client == NULL) {
        error_setg(errp, "avahi_client_new failed: %s", avahi_strerror(err));
        mdns_service_unregister(svc);
        return NULL;
    }

    return svc;
}

void mdns_service_unregister(MDNSService* svc)
{
    if (svc == NULL) { return; }

    if (svc->client != NULL) { avahi_client_free(svc->client); }
    if (svc->poll != NULL) { avahi_glib_poll_free(svc->poll); }

    g_free(svc->name);
    g_free(svc->type);
    g_free(svc->txt);
    g_free(svc);
}

#else

MDNSService* mdns_service_register(const char* name, const char* type, uint16_t port, const char* txt, Error** errp)
{
    error_setg(errp, "This build has no mDNS responder to advertise through");
    return NULL;
}

void mdns_service_unregister(MDNSService* svc) { assert_null(svc); }

#endif

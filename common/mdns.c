/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Avahi-backed mDNS-SD publisher. See mdns.h for API contract.
 *
 * Build with `-DAOETHER_HAVE_AVAHI` and `-lavahi-client -lavahi-common` to
 * get the real implementation; otherwise mdns_stub.c is compiled instead
 * and all calls become no-ops.
 */

#include "mdns.h"

#ifdef AOETHER_HAVE_AVAHI

#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/alternative.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/thread-watch.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct aoether_mdns {
    AvahiThreadedPoll *poll;
    AvahiClient       *client;
    AvahiEntryGroup   *group;
    char              *service_type;
    char              *instance_name;   /* owned; may be renamed on collision */
    uint16_t           port;
    AvahiStringList   *txt;             /* owned */
};

static void entry_group_cb(AvahiEntryGroup *g, AvahiEntryGroupState state, void *userdata);
static void client_cb(AvahiClient *c, AvahiClientState state, void *userdata);

static int add_service(struct aoether_mdns *m)
{
    int rc;
    if (!m->group) {
        m->group = avahi_entry_group_new(m->client, entry_group_cb, m);
        if (!m->group) {
            fprintf(stderr, "mdns: avahi_entry_group_new failed: %s\n",
                    avahi_strerror(avahi_client_errno(m->client)));
            return -1;
        }
    }

    rc = avahi_entry_group_add_service_strlst(m->group,
                                              AVAHI_IF_UNSPEC,
                                              AVAHI_PROTO_UNSPEC,
                                              0,
                                              m->instance_name,
                                              m->service_type,
                                              NULL, NULL,
                                              m->port,
                                              m->txt);
    if (rc == AVAHI_ERR_COLLISION) {
        char *n = avahi_alternative_service_name(m->instance_name);
        if (!n) return -1;
        avahi_free(m->instance_name);
        m->instance_name = strdup(n);
        avahi_free(n);
        if (!m->instance_name) return -1;
        avahi_entry_group_reset(m->group);
        return add_service(m);
    }
    if (rc < 0) {
        fprintf(stderr, "mdns: add_service(%s) failed: %s\n",
                m->service_type, avahi_strerror(rc));
        return -1;
    }

    rc = avahi_entry_group_commit(m->group);
    if (rc < 0) {
        fprintf(stderr, "mdns: commit failed: %s\n", avahi_strerror(rc));
        return -1;
    }
    return 0;
}

static void entry_group_cb(AvahiEntryGroup *g, AvahiEntryGroupState state, void *userdata)
{
    struct aoether_mdns *m = userdata;
    switch (state) {
    case AVAHI_ENTRY_GROUP_ESTABLISHED:
        fprintf(stderr, "mdns: published %s as \"%s\"\n",
                m->service_type, m->instance_name);
        break;
    case AVAHI_ENTRY_GROUP_COLLISION: {
        char *n = avahi_alternative_service_name(m->instance_name);
        if (n) {
            avahi_free(m->instance_name);
            m->instance_name = strdup(n);
            avahi_free(n);
            fprintf(stderr, "mdns: name collision, retrying as \"%s\"\n",
                    m->instance_name);
            add_service(m);
        }
        break;
    }
    case AVAHI_ENTRY_GROUP_FAILURE:
        fprintf(stderr, "mdns: entry group failed: %s\n",
                avahi_strerror(avahi_client_errno(m->client)));
        break;
    case AVAHI_ENTRY_GROUP_UNCOMMITED:
    case AVAHI_ENTRY_GROUP_REGISTERING:
    default:
        break;
    }
    (void)g;
}

static void client_cb(AvahiClient *c, AvahiClientState state, void *userdata)
{
    struct aoether_mdns *m = userdata;
    m->client = c;
    switch (state) {
    case AVAHI_CLIENT_S_RUNNING:
        add_service(m);
        break;
    case AVAHI_CLIENT_FAILURE:
        fprintf(stderr, "mdns: client failure: %s\n",
                avahi_strerror(avahi_client_errno(c)));
        break;
    case AVAHI_CLIENT_S_COLLISION:
    case AVAHI_CLIENT_S_REGISTERING:
        if (m->group) avahi_entry_group_reset(m->group);
        break;
    case AVAHI_CLIENT_CONNECTING:
    default:
        break;
    }
}

struct aoether_mdns *aoether_mdns_publish(const char *service_type,
                                          const char *instance_name,
                                          uint16_t port,
                                          const struct aoether_mdns_txt *txt,
                                          size_t txt_count)
{
    if (!service_type || !instance_name) return NULL;

    struct aoether_mdns *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->port = port;
    m->service_type = strdup(service_type);
    m->instance_name = strdup(instance_name);
    if (!m->service_type || !m->instance_name) goto fail;

    for (size_t i = 0; i < txt_count; i++) {
        const struct aoether_mdns_txt *t = &txt[i];
        if (!t->key) continue;
        char buf[256];
        if (t->value) {
            int n = snprintf(buf, sizeof(buf), "%s=%s", t->key, t->value);
            if (n <= 0 || (size_t)n >= sizeof(buf)) continue;
        } else {
            size_t kl = strlen(t->key);
            if (kl >= sizeof(buf)) continue;
            memcpy(buf, t->key, kl + 1);
        }
        AvahiStringList *next = avahi_string_list_add(m->txt, buf);
        if (!next) goto fail;
        m->txt = next;
    }

    m->poll = avahi_threaded_poll_new();
    if (!m->poll) goto fail;

    int err = 0;
    m->client = avahi_client_new(avahi_threaded_poll_get(m->poll),
                                 0, client_cb, m, &err);
    if (!m->client) {
        fprintf(stderr, "mdns: avahi_client_new failed: %s\n",
                avahi_strerror(err));
        goto fail;
    }

    if (avahi_threaded_poll_start(m->poll) < 0) {
        fprintf(stderr, "mdns: threaded_poll_start failed\n");
        goto fail;
    }

    return m;

fail:
    aoether_mdns_close(m);
    return NULL;
}

void aoether_mdns_close(struct aoether_mdns *m)
{
    if (!m) return;
    if (m->poll) avahi_threaded_poll_stop(m->poll);
    if (m->client) avahi_client_free(m->client);
    if (m->poll) avahi_threaded_poll_free(m->poll);
    if (m->txt) avahi_string_list_free(m->txt);
    free(m->service_type);
    free(m->instance_name);
    free(m);
}

int aoether_mdns_available(void) { return 1; }

#else  /* !AOETHER_HAVE_AVAHI */

#include <stdio.h>

struct aoether_mdns *aoether_mdns_publish(const char *service_type,
                                          const char *instance_name,
                                          uint16_t port,
                                          const struct aoether_mdns_txt *txt,
                                          size_t txt_count)
{
    (void)service_type; (void)instance_name; (void)port; (void)txt; (void)txt_count;
    fprintf(stderr, "mdns: not compiled in (install libavahi-client-dev and rebuild)\n");
    return NULL;
}

void aoether_mdns_close(struct aoether_mdns *m) { (void)m; }

int aoether_mdns_available(void) { return 0; }

#endif

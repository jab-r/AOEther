/* aoether-browse — discover AOEther receivers on the local network.
 *
 * Browses the mDNS-SD service type `_aoether._udp`, resolves each hit, and
 * prints a one-line summary per receiver (name, address:port, and the
 * TXT-record capabilities published by the receiver's `--announce`).
 *
 * Runs for --timeout seconds (default 3), then exits. Useful on its own
 * (`aoether-browse`) or from shell scripts that pre-populate a talker
 * invocation.
 *
 * For manual use from any Linux box you can instead run:
 *   avahi-browse -r -t _aoether._udp
 *
 * This helper exists because Avahi's tools vary across distros and
 * scripting against `avahi-browse`'s text output is brittle.
 */

#ifdef AOETHER_HAVE_AVAHI

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/simple-watch.h>

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static AvahiSimplePoll *g_poll;

static void resolve_cb(AvahiServiceResolver *r,
                       AvahiIfIndex interface,
                       AvahiProtocol protocol,
                       AvahiResolverEvent event,
                       const char *name,
                       const char *type,
                       const char *domain,
                       const char *host_name,
                       const AvahiAddress *address,
                       uint16_t port,
                       AvahiStringList *txt,
                       AvahiLookupResultFlags flags,
                       void *userdata)
{
    (void)interface; (void)protocol; (void)type; (void)domain;
    (void)flags; (void)userdata;

    if (event == AVAHI_RESOLVER_FAILURE) {
        fprintf(stderr, "resolve %s failed\n", name);
        avahi_service_resolver_free(r);
        return;
    }

    char addr[AVAHI_ADDRESS_STR_MAX];
    avahi_address_snprint(addr, sizeof(addr), address);
    printf("%-32s  %s:%u  host=%s\n", name, addr, port, host_name);

    for (AvahiStringList *t = txt; t; t = avahi_string_list_get_next(t)) {
        char *key = NULL, *val = NULL;
        size_t val_size = 0;
        if (avahi_string_list_get_pair(t, &key, &val, &val_size) == 0) {
            printf("    %s=%s\n", key, val ? val : "");
            avahi_free(key);
            avahi_free(val);
        }
    }

    avahi_service_resolver_free(r);
}

static void browse_cb(AvahiServiceBrowser *b,
                      AvahiIfIndex interface,
                      AvahiProtocol protocol,
                      AvahiBrowserEvent event,
                      const char *name,
                      const char *type,
                      const char *domain,
                      AvahiLookupResultFlags flags,
                      void *userdata)
{
    (void)b; (void)flags;
    AvahiClient *client = userdata;

    switch (event) {
    case AVAHI_BROWSER_NEW:
        avahi_service_resolver_new(client, interface, protocol,
                                   name, type, domain,
                                   AVAHI_PROTO_UNSPEC, 0,
                                   resolve_cb, client);
        break;
    case AVAHI_BROWSER_FAILURE:
        fprintf(stderr, "browser failure: %s\n",
                avahi_strerror(avahi_client_errno(client)));
        avahi_simple_poll_quit(g_poll);
        break;
    case AVAHI_BROWSER_ALL_FOR_NOW:
    case AVAHI_BROWSER_CACHE_EXHAUSTED:
    case AVAHI_BROWSER_REMOVE:
    default:
        break;
    }
}

int main(int argc, char **argv)
{
    int timeout_ms = 3000;
    const char *service = "_aoether._udp";

    static const struct option opts[] = {
        { "timeout", required_argument, 0, 't' },
        { "service", required_argument, 0, 's' },
        { "help",    no_argument,       0, 'h' },
        { 0, 0, 0, 0 },
    };
    int c;
    while ((c = getopt_long(argc, argv, "t:s:h", opts, NULL)) != -1) {
        switch (c) {
        case 't': timeout_ms = atoi(optarg) * 1000; break;
        case 's': service = optarg; break;
        case 'h':
        default:
            fprintf(stderr,
                    "usage: %s [--timeout S] [--service _aoether._udp]\n",
                    argv[0]);
            return c == 'h' ? 0 : 2;
        }
    }

    g_poll = avahi_simple_poll_new();
    if (!g_poll) return 1;

    int err = 0;
    AvahiClient *client = avahi_client_new(avahi_simple_poll_get(g_poll),
                                           0, NULL, NULL, &err);
    if (!client) {
        fprintf(stderr, "avahi_client_new: %s\n", avahi_strerror(err));
        return 1;
    }

    AvahiServiceBrowser *br =
        avahi_service_browser_new(client, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
                                  service, NULL, 0, browse_cb, client);
    if (!br) {
        fprintf(stderr, "browser_new: %s\n",
                avahi_strerror(avahi_client_errno(client)));
        return 1;
    }

    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        if (avahi_simple_poll_iterate(g_poll, 100) != 0) break;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000 +
                          (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed_ms >= timeout_ms) break;
    }

    avahi_service_browser_free(br);
    avahi_client_free(client);
    avahi_simple_poll_free(g_poll);
    return 0;
}

#else  /* !AOETHER_HAVE_AVAHI */

#include <stdio.h>
int main(void)
{
    fprintf(stderr,
            "aoether-browse: built without libavahi-client.\n"
            "Install libavahi-client-dev and rebuild, or use `avahi-browse -r _aoether._udp`.\n");
    return 1;
}

#endif

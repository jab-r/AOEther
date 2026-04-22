#pragma once

/* Small wrapper around Avahi client for publishing one mDNS-SD service.
 *
 * Used by AOEther M7 Phase A to let receivers advertise themselves on the
 * local network with their DAC capabilities in TXT records, so talkers
 * (or operators running `avahi-browse -r _aoether._udp`) can discover them
 * without static IP/MAC configuration.
 *
 * The publish call is best-effort: if libavahi is not available at build
 * time, or avahi-daemon is not running at runtime, the call returns NULL
 * and the caller continues without an advertisement. Discovery is a
 * convenience layer, never part of the data path.
 *
 * Threading: the Avahi backend runs its own thread internally; callers do
 * not integrate it into their poll loop.
 */

#include <stddef.h>
#include <stdint.h>

struct aoether_mdns;

struct aoether_mdns_txt {
    const char *key;
    const char *value;   /* NULL is allowed and means the key is boolean */
};

/* Publish one service. Returns NULL on failure (avahi unavailable, daemon
 * down, name collision, etc.); in that case the caller should log and
 * continue — mDNS publication is optional. */
struct aoether_mdns *aoether_mdns_publish(const char *service_type,
                                          const char *instance_name,
                                          uint16_t port,
                                          const struct aoether_mdns_txt *txt,
                                          size_t txt_count);

void aoether_mdns_close(struct aoether_mdns *m);

/* True if this binary was built with Avahi support. Useful for banners. */
int aoether_mdns_available(void);

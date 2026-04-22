#pragma once

/* AOEther → la_avdecc thin C wrapper.
 *
 * AOEther M7 Phase B uses L-Acoustics' open-source AVDECC library
 * (github.com/L-Acoustics/avdecc) to provide a Milan-compatible entity
 * that Hive and similar controllers can discover and bind.  This header
 * exposes only the coarse operations AOEther's talker and receiver need:
 *
 *   - open an entity in LISTENER or TALKER role with one stream of a
 *     given format, channel count, and rate;
 *   - advertise it via ADP until close;
 *   - register callbacks for ACMP CONNECT_TX / CONNECT_RX so AOEther can
 *     react to Hive's "Connect" button by learning the peer's MAC and
 *     stream ID at runtime.
 *
 * la_avdecc is C++17; this wrapper is C11 so everything above the wrapper
 * stays in the existing C build.  The wrapper's implementation
 * (common/avdecc.c → libaoether_avdecc.a produced by avdecc/Makefile)
 * compiles as C++ when AOETHER_HAVE_AVDECC is defined and links la_avdecc
 * through its C binding headers; otherwise it compiles as a no-op stub so
 * the data-path binaries still build on systems without the library.
 */

#include <stddef.h>
#include <stdint.h>

/* Keep in sync with the transport / format enums in receiver.c and
 * talker.c. We don't include those headers here to avoid a circular
 * dep; the wrapper translates these into AVDECC descriptors internally. */
enum aoether_avdecc_role {
    AOETHER_AVDECC_LISTENER = 0,   /* receives streams (our receiver) */
    AOETHER_AVDECC_TALKER   = 1,   /* emits streams (our talker) */
};

struct aoether_avdecc_config {
    enum aoether_avdecc_role role;

    /* Human-readable entity name published via ENTITY / STRINGS
     * descriptors.  Shown in Hive's controller pane.  If NULL, a default
     * derived from hostname + role is used. */
    const char *entity_name;

    /* Network interface name (e.g. "eth0") AVDECC rides on. AVDECC is
     * L2-only; this must be an Ethernet-capable interface. */
    const char *iface;

    /* Stream parameters. For Phase B step 1 we publish one STREAM_INPUT
     * (listener) or STREAM_OUTPUT (talker) reflecting the current run's
     * --channels, --rate, --format settings. Capability matrices come
     * in Phase B step 3. */
    int          channels;
    int          rate_hz;
    const char  *format_name;   /* "pcm", "dsd64", ... */
};

/* ACMP binding callback: fired when a controller (Hive) issues
 * CONNECT_RX on a listener entity, or CONNECT_TX on a talker entity.
 * The wrapper decodes peer_mac (6 bytes) and stream_id (u64) from the
 * AVDECC-native representation and hands them off to AOEther, which
 * uses them to steer its existing data path.
 *
 * Called from the la_avdecc worker thread; the callee must be
 * reentrancy-safe relative to the main loop (use atomics or a pipe). */
typedef void (*aoether_avdecc_bind_cb)(const uint8_t peer_mac[6],
                                       uint64_t      peer_stream_id,
                                       void         *user);

/* Unbind: controller issued DISCONNECT_RX / DISCONNECT_TX. */
typedef void (*aoether_avdecc_unbind_cb)(void *user);

struct aoether_avdecc;

struct aoether_avdecc *aoether_avdecc_open(const struct aoether_avdecc_config *cfg,
                                           aoether_avdecc_bind_cb    on_bind,
                                           aoether_avdecc_unbind_cb  on_unbind,
                                           void                     *user);

void aoether_avdecc_close(struct aoether_avdecc *e);

/* True if this binary was built against la_avdecc. Useful for banners. */
int aoether_avdecc_available(void);

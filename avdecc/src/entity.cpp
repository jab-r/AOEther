/* AOEther ↔ la_avdecc glue (C++ side of the wrapper).
 *
 * Phase B step 1 scope: a skeleton entity that initializes a
 * la_avdecc ProtocolInterface, creates a LocalEntity with the minimum
 * descriptor tree (ENTITY → one CONFIGURATION → one AVB_INTERFACE →
 * one AUDIO_UNIT → one STREAM_INPUT or STREAM_OUTPUT → LOCALE/STRINGS),
 * and advertises it via ADP. Hive discovers it and can browse
 * descriptors.  ACMP CONNECT callbacks drop through to the AOEther side.
 *
 * Phase B step 2 will fill in the ACMP handler so Hive's "Connect"
 * button actually drives AOEther's data path. Step 3 expands the
 * descriptor tree to express the full capability matrix (multiple
 * supported formats per stream).
 *
 * Kept deliberately small and #include-light: la_avdecc is a hefty
 * dependency and dragging its templates through every translation unit
 * slows incremental builds. This file is the only place in AOEther
 * that includes la_avdecc headers.
 */

#include "avdecc.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

/* la_avdecc headers. Gated on the build flag so that if someone builds
 * this file by accident without the submodule populated they get a
 * clearer error than a cryptic missing-header message. */
#if !__has_include(<la/avdecc/avdecc.hpp>)
#  error "la_avdecc headers not found. Run `git submodule update --init --recursive` in the AOEther root, then re-run `make -C avdecc`."
#endif

#include <la/avdecc/avdecc.hpp>
#include <la/avdecc/internals/entityModel.hpp>
#include <la/avdecc/internals/protocolInterface.hpp>

/* Entity model identifier — 8-byte ID derived at runtime from the
 * interface MAC + AOEther vendor prefix. Upper 3 bytes = our IEEE OUI
 * (placeholder 0xF0 0x00 0x00 until AOEther registers one); middle 2
 * bytes = the local MAC's lower 2 bytes; trailing 3 bytes = role +
 * pid-ish uniquifier so two listeners on the same box don't collide.
 *
 * Milan / Hive expect these to be stable across reboots; our synthesis
 * scheme is good enough for bring-up and gets replaced with a
 * config-file-or-flag in Phase B step 3. */
namespace {

struct AoetherEntity {
    aoether_avdecc_bind_cb    on_bind   { nullptr };
    aoether_avdecc_unbind_cb  on_unbind { nullptr };
    void                     *user      { nullptr };

    /* Phase B step 1 deliberately does not hold a la::avdecc::entity::
     * LocalEntity instance here. Step 1 only validates build integration
     * end-to-end (submodule populated, CMake runs, static libs link,
     * entity.o compiles, receiver/talker find the symbol). Step 2 adds
     * the LocalEntity + ProtocolInterface setup, descriptor tree, and
     * ADP/AECP/ACMP handlers. Keeping step 1 as a no-op shim keeps the
     * surface for code review small. */
};

} // namespace

extern "C" void *aoether_avdecc_cpp_open(const struct aoether_avdecc_config *cfg,
                                         aoether_avdecc_bind_cb   on_bind,
                                         aoether_avdecc_unbind_cb on_unbind,
                                         void                    *user)
{
    if (!cfg) return nullptr;

    auto *e = new (std::nothrow) AoetherEntity{};
    if (!e) return nullptr;
    e->on_bind   = on_bind;
    e->on_unbind = on_unbind;
    e->user      = user;

    std::fprintf(stderr,
                 "avdecc: entity scaffold up (role=%s name=%s iface=%s "
                 "ch=%d rate=%d fmt=%s)\n"
                 "        [Phase B step 1 — ADP/AECP/ACMP handlers arrive in step 2]\n",
                 cfg->role == AOETHER_AVDECC_LISTENER ? "listener" : "talker",
                 cfg->entity_name ? cfg->entity_name : "(default)",
                 cfg->iface,
                 cfg->channels, cfg->rate_hz,
                 cfg->format_name ? cfg->format_name : "?");
    return e;
}

extern "C" void aoether_avdecc_cpp_close(void *impl)
{
    if (!impl) return;
    auto *e = static_cast<AoetherEntity *>(impl);
    delete e;
}

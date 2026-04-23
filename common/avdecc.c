/* SPDX-License-Identifier: GPL-3.0-or-later */
/* la_avdecc-backed AVDECC entity.  See avdecc.h for API contract.
 *
 * This file is the C entry point.  When AOETHER_HAVE_AVDECC is defined
 * the implementation calls into libaoether_avdecc.a (built from the C++
 * glue in avdecc/src/entity.cpp, which wraps la_avdecc's C++ API).
 * Otherwise it falls back to no-op stubs so the data-path binaries build
 * on systems without la_avdecc present.
 *
 * Keeping the C↔C++ boundary in avdecc/src/entity.cpp rather than here
 * lets the main binaries stay pure C11 — only the small static archive
 * produced by avdecc/Makefile needs a C++ compiler.
 */

#include "avdecc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AOETHER_HAVE_AVDECC

/* Implemented in avdecc/src/entity.cpp, linked from libaoether_avdecc.a.
 * The opaque pointer type is defined on the C++ side. */
extern void *aoether_avdecc_cpp_open(const struct aoether_avdecc_config *cfg,
                                     aoether_avdecc_bind_cb             on_bind,
                                     aoether_avdecc_unbind_cb           on_unbind,
                                     void                              *user);
extern void  aoether_avdecc_cpp_close(void *impl);

struct aoether_avdecc {
    void *impl;
};

struct aoether_avdecc *aoether_avdecc_open(const struct aoether_avdecc_config *cfg,
                                           aoether_avdecc_bind_cb    on_bind,
                                           aoether_avdecc_unbind_cb  on_unbind,
                                           void                     *user)
{
    if (!cfg || !cfg->iface) return NULL;
    struct aoether_avdecc *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->impl = aoether_avdecc_cpp_open(cfg, on_bind, on_unbind, user);
    if (!e->impl) {
        free(e);
        return NULL;
    }
    return e;
}

void aoether_avdecc_close(struct aoether_avdecc *e)
{
    if (!e) return;
    if (e->impl) aoether_avdecc_cpp_close(e->impl);
    free(e);
}

int aoether_avdecc_available(void) { return 1; }

#else  /* !AOETHER_HAVE_AVDECC */

struct aoether_avdecc *aoether_avdecc_open(const struct aoether_avdecc_config *cfg,
                                           aoether_avdecc_bind_cb    on_bind,
                                           aoether_avdecc_unbind_cb  on_unbind,
                                           void                     *user)
{
    (void)cfg; (void)on_bind; (void)on_unbind; (void)user;
    fprintf(stderr,
            "avdecc: not compiled in (run `git submodule update --init` then\n"
            "         `make -C avdecc` and rebuild the receiver/talker)\n");
    return NULL;
}

void aoether_avdecc_close(struct aoether_avdecc *e) { (void)e; }

int aoether_avdecc_available(void) { return 0; }

#endif

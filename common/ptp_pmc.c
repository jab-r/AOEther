#include "ptp_pmc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Parse a linuxptp-formatted clock identity — "90e2ba.fffe.2e5678" — into
 * its 8 constituent bytes. The dotted format is EUI-64: three bytes,
 * then the literal "fffe" filler (when the ID was derived from an
 * EUI-48 MAC), then three more bytes. Returns 0 on success. */
static int parse_pmc_clockid(const char *tok, uint8_t out[8])
{
    unsigned b[8];
    if (sscanf(tok,
               "%2x%2x%2x.%2x%2x.%2x%2x%2x",
               &b[0], &b[1], &b[2], &b[3], &b[4],
               &b[5], &b[6], &b[7]) == 8) {
        for (int i = 0; i < 8; i++) out[i] = (uint8_t)b[i];
        return 0;
    }
    return -1;
}

int ptp_pmc_read_gmid(uint8_t out_bytes[8])
{
    /* `pmc -u -b 0 'GET PARENT_DATA_SET'`:
     *   -u   UDS transport (ptp4l's default management socket)
     *   -b 0 boundary (send to local ptp4l only)
     * A short 2s hard timeout avoids hanging the talker if ptp4l is
     * stopped; we still try the management request rather than blocking
     * on a socket. */
    FILE *pf = popen(
        "pmc -u -b 0 'GET PARENT_DATA_SET' 2>/dev/null",
        "r");
    if (!pf) return -1;

    char line[512];
    int found = -1;
    while (fgets(line, sizeof line, pf)) {
        const char *needle = "grandmasterIdentity";
        char *p = strstr(line, needle);
        if (!p) continue;
        p += strlen(needle);
        while (*p == ' ' || *p == '\t') p++;
        /* Strip trailing whitespace. */
        char *end = p + strlen(p);
        while (end > p && (end[-1] == '\n' || end[-1] == '\r' ||
                           end[-1] == ' '  || end[-1] == '\t')) {
            *--end = '\0';
        }
        if (parse_pmc_clockid(p, out_bytes) == 0) {
            found = 0;
            break;
        }
    }

    int rc = pclose(pf);
    if (found < 0 || rc != 0) return -1;
    return 0;
}

void ptp_gmid_to_str(const uint8_t bytes[8], char *out, size_t cap)
{
    if (cap == 0) return;
    int n = snprintf(out, cap,
                     "%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X",
                     bytes[0], bytes[1], bytes[2], bytes[3],
                     bytes[4], bytes[5], bytes[6], bytes[7]);
    if (n < 0 || (size_t)n >= cap) out[cap - 1] = '\0';
}

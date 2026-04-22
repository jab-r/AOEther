#include "sap.h"

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int sap_build(uint8_t *out, size_t out_cap,
              enum sap_kind kind,
              uint32_t origin_ipv4_be,
              uint16_t msg_id_hash,
              const char *sdp,
              size_t sdp_len)
{
    if (!out || !sdp) return -1;
    size_t need = SAP_HDR_LEN + SAP_MIME_LEN + sdp_len;
    if (out_cap < need) return -1;

    /* Byte 0: V=1 (bits 7:5), A=0 (IPv4), R=0, T=kind, E=0, C=0 */
    uint8_t flags = (1u << 5);
    if (kind == SAP_DELETION) flags |= (1u << 2);

    out[0] = flags;
    out[1] = 0;                         /* auth len */
    out[2] = (uint8_t)(msg_id_hash >> 8);
    out[3] = (uint8_t)(msg_id_hash & 0xFF);
    memcpy(&out[4], &origin_ipv4_be, 4);

    memcpy(&out[SAP_HDR_LEN], SAP_MIME, SAP_MIME_LEN);
    memcpy(&out[SAP_HDR_LEN + SAP_MIME_LEN], sdp, sdp_len);

    return (int)need;
}

int sap_parse(const uint8_t *buf, size_t len,
              enum sap_kind *kind_out,
              uint32_t *origin_ipv4_be_out,
              uint16_t *msg_id_hash_out,
              const char **sdp_out,
              size_t *sdp_len_out)
{
    if (!buf || len < SAP_HDR_LEN + SAP_MIME_LEN) return -1;

    uint8_t flags = buf[0];
    uint8_t version = (flags >> 5) & 0x7;
    uint8_t addr_type = (flags >> 4) & 0x1;
    uint8_t msg_type = (flags >> 2) & 0x1;
    uint8_t enc = (flags >> 1) & 0x1;
    uint8_t cmp = flags & 0x1;

    if (version != 1) return -1;
    if (addr_type != 0) return -1;    /* IPv4 only for now */
    if (enc || cmp) return -1;
    if (buf[1] != 0) return -1;       /* refuse auth */

    uint16_t msg_id = ((uint16_t)buf[2] << 8) | buf[3];
    uint32_t origin;
    memcpy(&origin, &buf[4], 4);

    size_t pos = SAP_HDR_LEN;

    /* RFC 2974 payload type is optional but in practice AES67 senders
     * always include "application/sdp\0". Tolerate its absence. */
    const char *sdp_ptr = (const char *)&buf[pos];
    size_t sdp_len = len - pos;
    if (sdp_len >= SAP_MIME_LEN &&
        memcmp(&buf[pos], SAP_MIME, SAP_MIME_LEN) == 0) {
        pos += SAP_MIME_LEN;
        sdp_ptr = (const char *)&buf[pos];
        sdp_len = len - pos;
    }

    if (kind_out) *kind_out = msg_type ? SAP_DELETION : SAP_ANNOUNCE;
    if (origin_ipv4_be_out) *origin_ipv4_be_out = origin;
    if (msg_id_hash_out) *msg_id_hash_out = msg_id;
    if (sdp_out) *sdp_out = sdp_ptr;
    if (sdp_len_out) *sdp_len_out = sdp_len;
    return 0;
}

static int bind_multicast_iface(int fd, const char *iface_name)
{
    if (!iface_name) return 0;

    /* IP_MULTICAST_IF by interface index via ip_mreqn — portable across
     * kernels that accept either the address or the index form. */
    struct ip_mreqn mreqn;
    memset(&mreqn, 0, sizeof mreqn);
    mreqn.imr_ifindex = (int)if_nametoindex(iface_name);
    if (mreqn.imr_ifindex == 0) {
        fprintf(stderr, "sap: if_nametoindex(%s) failed: %s\n",
                iface_name, strerror(errno));
        return -1;
    }
    if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF,
                   &mreqn, sizeof mreqn) < 0) {
        fprintf(stderr, "sap: IP_MULTICAST_IF: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

int sap_open_tx_socket(const char *iface_name)
{
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return -1;

    int ttl = SAP_DEFAULT_TTL;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    int loop = 0;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);

    if (bind_multicast_iface(fd, iface_name) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int sap_open_rx_socket(const char *iface_name)
{
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return -1;

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof reuse);
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(SAP_PORT);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        fprintf(stderr, "sap: bind(%d): %s\n", SAP_PORT, strerror(errno));
        close(fd);
        return -1;
    }

    struct ip_mreqn mreqn;
    memset(&mreqn, 0, sizeof mreqn);
    if (inet_pton(AF_INET, SAP_IPV4_ADDR_STR, &mreqn.imr_multiaddr) != 1) {
        close(fd);
        return -1;
    }
    if (iface_name) {
        mreqn.imr_ifindex = (int)if_nametoindex(iface_name);
        if (mreqn.imr_ifindex == 0) {
            fprintf(stderr, "sap: if_nametoindex(%s): %s\n",
                    iface_name, strerror(errno));
            close(fd);
            return -1;
        }
    }
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   &mreqn, sizeof mreqn) < 0) {
        fprintf(stderr, "sap: IP_ADD_MEMBERSHIP: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

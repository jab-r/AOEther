#include "packet.h"

#include <arpa/inet.h>

void aoe_hdr_build(struct aoe_hdr *h,
                   uint16_t stream_id,
                   uint32_t sequence,
                   uint32_t presentation_time,
                   uint8_t channel_count,
                   uint8_t format,
                   uint8_t payload_count,
                   uint8_t flags)
{
    h->magic = AOE_MAGIC;
    h->version = AOE_VERSION;
    h->stream_id = htons(stream_id);
    h->sequence = htonl(sequence);
    h->presentation_time = htonl(presentation_time);
    h->channel_count = channel_count;
    h->format = format;
    h->payload_count = payload_count;
    h->flags = flags;
}

int aoe_hdr_valid(const struct aoe_hdr *h)
{
    return h->magic == AOE_MAGIC && h->version == AOE_VERSION;
}

void aoe_c_hdr_build_feedback(struct aoe_c_hdr *h,
                              uint16_t stream_id,
                              uint16_t sequence,
                              uint32_t q16_16_samples_per_ms)
{
    h->magic = AOE_C_MAGIC;
    h->version = AOE_VERSION;
    h->frame_type = AOE_C_TYPE_FEEDBACK;
    h->flags = 0;
    h->stream_id = htons(stream_id);
    h->sequence = htons(sequence);
    h->value = htonl(q16_16_samples_per_ms);
    h->reserved = 0;
}

int aoe_c_hdr_valid(const struct aoe_c_hdr *h)
{
    return h->magic == AOE_C_MAGIC && h->version == AOE_VERSION;
}

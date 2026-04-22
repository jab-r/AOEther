/* AOEther ↔ la_avdecc glue (C++ side of the wrapper).
 *
 * M7 Phase B step 2: real AVDECC entity.  Opens a PCap-backed
 * ProtocolInterface on the named iface, creates a la_avdecc
 * AggregateEntity with a minimal EntityTree (one configuration, the
 * --name flag surfaced as the ENTITY descriptor's entityName), and
 * enables ADP advertising.  Hive and other Milan controllers will see
 * the entity and can browse its descriptors.
 *
 * Step 3 (next commit) expands the ConfigurationTree with STREAM_INPUT
 * or STREAM_OUTPUT descriptors and wires an ACMP delegate so Hive's
 * "Connect" button drives AOEther's data path.
 *
 * Kept #include-light: la_avdecc is a heavy dependency and dragging its
 * templates through every translation unit slows incremental builds.
 * This file is the only place in AOEther that includes la_avdecc.
 */

#include "avdecc.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <unistd.h>

#if !__has_include(<la/avdecc/avdecc.hpp>)
#  error "la_avdecc headers not found. Run `git submodule update --init --recursive` in the AOEther root, then re-run `make -C avdecc`."
#endif

#include <la/avdecc/avdecc.hpp>
#include <la/avdecc/internals/aggregateEntity.hpp>
#include <la/avdecc/internals/endStation.hpp>
#include <la/avdecc/internals/entityEnums.hpp>
#include <la/avdecc/internals/entityModelTree.hpp>
#include <la/avdecc/internals/entityModelTypes.hpp>
#include <la/avdecc/internals/protocolInterface.hpp>
#include <la/avdecc/internals/uniqueIdentifier.hpp>

/* Vendor IDs for the EntityModelID.  AOEther does not yet own an IEEE
 * OUI, so we use a CID from the IEEE "private" range for development.
 * Swap to a real OUI when the project registers one; the EntityModelID
 * must be stable across reboots for any given AOEther build, because
 * Milan controllers cache it as the model fingerprint. */
static constexpr std::uint64_t AOETHER_VENDOR_ID  = 0xFFFFFEu;      /* placeholder */
static constexpr std::uint32_t AOETHER_MODEL_ID   = 0x000001u;      /* AOEther receiver/talker */
static constexpr std::uint8_t  AOETHER_DEVICE_REV = 0x01u;
static constexpr std::uint16_t AOETHER_CONF_INDEX = 0u;

namespace {

using la::avdecc::EndStation;
using la::avdecc::UniqueIdentifier;
using la::avdecc::protocol::ProtocolInterface;
using la::avdecc::entity::AggregateEntity;
using la::avdecc::entity::Entity;
using la::avdecc::entity::EntityCapability;
using la::avdecc::entity::EntityCapabilities;
using la::avdecc::entity::ListenerCapability;
using la::avdecc::entity::ListenerCapabilities;
using la::avdecc::entity::TalkerCapability;
using la::avdecc::entity::TalkerCapabilities;
namespace model = la::avdecc::entity::model;

/* Pack a 24-bit vendor OUI + 24-bit model ID + 16-bit revision into
 * one 64-bit EntityModelID, per IEEE 1722.1 convention. */
constexpr UniqueIdentifier makeEntityModelID(std::uint64_t ouiVendor,
                                             std::uint32_t modelId,
                                             std::uint16_t revision) noexcept
{
    const std::uint64_t v =
        ((ouiVendor & 0xFFFFFFull) << 40) |
        ((std::uint64_t(modelId) & 0xFFFFFFull) << 16) |
        (std::uint64_t(revision) & 0xFFFFu);
    return UniqueIdentifier{ v };
}

/* Hive only renders the entityName from the current configuration's
 * localizedStrings or from the dynamicModel.entityName field.  We set
 * the latter directly — AvdeccFixedString is a 64-byte NUL-terminated
 * buffer, wide enough for any human-friendly name. */
static void setFixedString(model::AvdeccFixedString &dst, const std::string &s)
{
    dst = model::AvdeccFixedString{ s };
}

struct AoetherEntity {
    aoether_avdecc_bind_cb   on_bind   { nullptr };
    aoether_avdecc_unbind_cb on_unbind { nullptr };
    void                    *user      { nullptr };

    EndStation::UniquePointer endStation { nullptr, nullptr };
    AggregateEntity          *aggEntity  { nullptr }; // owned by endStation
    model::EntityTree         entityTree {};
};

/* Build the smallest EntityTree that Hive can render — one
 * configuration with an entityName.  Step 3 adds STREAM_INPUT or
 * STREAM_OUTPUT descriptors and their AUDIO_UNIT / AVB_INTERFACE
 * parents so the "Connect" button has something to connect. */
static void buildMinimalTree(model::EntityTree &tree, const std::string &name)
{
    /* Static model: vendor/model name strings are references into the
     * STRINGS descriptor.  Leaving them at default (no-reference)
     * prints as "(null)" in Hive; step 3 will populate a STRINGS
     * descriptor and reference it here so the vendor/model rows render. */
    tree.staticModel = model::EntityNodeStaticModel{};

    /* Dynamic model: the entity name is what shows up as the row label
     * in Hive's controller pane. */
    tree.dynamicModel = model::EntityNodeDynamicModel{};
    setFixedString(tree.dynamicModel.entityName,    name);
    setFixedString(tree.dynamicModel.groupName,     "AOEther");
    setFixedString(tree.dynamicModel.firmwareVersion, "0.1 (M7 Phase B step 2)");
    setFixedString(tree.dynamicModel.serialNumber,  "");
    tree.dynamicModel.currentConfiguration = AOETHER_CONF_INDEX;

    /* One configuration — stays mostly empty until step 3 adds
     * stream / audio-unit / avb-interface descriptors. */
    model::ConfigurationTree conf{};
    conf.dynamicModel.objectName          = model::AvdeccFixedString{ "AOEther Stream" };
    conf.dynamicModel.isActiveConfiguration = true;
    tree.configurationTrees[AOETHER_CONF_INDEX] = std::move(conf);
}

static std::string defaultEntityName(const aoether_avdecc_config &cfg)
{
    if (cfg.entity_name && *cfg.entity_name) return cfg.entity_name;
    char host[128];
    if (gethostname(host, sizeof(host)) != 0) {
        std::snprintf(host, sizeof(host), "aoether");
    }
    host[sizeof(host) - 1] = '\0';
    std::string n = host;
    n += (cfg.role == AOETHER_AVDECC_LISTENER) ? "-listener" : "-talker";
    return n;
}

} // namespace

extern "C" void *aoether_avdecc_cpp_open(const struct aoether_avdecc_config *cfg,
                                         aoether_avdecc_bind_cb   on_bind,
                                         aoether_avdecc_unbind_cb on_unbind,
                                         void                    *user)
{
    if (!cfg || !cfg->iface) return nullptr;

    auto *e = new (std::nothrow) AoetherEntity{};
    if (!e) return nullptr;
    e->on_bind   = on_bind;
    e->on_unbind = on_unbind;
    e->user      = user;

    const std::string name = defaultEntityName(*cfg);

    try {
        /* PCap on Linux opens AF_PACKET under the hood; same capability
         * gate as our raw-Ethernet data path, so if the binary is
         * running with CAP_NET_RAW this Just Works. */
        e->endStation = EndStation::create(ProtocolInterface::Type::PCap,
                                           cfg->iface,
                                           std::nullopt /* default executor */);
    } catch (const EndStation::Exception &ex) {
        std::fprintf(stderr,
                     "avdecc: EndStation::create failed on iface=%s: %s\n",
                     cfg->iface, ex.what());
        delete e;
        return nullptr;
    }

    /* Different progIDs for listener vs talker so both can run on one
     * MAC without colliding EntityIDs. */
    const std::uint16_t progID = (cfg->role == AOETHER_AVDECC_LISTENER) ? 0xA0E1 : 0xA0E2;

    const UniqueIdentifier entityModelID =
        makeEntityModelID(AOETHER_VENDOR_ID,
                          AOETHER_MODEL_ID,
                          (static_cast<std::uint16_t>(AOETHER_DEVICE_REV) << 8) |
                          (cfg->role == AOETHER_AVDECC_LISTENER ? 0x01 : 0x02));

    buildMinimalTree(e->entityTree, name);

    try {
        e->aggEntity = e->endStation->addAggregateEntity(progID,
                                                         entityModelID,
                                                         &e->entityTree,
                                                         nullptr /* no controller delegate */);
    } catch (const EndStation::Exception &ex) {
        std::fprintf(stderr, "avdecc: addAggregateEntity failed: %s\n", ex.what());
        delete e;
        return nullptr;
    }

    /* Set ADP capabilities from role. AOEther implements AEM so controllers
     * can browse descriptors; we do not claim ClassA/B or gPTP yet
     * (those land in M7 Phase B step 3 + M3 Phase B hardware PTP). */
    auto &ci = e->aggEntity->getCommonInformation();
    ci.entityCapabilities = EntityCapabilities{ EntityCapability::AemSupported };
    if (cfg->role == AOETHER_AVDECC_LISTENER) {
        ci.listenerStreamSinks   = 1;
        ci.listenerCapabilities  = ListenerCapabilities{ ListenerCapability::Implemented,
                                                         ListenerCapability::AudioSink };
    } else {
        ci.talkerStreamSources   = 1;
        ci.talkerCapabilities    = TalkerCapabilities{ TalkerCapability::Implemented,
                                                       TalkerCapability::AudioSource };
    }

    if (!e->aggEntity->enableEntityAdvertising(10 /* available seconds */, std::nullopt)) {
        std::fprintf(stderr, "avdecc: enableEntityAdvertising failed (EntityID already in use?)\n");
        /* Not fatal — entity exists, just not advertising. */
    }

    std::fprintf(stderr,
                 "avdecc: entity up (role=%s name=\"%s\" iface=%s EID=0x%016llx)\n"
                 "        [Phase B step 2 — streams + ACMP handler arrive in step 3]\n",
                 cfg->role == AOETHER_AVDECC_LISTENER ? "listener" : "talker",
                 name.c_str(),
                 cfg->iface,
                 static_cast<unsigned long long>(ci.entityID.getValue()));
    return e;
}

extern "C" void aoether_avdecc_cpp_close(void *impl)
{
    if (!impl) return;
    auto *e = static_cast<AoetherEntity *>(impl);
    if (e->aggEntity) {
        e->aggEntity->disableEntityAdvertising(std::nullopt);
        e->aggEntity = nullptr; /* owned by endStation */
    }
    e->endStation.reset();
    delete e;
}

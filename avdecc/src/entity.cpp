/* AOEther ↔ la_avdecc glue (C++ side of the wrapper).
 *
 * M7 Phase B step 3: full descriptor tree.  Beyond step 2's entity
 * name and configuration label, the tree now carries one
 * STREAM_INPUT (listener role) or STREAM_OUTPUT (talker role) plus the
 * AUDIO_UNIT / AVB_INTERFACE / CLOCK_SOURCE / CLOCK_DOMAIN /
 * AUDIO_CLUSTER / AUDIO_MAP descendants Hive needs to render Connect.
 *
 * The stream is advertised in an AAF-48k-24bit-2ch format by default
 * (multichannel variants land with the capability-matrix work in
 * Phase B step 5 / M8).  DSD streams are still exposed on the L2
 * or IP path only — AAF is PCM-only by design, so listener/talker
 * entities for DSD run with a no-op AVDECC side until AVDECC gains
 * a native-DSD format, which is post-M9 work.
 *
 * Step 4 (next commit) wires ACMP CONNECT state changes into the
 * AOEther data path via the on_bind / on_unbind callbacks declared
 * in avdecc.h.  Until step 4 lands, Hive's Connect button updates
 * la_avdecc's internal state and is visible in Hive, but AOEther's
 * data path keeps using CLI --dest-mac / learned-first-frame.
 *
 * Kept #include-light: la_avdecc is a heavy dependency and dragging
 * its templates through every translation unit slows incremental
 * builds.  This file is the only place in AOEther that includes
 * la_avdecc.
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
#include <la/avdecc/internals/protocolAcmpdu.hpp>
#include <la/avdecc/internals/protocolDefines.hpp>
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

struct AoetherEntity;

/* Observer on the ProtocolInterface that watches ACMP traffic and fires
 * on_bind / on_unbind when our entity becomes the target of a Connect or
 * Disconnect command.  Runs on la_avdecc's executor thread — callbacks
 * invoked here must be reentrancy-safe relative to the C main loops
 * that consume them.  The C-side wrappers in receiver.c / talker.c use
 * a small mutex-guarded struct for exactly that. */
class AcmpObserver final : public ProtocolInterface::Observer {
public:
    AcmpObserver(AoetherEntity *parent, aoether_avdecc_role role, UniqueIdentifier ourEID) noexcept
        : _parent{ parent }, _role{ role }, _ourEID{ ourEID } {}

    void onAcmpCommand(ProtocolInterface* const, la::avdecc::protocol::Acmpdu const &pdu) noexcept override
    {
        handle(pdu);
    }
    void onAcmpResponse(ProtocolInterface* const, la::avdecc::protocol::Acmpdu const &pdu) noexcept override
    {
        handle(pdu);
    }

private:
    void handle(la::avdecc::protocol::Acmpdu const &pdu) noexcept;

    AoetherEntity          *_parent;
    aoether_avdecc_role     _role;
    UniqueIdentifier        _ourEID;
};

struct AoetherEntity {
    aoether_avdecc_bind_cb   on_bind   { nullptr };
    aoether_avdecc_unbind_cb on_unbind { nullptr };
    void                    *user      { nullptr };

    EndStation::UniquePointer endStation { nullptr, nullptr };
    AggregateEntity          *aggEntity  { nullptr }; // owned by endStation
    model::EntityTree         entityTree {};

    std::unique_ptr<AcmpObserver> observer;
    bool                          bound { false };
};

void AcmpObserver::handle(la::avdecc::protocol::Acmpdu const &pdu) noexcept
{
    using Msg = la::avdecc::protocol::AcmpMessageType;
    const Msg msg = pdu.getMessageType();

    /* Does this PDU pertain to us?  For listeners we watch RX-side
     * messages whose listener_entity_id matches our EID; for talkers
     * we watch TX-side messages whose talker_entity_id matches us. */
    const UniqueIdentifier tgt = (_role == AOETHER_AVDECC_LISTENER)
                                 ? pdu.getListenerEntityID()
                                 : pdu.getTalkerEntityID();
    if (tgt != _ourEID) return;

    const bool isListenerConnect    = (_role == AOETHER_AVDECC_LISTENER) &&
                                      (msg == Msg::ConnectRxResponse);
    const bool isListenerDisconnect = (_role == AOETHER_AVDECC_LISTENER) &&
                                      (msg == Msg::DisconnectRxResponse);
    const bool isTalkerConnect      = (_role == AOETHER_AVDECC_TALKER) &&
                                      (msg == Msg::ConnectTxCommand);
    const bool isTalkerDisconnect   = (_role == AOETHER_AVDECC_TALKER) &&
                                      (msg == Msg::DisconnectTxCommand);

    if (isListenerConnect || isTalkerConnect) {
        const auto mac = pdu.getStreamDestAddress();
        std::uint64_t streamID = 0;
        if (_role == AOETHER_AVDECC_LISTENER) {
            /* Listener: identifying a specific talker by its EID +
             * unique ID is more precise than the multicast MAC. For
             * now we surface the dest MAC (what AOEther's listener
             * actually filters on) and pack the talker's stream
             * identifier into the 64-bit stream_id field. */
            streamID = (static_cast<std::uint64_t>(pdu.getTalkerEntityID().getValue()) & 0xFFFFFFFFFFFFull) |
                       (static_cast<std::uint64_t>(pdu.getTalkerUniqueID()) << 48);
        } else {
            streamID = (static_cast<std::uint64_t>(pdu.getListenerEntityID().getValue()) & 0xFFFFFFFFFFFFull) |
                       (static_cast<std::uint64_t>(pdu.getListenerUniqueID()) << 48);
        }

        uint8_t peerMac[6];
        std::memcpy(peerMac, mac.data(), 6);

        std::fprintf(stderr,
                     "avdecc: ACMP %s → bind peer=%02x:%02x:%02x:%02x:%02x:%02x stream_id=0x%016llx\n",
                     isListenerConnect ? "CONNECT_RX_RESPONSE" : "CONNECT_TX_COMMAND",
                     peerMac[0], peerMac[1], peerMac[2], peerMac[3], peerMac[4], peerMac[5],
                     static_cast<unsigned long long>(streamID));

        _parent->bound = true;
        if (_parent->on_bind) _parent->on_bind(peerMac, streamID, _parent->user);
    } else if ((isListenerDisconnect || isTalkerDisconnect) && _parent->bound) {
        std::fprintf(stderr, "avdecc: ACMP DISCONNECT → unbind\n");
        _parent->bound = false;
        if (_parent->on_unbind) _parent->on_unbind(_parent->user);
    }
}

/* Build a full EntityTree Hive can render + offer Connect on.
 *
 * One AUDIO_UNIT, one AVB_INTERFACE, one CLOCK_SOURCE, one CLOCK_DOMAIN,
 * one STREAM_INPUT (listener) or one STREAM_OUTPUT (talker), each with
 * the minimum static + dynamic fields populated.
 *
 * Stream format is hardcoded to 48 kHz / 24-bit / stereo AAF for now.
 * Wider format matrices (multichannel, hi-res, variable channel count)
 * land with the capability-advertisement work in Phase B step 5. */
static void buildEntityTree(model::EntityTree &tree,
                            const std::string &name,
                            const aoether_avdecc_config &cfg)
{
    using namespace la::avdecc::entity::model;

    tree.staticModel  = EntityNodeStaticModel{};
    tree.dynamicModel = EntityNodeDynamicModel{};
    setFixedString(tree.dynamicModel.entityName,    name);
    setFixedString(tree.dynamicModel.groupName,     "AOEther");
    setFixedString(tree.dynamicModel.firmwareVersion, "0.1 (M7 Phase B step 3)");
    setFixedString(tree.dynamicModel.serialNumber,  "");
    tree.dynamicModel.currentConfiguration = AOETHER_CONF_INDEX;

    ConfigurationTree conf{};
    conf.dynamicModel.objectName            = AvdeccFixedString{ "AOEther Stream" };
    conf.dynamicModel.isActiveConfiguration = true;

    const SamplingRate sr48k{ 48000u };
    const std::uint16_t streamChannels = 2;

    /* AAF-PCM 48k/24-bit/2ch/6-samples-per-frame. samplesPerFrame=6
     * matches AAF's SDT 8 kHz packet cadence at 48 kHz (48000/8000=6). */
    const StreamFormat aafStreamFormat =
        StreamFormatInfo::buildFormat_AAF(streamChannels,
                                          /*isUpToChannelsCount=*/false,
                                          sr48k,
                                          SampleFormat::Int24,
                                          /*sampleBitDepth=*/24,
                                          /*samplesPerFrame=*/6);

    /* -- AVB_INTERFACE[0] ------------------------------------------------- */
    AvbInterfaceNodeModels avb{};
    avb.staticModel.localizedDescription = LocalizedStringReference{};
    avb.staticModel.interfaceFlags       = la::avdecc::entity::AvbInterfaceFlags{};
    avb.dynamicModel.objectName          = AvdeccFixedString{ "eth0" };
    /* MAC address is filled in after open from the iface's real HW
     * address; leaving it zeroed here produces an invalid ADP field,
     * so we patch it in aoether_avdecc_cpp_open after EndStation
     * creation gives us access to the protocol interface. */
    conf.avbInterfaceModels[AvbInterfaceIndex{ 0 }] = std::move(avb);

    /* -- CLOCK_SOURCE[0] -------------------------------------------------- */
    ClockSourceNodeModels cs{};
    cs.staticModel.localizedDescription = LocalizedStringReference{};
    cs.staticModel.clockSourceType      = ClockSourceType::Internal;
    cs.staticModel.clockSourceLocationType  = DescriptorType::AudioUnit;
    cs.staticModel.clockSourceLocationIndex = DescriptorIndex{ 0 };
    cs.dynamicModel.objectName          = AvdeccFixedString{ "Internal" };
    conf.clockSourceModels[ClockSourceIndex{ 0 }] = std::move(cs);

    /* -- CLOCK_DOMAIN[0] -------------------------------------------------- */
    ClockDomainNodeModels cd{};
    cd.staticModel.localizedDescription = LocalizedStringReference{};
    cd.staticModel.clockSources         = ClockSources{ ClockSourceIndex{ 0 } };
    cd.dynamicModel.objectName          = AvdeccFixedString{ "Clock Domain" };
    cd.dynamicModel.clockSourceIndex    = ClockSourceIndex{ 0 };
    conf.clockDomainModels[ClockDomainIndex{ 0 }] = std::move(cd);

    /* -- AUDIO_UNIT[0] ---------------------------------------------------- */
    AudioUnitTree au{};
    au.audioUnitModels.staticModel.localizedDescription = LocalizedStringReference{};
    au.audioUnitModels.staticModel.clockDomainIndex     = ClockDomainIndex{ 0 };
    au.audioUnitModels.staticModel.samplingRates        = SamplingRates{ sr48k };
    au.audioUnitModels.dynamicModel.objectName          = AvdeccFixedString{ "Audio Unit" };
    au.audioUnitModels.dynamicModel.currentSamplingRate = sr48k;
    if (cfg.role == AOETHER_AVDECC_LISTENER) {
        au.audioUnitModels.staticModel.numberOfStreamInputPorts = 1;
    } else {
        au.audioUnitModels.staticModel.numberOfStreamOutputPorts = 1;
    }
    /* One STREAM_PORT_INPUT or STREAM_PORT_OUTPUT with a single
     * AUDIO_CLUSTER (the stereo pair).  No AUDIO_MAP entries — Hive
     * accepts an empty mapping, and full channel mapping is step 5's
     * work along with the multichannel format matrix. */
    StreamPortTree sp{};
    sp.staticModel.clockDomainIndex = ClockDomainIndex{ 0 };
    sp.staticModel.numberOfClusters = 1;
    sp.staticModel.baseCluster      = ClusterIndex{ 0 };
    AudioClusterNodeModels cluster{};
    cluster.staticModel.localizedDescription = LocalizedStringReference{};
    cluster.staticModel.signalType           = DescriptorType::Invalid;
    cluster.staticModel.signalIndex          = DescriptorIndex{ 0 };
    cluster.staticModel.channelCount         = streamChannels;
    cluster.staticModel.format               = AudioClusterFormat::Mbla;
    cluster.dynamicModel.objectName          = AvdeccFixedString{ "L/R" };
    sp.audioClusterModels[ClusterIndex{ 0 }] = std::move(cluster);
    if (cfg.role == AOETHER_AVDECC_LISTENER) {
        au.streamPortInputTrees[StreamPortIndex{ 0 }] = std::move(sp);
    } else {
        au.streamPortOutputTrees[StreamPortIndex{ 0 }] = std::move(sp);
    }
    conf.audioUnitTrees[AudioUnitIndex{ 0 }] = std::move(au);

    /* -- STREAM_INPUT[0] or STREAM_OUTPUT[0] ------------------------------ */
    StreamNodeStaticModel sm{};
    sm.localizedDescription = LocalizedStringReference{};
    sm.clockDomainIndex     = ClockDomainIndex{ 0 };
    sm.streamFlags          = la::avdecc::entity::StreamFlags{};
    sm.avbInterfaceIndex    = AvbInterfaceIndex{ 0 };
    sm.bufferLength         = 0;
    sm.formats              = StreamFormats{ aafStreamFormat };
    if (cfg.role == AOETHER_AVDECC_LISTENER) {
        StreamInputNodeModels s{};
        s.staticModel              = std::move(sm);
        s.dynamicModel.objectName  = AvdeccFixedString{ "Input Stream" };
        s.dynamicModel.streamFormat = aafStreamFormat;
        conf.streamInputModels[StreamIndex{ 0 }] = std::move(s);
    } else {
        StreamOutputNodeModels s{};
        s.staticModel              = std::move(sm);
        s.dynamicModel.objectName  = AvdeccFixedString{ "Output Stream" };
        s.dynamicModel.streamFormat = aafStreamFormat;
        conf.streamOutputModels[StreamIndex{ 0 }] = std::move(s);
    }

    tree.configurationTrees[AOETHER_CONF_INDEX] = std::move(conf);
}

/* Read the iface MAC via SIOCGIFHWADDR and patch it into the
 * AVB_INTERFACE descriptor's dynamic model. la_avdecc also fills this
 * in internally from the ProtocolInterface for ADPDU emission, but
 * the AEM descriptor needs a matching value so Hive's interface pane
 * renders the expected MAC. */
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
static bool readIfaceMac(const char *iface, std::uint8_t mac[6])
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return false;
    struct ifreq ifr{};
    std::strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
    const bool ok = ioctl(s, SIOCGIFHWADDR, &ifr) == 0;
    ::close(s);
    if (!ok) return false;
    std::memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
    return true;
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

    buildEntityTree(e->entityTree, name, *cfg);

    /* Patch the iface's real MAC into the AVB_INTERFACE dynamic model
     * before handing the tree to AggregateEntity::create (which takes
     * a snapshot on construction). */
    std::uint8_t hwaddr[6] = {};
    if (readIfaceMac(cfg->iface, hwaddr)) {
        auto &confRef = e->entityTree.configurationTrees[AOETHER_CONF_INDEX];
        auto itAvb = confRef.avbInterfaceModels.find(
            la::avdecc::entity::model::AvbInterfaceIndex{ 0 });
        if (itAvb != confRef.avbInterfaceModels.end()) {
            std::memcpy(itAvb->second.dynamicModel.macAddress.data(), hwaddr, 6);
        }
    }

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

    /* Register our ACMP observer on the ProtocolInterface so Hive's
     * Connect / Disconnect actions reach the AOEther data path. */
    if (auto *pi = e->endStation->getProtocolInterface()) {
        e->observer = std::make_unique<AcmpObserver>(e, cfg->role, ci.entityID);
        try {
            pi->registerObserver(e->observer.get());
        } catch (const std::exception &ex) {
            std::fprintf(stderr, "avdecc: registerObserver failed: %s\n", ex.what());
            e->observer.reset();
        }
    }

    std::fprintf(stderr,
                 "avdecc: entity up (role=%s name=\"%s\" iface=%s EID=0x%016llx)\n"
                 "        [Phase B step 4 — ACMP wired to data path]\n",
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
    if (e->observer && e->endStation) {
        if (auto *pi = e->endStation->getProtocolInterface()) {
            try {
                pi->unregisterObserver(e->observer.get());
            } catch (...) { /* best-effort on shutdown */ }
        }
    }
    e->observer.reset();
    if (e->aggEntity) {
        e->aggEntity->disableEntityAdvertising(std::nullopt);
        e->aggEntity = nullptr; /* owned by endStation */
    }
    e->endStation.reset();
    delete e;
}

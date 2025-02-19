#pragma once

#include <library/cpp/actors/core/actorid.h>
#include <library/cpp/actors/core/actorsystem.h>
#include <library/cpp/actors/util/datetime.h>
#include <library/cpp/monlib/dynamic_counters/counters.h>
#include <library/cpp/monlib/metrics/metric_registry.h>
#include <util/generic/map.h>
#include <util/generic/set.h>
#include <util/system/datetime.h>

#include "poller_tcp.h"
#include "logging.h"
#include "event_filter.h"

#include <atomic>

namespace NActors {
    enum class EEncryptionMode {
        DISABLED, // no encryption is required at all
        OPTIONAL, // encryption is enabled when supported by both peers
        REQUIRED, // encryption is mandatory
    };

    struct TInterconnectSettings {
        TDuration Handshake;
        TDuration DeadPeer;
        TDuration CloseOnIdle;
        ui32 SendBufferDieLimitInMB = 0;
        ui64 OutputBuffersTotalSizeLimitInMB = 0;
        ui32 TotalInflightAmountOfData = 0;
        bool MergePerPeerCounters = false;
        bool MergePerDataCenterCounters = false;
        ui32 TCPSocketBufferSize = 0;
        TDuration PingPeriod = TDuration::Seconds(3);
        TDuration ForceConfirmPeriod = TDuration::Seconds(1);
        TDuration LostConnection;
        TDuration BatchPeriod;
        bool BindOnAllAddresses = true;
        EEncryptionMode EncryptionMode = EEncryptionMode::DISABLED;
        bool TlsAuthOnly = false;
        TString Certificate; // certificate data in PEM format
        TString PrivateKey; // private key for the certificate in PEM format
        TString CaFilePath; // path to certificate authority file
        TString CipherList; // encryption algorithms
        TDuration MessagePendingTimeout = TDuration::Seconds(1); // timeout for which messages are queued while in PendingConnection state
        ui64 MessagePendingSize = Max<ui64>(); // size of the queue
        ui32 MaxSerializedEventSize = NActors::EventMaxByteSize;
        ui32 PreallocatedBufferSize = 8 << 10; // 8 KB
        ui32 NumPreallocatedBuffers = 16;

        ui32 GetSendBufferSize() const {
            ui32 res = 512 * 1024; // 512 kb is the default value for send buffer
            if (TCPSocketBufferSize) {
                res = TCPSocketBufferSize;
            }
            return res;
        }
    };

    struct TChannelSettings {
        ui16 Weight;
    };

    typedef TMap<ui16, TChannelSettings> TChannelsConfig;

    using TRegisterMonPageCallback = std::function<void(const TString& path, const TString& title,
                                                        TActorSystem* actorSystem, const TActorId& actorId)>;

    using TInitWhiteboardCallback = std::function<void(ui16 icPort, TActorSystem* actorSystem)>;

    using TUpdateWhiteboardCallback = std::function<void(const TString& peer, bool connected, bool green, bool yellow,
                                                         bool orange, bool red, TActorSystem* actorSystem)>;

    struct TInterconnectProxyCommon : TAtomicRefCount<TInterconnectProxyCommon> {
        TActorId NameserviceId;
        NMonitoring::TDynamicCounterPtr MonCounters;
        std::shared_ptr<NMonitoring::IMetricRegistry> Metrics;
        TChannelsConfig ChannelsConfig;
        TInterconnectSettings Settings;
        TRegisterMonPageCallback RegisterMonPage;
        TActorId DestructorId;
        std::shared_ptr<std::atomic<TAtomicBase>> DestructorQueueSize;
        TAtomicBase MaxDestructorQueueSize = 1024 * 1024 * 1024;
        TString ClusterUUID;
        TVector<TString> AcceptUUID;
        ui64 StartTime = GetCycleCountFast();
        TString TechnicalSelfHostName;
        TInitWhiteboardCallback InitWhiteboard;
        TUpdateWhiteboardCallback UpdateWhiteboard;
        ui32 HandshakeBallastSize = 0;
        TAtomic StartedSessionKiller = 0;
        TScopeId LocalScopeId;
        std::shared_ptr<TEventFilter> EventFilter;
        TString Cookie; // unique random identifier of a node instance (generated randomly at every start)
        std::unordered_map<ui16, TString> ChannelName;

        struct TVersionInfo {
            TString Tag; // version tag for this node
            TSet<TString> AcceptedTags; // we accept all enlisted version tags of peer nodes, but no others; empty = accept all
        };

        TMaybe<TVersionInfo> VersionInfo;

        using TPtr = TIntrusivePtr<TInterconnectProxyCommon>;
    };

}

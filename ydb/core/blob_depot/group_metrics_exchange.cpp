#include "blob_depot_tablet.h"
#include "data.h"
#include "space_monitor.h"

namespace NKikimr::NBlobDepot {

    void TBlobDepot::DoGroupMetricsExchange() {
        std::set<ui32> groups;

        for (const auto& channel : Info()->Channels) {
            if (auto *entry = channel.LatestEntry()) {
                groups.insert(entry->GroupID);
            }
        }

        auto ev = std::make_unique<TEvBlobStorage::TEvControllerGroupMetricsExchange>();
        auto& record = ev->Record;
        for (const ui32 groupId : groups) {
            record.AddGroupsToQuery(groupId);
        }

        Send(MakeBlobStorageNodeWardenID(SelfId().NodeId()), ev.release());
        TActivationContext::Schedule(TDuration::Seconds(10), new IEventHandle(TEvPrivate::EvDoGroupMetricsExchange, 0,
            SelfId(), {}, nullptr, 0));
    }

    void TBlobDepot::Handle(TEvBlobStorage::TEvControllerGroupMetricsExchange::TPtr ev) {
        STLOG(PRI_DEBUG, BLOB_DEPOT, BDT58, "TEvControllerGroupMetricsExchange", (Id, GetLogId()), (Msg, ev->Get()->Record));

        if (Config.HasVirtualGroupId()) {
            auto response = std::make_unique<TEvBlobStorage::TEvControllerGroupMetricsExchange>();
            auto& record = response->Record;
            auto *m = record.AddGroupMetrics();
            m->SetGroupId(Config.GetVirtualGroupId());
            auto *params = m->MutableGroupParameters();

            NKikimrBlobStorage::TPDiskSpaceColor::E systemColor = NKikimrBlobStorage::TPDiskSpaceColor::GREEN;
            NKikimrBlobStorage::TPDiskSpaceColor::E dataColor = NKikimrBlobStorage::TPDiskSpaceColor::BLACK;

            std::unordered_map<ui32, const NKikimrBlobStorage::TGroupMetrics*> metrics;
            for (const auto& m : ev->Get()->Record.GetGroupMetrics()) {
                metrics.emplace(m.GetGroupId(), &m);
            }

            auto addResources = [](auto *to, const auto& from) {
                to->SetSpace(to->GetSpace() + from.GetSpace());
                to->SetIOPS(to->GetIOPS() + from.GetIOPS());
                to->SetReadThroughput(to->GetReadThroughput() + from.GetReadThroughput());
                to->SetWriteThroughput(to->GetWriteThroughput() + from.GetWriteThroughput());
            };

            std::unordered_set<ui32> processedDataGroups;

            for (const auto& channel : Info()->Channels) {
                if (auto *entry = channel.LatestEntry(); entry && channel.Channel < Channels.size()) {
                    TChannelInfo& ch = Channels[channel.Channel];
                    if (const auto it = metrics.find(entry->GroupID); it != metrics.end()) {
                        const auto& p = it->second->GetGroupParameters();
                        switch (ch.ChannelKind) {
                            case NKikimrBlobDepot::TChannelKind::System:
                                systemColor = Max(systemColor, p.GetSpaceColor());
                                break;

                            case NKikimrBlobDepot::TChannelKind::Data:
                                if (processedDataGroups.insert(entry->GroupID).second) { // count each data group exactly once
                                    dataColor = Min(dataColor, p.GetSpaceColor());
                                    params->SetAvailableSize(params->GetAvailableSize() + p.GetAvailableSize());
                                    if (p.HasAssuredResources()) {
                                        addResources(params->MutableAssuredResources(), p.GetAssuredResources());
                                    }
                                    if (p.HasCurrentResources()) {
                                        addResources(params->MutableCurrentResources(), p.GetCurrentResources());
                                    }
                                }
                                break;

                            case NKikimrBlobDepot::TChannelKind::Log:
                                break;
                        }
                    }
                }
            }

            auto ev = std::make_unique<NNodeWhiteboard::TEvWhiteboard::TEvBSGroupStateUpdate>();
            auto& wb = ev->Record;
            wb.SetGroupID(Config.GetVirtualGroupId());
            wb.SetAllocatedSize(Data->GetTotalStoredDataSize());
            wb.SetAvailableSize(params->GetAvailableSize());
            wb.SetReadThroughput(0);
            wb.SetWriteThroughput(0);
            Send(NNodeWhiteboard::MakeNodeWhiteboardServiceId(SelfId().NodeId()), ev.release());

            params->SetAllocatedSize(Data->GetTotalStoredDataSize());
            Send(MakeBlobStorageNodeWardenID(SelfId().NodeId()), response.release());

            // TODO(alexvru): use a better approach
            const double approximateFreeSpaceShare = (double)params->GetAvailableSize() / (params->GetAvailableSize() +
                params->GetAllocatedSize());

            SpaceMonitor->SetSpaceColor(dataColor, approximateFreeSpaceShare); // the best data channel space color works for the whole depot
        }
    }

} // NKikimr::NBlobDepot

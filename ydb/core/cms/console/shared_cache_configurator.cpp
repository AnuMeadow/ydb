#include "shared_cache_configurator.h"
#include "configs_dispatcher.h"
#include "console.h"

#include <ydb/core/tablet_flat/shared_sausagecache.h>

#include <library/cpp/actors/core/actor_bootstrapped.h>

namespace NKikimr::NConsole {

class TSharedCacheConfigurator : public TActorBootstrapped<TSharedCacheConfigurator> {
public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::SHARED_CACHE_CONFIGURATOR;
    }

    TSharedCacheConfigurator() = default;

    void Bootstrap(const TActorContext& ctx) {
        LOG_DEBUG_S(ctx, NKikimrServices::CMS_CONFIGS, "TSharedCacheConfigurator Bootstrap");

        ui32 item1 = (ui32)NKikimrConsole::TConfigItem::BootstrapConfigItem;
        ui32 item2 = (ui32)NKikimrConsole::TConfigItem::SharedCacheConfigItem;
        ctx.Send(MakeConfigsDispatcherID(SelfId().NodeId()),
                new TEvConfigsDispatcher::TEvSetConfigSubscriptionRequest({ item1, item2 }));
        Become(&TThis::StateWork);
    }

    void Handle(TEvConsole::TEvConfigNotificationRequest::TPtr& ev, const TActorContext& ctx) {
        const auto& record = ev->Get()->Record;

        LOG_INFO_S(ctx, NKikimrServices::CMS_CONFIGS,
                "TSharedCacheConfigurator: got new config: " << record.GetConfig().ShortDebugString());

        NKikimrSharedCache::TSharedCacheConfig cfg;
        if (record.GetConfig().HasBootstrapConfig() && record.GetConfig().GetBootstrapConfig().HasSharedCacheConfig()) {
            cfg.MergeFrom(record.GetConfig().GetBootstrapConfig().GetSharedCacheConfig());
        }
        if (record.GetConfig().HasSharedCacheConfig()) {
            cfg.MergeFrom(record.GetConfig().GetSharedCacheConfig());
        }

        ApplyConfig(std::move(cfg), ctx);

        auto response = MakeHolder<TEvConsole::TEvConfigNotificationResponse>(record);
        ctx.Send(ev->Sender, response.Release(), 0, ev->Cookie);
    }

    void ApplyConfig(NKikimrSharedCache::TSharedCacheConfig&& cfg, const TActorContext& ctx) {
        auto event = MakeHolder<TEvSharedPageCache::TEvConfigure>();
        event->Record.Swap(&cfg);

        LOG_DEBUG_S(ctx, NKikimrServices::CMS_CONFIGS,
                "Applying new shared cache config: " << event->Record.ShortDebugString());

        ctx.Send(MakeSharedPageCacheId(0), event.Release());
    }

    STFUNC(StateWork) {
        switch (ev->GetTypeRewrite()) {
            HFunc(TEvConsole::TEvConfigNotificationRequest, Handle);
            IgnoreFunc(TEvConfigsDispatcher::TEvSetConfigSubscriptionResponse);

        default:
            Y_FAIL("unexpected event type: %" PRIx32 " event: %s",
                   ev->GetTypeRewrite(), ev->HasEvent() ? ev->GetBase()->ToString().data() : "serialized?");
            break;
        }
    }
};

IActor *CreateSharedCacheConfigurator()
{
    return new TSharedCacheConfigurator();
}

} // namespace NKikimr::NConsole

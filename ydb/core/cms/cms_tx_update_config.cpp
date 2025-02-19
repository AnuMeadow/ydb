#include "cms_impl.h"
#include "scheme.h"
#include "sentinel.h"

namespace NKikimr::NCms {

class TCms::TTxUpdateConfig : public TTransactionBase<TCms> {
public:
    TTxUpdateConfig(TCms *self,
                    const NKikimrCms::TCmsConfig &config,
                    TAutoPtr<IEventHandle> response,
                    ui64 subscriptionId = 0)
        : TBase(self)
        , Config(config)
        , Response(response)
        , SubscriptionId(subscriptionId)
        , Modify(false)
    {
    }

    TTxType GetTxType() const override { return TXTYPE_UPDATE_CONFIG; }

    bool Execute(TTransactionContext &txc, const TActorContext &ctx) override
    {
        LOG_DEBUG_S(ctx, NKikimrServices::CMS,
                    "TTxUpdateConfig Execute");

        if (SubscriptionId != Self->ConfigSubscriptionId) {
            LOG_ERROR_S(ctx, NKikimrServices::CMS,
                        "Config subscription id mismatch (" << SubscriptionId
                        << " vs expected " << Self->ConfigSubscriptionId << ")");
            return true;
        }

        NIceDb::TNiceDb db(txc.DB);
        db.Table<Schema::Param>().Key(1)
            .Update<Schema::Param::Config>(Config);

        Modify = true;

        return true;
    }

    void Complete(const TActorContext &ctx) override
    {
        LOG_DEBUG(ctx, NKikimrServices::CMS, "TTxUpdateConfig Complete");

        if (Modify) {
            Self->State->Config.Deserialize(Config);

            LOG_DEBUG_S(ctx, NKikimrServices::CMS,
                        "Updated config: " << Config.ShortDebugString());

            ctx.Send(Response.Release());
        }

        if (Self->State->Config.SentinelConfig.Enable) {
            if (!Self->State->Sentinel) {
                Self->State->Sentinel = Self->RegisterWithSameMailbox(CreateSentinel(Self->State));
            }
        } else {
            if (Self->State->Sentinel) {
                ctx.Send(Self->State->Sentinel, new TEvents::TEvPoisonPill());
                Self->State->Sentinel = TActorId();
            }
        }
    }

private:
    NKikimrCms::TCmsConfig Config;
    TAutoPtr<IEventHandle> Response;
    ui64 SubscriptionId;
    bool Modify;
};

ITransaction *TCms::CreateTxUpdateConfig(TEvConsole::TEvConfigNotificationRequest::TPtr &ev)
{
    auto &rec = ev->Get()->Record;

    auto response = MakeHolder<TEvConsole::TEvConfigNotificationResponse>();
    response->Record.SetSubscriptionId(rec.GetSubscriptionId());
    response->Record.MutableConfigId()->CopyFrom(rec.GetConfigId());

    return new TTxUpdateConfig(this, rec.GetConfig().GetCmsConfig(),
                               new IEventHandle(ev->Sender, ev->Recipient,
                                                response.Release(), 0, ev->Cookie),
                               rec.GetSubscriptionId());
}

ITransaction *TCms::CreateTxUpdateConfig(TEvCms::TEvSetConfigRequest::TPtr &ev)
{
    TAutoPtr<TEvCms::TEvSetConfigResponse> response
        = new TEvCms::TEvSetConfigResponse;
    response->Record.MutableStatus()->SetCode(NKikimrCms::TStatus::OK);
    return new TTxUpdateConfig(this, ev->Get()->Record.GetConfig(),
                               new IEventHandle(ev->Sender, ev->Recipient,
                                                response.Release(), 0, ev->Cookie),
                               ConfigSubscriptionId);
}

} // namespace NKikimr::NCms

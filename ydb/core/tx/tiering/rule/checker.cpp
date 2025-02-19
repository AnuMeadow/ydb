#include "checker.h"
#include "ss_checker.h"

#include <ydb/core/tx/schemeshard/schemeshard.h>
#include <ydb/core/tx/tiering/external_data.h>
#include <ydb/core/tx/tiering/rule/ss_fetcher.h>
#include <ydb/services/bg_tasks/abstract/interface.h>
#include <ydb/services/metadata/secret/snapshot.h>
#include <ydb/services/metadata/secret/fetcher.h>

namespace NKikimr::NColumnShard::NTiers {

void TRulePreparationActor::StartChecker() {
    if (!Tierings || !Secrets || !SSCheckResult) {
        return;
    }
    auto g = PassAwayGuard();
    if (!SSCheckResult->GetContent().GetOperationAllow()) {
        Controller->PreparationProblem(SSCheckResult->GetContent().GetDenyReason());
        return;
    }

    for (auto&& tiering : Objects) {
        for (auto&& interval : tiering.GetIntervals()) {
            auto tier = Tierings->GetTierById(interval.GetTierName());
            if (!tier) {
                Controller->PreparationProblem("unknown tier usage: " + interval.GetTierName());
                return;
            } else if (!Secrets->CheckSecretAccess(tier->GetProtoConfig().GetObjectStorage().GetAccessKey(), Context.GetUserToken())) {
                Controller->PreparationProblem("no access for secret: " + tier->GetProtoConfig().GetObjectStorage().GetAccessKey());
                return;
            } else if (!Secrets->CheckSecretAccess(tier->GetProtoConfig().GetObjectStorage().GetSecretKey(), Context.GetUserToken())) {
                Controller->PreparationProblem("no access for secret: " + tier->GetProtoConfig().GetObjectStorage().GetSecretKey());
                return;
            }
        }
    }
    Controller->PreparationFinished(std::move(Objects));
}

void TRulePreparationActor::Handle(NSchemeShard::TEvSchemeShard::TEvProcessingResponse::TPtr& ev) {
    auto& proto = ev->Get()->Record;
    if (proto.HasError()) {
        Controller->PreparationProblem(proto.GetError().GetErrorMessage());
        PassAway();
    } else if (proto.HasContent()) {
        SSCheckResult = SSFetcher->UnpackResult(ev->Get()->Record.GetContent().GetData());
        if (!SSCheckResult) {
            Controller->PreparationProblem("cannot unpack ss-fetcher result for class " + SSFetcher->GetClassName());
            PassAway();
        } else {
            StartChecker();
        }
    } else {
        Y_VERIFY(false);
    }
}

void TRulePreparationActor::Handle(NMetadata::NProvider::TEvRefreshSubscriberData::TPtr& ev) {
    if (auto snapshot = ev->Get()->GetSnapshotPtrAs<TConfigsSnapshot>()) {
        Tierings = snapshot;
    } else if (auto snapshot = ev->Get()->GetSnapshotPtrAs<NMetadata::NSecret::TSnapshot>()) {
        Secrets = snapshot;
    } else {
        Y_VERIFY(false);
    }
    StartChecker();
}

void TRulePreparationActor::Bootstrap() {
    Become(&TThis::StateMain);
    Send(NMetadata::NProvider::MakeServiceId(SelfId().NodeId()),
        new NMetadata::NProvider::TEvAskSnapshot(std::make_shared<TSnapshotConstructor>()));
    Send(NMetadata::NProvider::MakeServiceId(SelfId().NodeId()),
        new NMetadata::NProvider::TEvAskSnapshot(std::make_shared<NMetadata::NSecret::TSnapshotsFetcher>()));
    {
        SSFetcher = std::make_shared<TFetcherCheckUserTieringPermissions>();
        SSFetcher->SetUserToken(Context.GetUserToken());
        SSFetcher->SetActivityType(Context.GetActivityType());
        for (auto&& i : Objects) {
            SSFetcher->MutableTieringRuleIds().emplace(i.GetTieringRuleId());
        }
        Register(new TSSFetchingActor(SSFetcher, std::make_shared<TSSFetchingController>(SelfId()), TDuration::Seconds(10)));
    }
}

TRulePreparationActor::TRulePreparationActor(std::vector<TTieringRule>&& objects,
    NMetadata::NModifications::IAlterPreparationController<TTieringRule>::TPtr controller,
    const NMetadata::NModifications::IOperationsManager::TModificationContext& context)
    : Objects(std::move(objects))
    , Controller(controller)
    , Context(context)
{

}

}

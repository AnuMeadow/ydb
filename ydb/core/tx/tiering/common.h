#pragma once
#include <ydb/core/base/events.h>

#include <ydb/library/accessor/accessor.h>

#include <library/cpp/actors/core/events.h>

namespace NKikimr::NColumnShard::NTiers {

enum EEvents {
    EvTierCleared = EventSpaceBegin(TKikimrEvents::ES_TIERING),
    EvSSFetchingResult,
    EvSSFetchingProblem,
    EvTimeout,
    EvTiersManagerReadyForUsage,
    EvEnd
};

class TEvTiersManagerReadyForUsage: public TEventLocal<TEvTiersManagerReadyForUsage, EvTiersManagerReadyForUsage> {

};

static_assert(EEvents::EvEnd < EventSpaceEnd(TKikimrEvents::ES_TIERING), "expect EvEnd < EventSpaceEnd(TKikimrEvents::ES_TIERING)");
}

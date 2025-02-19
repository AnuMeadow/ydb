#pragma once

#include <ydb/core/yq/libs/common/util.h>
#include <ydb/core/yq/libs/config/protos/control_plane_storage.pb.h>
#include <ydb/core/yq/libs/control_plane_storage/events/events.h>

#include <google/protobuf/timestamp.pb.h>

#include <util/datetime/base.h>

namespace NYq {

class TRetryPolicyItem {
public:
    TRetryPolicyItem() = default;
    TRetryPolicyItem(ui64 retryCount, const TDuration& retryPeriod, const TDuration& backoffPeriod)
    : RetryCount(retryCount), RetryPeriod(retryPeriod), BackoffPeriod(backoffPeriod)
    { }
    ui64 RetryCount = 0;
    TDuration RetryPeriod = TDuration::Zero();
    TDuration BackoffPeriod = TDuration::Zero();
};

class TRetryLimiter {
public:
    TRetryLimiter() = default;
    TRetryLimiter(ui64 retryCount, const TInstant& retryCounterUpdatedAt, double retryRate);
    void Assign(ui64 retryCount, const TInstant& retryCounterUpdatedAt, double retryRate);
    bool UpdateOnRetry(const TInstant& lastSeenAt, const TRetryPolicyItem& policy, const TInstant now = Now());
    ui64 RetryCount = 0;
    TInstant RetryCounterUpdatedAt = TInstant::Zero();
    double RetryRate = 0.0;
};

bool IsTerminalStatus(YandexQuery::QueryMeta::ComputeStatus status);

bool IsAbortedStatus(YandexQuery::QueryMeta::ComputeStatus status);

TDuration GetDuration(const TString& value, const TDuration& defaultValue);

NConfig::TControlPlaneStorageConfig FillDefaultParameters(NConfig::TControlPlaneStorageConfig config);

bool DoesPingTaskUpdateQueriesTable(const Fq::Private::PingTaskRequest& request);

NYdb::TValue PackItemsToList(const TVector<NYdb::TValue>& items);

std::pair<TString, TString> SplitId(const TString& id, char delim = '-');

bool IsValidIntervalUnit(const TString& unit);

bool IsValidDateTimeFormatName(const TString& formatName);

bool IsValidTimestampFormatName(const TString& formatName);

} // namespace NYq

#pragma once
#include "config.h"

#include <ydb/library/aclib/aclib.h>
#include <ydb/services/metadata/initializer/common.h>

#include <library/cpp/actors/core/actor.h>
#include <library/cpp/actors/core/actorid.h>

namespace NKikimr::NBackgroundTasks {

class TTask;

class TExecutorController: public NMetadata::NInitializer::IInitializerOutput {
private:
    const NActors::TActorIdentity ExecutorActorId;
    YDB_READONLY_DEF(TConfig, Config);
    const NACLib::TUserToken UserToken;
public:
    using TPtr = std::shared_ptr<TExecutorController>;
    TExecutorController(const NActors::TActorIdentity& executorActorId, const TConfig& config)
        : ExecutorActorId(executorActorId)
        , Config(config)
        , UserToken(NACLib::TSystemUsers::Metadata())
    {

    }

    const NACLib::TUserToken& GetUserToken() const {
        return UserToken;
    }

    TString GetTableName() const {
        return Config.GetTablePath();
    }

    const NMetadata::NRequest::TConfig& GetRequestConfig() const {
        return Config.GetRequestConfig();
    }

    virtual void InitializationFinished(const TString& id) const override;
    void LockPingerFinished() const;
    void TaskFetched(const TTask& task) const;
    void TaskFinished(const TString& taskId) const;
    void AssignFinished() const;
    void FetchingFinished() const;
};

}

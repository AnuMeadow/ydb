#pragma once

#include "ydb_command.h"
#include "ydb_common.h"

#include <ydb/public/sdk/cpp/client/ydb_table/table.h>
#include <ydb/public/sdk/cpp/client/draft/ydb_scripting.h>
#include <ydb/public/lib/ydb_cli/common/format.h>
#include <ydb/public/lib/ydb_cli/common/interruptible.h>
#include <ydb/public/lib/ydb_cli/common/parameters.h>
#include <ydb/public/lib/json_value/ydb_json_value.h>

namespace NYdb {
namespace NConsoleClient {

class TCommandTable : public TClientCommandTree {
public:
    TCommandTable();
};

class TCommandQuery : public TClientCommandTree {
public:
    TCommandQuery();
};

class TCommandIndex : public TClientCommandTree {
public:
    TCommandIndex();
};

class TCommandAttribute : public TClientCommandTree {
public:
    TCommandAttribute();
};

class TCommandTtl: public TClientCommandTree {
public:
    TCommandTtl();
};

class TTableCommand : public TYdbOperationCommand {
public:
    TTableCommand(
        const TString& name,
        const std::initializer_list<TString>& aliases = std::initializer_list<TString>(),
        const TString& description = TString()
    );

    virtual void Config(TConfig& config) override;
    NYdb::NTable::TSession GetSession(TConfig& config);
};

class TCommandCreateTable : public TTableCommand, public TCommandWithPath {
public:
    TCommandCreateTable();
    virtual void Config(TConfig& config) override;
    virtual void Parse(TConfig& config) override;
    virtual int Run(TConfig& config) override;

private:
    TVector<TString> Columns;
    TVector<TString> PrimaryKeys;
    TVector<TString> Indexes;
    TString PresetName;
    TString ExecutionPolicy;
    TString CompactionPolicy;
    TString PartitioningPolicy;
    TString AutoPartitioning;
    TString UniformPartitions;
    TString ReplicationPolicy;
    TString ReplicasCount;
    bool CreatePerAvailabilityZone = false;
    bool AllowPromotion = false;
};

class TCommandDropTable : public TTableCommand, public TCommandWithPath {
public:
    TCommandDropTable();
    virtual void Config(TConfig& config) override;
    virtual void Parse(TConfig& config) override;
    virtual int Run(TConfig& config) override;
};

class TCommandQueryBase {
protected:
    void CheckQueryOptions() const;
    void CheckQueryFile();

protected:
    TString Query;
    TString QueryFile;
};

class TCommandExecuteQuery : public TTableCommand, TCommandQueryBase, TCommandWithParameters,
    public TCommandWithFormat, public TInterruptibleCommand
{
public:
    TCommandExecuteQuery();
    virtual void Config(TConfig& config) override;
    virtual void Parse(TConfig& config) override;
    virtual int Run(TConfig& config) override;

    int ExecuteDataQuery(TConfig& config);
    void PrintDataQueryResponse(NTable::TDataQueryResult& result);

    int ExecuteSchemeQuery(TConfig& config);

    int ExecuteScanQuery(TConfig& config);
    void PrintScanQueryResponse(NTable::TScanQueryPartIterator& result);

private:
    TString CollectStatsMode;
    TString TxMode;
    TString QueryType;
    bool BasicStats = false;
};

class TCommandExplain : public TTableCommand, public TCommandWithFormat, TCommandQueryBase, TInterruptibleCommand {
public:
    TCommandExplain();
    virtual void Config(TConfig& config) override;
    virtual void Parse(TConfig& config) override;
    virtual int Run(TConfig& config) override;

private:

    bool PrintAst = false;
    TString QueryType;
    bool Analyze = false;
};

class TCommandReadTable : public TYdbCommand, public TCommandWithPath,
    public TCommandWithFormat, public TInterruptibleCommand
{
public:
    TCommandReadTable();
    virtual void Config(TConfig& config) override;
    virtual void Parse(TConfig& config) override;
    virtual int Run(TConfig& config) override;

private:
    void PrintResponse(NTable::TTablePartIterator& result);

    bool Ordered = false;
    bool CountOnly = false;
    bool FromExclusive = false;
    bool ToExclusive = false;
    ui64 RowLimit = 0;
    TString Columns;
    TString From;
    TString To;
};

class TCommandIndexAdd : public TClientCommandTree {
public:
    TCommandIndexAdd();
};

class TCommandIndexAddGlobal : public TYdbCommand, public TCommandWithPath, public TCommandWithFormat {
public:
    TCommandIndexAddGlobal(
        NTable::EIndexType type,
        const TString& name,
        const std::initializer_list<TString>& aliases = std::initializer_list<TString>(),
        const TString& description = TString()
    );
    virtual void Config(TConfig& config) override;
    virtual void Parse(TConfig& config) override;
    virtual int Run(TConfig& config) override;
protected:
    TString IndexName;
    TString Columns;
    TString DataColumns;
private:
    const NTable::EIndexType IndexType;
};

class TCommandIndexAddGlobalSync : public TCommandIndexAddGlobal {
public:
    TCommandIndexAddGlobalSync();
};

class TCommandIndexAddGlobalAsync : public TCommandIndexAddGlobal {
public:
    TCommandIndexAddGlobalAsync();
};

class TCommandIndexDrop : public TYdbCommand, public TCommandWithPath {
public:
    TCommandIndexDrop();
    virtual void Config(TConfig& config) override;
    virtual void Parse(TConfig& config) override;
    virtual int Run(TConfig& config) override;
private:
    TString IndexName;
};

class TCommandAttributeAdd : public TYdbCommand, public TCommandWithPath {
public:
    TCommandAttributeAdd();
    virtual void Config(TConfig& config) override;
    virtual void Parse(TConfig& config) override;
    virtual int Run(TConfig& config) override;
private:
    THashMap<TString, TString> Attributes;
};

class TCommandAttributeDrop : public TYdbCommand, public TCommandWithPath {
public:
    TCommandAttributeDrop();
    virtual void Config(TConfig& config) override;
    virtual void Parse(TConfig& config) override;
    virtual int Run(TConfig& config) override;
private:
    TVector<TString> AttributeKeys;
};

class TCommandTtlSet : public TYdbCommand, public TCommandWithPath {
public:
    TCommandTtlSet();
    virtual void Config(TConfig& config) override;
    virtual void Parse(TConfig& config) override;
    virtual int Run(TConfig& config) override;
private:
    TString ColumnName;
    TMaybe<NTable::TTtlSettings::EUnit> ColumnUnit;
    TDuration ExpireAfter;
    TDuration RunInterval = TDuration::Zero();
};

class TCommandTtlReset : public TYdbCommand, public TCommandWithPath {
public:
    TCommandTtlReset();
    virtual void Config(TConfig& config) override;
    virtual void Parse(TConfig& config) override;
    virtual int Run(TConfig& config) override;
};

}
}

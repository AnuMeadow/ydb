#include <ydb/core/kqp/ut/common/kqp_ut_common.h>

#include <ydb/public/sdk/cpp/client/ydb_proto/accessor.h>
#include <ydb/public/sdk/cpp/client/ydb_table/table.h>

#include <library/cpp/json/json_reader.h>
#include <library/cpp/threading/local_executor/local_executor.h>

#include <util/string/printf.h>

namespace NKikimr {
namespace NKqp {

using namespace NYdb;
using namespace NYdb::NTable;
using namespace NYdb::NScripting;

namespace {

NYdb::NTable::TDataQueryResult ExecuteDataQuery(TSession& session, const TString& query) {
    const auto txSettings = TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx();
    return session.ExecuteDataQuery(query, txSettings,
        TExecDataQuerySettings().KeepInQueryCache(true)).ExtractValueSync();
}

NYdb::NTable::TDataQueryResult ExecuteDataQuery(TSession& session, const TString& query, TParams& params) {
    const auto txSettings = TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx();
    return session.ExecuteDataQuery(query, txSettings, params,
        TExecDataQuerySettings().KeepInQueryCache(true)).ExtractValueSync();
}

void CreateTableWithMultishardIndex(Tests::TClient& client, bool async) {
    const TString scheme =  R"(Name: "MultiShardIndexed"
        Columns { Name: "key"    Type: "Uint64" }
        Columns { Name: "fk"    Type: "Uint32" }
        Columns { Name: "value"  Type: "Utf8" }
        KeyColumnNames: ["key"]
        SplitBoundary { KeyPrefix { Tuple { Optional { Uint64: 3 } } } }
        SplitBoundary { KeyPrefix { Tuple { Optional { Uint64: 100 } } } }
    )";

    NKikimrSchemeOp::TTableDescription desc;
    bool parseOk = ::google::protobuf::TextFormat::ParseFromString(scheme, &desc);
    UNIT_ASSERT(parseOk);
    NKikimrSchemeOp::EIndexType type = async
        ? NKikimrSchemeOp::EIndexType::EIndexTypeGlobalAsync
        : NKikimrSchemeOp::EIndexType::EIndexTypeGlobal;

    auto status = client.TClient::CreateTableWithUniformShardedIndex("/Root", desc, "index", {"fk"}, type);
    UNIT_ASSERT_VALUES_EQUAL(status, NMsgBusProxy::MSTATUS_OK);
}

void CreateTableWithMultishardIndexAndDataColumn(Tests::TClient& client) {
    const TString scheme =  R"(Name: "MultiShardIndexedWithDataColumn"
        Columns { Name: "key"    Type: "Uint64" }
        Columns { Name: "fk"    Type: "Uint32" }
        Columns { Name: "value"  Type: "Utf8" }
        Columns { Name: "ext_value"  Type: "Utf8" }
        KeyColumnNames: ["key"]
        SplitBoundary { KeyPrefix { Tuple { Optional { Uint64: 3 } } } }
        SplitBoundary { KeyPrefix { Tuple { Optional { Uint64: 100 } } } }
    )";

    NKikimrSchemeOp::TTableDescription desc;
    bool parseOk = ::google::protobuf::TextFormat::ParseFromString(scheme, &desc);
    UNIT_ASSERT(parseOk);

    auto status = client.TClient::CreateTableWithUniformShardedIndex("/Root", desc, "index", {"fk"}, NKikimrSchemeOp::EIndexType::EIndexTypeGlobal, {"value"});
    UNIT_ASSERT_VALUES_EQUAL(status, NMsgBusProxy::MSTATUS_OK);
}

void FillTableWithDataColumn(NYdb::NTable::TTableClient& db) {
    auto param = db.GetParamsBuilder()
        .AddParam("$rows")
            .BeginList()
            .AddListItem()
                .BeginStruct()
                    .AddMember("key").Uint64(1)
                    .AddMember("fk").Uint32(1000000000u)
                    .AddMember("value").Utf8("v1")
                .EndStruct()
            .AddListItem()
                .BeginStruct()
                    .AddMember("key").Uint64(2)
                    .AddMember("fk").Uint32(2000000000u)
                    .AddMember("value").Utf8("v2")
                .EndStruct()
            .AddListItem()
                .BeginStruct()
                    .AddMember("key").Uint64(3)
                    .AddMember("fk").Uint32(3000000000u)
                    .AddMember("value").Utf8("v3")
                .EndStruct()
            .AddListItem()
                .BeginStruct()
                    .AddMember("key").Uint64(4)
                    .AddMember("fk").Uint32(4294967295u)
                    .AddMember("value").Utf8("v4")
                .EndStruct()
            .EndList()
            .Build()
        .Build();

    const TString query(R"(
        DECLARE $rows AS List < Struct<key: Uint64, fk: Uint32, value: Utf8 > >;
        UPSERT INTO `/Root/MultiShardIndexedWithDataColumn` (key, fk, value)
        SELECT key, fk, value FROM AS_TABLE($rows);
    )");

    auto session = db.CreateSession().GetValueSync().GetSession();
    auto result = ExecuteDataQuery(session, query, param);
    UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
}

void FillTable(NYdb::NTable::TSession& session) {
    const TString query(Q_(R"(
        UPSERT INTO `/Root/MultiShardIndexed` (key, fk, value) VALUES
        (1, 1000000000, "v1"),
        (2, 2000000000, "v2"),
        (3, 3000000000, "v3"),
        (4, 4294967295, "v4");
    )"));

    auto result = ExecuteDataQuery(session, query);
    UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
}

}

Y_UNIT_TEST_SUITE(KqpMultishardIndex) {
    Y_UNIT_TEST(SortedRangeReadDesc) {
        TKikimrRunner kikimr(SyntaxV1Settings());
        CreateTableWithMultishardIndex(kikimr.GetTestClient(), false);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        FillTable(session);

        {
            const TString query(Q_(R"(
                SELECT * FROM `/Root/MultiShardIndexed` VIEW index ORDER BY fk DESC LIMIT 1;
            )"));

            auto result = ExecuteDataQuery(session, query);
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[4294967295u];[4u];[\"v4\"]]]");
        }
    }

    Y_UNIT_TEST(SecondaryIndexSelect) {
        TKikimrRunner kikimr(SyntaxV1Settings());
        CreateTableWithMultishardIndex(kikimr.GetTestClient(), false);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        FillTable(session);

        {
            const TString query(Q1_(R"(
                SELECT key FROM `/Root/MultiShardIndexed` VIEW index WHERE fk = 2000000000;
            )"));

            auto result = ExecuteDataQuery(session, query);
            UNIT_ASSERT_C(result.GetIssues().Empty(), result.GetIssues().ToString());
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[2u]]]");
        }

        {
            const TString query(Q1_(R"(
                SELECT key, fk FROM `/Root/MultiShardIndexed` VIEW index WHERE fk > 1000000000 ORDER BY fk LIMIT 1;
            )"));

            auto result = ExecuteDataQuery(session, query);
            UNIT_ASSERT_C(result.GetIssues().Empty(), result.GetIssues().ToString());
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[2u];[2000000000u]]]");
        }

        {
            const TString query(Q1_(R"(
                SELECT value FROM `/Root/MultiShardIndexed` VIEW index WHERE fk = 2000000000;
            )"));

            auto result = ExecuteDataQuery(session, query);
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[\"v2\"]]]");
        }

        {
            const TString query(Q1_(R"(
                SELECT fk, value FROM `/Root/MultiShardIndexed` VIEW index WHERE fk > 1000000000 ORDER BY fk LIMIT 1;
            )"));

            auto result = ExecuteDataQuery(session, query);
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[2000000000u];[\"v2\"]]]");
        }

        {
            const TString query(Q1_(R"(
                SELECT key FROM `/Root/MultiShardIndexed` VIEW index WHERE fk = 2000000000 AND key = 2;
            )"));

            auto result = ExecuteDataQuery(session, query);
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[2u]]]");
        }

        {
            const TString query(Q1_(R"(
                SELECT value FROM `/Root/MultiShardIndexed` VIEW index WHERE fk = 2000000000 AND key = 2;
            )"));

            auto result = ExecuteDataQuery(session, query);
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[\"v2\"]]]");
        }

        {
            const TString query(Q1_(R"(
                SELECT value FROM `/Root/MultiShardIndexed` VIEW index WHERE fk = 2000000000 AND value = "v2";
            )"));

            auto result = ExecuteDataQuery(session, query);
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[\"v2\"]]]");
        }

        {
            const TString query(Q1_(R"(
                SELECT key FROM `/Root/MultiShardIndexed` VIEW index WHERE fk = 2000000000 AND value = "v2";
            )"));

            auto result = ExecuteDataQuery(session, query);
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[2u]]]");
        }

        {
            const TString query(Q1_(R"(
                SELECT key, fk, value FROM `/Root/MultiShardIndexed` VIEW index WHERE fk = 2000000000 AND value = "v2" ORDER BY fk, key;
            )"));

            auto result = ExecuteDataQuery(session, query);
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[2u];[2000000000u];[\"v2\"]]]");
        }

        {
            const TString query(Q1_(R"(
                SELECT value FROM `/Root/MultiShardIndexed` VIEW index WHERE key = 2;
            )"));

            auto result = ExecuteDataQuery(session, query);

            UNIT_ASSERT_C(HasIssue(result.GetIssues(), NYql::TIssuesIds::KIKIMR_WRONG_INDEX_USAGE,
                [](const NYql::TIssue& issue) {
                    return issue.GetMessage().Contains("Given predicate is not suitable for used index: index");
                }), result.GetIssues().ToString());

            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[\"v2\"]]]");
        }

        {
            const TString query(Q1_(R"(
                SELECT fk FROM `/Root/MultiShardIndexed` VIEW index WHERE key = 2;
            )"));

            auto result = ExecuteDataQuery(session, query);

            UNIT_ASSERT_C(HasIssue(result.GetIssues(), NYql::TIssuesIds::KIKIMR_WRONG_INDEX_USAGE,
                [](const NYql::TIssue& issue) {
                    return issue.GetMessage().Contains("Given predicate is not suitable for used index: index");
                }), result.GetIssues().ToString());
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[2000000000u]]]");
        }

        {
            const TString query(Q1_(R"(
                SELECT value, key FROM `/Root/MultiShardIndexed` VIEW index WHERE key > 2 ORDER BY key DESC;
            )"));

            auto result = ExecuteDataQuery(session, query);

            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[\"v4\"];[4u]];[[\"v3\"];[3u]]]");
        }
    }

    Y_UNIT_TEST(YqWorksFineAfterAlterIndexTableDirectly) {
        TKikimrRunner kikimr(SyntaxV1Settings());
        CreateTableWithMultishardIndex(kikimr.GetTestClient(), false);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        FillTable(session);

        kikimr.GetTestServer().GetRuntime()->GetAppData().AdministrationAllowedSIDs.push_back("root@builtin");

        { // without token request is forbidded
            Tests::TClient& client = kikimr.GetTestClient();
            const TString scheme =  R"(
                Name: "indexImplTable"
                PartitionConfig {
                    PartitioningPolicy {
                        MinPartitionsCount: 1
                        SizeToSplit: 100500
                        FastSplitSettings {
                            SizeThreshold: 100500
                            RowCountThreshold: 100500
                        }
                    }
                }
            )";
            auto result = client.AlterTable("/Root/MultiShardIndexed/index", scheme, "user@builtin");
            UNIT_ASSERT_VALUES_EQUAL_C(result->Record.GetStatus(), NMsgBusProxy::MSTATUS_ERROR, "User must not be able to alter index impl table");
            UNIT_ASSERT_VALUES_EQUAL(result->Record.GetErrorReason(), "Administrative access denied");
        }

        { // with root token request is accepted
            Tests::TClient& client = kikimr.GetTestClient();
            const TString scheme =  R"(
                Name: "indexImplTable"
                PartitionConfig {
                    PartitioningPolicy {
                        MinPartitionsCount: 1
                        SizeToSplit: 100500
                        FastSplitSettings {
                            SizeThreshold: 100500
                            RowCountThreshold: 100500
                        }
                    }
                }
            )";
            auto result = client.AlterTable("/Root/MultiShardIndexed/index", scheme, "root@builtin");
            UNIT_ASSERT_VALUES_EQUAL_C(result->Record.GetStatus(), NMsgBusProxy::MSTATUS_OK, "Super user must be able to alter partition config");
        }

        { // after alter yql works fine
            const TString query(R"(
                SELECT * FROM `/Root/MultiShardIndexed` VIEW index ORDER BY fk DESC LIMIT 1;
            )");

            auto result = session.ExecuteDataQuery(
                                     query,
                                     TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                              .ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[4294967295u];[4u];[\"v4\"]]]");
        }

        FillTable(session);

        { // just for sure, public api got error when alter index
            auto settings = NYdb::NTable::TAlterTableSettings()
                .BeginAlterPartitioningSettings()
                    .SetPartitionSizeMb(50)
                    .SetMinPartitionsCount(4)
                    .SetMaxPartitionsCount(5)
                .EndAlterPartitioningSettings();

            auto result = session.AlterTable("/Root/MultiShardIndexed/index/indexImplTable", settings).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SCHEME_ERROR, result.GetIssues().ToString());
        }

        { // however public api is able to perform alter index if user has AlterSchema right and user is a member of the list AdministrationAllowedSIDs
            auto clSettings = NYdb::NTable::TClientSettings().AuthToken("root@builtin").UseQueryCache(false);
            auto client =  NYdb::NTable::TTableClient(kikimr.GetDriver(), clSettings);
            auto session = client.CreateSession().GetValueSync().GetSession();

            auto settings = NYdb::NTable::TAlterTableSettings()
                .BeginAlterPartitioningSettings()
                    .SetPartitionSizeMb(50)
                    .SetMinPartitionsCount(4)
                    .SetMaxPartitionsCount(5)
                .EndAlterPartitioningSettings();

            auto result = session.AlterTable("/Root/MultiShardIndexed/index/indexImplTable", settings).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());
        }

    }

    Y_UNIT_TEST_TWIN(DataColumnUpsertMixedSemantic, WithMvcc) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(WithMvcc)
            .SetEnableMvccSnapshotReads(WithMvcc)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateTableWithMultishardIndexAndDataColumn(kikimr.GetTestClient());
        FillTableWithDataColumn(db);

        // Just check table prepared
        {
            const auto yson = ReadTableToYson(session, "/Root/MultiShardIndexedWithDataColumn/index/indexImplTable");
            const TString expected = R"([[[1000000000u];[1u];["v1"]];[[2000000000u];[2u];["v2"]];[[3000000000u];[3u];["v3"]];[[4294967295u];[4u];["v4"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        // Upsert using pk and some other column. Index will be read from table value from user input.
        // For the first row - insert semantic, the pk absent in the table
        // for the second row - update semantic, the pk found
        // This test checks the implementation has correct handle not exact semantic for table lookup
        // (the lenth of lookup result is not equal to the lengh of the user input)
        {
            const TString query1(Q1_(R"(
                UPSERT INTO `/Root/MultiShardIndexedWithDataColumn` (key, value, ext_value) VALUES
                (0u, "v0_", "Something"),
                (1u, "v1_", "Something");
            )"));

            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }

        {
            const auto& yson = ReadTableToYson(session, "/Root/MultiShardIndexedWithDataColumn/index/indexImplTable");
            const TString expected = R"([[#;[0u];["v0_"]];[[1000000000u];[1u];["v1_"]];[[2000000000u];[2u];["v2"]];[[3000000000u];[3u];["v3"]];[[4294967295u];[4u];["v4"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
    }

    Y_UNIT_TEST_TWIN(DataColumnWriteNull, WithMvcc) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(WithMvcc)
            .SetEnableMvccSnapshotReads(WithMvcc)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateTableWithMultishardIndexAndDataColumn(kikimr.GetTestClient());
        FillTableWithDataColumn(db);

        // Just check table prepared
        {
            const auto& yson = ReadTableToYson(session, "/Root/MultiShardIndexedWithDataColumn/index/indexImplTable");
            const TString expected = R"([[[1000000000u];[1u];["v1"]];[[2000000000u];[2u];["v2"]];[[3000000000u];[3u];["v3"]];[[4294967295u];[4u];["v4"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        // Upsert using pk and some other column. Index will be read from table value from user input.
        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/MultiShardIndexedWithDataColumn` (key, value, ext_value) VALUES
                (1u, NULL, "Something");
            )"));

            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTableToYson(session, "/Root/MultiShardIndexedWithDataColumn/index/indexImplTable");
            const TString expected = R"([[[1000000000u];[1u];#];[[2000000000u];[2u];["v2"]];[[3000000000u];[3u];["v3"]];[[4294967295u];[4u];["v4"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
        // Upsert using pk and some other column. But pass null as input.

        {
            auto param = db.GetParamsBuilder()
                .AddParam("$rows")
                    .EmptyList(
                        TTypeBuilder()
                            .BeginStruct()
                                .AddMember("key").BeginOptional().Primitive(EPrimitiveType::Uint64).EndOptional()
                                .AddMember("value").BeginOptional().Primitive(EPrimitiveType::Utf8).EndOptional()
                                .AddMember("ext_value").BeginOptional().Primitive(EPrimitiveType::Utf8).EndOptional()
                            .EndStruct()
                        .Build()
                    )
                    .Build()
                .Build();

            const TString query1(Q1_(R"(
                DECLARE $rows AS List<Struct<
                    key : Uint64?,
                    value : Utf8?,
                    ext_value : Utf8?
                >>;
                UPSERT INTO `/Root/MultiShardIndexedWithDataColumn`
                SELECT * FROM AS_TABLE($rows);
            )"));

            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(), param)
                          .ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }

        {
            const auto& yson = ReadTableToYson(session, "/Root/MultiShardIndexedWithDataColumn/index/indexImplTable");
            const TString expected = R"([[[1000000000u];[1u];#];[[2000000000u];[2u];["v2"]];[[3000000000u];[3u];["v3"]];[[4294967295u];[4u];["v4"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
    }

    Y_UNIT_TEST_TWIN(DataColumnWrite, WithMvcc) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(WithMvcc)
            .SetEnableMvccSnapshotReads(WithMvcc)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateTableWithMultishardIndexAndDataColumn(kikimr.GetTestClient());
        FillTableWithDataColumn(db);

        // Upsert using previous inserved pk and fk, check data column realy updated
        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/MultiShardIndexedWithDataColumn` (key, fk, value) VALUES
                (4u, 4294967295u, "v4_1");
            )"));

            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTableToYson(session, "/Root/MultiShardIndexedWithDataColumn/index/indexImplTable");
            const TString expected = R"([[[1000000000u];[1u];["v1"]];[[2000000000u];[2u];["v2"]];[[3000000000u];[3u];["v3"]];[[4294967295u];[4u];["v4_1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        // Upsert using previous inserved pk but without fk, check data column still realy updated
        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/MultiShardIndexedWithDataColumn` (key, value) VALUES
                (4u, "v4_2");
            )"));

            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }

        {
            const auto& yson = ReadTableToYson(session, "/Root/MultiShardIndexedWithDataColumn/index/indexImplTable");
            const TString expected = R"([[[1000000000u];[1u];["v1"]];[[2000000000u];[2u];["v2"]];[[3000000000u];[3u];["v3"]];[[4294967295u];[4u];["v4_2"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        // Upsert using new pk without fk
        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/MultiShardIndexedWithDataColumn` (key, value) VALUES
                (4000000000u, "vvvv");
            )"));

            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTableToYson(session, "/Root/MultiShardIndexedWithDataColumn/index/indexImplTable");
            const TString expected = R"([[#;[4000000000u];["vvvv"]];[[1000000000u];[1u];["v1"]];[[2000000000u];[2u];["v2"]];[[3000000000u];[3u];["v3"]];[[4294967295u];[4u];["v4_2"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        // Upsert using pk, fk, and some other column. Data column in index must have old value
        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/MultiShardIndexedWithDataColumn` (key, fk, ext_value) VALUES
                (3u, 3000000000u, "Something");
            )"));

            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTableToYson(session, "/Root/MultiShardIndexedWithDataColumn/index/indexImplTable");
            const TString expected = R"([[#;[4000000000u];["vvvv"]];[[1000000000u];[1u];["v1"]];[[2000000000u];[2u];["v2"]];[[3000000000u];[3u];["v3"]];[[4294967295u];[4u];["v4_2"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        // Replace row
        {
            const TString query1(Q_(R"(
                REPLACE INTO `/Root/MultiShardIndexedWithDataColumn` (key, fk, value) VALUES
                (3u, 3000000000u, "v3_3");
            )"));

            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTableToYson(session, "/Root/MultiShardIndexedWithDataColumn/index/indexImplTable");
            const TString expected = R"([[#;[4000000000u];["vvvv"]];[[1000000000u];[1u];["v1"]];[[2000000000u];[2u];["v2"]];[[3000000000u];[3u];["v3_3"]];[[4294967295u];[4u];["v4_2"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        // Replace row but no specify data column
        {
            const TString query1(Q_(R"(
                REPLACE INTO `/Root/MultiShardIndexedWithDataColumn` (key, fk) VALUES
                (3u, 3000000000u);
            )"));

            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTableToYson(session, "/Root/MultiShardIndexedWithDataColumn/index/indexImplTable");
            const TString expected = R"([[#;[4000000000u];["vvvv"]];[[1000000000u];[1u];["v1"]];[[2000000000u];[2u];["v2"]];[[3000000000u];[3u];#];[[4294967295u];[4u];["v4_2"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        // Replace row specify data column but no index column
        {
            const TString query1(Q_(R"(
                REPLACE INTO `/Root/MultiShardIndexedWithDataColumn` (key, value) VALUES
                (2u, "v2_3");
            )"));

            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTableToYson(session, "/Root/MultiShardIndexedWithDataColumn/index/indexImplTable");
            const TString expected = R"([[#;[2u];["v2_3"]];[#;[4000000000u];["vvvv"]];[[1000000000u];[1u];["v1"]];[[3000000000u];[3u];#];[[4294967295u];[4u];["v4_2"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        // Replace just by pk
        {
            const TString query1(Q_(R"(
                REPLACE INTO `/Root/MultiShardIndexedWithDataColumn` (key) VALUES
                (2u);
            )"));

            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTableToYson(session, "/Root/MultiShardIndexedWithDataColumn/index/indexImplTable");
            const TString expected = R"([[#;[2u];#];[#;[4000000000u];["vvvv"]];[[1000000000u];[1u];["v1"]];[[3000000000u];[3u];#];[[4294967295u];[4u];["v4_2"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        // Delete
        {
            const TString query1(Q_(R"(
                DELETE FROM `/Root/MultiShardIndexedWithDataColumn` ON (key) VALUES
                (4000000000u), (1u);
            )"));

            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }

        {
            const auto& yson = ReadTableToYson(session, "/Root/MultiShardIndexedWithDataColumn/index/indexImplTable");
            const TString expected = R"([[#;[2u];#];[[3000000000u];[3u];#];[[4294967295u];[4u];["v4_2"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        // Delete all
        {
            const TString query1(Q_(R"(
                DELETE FROM `/Root/MultiShardIndexedWithDataColumn`;
            )"));

            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }

        {
            const auto& yson = ReadTableToYson(session, "/Root/MultiShardIndexedWithDataColumn/index/indexImplTable");
            const TString expected = "[]";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        // Insert new row in empty table
        {
            const TString query1(Q_(R"(
                INSERT INTO `/Root/MultiShardIndexedWithDataColumn` (key, fk, value) VALUES
                (1u, 1000000000u, "Value1");
            )"));

            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }

        {
            const auto& yson = ReadTableToYson(session, "/Root/MultiShardIndexedWithDataColumn/index/indexImplTable");
            const TString expected = R"([[[1000000000u];[1u];["Value1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        // Insert row with same pk
        {
            const TString query1(R"(
                INSERT INTO `/Root/MultiShardIndexedWithDataColumn` (key, fk, value) VALUES
                (1u, 1000000000u, "Value1_1");
            )");

            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT(!result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::PRECONDITION_FAILED);
        }

        // Index table has not been changed
        {
            const auto& yson = ReadTableToYson(session, "/Root/MultiShardIndexedWithDataColumn/index/indexImplTable");
            const TString expected = R"([[[1000000000u];[1u];["Value1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        // Insert new row but no specify index column
        {
            const TString query1(Q_(R"(
                INSERT INTO `/Root/MultiShardIndexedWithDataColumn` (key, value) VALUES
                (2u, "Value2");
            )"));

            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
        }

        {
            const auto& yson = ReadTableToYson(session, "/Root/MultiShardIndexedWithDataColumn/index/indexImplTable");
            const TString expected = R"([[#;[2u];["Value2"]];[[1000000000u];[1u];["Value1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        // Update on
        {
            const TString query1(Q_(R"(
                UPDATE `/Root/MultiShardIndexedWithDataColumn` ON (key, fk, value) VALUES
                (0u, 0u, "Value0_0"),
                (1u, 1000000000u, "Value1_1");
            )"));

            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());

        }

        {
            const auto& yson = ReadTableToYson(session, "/Root/MultiShardIndexedWithDataColumn/index/indexImplTable");
            const TString expected = R"([[#;[2u];["Value2"]];[[1000000000u];[1u];["Value1_1"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {

            const TString query1(Q_(R"(
                UPDATE `/Root/MultiShardIndexedWithDataColumn` ON (key, value) VALUES
                (1u, "Value1_2");
            )"));

            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());

        }

        {
            const auto& yson = ReadTableToYson(session, "/Root/MultiShardIndexedWithDataColumn/index/indexImplTable");
            const TString expected = R"([[#;[2u];["Value2"]];[[1000000000u];[1u];["Value1_2"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        {
            const TString query1(Q_(R"(
                UPDATE `/Root/MultiShardIndexedWithDataColumn` ON (key, fk, ext_value) VALUES
                (1u, 11u, "Something");
            )"));

            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTableToYson(session, "/Root/MultiShardIndexedWithDataColumn/index/indexImplTable");
            const TString expected = R"([[#;[2u];["Value2"]];[[11u];[1u];["Value1_2"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        // Update where - update data column
        {
            const TString query1(Q_(R"(
                UPDATE `/Root/MultiShardIndexedWithDataColumn` SET value = "Value1_3"
                WHERE key = 1u;
            )"));

            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTableToYson(session, "/Root/MultiShardIndexedWithDataColumn/index/indexImplTable");
            const TString expected = R"([[#;[2u];["Value2"]];[[11u];[1u];["Value1_3"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        // Update were - do not touch index
        {
            const TString query1(Q_(R"(
                UPDATE `/Root/MultiShardIndexedWithDataColumn` SET ext_value = "Something2"
                WHERE key = 1u;
            )"));

            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTableToYson(session, "/Root/MultiShardIndexedWithDataColumn/index/indexImplTable");
            const TString expected = R"([[#;[2u];["Value2"]];[[11u];[1u];["Value1_3"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }

        // Update where - update index column
        {
            const TString query1(Q_(R"(
                UPDATE `/Root/MultiShardIndexedWithDataColumn` SET fk = 111111111u
                WHERE key = 1u;
            )"));

            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());

        }

        {
            const auto& yson = ReadTableToYson(session, "/Root/MultiShardIndexedWithDataColumn/index/indexImplTable");
            const TString expected = R"([[#;[2u];["Value2"]];[[111111111u];[1u];["Value1_3"]]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
    }

    Y_UNIT_TEST_TWIN(DataColumnSelect, WithMvcc) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(WithMvcc)
            .SetEnableMvccSnapshotReads(WithMvcc)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateSampleTablesWithIndex(session);
        CreateTableWithMultishardIndexAndDataColumn(kikimr.GetTestClient());
        FillTableWithDataColumn(db);

        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/SecondaryKeys` (Key, Fk, Value) VALUES
                (333, 2000000000u, "xxx");
            )"));

            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            NYdb::NTable::TExecDataQuerySettings execSettings;
            execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);
            const TString query(Q1_(R"(
                SELECT value FROM `/Root/MultiShardIndexedWithDataColumn` VIEW index WHERE fk = 3000000000u;
            )"));

            auto result = session.ExecuteDataQuery(
                                     query,
                                     TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                                     execSettings)
                              .ExtractValueSync();
            UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[\"v3\"]]]");

            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases().size(), 1);

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).name(), "/Root/MultiShardIndexedWithDataColumn/index/indexImplTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).reads().rows(), 1);

        }

        {
            NYdb::NTable::TExecDataQuerySettings execSettings;
            execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);
            const TString query = Q1_(R"(
                SELECT value FROM `/Root/MultiShardIndexedWithDataColumn` VIEW index WHERE fk IN (3000000000u);
            )");

            auto result = session.ExecuteDataQuery(
                                     query,
                                     TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                                     execSettings)
                              .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)), "[[[\"v3\"]]]");

            auto& stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());

            UNIT_ASSERT_VALUES_EQUAL_C(stats.query_phases().size(), 1, stats.DebugString());

            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).name(), "/Root/MultiShardIndexedWithDataColumn/index/indexImplTable");
            UNIT_ASSERT_VALUES_EQUAL(stats.query_phases(0).table_access(0).reads().rows(), 1);
        }

        {
            NYdb::NTable::TExecDataQuerySettings execSettings;
            execSettings.CollectQueryStats(ECollectQueryStatsMode::Basic);

            const TString query = Q1_(R"(
                SELECT t2.value FROM `/Root/SecondaryKeys` as t1
                    INNER JOIN `/Root/MultiShardIndexedWithDataColumn` VIEW index as t2 ON t2.fk = t1.Fk;
            )");

            auto result = session.ExecuteDataQuery(
                    query,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(),
                    execSettings)
                .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
            UNIT_ASSERT_VALUES_EQUAL(NYdb::FormatResultSetYson(result.GetResultSet(0)),
                "[[[\"v2\"]]]");
        }
    }

    Y_UNIT_TEST_TWIN(DuplicateUpsert, WithMvcc) {
        auto setting = NKikimrKqp::TKqpSetting();
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(WithMvcc)
            .SetEnableMvccSnapshotReads(WithMvcc)
            .SetKqpSettings({setting});
        TKikimrRunner kikimr(serverSettings);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateTableWithMultishardIndexAndDataColumn(kikimr.GetTestClient());

        {
            const TString query1(Q_(R"(
                UPSERT INTO `/Root/MultiShardIndexedWithDataColumn` (key, fk, ext_value) VALUES
                (3u, 3000000000u, "Something"),
                (3u, 3000000001u, "Something1"),
                (3u, 3000000002u, "Something2");
            )"));

            auto result = session.ExecuteDataQuery(
                                 query1,
                                 TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx())
                          .ExtractValueSync();
            UNIT_ASSERT(result.IsSuccess());
        }

        {
            const auto& yson = ReadTableToYson(session, "/Root/MultiShardIndexedWithDataColumn/index/indexImplTable");
            const TString expected = R"([[[3000000002u];[3u];#]])";
            UNIT_ASSERT_VALUES_EQUAL(yson, expected);
        }
    }

    Y_UNIT_TEST_TWIN(SortByPk, WithMvcc) {
        auto serverSettings = TKikimrSettings()
            .SetEnableMvcc(WithMvcc)
            .SetEnableMvccSnapshotReads(WithMvcc);
        TKikimrRunner kikimr(serverSettings);

        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateTableWithMultishardIndex(kikimr.GetTestClient(), false);
        FillTable(session);

        AssertSuccessResult(session.ExecuteDataQuery(Q1_(R"(
            UPSERT INTO `/Root/MultiShardIndexed` (key, fk, value) VALUES
                (10u, 1000, "NewValue1"),
                (11u, 1001, "NewValue2"),
                (12u, 1002, "NewValue3"),
                (13u, 1003, "NewValue4"),
                (14u, 1004, "NewValue5"),
                (15u, 1005, "NewValue6"),
                (101u, 1011, "NewValue7");
        )"), TTxControl::BeginTx().CommitTx()).ExtractValueSync());

        auto query = Q1_(R"(
            SELECT * FROM MultiShardIndexed VIEW index
            WHERE fk > 100
            ORDER BY fk, key
            LIMIT 100;
        )");

        auto explainResult = session.ExplainDataQuery(query).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(explainResult.GetStatus(), EStatus::SUCCESS, explainResult.GetIssues().ToString());

        // Cerr << explainResult.GetPlan() << Endl;

        NJson::TJsonValue plan;
        NJson::ReadJsonTree(explainResult.GetPlan(), &plan, true);
        auto node = FindPlanNodeByKv(plan, "Name", "TopSort");
        UNIT_ASSERT(node.IsDefined());

        auto result = session.ExecuteDataQuery(query, TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SUCCESS, result.GetIssues().ToString());

        CompareYson(R"([
            [[1000u];[10u];["NewValue1"]];
            [[1001u];[11u];["NewValue2"]];
            [[1002u];[12u];["NewValue3"]];
            [[1003u];[13u];["NewValue4"]];
            [[1004u];[14u];["NewValue5"]];
            [[1005u];[15u];["NewValue6"]];
            [[1011u];[101u];["NewValue7"]];
            [[1000000000u];[1u];["v1"]];
            [[2000000000u];[2u];["v2"]];
            [[3000000000u];[3u];["v3"]];
            [[4294967295u];[4u];["v4"]]
        ])", FormatResultSetYson(result.GetResultSet(0)));
    }

    void CheckWriteIntoRenamingIndex(bool asyncIndex) {
        TKikimrRunner kikimr;

        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();
        CreateTableWithMultishardIndex(kikimr.GetTestClient(), asyncIndex);

        auto buildParam = [&db](ui64 id) {
            return db.GetParamsBuilder()
               .AddParam("$rows")
                .BeginList()
                .AddListItem()
                    .BeginStruct()
                        .AddMember("key").Uint64(id)
                        .AddMember("fk").Uint32(id)
                        .AddMember("value").Utf8("v1")
                    .EndStruct()
                .AddListItem()
                    .BeginStruct()
                        .AddMember("key").Uint64(id << 31)
                        .AddMember("fk").Uint32(id + (1u << 31))
                        .AddMember("value").Utf8("v2")
                    .EndStruct()
                .EndList()
                .Build()
            .Build();
        };

        TMutex SyncMutex;
        TCondVar SyncCondVar;
        const size_t rows = 1000;
        size_t rowsInserted = 0;

        NPar::LocalExecutor().RunAdditionalThreads(2);
        NPar::LocalExecutor().ExecRange([&](int id) mutable {
            switch (id) {
                case 0: {
                    size_t count = rows;
                    while (--count) {
                        const TString q(R"(
                            DECLARE $rows AS List < Struct<key: Uint64, fk: Uint32, value: Utf8 > >;
                            UPSERT INTO `/Root/MultiShardIndexed` (key, fk, value)
                            SELECT key, fk, value FROM AS_TABLE($rows);
                        )");
                        auto r = db.RetryOperationSync([=](TSession s) {
                            auto p = buildParam(count);
                            auto t = ExecuteDataQuery(s, q, p);
                            return t;
                        });
                        UNIT_ASSERT_C(r.IsSuccess(), r.GetIssues().ToString());
                        rowsInserted += 2;
                        // Let write 10 rows before the index will be renamed
                        if (count == (rows - 10)) {
                            TGuard<TMutex> guard(SyncMutex);
                            SyncCondVar.Signal();
                        }
                    }
                }
                break;
                case 1: {
                    TGuard<TMutex> guard(SyncMutex);
                    SyncCondVar.WaitI(SyncMutex);
                    auto s = db.CreateSession().GetValueSync().GetSession();
                    auto st = s.ExecuteSchemeQuery(R"(
                        ALTER TABLE `/Root/MultiShardIndexed` RENAME INDEX index TO index_new;
                    )").ExtractValueSync();
                    UNIT_ASSERT_VALUES_EQUAL_C(st.GetStatus(), EStatus::SUCCESS, st.GetIssues().ToString());
                }
                break;
                default:
                    Y_FAIL("Unknown id");
            }
        }, 0, 2, NPar::TLocalExecutor::WAIT_COMPLETE | NPar::TLocalExecutor::MED_PRIORITY);

        {
            TReadTableSettings settings;
            settings.Ordered(true);
            auto it = session.ReadTable("/Root/MultiShardIndexed/index_new/indexImplTable", settings).GetValueSync();
            UNIT_ASSERT(it.IsSuccess());

            int shard = 0;
            size_t rowsRead = 0;
            for (;;) {
                auto tablePart = it.ReadNext().GetValueSync();
                if (tablePart.EOS()) {
                    break;
                }

                UNIT_ASSERT_VALUES_EQUAL(tablePart.IsSuccess(), true);
                auto resultSet = tablePart.ExtractPart();

                auto rsParser = TResultSetParser(resultSet);

                ui32 startVal = 1;
                while (rsParser.TryNextRow()) {
                    auto val = rsParser.GetValue(0);
                    TValueParser vp(val);
                    vp.OpenOptional();
                    if (!vp.IsNull()) {
                        rowsRead++;
                        switch (shard) {
                            case 0:
                                UNIT_ASSERT_VALUES_EQUAL(vp.GetUint32(), startVal++);
                                break;
                            case 1:
                                UNIT_ASSERT_VALUES_EQUAL(vp.GetUint32(), (startVal++) + (1u << 31));
                                break;
                            default:
                                Y_FAIL("unexpected shard id");
                        }
                    }
                }
                shard++;
            }
            UNIT_ASSERT_VALUES_EQUAL(rowsInserted, rowsRead);
        }
    }

    Y_UNIT_TEST(WriteIntoRenamingSyncIndex) {
        CheckWriteIntoRenamingIndex(false);
    }

    Y_UNIT_TEST(WriteIntoRenamingAsyncIndex) {
        CheckWriteIntoRenamingIndex(true);
    }
}

}
}

#pragma once
#include <ydb/library/yql/minikql/defs.h>
#include <ydb/library/yql/minikql/mkql_node.h>
#include <ydb/library/mkql_proto/protos/minikql.pb.h>
#include <ydb/public/api/protos/ydb_value.pb.h>
#include <ydb/library/mkql_proto/mkql_proto.h>
#include <ydb/core/scheme/scheme_tablecell.h>

namespace NKikimr {

namespace NMiniKQL {

class THolderFactory;

NUdf::TUnboxedValue ImportValueFromProto(TType* type, const Ydb::Value& value, const THolderFactory& factory);

// NOTE: TCell's can reference memomry from tupleValue
bool CellsFromTuple(const NKikimrMiniKQL::TType* tupleType,
                    const NKikimrMiniKQL::TValue& tupleValue,
                    const TConstArrayRef<NScheme::TTypeInfo>& expectedTypes,
                    bool allowCastFromString,
                    TVector<TCell>& key,
                    TString& errStr);

bool CellToValue(NScheme::TTypeInfo type, const TCell& c, NKikimrMiniKQL::TValue& val, TString& errStr);

} // namspace NMiniKQL
} // namspace NKikimr

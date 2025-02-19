#include "mkql_block_agg.h"
#include "mkql_block_agg_factory.h"
#include "mkql_bit_utils.h"
#include "mkql_rh_hash.h"

#include <ydb/library/yql/minikql/computation/mkql_computation_node_impl.h>
#include <ydb/library/yql/minikql/computation/mkql_computation_node_holders.h>

#include <ydb/library/yql/minikql/mkql_node_cast.h>
#include <ydb/library/yql/minikql/mkql_node_builder.h>

#include <ydb/library/yql/minikql/arrow/arrow_defs.h>

#include <arrow/scalar.h>
#include <arrow/array/array_primitive.h>
#include <arrow/array/builder_primitive.h>

namespace NKikimr {
namespace NMiniKQL {

namespace {

class TSSOKey {
public:
    static constexpr size_t SSO_Length = 16;
    static_assert(SSO_Length < 128); // should fit into 7 bits

    static bool CanBeInplace(TStringBuf data) {
        return data.Size() + 1 <= sizeof(TSSOKey);
    }

    static TSSOKey Inplace(TStringBuf data) {
        Y_ASSERT(CanBeInplace(data));
        TSSOKey ret(1 | (data.Size() << 1), 0);
        memcpy(ret.U.I.Buffer_, data.Data(), data.Size());
        return ret;
    }

    static TSSOKey External(TStringBuf data) {
        return TSSOKey(data.Size() << 1, data.Data());
    }

    bool IsInplace() const {
        return U.I.SmallLength_ & 1;
    }

    TStringBuf AsView() const {
        if (IsInplace()) {
            // inplace
            return TStringBuf(U.I.Buffer_, U.I.SmallLength_ >> 1);
        } else {
            // external
            return TStringBuf(U.E.Ptr_, U.E.Length_ >> 1);
        }
    }

    void UpdateExternalPointer(const char *ptr) {
        Y_ASSERT(!IsInplace());
        U.E.Ptr_ = ptr;
    }

private:
    TSSOKey(ui64 length, const char* ptr) {
        U.E.Length_ = length;
        U.E.Ptr_ = ptr;
    }

private:
    union {
        struct TExternal {
            ui64 Length_;
            const char* Ptr_;
        } E;
        struct TInplace {
            ui8 SmallLength_;
            char Buffer_[SSO_Length];
        } I;
    } U;
};

}
}
}

namespace std {
    template <>
    struct hash<NKikimr::NMiniKQL::TSSOKey> {
        using argument_type = NKikimr::NMiniKQL::TSSOKey;
        using result_type = size_t;
        inline result_type operator()(argument_type const& s) const noexcept {
            return std::hash<std::string_view>()(s.AsView());
        }
    };

    template <>
    struct equal_to<NKikimr::NMiniKQL::TSSOKey> {
        using argument_type = NKikimr::NMiniKQL::TSSOKey;
        bool operator()(argument_type x, argument_type y) const {
            return x.AsView() == y.AsView();
        }
        bool operator()(argument_type x, TStringBuf y) const {
            return x.AsView() == y;
        }
        using is_transparent = void;
    };
}

namespace NKikimr {
namespace NMiniKQL {

namespace {

template <typename T>
struct TAggParams {
    std::unique_ptr<IPreparedBlockAggregator<T>> Prepared_;
};

struct TKeyParams {
    ui32 Index;
    TType* Type;
};

class TInputBuffer {
public:
    TInputBuffer(TStringBuf buf)
        : Buf_(buf)
    {}

    char PopChar() {
        Ensure(1);
        char c = Buf_.Data()[Pos_];
        ++Pos_;
        return c;
    }

    template <typename T>
    T PopNumber() {
        Ensure(sizeof(T));
        T t = *(const T*)(Buf_.Data() + Pos_);
        Pos_ += sizeof(T);
        return t;
    }

private:
    void Ensure(size_t delta) {
        MKQL_ENSURE(Pos_ + delta <= Buf_.Size(), "Unexpected end of buffer");
    }

private:
    size_t Pos_ = 0;
    TStringBuf Buf_;
};

class TOutputBuffer {
public:
    void PushChar(char c) {
        Ensure(1);
        Vec_[Pos_] = c;
        ++Pos_;
    }

    template <typename T>
    void PushNumber(T t) {
        Ensure(sizeof(T));
        *(T*)&Vec_[Pos_] = t;
        Pos_ += sizeof(T);
    }

    // fill with zeros
    void Resize(size_t size) {
        Pos_ = 0;
        Vec_.clear();
        Vec_.resize(size);
    }

    void Rewind() {
        Pos_ = 0;
    }

    TStringBuf Finish() const {
        return TStringBuf(Vec_.data(), Vec_.data() + Pos_);
    }

private:
    void Ensure(size_t delta) {
        if (Pos_ + delta > Vec_.size()) {
            Vec_.reserve(Max(2 * Vec_.capacity(), Pos_ + delta));
            Vec_.resize(Pos_ + delta);
        }
    }

private:
    size_t Pos_ = 0;
    TVector<char> Vec_;
};

class IKeyColumnBuilder {
public:
    virtual ~IKeyColumnBuilder() = default;

    // decode part of buffer and advances position
    virtual void Add(TInputBuffer& in) = 0;

    virtual NUdf::TUnboxedValue Build() = 0;
};

class IKeySerializer {
public:
    virtual ~IKeySerializer() = default;

    // handle scalar or array item
    virtual void Serialize(const arrow::Datum& value, ui64 index, TOutputBuffer& out) const = 0;

    virtual std::unique_ptr<IKeyColumnBuilder> MakeBuilder(ui64 size, TComputationContext& ctx) const = 0;
};

template <typename T, typename TBuilder, bool IsOptional>
class TFixedSizeKeyColumnBuilder : public IKeyColumnBuilder  {
public:
    TFixedSizeKeyColumnBuilder(ui64 size, const std::shared_ptr<arrow::DataType>& dataType, TComputationContext& ctx)
        : Builder_(dataType, &ctx.ArrowMemoryPool)
        , Ctx_(ctx)
    {
        ARROW_OK(this->Builder_.Reserve(size));
    }

    void Add(TInputBuffer& in) final {
        if constexpr (IsOptional) {
            if (in.PopChar()) {
                auto x = in.PopNumber<T>();
                this->Builder_.UnsafeAppend(x);
            } else {
                this->Builder_.UnsafeAppendNull();
            }
        } else {
            auto x = in.PopNumber<T>();
            this->Builder_.UnsafeAppend(x);
        }
    }

    NUdf::TUnboxedValue Build() final {
        std::shared_ptr<arrow::ArrayData> result;
        ARROW_OK(this->Builder_.FinishInternal(&result));
        return Ctx_.HolderFactory.CreateArrowBlock(result);
    }

private:
    TBuilder Builder_;
    TComputationContext& Ctx_;
};

template <typename T, typename TScalar, typename TBuilder, bool IsOptional>
class TFixedSizeKeySerializer : public IKeySerializer {
public:
    TFixedSizeKeySerializer(const std::shared_ptr<arrow::DataType>& dataType)
        : DataType_(dataType)
    {}

    virtual void Serialize(const arrow::Datum& value, ui64 index, TOutputBuffer& out) const final {
        T x;
        if (value.is_scalar()) {
            const auto& scalar = value.scalar_as<TScalar>();
            if constexpr (IsOptional) {
                if (scalar.is_valid) {
                    out.PushChar(1);
                    x = scalar.value;
                } else {
                    out.PushChar(0);
                    return;
                }
               
            } else {
                Y_ASSERT(scalar.is_valid);
                x = scalar.value;
            }
        } else {
            const auto& array = *value.array();
            if constexpr (IsOptional) {
                if (array.GetNullCount() == 0 || arrow::BitUtil::GetBit(array.GetValues<uint8_t>(0, 0), index + array.offset)) {
                    out.PushChar(1);
                    x = array.GetValues<T>(1)[index];
                } else {
                    out.PushChar(0);
                    return;
                }
            } else {
                x = array.GetValues<T>(1)[index];
            }
        }

        out.PushNumber<T>(x);
    }

    std::unique_ptr<IKeyColumnBuilder> MakeBuilder(ui64 size, TComputationContext& ctx) const final {
        return std::make_unique<TFixedSizeKeyColumnBuilder<T, TBuilder, IsOptional>>(size, DataType_, ctx);
    }

private:
    const std::shared_ptr<arrow::DataType> DataType_;
};

size_t GetBitmapPopCount(const std::shared_ptr<arrow::ArrayData>& arr) {
    size_t len = (size_t)arr->length;
    MKQL_ENSURE(arr->GetNullCount() == 0, "Bitmap block should not have nulls");
    const ui8* src = arr->GetValues<ui8>(1);
    return GetSparseBitmapPopCount(src, len);
}

class TBlockCombineAllWrapper : public TStatefulWideFlowComputationNode<TBlockCombineAllWrapper> {
public:
    TBlockCombineAllWrapper(TComputationMutables& mutables,
        IComputationWideFlowNode* flow,
        std::optional<ui32> filterColumn,
        size_t width,
        TVector<TAggParams<IBlockAggregatorCombineAll>>&& aggsParams)
        : TStatefulWideFlowComputationNode(mutables, flow, EValueRepresentation::Any)
        , Flow_(flow)
        , FilterColumn_(filterColumn)
        , Width_(width)
        , AggsParams_(std::move(aggsParams))
    {
        MKQL_ENSURE(Width_ > 0, "Missing block length column");
    }

    EFetchResult DoCalculate(NUdf::TUnboxedValue& state,
        TComputationContext& ctx,
        NUdf::TUnboxedValue*const* output) const
    {
        auto& s = GetState(state, ctx);
        if (s.IsFinished_) {
            return EFetchResult::Finish;
        }

        for (;;) {
            auto result = Flow_->FetchValues(ctx, s.ValuePointers_.data());
            if (result == EFetchResult::Yield) {
                return result;
            } else if (result == EFetchResult::One) {
                ui64 batchLength = GetBatchLength(s.Values_.data());
                if (!batchLength) {
                    continue;
                }

                std::optional<ui64> filtered;
                if (FilterColumn_) {
                    auto filterDatum = TArrowBlock::From(s.Values_[*FilterColumn_]).GetDatum();
                    if (filterDatum.is_scalar()) {
                        if (!filterDatum.scalar_as<arrow::UInt8Scalar>().value) {
                            continue;
                        }
                    } else {
                        ui64 popCount = GetBitmapPopCount(filterDatum.array());
                        if (popCount == 0) {
                            continue;
                        }

                        if (popCount < batchLength) {
                            filtered = popCount;
                        }
                    }
                }

                s.HasValues_ = true;
                char* ptr = s.AggStates_.data();
                for (size_t i = 0; i < s.Aggs_.size(); ++i) {
                    if (output[i]) {
                        s.Aggs_[i]->AddMany(ptr, s.Values_.data(), batchLength, filtered);
                    }

                    ptr += s.Aggs_[i]->StateSize;
                }
            } else {
                s.IsFinished_ = true;
                if (!s.HasValues_) {
                    return EFetchResult::Finish;
                }

                char* ptr = s.AggStates_.data();
                for (size_t i = 0; i < s.Aggs_.size(); ++i) {
                    if (auto* out = output[i]; out != nullptr) {
                        *out = s.Aggs_[i]->FinishOne(ptr);
                    }

                    ptr += s.Aggs_[i]->StateSize;
                }

                return EFetchResult::One;
            }
        }

        return EFetchResult::Finish;
    }

private:
    struct TState : public TComputationValue<TState> {
        TVector<NUdf::TUnboxedValue> Values_;
        TVector<NUdf::TUnboxedValue*> ValuePointers_;
        TVector<std::unique_ptr<IBlockAggregatorCombineAll>> Aggs_;
        bool IsFinished_ = false;
        bool HasValues_ = false;
        TVector<char> AggStates_;

        TState(TMemoryUsageInfo* memInfo, size_t width, std::optional<ui32> filterColumn, const TVector<TAggParams<IBlockAggregatorCombineAll>>& params, TComputationContext& ctx)
            : TComputationValue(memInfo)
            , Values_(width)
            , ValuePointers_(width)
        {
            for (size_t i = 0; i < width; ++i) {
                ValuePointers_[i] = &Values_[i];
            }

            ui32 totalStateSize = 0;
            for (const auto& p : params) {
                Aggs_.emplace_back(p.Prepared_->Make(ctx));
                totalStateSize += Aggs_.back()->StateSize;
            }

            AggStates_.resize(totalStateSize);
            char* ptr = AggStates_.data();
            for (const auto& agg : Aggs_) {
                agg->InitState(ptr);
                ptr += agg->StateSize;
            }
        }
    };

private:
    void RegisterDependencies() const final {
        FlowDependsOn(Flow_);
    }

    TState& GetState(NUdf::TUnboxedValue& state, TComputationContext& ctx) const {
        if (!state.HasValue()) {
            state = ctx.HolderFactory.Create<TState>(Width_, FilterColumn_, AggsParams_, ctx);
        }
        return *static_cast<TState*>(state.AsBoxed().Get());
    }

    ui64 GetBatchLength(const NUdf::TUnboxedValue* columns) const {
        return TArrowBlock::From(columns[Width_ - 1]).GetDatum().scalar_as<arrow::UInt64Scalar>().value;
    }

private:
    IComputationWideFlowNode* Flow_;
    std::optional<ui32> FilterColumn_;
    const size_t Width_;
    const TVector<TAggParams<IBlockAggregatorCombineAll>> AggsParams_;
};

template <typename T>
T MakeKey(TStringBuf s) {
    Y_ASSERT(s.Size() <= sizeof(T));
    return *(const T*)s.Data();
}

template <>
TSSOKey MakeKey(TStringBuf s) {
    if (TSSOKey::CanBeInplace(s)) {
        return TSSOKey::Inplace(s);
    } else {
        return TSSOKey::External(s);
    }
}

void MoveKeyToArena(TSSOKey& key, TPagedArena& arena) {
    if (key.IsInplace()) {
        return;
    }

    auto view = key.AsView();
    auto ptr = (char*)arena.Alloc(view.Size());
    memcpy(ptr, view.Data(), view.Size());
    key.UpdateExternalPointer(ptr);
}

template <typename T>
TStringBuf GetKeyView(const T& key) {
    return TStringBuf((const char*)&key, sizeof(T));
}

template <>
TStringBuf GetKeyView(const TSSOKey& key) {
    return key.AsView();
}

template <typename TKey, typename TAggregator, bool UseSet, bool UseFilter, bool Finalize, typename TDerived>
class THashedWrapperBase : public TStatefulWideFlowComputationNode<TDerived> {
public:
    using TSelf = THashedWrapperBase<TKey, TAggregator, UseSet, UseFilter, Finalize, TDerived>;
    using TBase = TStatefulWideFlowComputationNode<TDerived>;

    THashedWrapperBase(TComputationMutables& mutables,
        IComputationWideFlowNode* flow,
        std::optional<ui32> filterColumn,
        size_t width,
        const std::vector<TKeyParams>& keys,
        std::vector<std::unique_ptr<IKeySerializer>>&& keySerializers,
        TVector<TAggParams<TAggregator>>&& aggsParams)
        : TBase(mutables, flow, EValueRepresentation::Any)
        , Flow_(flow)
        , FilterColumn_(filterColumn)
        , Width_(width)
        , OutputWidth_(keys.size() + aggsParams.size() + 1)
        , Keys_(keys)
        , KeySerializers_(std::move(keySerializers))
        , AggsParams_(std::move(aggsParams))
    {
        MKQL_ENSURE(Width_ > 0, "Missing block length column");
        if constexpr (UseFilter) {
            MKQL_ENSURE(filterColumn, "Missing filter column");
            MKQL_ENSURE(!Finalize, "Filter isn't compatible with Finalize");
        } else {
            MKQL_ENSURE(!filterColumn, "Unexpected filter column");
        }
    }

    EFetchResult DoCalculate(NUdf::TUnboxedValue& state,
        TComputationContext& ctx,
        NUdf::TUnboxedValue*const* output) const
    {
        auto& s = GetState(state, ctx);
        if (s.IsFinished_) {
            return EFetchResult::Finish;
        }

        for (;;) {
            auto result = Flow_->FetchValues(ctx, s.ValuePointers_.data());
            if (result == EFetchResult::Yield) {
                return result;
            } else if (result == EFetchResult::One) {
                ui64 batchLength = GetBatchLength(s.Values_.data());
                if (!batchLength) {
                    continue;
                }

                const ui8* filterBitmap = nullptr;
                if constexpr (UseFilter) {
                    auto filterDatum = TArrowBlock::From(s.Values_[*FilterColumn_]).GetDatum();
                    if (filterDatum.is_scalar()) {
                        if (!filterDatum.template scalar_as<arrow::UInt8Scalar>().value) {
                            continue;
                        }
                    } else {
                        const auto& arr = filterDatum.array();
                        filterBitmap = arr->template GetValues<ui8>(1);
                        ui64 popCount = GetBitmapPopCount(arr);
                        if (popCount == 0) {
                            continue;
                        }
                    }
                }

                s.HasValues_ = true;
                TVector<arrow::Datum> keysDatum;
                keysDatum.reserve(Keys_.size());
                for (ui32 i = 0; i < Keys_.size(); ++i) {
                    keysDatum.emplace_back(TArrowBlock::From(s.Values_[Keys_[i].Index]).GetDatum());
                }

                TOutputBuffer out;
                out.Resize(sizeof(TKey));
                for (ui64 row = 0; row < batchLength; ++row) {
                    if constexpr (UseFilter) {
                        if (filterBitmap && !filterBitmap[row]) {
                            continue;
                        }
                    }

                    out.Rewind();
                    // encode key
                    for (ui32 i = 0; i < keysDatum.size(); ++i) {
                        KeySerializers_[i]->Serialize(keysDatum[i], row, out);
                    }

                    auto str = out.Finish();
                    TKey key = MakeKey<TKey>(str);
                    bool isNew;
                    if constexpr (UseSet) {
                        auto iter = s.HashSet_->Insert(key, isNew);
                        if (isNew) {
                            if constexpr (std::is_same<TKey, TSSOKey>::value) {
                                MoveKeyToArena(s.HashSet_->GetKey(iter), s.Arena_);
                            }

                            s.HashSet_->CheckGrow();
                        }
                    } else {
                        auto iter = s.HashMap_->Insert(key, isNew);
                        char* ptr = (char*)s.HashMap_->GetPayload(iter);
                        if (isNew) {
                            for (size_t i = 0; i < s.Aggs_.size(); ++i) {
                                if (output[Keys_.size() + i]) {
                                    if constexpr (Finalize) {
                                        s.Aggs_[i]->LoadState(ptr, s.Values_.data(), row);
                                    } else {
                                        s.Aggs_[i]->InitKey(ptr, s.Values_.data(), row);
                                    }
                                }

                                ptr += s.Aggs_[i]->StateSize;
                            }

                            if constexpr (std::is_same<TKey, TSSOKey>::value) {
                                MoveKeyToArena(s.HashMap_->GetKey(iter), s.Arena_);
                            }

                            s.HashMap_->CheckGrow();
                        } else {
                            for (size_t i = 0; i < s.Aggs_.size(); ++i) {
                                if (output[Keys_.size() + i]) {
                                    if constexpr (Finalize) {
                                        s.Aggs_[i]->UpdateState(ptr, s.Values_.data(), row);
                                    } else {
                                        s.Aggs_[i]->UpdateKey(ptr, s.Values_.data(), row);
                                    }
                                }

                                ptr += s.Aggs_[i]->StateSize;
                            }
                        }
                    }
                }
            } else {
                s.IsFinished_ = true;
                if (!s.HasValues_) {
                    return EFetchResult::Finish;
                }

                // export results, TODO: split by batches
                ui64 size;
                if constexpr (UseSet) {
                    size = s.HashSet_->GetSize();
                } else {
                    size = s.HashMap_->GetSize();
                }

                TVector<std::unique_ptr<IKeyColumnBuilder>> keyBuilders;
                for (const auto& ks : KeySerializers_) {
                    keyBuilders.emplace_back(ks->MakeBuilder(size, ctx));
                }

                if constexpr (UseSet) {
                    for (auto iter = s.HashSet_->Begin(); iter != s.HashSet_->End(); s.HashSet_->Advance(iter)) {
                        if (s.HashSet_->GetPSL(iter) < 0) {
                            continue;
                        }

                        const TKey& key = s.HashSet_->GetKey(iter);
                        TInputBuffer in(GetKeyView<TKey>(key));
                        for (auto& kb : keyBuilders) {
                            kb->Add(in);
                        }
                    }
                } else {
                    TVector<std::unique_ptr<IAggColumnBuilder>> aggBuilders;
                    for (const auto& a : s.Aggs_) {
                        if constexpr (Finalize) {
                            aggBuilders.emplace_back(a->MakeResultBuilder(size));
                        } else {
                            aggBuilders.emplace_back(a->MakeStateBuilder(size));
                        }
                    }

                    for (auto iter = s.HashMap_->Begin(); iter != s.HashMap_->End(); s.HashMap_->Advance(iter)) {
                        if (s.HashMap_->GetPSL(iter) < 0) {
                            continue;
                        }

                        const TKey& key = s.HashMap_->GetKey(iter);
                        auto ptr = (const char*)s.HashMap_->GetPayload(iter);
                        TInputBuffer in(GetKeyView<TKey>(key));
                        for (auto& kb : keyBuilders) {
                            kb->Add(in);
                        }

                        for (size_t i = 0; i < s.Aggs_.size(); ++i) {
                            if (output[Keys_.size() + i]) {
                                aggBuilders[i]->Add(ptr);
                            }

                            ptr += s.Aggs_[i]->StateSize;
                        }
                    }

                    for (size_t i = 0; i < s.Aggs_.size(); ++i) {
                        if (output[Keys_.size() + i]) {
                            *output[Keys_.size() + i] = aggBuilders[i]->Build();
                        }
                    }
                }

                for (ui32 i = 0; i < Keys_.size(); ++i) {
                    if (output[i]) {
                        *output[i] = keyBuilders[i]->Build();
                    }
                }

                MKQL_ENSURE(output[OutputWidth_ - 1], "Block size should not be marked as unused");
                *output[OutputWidth_ - 1] = ctx.HolderFactory.CreateArrowBlock(arrow::Datum(std::make_shared<arrow::UInt64Scalar>(size)));
                return EFetchResult::One;
            }
        }
    }

private:
    struct TState : public TComputationValue<TState> {
        using TBase = TComputationValue<TState>;

        TVector<NUdf::TUnboxedValue> Values_;
        TVector<NUdf::TUnboxedValue*> ValuePointers_;
        TVector<std::unique_ptr<TAggregator>> Aggs_;
        bool IsFinished_ = false;
        bool HasValues_ = false;
        ui32 TotalStateSize_ = 0;
        std::unique_ptr<TRobinHoodHashMap<TKey, std::equal_to<TKey>, std::hash<TKey>, TMKQLAllocator<char>>> HashMap_;
        std::unique_ptr<TRobinHoodHashSet<TKey, std::equal_to<TKey>, std::hash<TKey>, TMKQLAllocator<char>>> HashSet_;
        TPagedArena Arena_;

        TState(TMemoryUsageInfo* memInfo, size_t width, std::optional<ui32> filterColumn, const TVector<TAggParams<TAggregator>>& params, TComputationContext& ctx)
            : TBase(memInfo)
            , Values_(width)
            , ValuePointers_(width)
            , Arena_(TlsAllocState)
        {
            for (size_t i = 0; i < width; ++i) {
                ValuePointers_[i] = &Values_[i];
            }

            for (const auto& p : params) {
                Aggs_.emplace_back(p.Prepared_->Make(ctx));
                TotalStateSize_ += Aggs_.back()->StateSize;
            }

            if constexpr (UseSet) {
                MKQL_ENSURE(params.empty(), "Only keys are supported");
                HashSet_ = std::make_unique<TRobinHoodHashSet<TKey, std::equal_to<TKey>, std::hash<TKey>, TMKQLAllocator<char>>>();
            } else {
                HashMap_ = std::make_unique<TRobinHoodHashMap<TKey, std::equal_to<TKey>, std::hash<TKey>, TMKQLAllocator<char>>>(TotalStateSize_);
            } 
        }
    };

private:
    void RegisterDependencies() const final {
        this->FlowDependsOn(Flow_);
    }

    TState& GetState(NUdf::TUnboxedValue& state, TComputationContext& ctx) const {
        if (!state.HasValue()) {
            state = ctx.HolderFactory.Create<TState>(Width_, FilterColumn_, AggsParams_, ctx);
        }
        return *static_cast<TState*>(state.AsBoxed().Get());
    }

    ui64 GetBatchLength(const NUdf::TUnboxedValue* columns) const {
        return TArrowBlock::From(columns[Width_ - 1]).GetDatum().scalar_as<arrow::UInt64Scalar>().value;
    }

private:
    IComputationWideFlowNode* Flow_;
    std::optional<ui32> FilterColumn_;
    const size_t Width_;
    const size_t OutputWidth_;
    const std::vector<TKeyParams> Keys_;
    const TVector<TAggParams<TAggregator>> AggsParams_;
    std::vector<std::unique_ptr<IKeySerializer>> KeySerializers_;
};

template <typename TKey, bool UseSet, bool UseFilter>
class TBlockCombineHashedWrapper : public THashedWrapperBase<TKey, IBlockAggregatorCombineKeys, UseSet, UseFilter, false, TBlockCombineHashedWrapper<TKey, UseSet, UseFilter>> {
public:
    using TSelf = TBlockCombineHashedWrapper<TKey, UseSet, UseFilter>;
    using TBase = THashedWrapperBase<TKey, IBlockAggregatorCombineKeys, UseSet, UseFilter, false, TSelf>;

    TBlockCombineHashedWrapper(TComputationMutables& mutables,
        IComputationWideFlowNode* flow,
        std::optional<ui32> filterColumn,
        size_t width,
        const std::vector<TKeyParams>& keys,
        std::vector<std::unique_ptr<IKeySerializer>>&& keySerializers,
        TVector<TAggParams<IBlockAggregatorCombineKeys>>&& aggsParams)
        : TBase(mutables, flow, filterColumn, width, keys, std::move(keySerializers), std::move(aggsParams))
    {}
};

template <typename TKey, bool UseSet>
class TBlockMergeFinalizeHashedWrapper : public THashedWrapperBase<TKey, IBlockAggregatorFinalizeKeys, UseSet, false, true, TBlockMergeFinalizeHashedWrapper<TKey, UseSet>> {
public:
    using TSelf = TBlockMergeFinalizeHashedWrapper<TKey, UseSet>;
    using TBase = THashedWrapperBase<TKey, IBlockAggregatorFinalizeKeys, UseSet, false, true, TSelf>;

    TBlockMergeFinalizeHashedWrapper(TComputationMutables& mutables,
        IComputationWideFlowNode* flow,
        size_t width,
        const std::vector<TKeyParams>& keys,
        std::vector<std::unique_ptr<IKeySerializer>>&& keySerializers,
        TVector<TAggParams<IBlockAggregatorFinalizeKeys>>&& aggsParams)
        : TBase(mutables, flow, {}, width, keys, std::move(keySerializers), std::move(aggsParams))
    {}
};

template <typename TAggregator>
std::unique_ptr<IPreparedBlockAggregator<TAggregator>> PrepareBlockAggregator(const IBlockAggregatorFactory& factory,
    TTupleType* tupleType,
    std::optional<ui32> filterColumn,
    const std::vector<ui32>& argsColumns,
    const TTypeEnvironment& env);

template <>
std::unique_ptr<IPreparedBlockAggregator<IBlockAggregatorCombineAll>> PrepareBlockAggregator<IBlockAggregatorCombineAll>(const IBlockAggregatorFactory& factory,
    TTupleType* tupleType,
    std::optional<ui32> filterColumn,
    const std::vector<ui32>& argsColumns,
    const TTypeEnvironment& env) {
    return factory.PrepareCombineAll(tupleType, filterColumn, argsColumns, env);
}

template <>
std::unique_ptr<IPreparedBlockAggregator<IBlockAggregatorCombineKeys>> PrepareBlockAggregator<IBlockAggregatorCombineKeys>(const IBlockAggregatorFactory& factory,
    TTupleType* tupleType,
    std::optional<ui32> filterColumn,
    const std::vector<ui32>& argsColumns,
    const TTypeEnvironment& env) {
    return factory.PrepareCombineKeys(tupleType, filterColumn, argsColumns, env);
}

template <>
std::unique_ptr<IPreparedBlockAggregator<IBlockAggregatorFinalizeKeys>> PrepareBlockAggregator<IBlockAggregatorFinalizeKeys>(const IBlockAggregatorFactory& factory,
    TTupleType* tupleType,
    std::optional<ui32> filterColumn,
    const std::vector<ui32>& argsColumns,
    const TTypeEnvironment& env) {
    MKQL_ENSURE(!filterColumn, "Unexpected filter column");
    return factory.PrepareFinalizeKeys(tupleType, argsColumns, env);
}

template <typename TAggregator>
void FillAggParams(TTupleLiteral* aggsVal, TTupleType* tupleType, std::optional<ui32> filterColumn, TVector<TAggParams<TAggregator>>& aggsParams, const TTypeEnvironment& env) {
    for (ui32 i = 0; i < aggsVal->GetValuesCount(); ++i) {
        auto aggVal = AS_VALUE(TTupleLiteral, aggsVal->GetValue(i));
        auto name = AS_VALUE(TDataLiteral, aggVal->GetValue(0))->AsValue().AsStringRef();

        std::vector<ui32> argColumns;
        for (ui32 j = 1; j < aggVal->GetValuesCount(); ++j) {
            argColumns.push_back(AS_VALUE(TDataLiteral, aggVal->GetValue(j))->AsValue().Get<ui32>());
        }

        TAggParams<TAggregator> p;
        p.Prepared_ = PrepareBlockAggregator<TAggregator>(GetBlockAggregatorFactory(name), tupleType, filterColumn, argColumns, env);
        aggsParams.emplace_back(std::move(p));
    }
}

template <bool UseSet, bool UseFilter>
IComputationNode* MakeBlockCombineHashedWrapper(
    ui32 totalKeysSize,
    TComputationMutables& mutables,
    IComputationWideFlowNode* flow,
    std::optional<ui32> filterColumn,
    size_t width,
    const std::vector<TKeyParams>& keys,
    std::vector<std::unique_ptr<IKeySerializer>>&& keySerializers,
    TVector<TAggParams<IBlockAggregatorCombineKeys>>&& aggsParams) {
    if (totalKeysSize <= sizeof(ui32)) {
        return new TBlockCombineHashedWrapper<ui32, UseSet, UseFilter>(mutables, flow, filterColumn, width, keys, std::move(keySerializers), std::move(aggsParams));
    }

    if (totalKeysSize <= sizeof(ui64)) {
        return new TBlockCombineHashedWrapper<ui64, UseSet, UseFilter>(mutables, flow, filterColumn, width, keys, std::move(keySerializers), std::move(aggsParams));
    }

    return new TBlockCombineHashedWrapper<TSSOKey, UseSet, UseFilter>(mutables, flow, filterColumn, width, keys, std::move(keySerializers), std::move(aggsParams));
}

template <bool UseSet>
IComputationNode* MakeBlockMergeFinalizeHashedWrapper(
    ui32 totalKeysSize,
    TComputationMutables& mutables,
    IComputationWideFlowNode* flow,
    size_t width,
    const std::vector<TKeyParams>& keys,
    std::vector<std::unique_ptr<IKeySerializer>>&& keySerializers,
    TVector<TAggParams<IBlockAggregatorFinalizeKeys>>&& aggsParams) {
    if (totalKeysSize <= sizeof(ui32)) {
        return new TBlockMergeFinalizeHashedWrapper<ui32, UseSet>(mutables, flow, width, keys, std::move(keySerializers), std::move(aggsParams));
    }

    if (totalKeysSize <= sizeof(ui64)) {
        return new TBlockMergeFinalizeHashedWrapper<ui64, UseSet>(mutables, flow, width, keys, std::move(keySerializers), std::move(aggsParams));
    }

    return new TBlockMergeFinalizeHashedWrapper<TSSOKey, UseSet>(mutables, flow, width, keys, std::move(keySerializers), std::move(aggsParams));
}

void PrepareKeys(const std::vector<TKeyParams>& keys, ui32& totalKeysSize, std::vector<std::unique_ptr<IKeySerializer>>& keySerializers) {
    totalKeysSize = 0;
    keySerializers.clear();
    for (const auto& k : keys) {
        auto itemType = AS_TYPE(TBlockType, k.Type)->GetItemType();
        bool isOptional;
        auto dataType = UnpackOptionalData(itemType, isOptional);
        if (isOptional) {
            totalKeysSize += 1;
        }

        switch (*dataType->GetDataSlot()) {
        case NUdf::EDataSlot::Int8:
            totalKeysSize += 1;
            if (isOptional) {
                keySerializers.emplace_back(std::make_unique<TFixedSizeKeySerializer<i8, arrow::Int8Scalar, arrow::Int8Builder, true>>(arrow::int8()));
            } else {
                keySerializers.emplace_back(std::make_unique<TFixedSizeKeySerializer<i8, arrow::Int8Scalar, arrow::Int8Builder, false>>(arrow::int8()));
            }

            break;
        case NUdf::EDataSlot::Bool:
        case NUdf::EDataSlot::Uint8:
            totalKeysSize += 1;
            if (isOptional) {
                keySerializers.emplace_back(std::make_unique<TFixedSizeKeySerializer<ui8, arrow::UInt8Scalar, arrow::UInt8Builder, true>>(arrow::uint8()));
            } else {
                keySerializers.emplace_back(std::make_unique<TFixedSizeKeySerializer<ui8, arrow::UInt8Scalar, arrow::UInt8Builder, false>>(arrow::uint8()));
            }

            break;
        case NUdf::EDataSlot::Int16:
            totalKeysSize += 2;
            if (isOptional) {
                keySerializers.emplace_back(std::make_unique<TFixedSizeKeySerializer<i16, arrow::Int16Scalar, arrow::Int16Builder, true>>(arrow::int16()));
            } else {
                keySerializers.emplace_back(std::make_unique<TFixedSizeKeySerializer<i16, arrow::Int16Scalar, arrow::Int16Builder, false>>(arrow::int16()));
            }

            break;
        case NUdf::EDataSlot::Uint16:
        case NUdf::EDataSlot::Date:
            totalKeysSize += 2;
            if (isOptional) {
                keySerializers.emplace_back(std::make_unique<TFixedSizeKeySerializer<ui16, arrow::UInt16Scalar, arrow::UInt16Builder, true>>(arrow::uint16()));
            } else {
                keySerializers.emplace_back(std::make_unique<TFixedSizeKeySerializer<ui16, arrow::UInt16Scalar, arrow::UInt16Builder, false>>(arrow::uint16()));
            }

            break;
        case NUdf::EDataSlot::Int32:
            totalKeysSize += 4;
            if (isOptional) {
                keySerializers.emplace_back(std::make_unique<TFixedSizeKeySerializer<i32, arrow::Int32Scalar, arrow::Int32Builder, true>>(arrow::int32()));
            } else {
                keySerializers.emplace_back(std::make_unique<TFixedSizeKeySerializer<i32, arrow::Int32Scalar, arrow::Int32Builder, false>>(arrow::int32()));
            }

            break;
        case NUdf::EDataSlot::Uint32:
        case NUdf::EDataSlot::Datetime:
            totalKeysSize += 4;
            if (isOptional) {
                keySerializers.emplace_back(std::make_unique<TFixedSizeKeySerializer<ui32, arrow::UInt32Scalar, arrow::UInt32Builder, true>>(arrow::uint32()));
            } else {
                keySerializers.emplace_back(std::make_unique<TFixedSizeKeySerializer<ui32, arrow::UInt32Scalar, arrow::UInt32Builder, false>>(arrow::uint32()));
            }

            break;
        case NUdf::EDataSlot::Int64:
        case NUdf::EDataSlot::Interval:
            totalKeysSize += 8;
            if (isOptional) {
                keySerializers.emplace_back(std::make_unique<TFixedSizeKeySerializer<i64, arrow::Int64Scalar, arrow::Int64Builder, true>>(arrow::int64()));
            } else {
                keySerializers.emplace_back(std::make_unique<TFixedSizeKeySerializer<i64, arrow::Int64Scalar, arrow::Int64Builder, false>>(arrow::int64()));
            }

            break;
        case NUdf::EDataSlot::Uint64:
        case NUdf::EDataSlot::Timestamp:
            totalKeysSize += 8;
            if (isOptional) {
                keySerializers.emplace_back(std::make_unique<TFixedSizeKeySerializer<ui64, arrow::UInt64Scalar, arrow::UInt64Builder, true>>(arrow::uint64()));
            } else {
                keySerializers.emplace_back(std::make_unique<TFixedSizeKeySerializer<ui64, arrow::UInt64Scalar, arrow::UInt64Builder, false>>(arrow::uint64()));
            }

            break;
        default:
            throw yexception() << "Unsupported key type";
        }
    }
}

}

IComputationNode* WrapBlockCombineAll(TCallable& callable, const TComputationNodeFactoryContext& ctx) {
    MKQL_ENSURE(callable.GetInputsCount() == 3, "Expected 3 args");
    const auto flowType = AS_TYPE(TFlowType, callable.GetInput(0).GetStaticType());
    const auto tupleType = AS_TYPE(TTupleType, flowType->GetItemType());

    auto wideFlow = dynamic_cast<IComputationWideFlowNode*>(LocateNode(ctx.NodeLocator, callable, 0));
    MKQL_ENSURE(wideFlow != nullptr, "Expected wide flow node");

    auto filterColumnVal = AS_VALUE(TOptionalLiteral, callable.GetInput(1));
    std::optional<ui32> filterColumn;
    if (filterColumnVal->HasItem()) {
        filterColumn = AS_VALUE(TDataLiteral, filterColumnVal->GetItem())->AsValue().Get<ui32>();
    }

    auto aggsVal = AS_VALUE(TTupleLiteral, callable.GetInput(2));
    TVector<TAggParams<IBlockAggregatorCombineAll>> aggsParams;
    FillAggParams<IBlockAggregatorCombineAll>(aggsVal, tupleType, filterColumn, aggsParams, ctx.Env);
    return new TBlockCombineAllWrapper(ctx.Mutables, wideFlow, filterColumn, tupleType->GetElementsCount(), std::move(aggsParams));
}

IComputationNode* WrapBlockCombineHashed(TCallable& callable, const TComputationNodeFactoryContext& ctx) {
    MKQL_ENSURE(callable.GetInputsCount() == 4, "Expected 4 args");
    const auto flowType = AS_TYPE(TFlowType, callable.GetInput(0).GetStaticType());
    const auto tupleType = AS_TYPE(TTupleType, flowType->GetItemType());

    auto wideFlow = dynamic_cast<IComputationWideFlowNode*>(LocateNode(ctx.NodeLocator, callable, 0));
    MKQL_ENSURE(wideFlow != nullptr, "Expected wide flow node");

    auto filterColumnVal = AS_VALUE(TOptionalLiteral, callable.GetInput(1));
    std::optional<ui32> filterColumn;
    if (filterColumnVal->HasItem()) {
        filterColumn = AS_VALUE(TDataLiteral, filterColumnVal->GetItem())->AsValue().Get<ui32>();
    }

    auto keysVal = AS_VALUE(TTupleLiteral, callable.GetInput(2));
    std::vector<TKeyParams> keys;
    for (ui32 i = 0; i < keysVal->GetValuesCount(); ++i) {
        ui32 index = AS_VALUE(TDataLiteral, keysVal->GetValue(i))->AsValue().Get<ui32>();
        keys.emplace_back(TKeyParams{ index, tupleType->GetElementType(index) });
    }

    auto aggsVal = AS_VALUE(TTupleLiteral, callable.GetInput(3));
    TVector<TAggParams<IBlockAggregatorCombineKeys>> aggsParams;
    FillAggParams<IBlockAggregatorCombineKeys>(aggsVal, tupleType, filterColumn, aggsParams, ctx.Env);

    ui32 totalKeysSize = 0;
    std::vector<std::unique_ptr<IKeySerializer>> keySerializers;
    PrepareKeys(keys, totalKeysSize, keySerializers);

    if (filterColumn) {
        if (aggsParams.size() == 0) {
            return MakeBlockCombineHashedWrapper<true, true>(totalKeysSize, ctx.Mutables, wideFlow, filterColumn, tupleType->GetElementsCount(), keys, std::move(keySerializers), std::move(aggsParams));
        } else {
            return MakeBlockCombineHashedWrapper<false, true>(totalKeysSize, ctx.Mutables, wideFlow, filterColumn, tupleType->GetElementsCount(), keys, std::move(keySerializers), std::move(aggsParams));
        }
    } else {
        if (aggsParams.size() == 0) {
            return MakeBlockCombineHashedWrapper<true, false>(totalKeysSize, ctx.Mutables, wideFlow, filterColumn, tupleType->GetElementsCount(), keys, std::move(keySerializers), std::move(aggsParams));
        } else {
            return MakeBlockCombineHashedWrapper<false, false>(totalKeysSize, ctx.Mutables, wideFlow, filterColumn, tupleType->GetElementsCount(), keys, std::move(keySerializers), std::move(aggsParams));
        }
    }
}

IComputationNode* WrapBlockMergeFinalizeHashed(TCallable& callable, const TComputationNodeFactoryContext& ctx) {
    MKQL_ENSURE(callable.GetInputsCount() == 3, "Expected 3 args");
    const auto flowType = AS_TYPE(TFlowType, callable.GetInput(0).GetStaticType());
    const auto tupleType = AS_TYPE(TTupleType, flowType->GetItemType());

    auto wideFlow = dynamic_cast<IComputationWideFlowNode*>(LocateNode(ctx.NodeLocator, callable, 0));
    MKQL_ENSURE(wideFlow != nullptr, "Expected wide flow node");

    auto keysVal = AS_VALUE(TTupleLiteral, callable.GetInput(1));
    std::vector<TKeyParams> keys;
    for (ui32 i = 0; i < keysVal->GetValuesCount(); ++i) {
        ui32 index = AS_VALUE(TDataLiteral, keysVal->GetValue(i))->AsValue().Get<ui32>();
        keys.emplace_back(TKeyParams{ index, tupleType->GetElementType(index) });
    }

    auto aggsVal = AS_VALUE(TTupleLiteral, callable.GetInput(2));
    TVector<TAggParams<IBlockAggregatorFinalizeKeys>> aggsParams;
    FillAggParams<IBlockAggregatorFinalizeKeys>(aggsVal, tupleType, {}, aggsParams, ctx.Env);

    ui32 totalKeysSize = 0;
    std::vector<std::unique_ptr<IKeySerializer>> keySerializers;
    PrepareKeys(keys, totalKeysSize, keySerializers);

    if (aggsParams.size() == 0) {
        return MakeBlockMergeFinalizeHashedWrapper<true>(totalKeysSize, ctx.Mutables, wideFlow, tupleType->GetElementsCount(), keys, std::move(keySerializers), std::move(aggsParams));
    } else {
        return MakeBlockMergeFinalizeHashedWrapper<false>(totalKeysSize, ctx.Mutables, wideFlow, tupleType->GetElementsCount(), keys, std::move(keySerializers), std::move(aggsParams));
    }
}

}
}

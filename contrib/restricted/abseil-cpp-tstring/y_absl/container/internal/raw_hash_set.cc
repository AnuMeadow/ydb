// Copyright 2018 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "y_absl/container/internal/raw_hash_set.h"

#include <atomic>
#include <cstddef>

#include "y_absl/base/config.h"

namespace y_absl {
Y_ABSL_NAMESPACE_BEGIN
namespace container_internal {

// A single block of empty control bytes for tables without any slots allocated.
// This enables removing a branch in the hot path of find().
alignas(16) Y_ABSL_CONST_INIT Y_ABSL_DLL const ctrl_t kEmptyGroup[16] = {
    ctrl_t::kSentinel, ctrl_t::kEmpty, ctrl_t::kEmpty, ctrl_t::kEmpty,
    ctrl_t::kEmpty,    ctrl_t::kEmpty, ctrl_t::kEmpty, ctrl_t::kEmpty,
    ctrl_t::kEmpty,    ctrl_t::kEmpty, ctrl_t::kEmpty, ctrl_t::kEmpty,
    ctrl_t::kEmpty,    ctrl_t::kEmpty, ctrl_t::kEmpty, ctrl_t::kEmpty};

#ifdef Y_ABSL_INTERNAL_NEED_REDUNDANT_CONSTEXPR_DECL
constexpr size_t Group::kWidth;
#endif

// Returns "random" seed.
inline size_t RandomSeed() {
#ifdef Y_ABSL_HAVE_THREAD_LOCAL
  static thread_local size_t counter = 0;
  size_t value = ++counter;
#else   // Y_ABSL_HAVE_THREAD_LOCAL
  static std::atomic<size_t> counter(0);
  size_t value = counter.fetch_add(1, std::memory_order_relaxed);
#endif  // Y_ABSL_HAVE_THREAD_LOCAL
  return value ^ static_cast<size_t>(reinterpret_cast<uintptr_t>(&counter));
}

bool ShouldInsertBackwards(size_t hash, const ctrl_t* ctrl) {
  // To avoid problems with weak hashes and single bit tests, we use % 13.
  // TODO(kfm,sbenza): revisit after we do unconditional mixing
  return (H1(hash, ctrl) ^ RandomSeed()) % 13 > 6;
}

void ConvertDeletedToEmptyAndFullToDeleted(ctrl_t* ctrl, size_t capacity) {
  assert(ctrl[capacity] == ctrl_t::kSentinel);
  assert(IsValidCapacity(capacity));
  for (ctrl_t* pos = ctrl; pos < ctrl + capacity; pos += Group::kWidth) {
    Group{pos}.ConvertSpecialToEmptyAndFullToDeleted(pos);
  }
  // Copy the cloned ctrl bytes.
  std::memcpy(ctrl + capacity + 1, ctrl, NumClonedBytes());
  ctrl[capacity] = ctrl_t::kSentinel;
}
// Extern template instantiotion for inline function.
template FindInfo find_first_non_full(const ctrl_t*, size_t, size_t);

}  // namespace container_internal
Y_ABSL_NAMESPACE_END
}  // namespace y_absl

/*
 * Copyright (C) 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/trace_processor/db/column/arrangement_overlay.h"

#include <array>
#include <cstdint>
#include <vector>

#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/db/column/data_layer.h"
#include "src/trace_processor/db/column/fake_storage.h"
#include "src/trace_processor/db/column/numeric_storage.h"
#include "src/trace_processor/db/column/types.h"
#include "src/trace_processor/db/column/utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::column {
namespace {

using testing::ElementsAre;
using testing::IsEmpty;

TEST(ArrangementOverlay, SingleSearch) {
  std::vector<uint32_t> arrangement{1, 1, 2, 2, 3, 3, 4, 4, 1, 1};
  auto fake = FakeStorageChain::SearchSubset(5, std::vector<uint32_t>{1, 2});
  ArrangementOverlay storage(&arrangement, Indices::State::kNonmonotonic);
  auto chain = storage.MakeChain(std::move(fake));

  ASSERT_EQ(chain->SingleSearch(FilterOp::kGe, SqlValue::Long(0u), 8),
            SingleSearchResult::kMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kGe, SqlValue::Long(0u), 4),
            SingleSearchResult::kNoMatch);
}

TEST(ArrangementOverlay, SearchAll) {
  std::vector<uint32_t> arrangement{1, 1, 2, 2, 3, 3, 4, 4, 1, 1};
  auto fake = FakeStorageChain::SearchAll(5);
  ArrangementOverlay storage(&arrangement, Indices::State::kNonmonotonic);
  auto chain = storage.MakeChain(std::move(fake));

  auto res = chain->Search(FilterOp::kGe, SqlValue::Long(0u), Range(2, 4));
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(2u, 3u));
}

TEST(ArrangementOverlay, SearchNone) {
  std::vector<uint32_t> arrangement{1, 1, 2, 2, 3, 3, 4, 4, 1, 1};
  auto fake = FakeStorageChain::SearchNone(5);
  ArrangementOverlay storage(&arrangement, Indices::State::kNonmonotonic);
  auto chain = storage.MakeChain(std::move(fake));

  auto res = chain->Search(FilterOp::kGe, SqlValue::Long(0u), Range(2, 4));
  ASSERT_THAT(utils::ToIndexVectorForTests(res), IsEmpty());
}

TEST(ArrangementOverlay, DISABLED_SearchLimited) {
  std::vector<uint32_t> arrangement{1, 1, 2, 2, 3, 3, 4, 4, 1, 1};
  auto fake = FakeStorageChain::SearchSubset(5, Range(4, 5));
  ArrangementOverlay storage(&arrangement, Indices::State::kNonmonotonic);
  auto chain = storage.MakeChain(std::move(fake));

  auto res = chain->Search(FilterOp::kGe, SqlValue::Long(0u), Range(2, 7));
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(6u));
}

TEST(ArrangementOverlay, SearchBitVector) {
  std::vector<uint32_t> arrangement{1, 1, 2, 2, 3, 3, 4, 4, 1, 1};
  auto fake = FakeStorageChain::SearchSubset(
      5, BitVector({false, true, false, true, false}));
  ArrangementOverlay storage(&arrangement, Indices::State::kNonmonotonic);
  auto chain = storage.MakeChain(std::move(fake));

  // Table bv:
  // 1, 1, 0, 0, 1, 1, 0, 0, 1, 1
  auto res = chain->Search(FilterOp::kGe, SqlValue::Long(0u), Range(0, 10));
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 4, 5, 8, 9));
}

TEST(ArrangementOverlay, IndexSearch) {
  std::vector<uint32_t> arrangement{1, 1, 2, 2, 3, 3, 4, 4, 1, 1};
  auto fake = FakeStorageChain::SearchSubset(
      5, BitVector({false, true, false, true, false}));
  ArrangementOverlay storage(&arrangement, Indices::State::kNonmonotonic);
  auto chain = storage.MakeChain(std::move(fake));

  std::vector<uint32_t> table_idx{7u, 1u, 3u};
  RangeOrBitVector res = chain->IndexSearch(
      FilterOp::kGe, SqlValue::Long(0u),
      Indices{table_idx.data(), static_cast<uint32_t>(table_idx.size()),
              Indices::State::kNonmonotonic});

  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1u));
}

TEST(ArrangementOverlay, OrderingSearch) {
  std::vector<uint32_t> arrangement{0, 2, 4, 1, 3};
  auto fake = FakeStorageChain::SearchSubset(
      5, BitVector({false, true, false, true, false}));
  ArrangementOverlay storage(&arrangement, Indices::State::kNonmonotonic);
  auto chain =
      storage.MakeChain(std::move(fake), DataLayer::ChainCreationArgs(true));

  RangeOrBitVector res =
      chain->Search(FilterOp::kGe, SqlValue::Long(0u), Range(0, 5));

  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(3, 4));
}

TEST(ArrangementOverlay, StableSort) {
  std::vector<uint32_t> numeric_data{0, 1, 2, 3, 4};
  NumericStorage<uint32_t> numeric(&numeric_data, ColumnType::kUint32, false);

  std::vector<uint32_t> arrangement{0, 2, 4, 1, 3};
  ArrangementOverlay storage(&arrangement, Indices::State::kNonmonotonic);
  auto chain = storage.MakeChain(numeric.MakeChain());

  std::vector tokens{
      column::DataLayerChain::SortToken{0, 0},
      column::DataLayerChain::SortToken{1, 1},
      column::DataLayerChain::SortToken{2, 2},
      column::DataLayerChain::SortToken{3, 3},
      column::DataLayerChain::SortToken{4, 4},
  };
  chain->StableSort(tokens.data(), tokens.data() + tokens.size(),
                    column::DataLayerChain::SortDirection::kAscending);
  ASSERT_THAT(utils::ExtractPayloadForTesting(tokens),
              ElementsAre(0, 3, 1, 4, 2));
}

}  // namespace
}  // namespace perfetto::trace_processor::column

/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "velox/common/base/Exceptions.h"
#include "velox/common/base/tests/GTestUtils.h"
#include "velox/common/file/FileSystems.h"
#include "velox/exec/PlanNodeStats.h"
#include "velox/exec/RowsStreamingWindowBuild.h"
#include "velox/exec/SortWindowBuild.h"
#include "velox/exec/tests/utils/AssertQueryBuilder.h"
#include "velox/exec/tests/utils/OperatorTestBase.h"
#include "velox/exec/tests/utils/PlanBuilder.h"
#include "velox/exec/tests/utils/TempDirectoryPath.h"
#include "velox/functions/prestosql/window/WindowFunctionsRegistration.h"

using namespace facebook::velox::exec::test;

namespace facebook::velox::exec {

namespace {

class WindowTest : public OperatorTestBase {
 public:
  void SetUp() override {
    OperatorTestBase::SetUp();
    window::prestosql::registerAllWindowFunctions();
    filesystems::registerLocalFileSystem();
  }

  common::SpillConfig getSpillConfig(
      const std::string& spillDir,
      bool enablePrefixSort) const {
    const auto prefixSortConfig = enablePrefixSort
        ? std::optional<common::PrefixSortConfig>(common::PrefixSortConfig())
        : std::nullopt;
    return common::SpillConfig(
        [spillDir]() -> const std::string& { return spillDir; },
        [&](uint64_t) {},
        "0.0.0",
        0,
        0,
        1 << 20,
        executor_.get(),
        5,
        10,
        0,
        0,
        0,
        0,
        0,
        "none",
        prefixSortConfig);
  }

  const std::shared_ptr<folly::Executor> executor_{
      std::make_shared<folly::CPUThreadPoolExecutor>(
          std::thread::hardware_concurrency())};

  tsan_atomic<bool> nonReclaimableSection_{false};
};

TEST_F(WindowTest, spill) {
  const vector_size_t size = 1'000;
  auto data = makeRowVector(
      {"d", "p", "s"},
      {
          // Payload.
          makeFlatVector<int64_t>(size, [](auto row) { return row; }),
          // Partition key.
          makeFlatVector<int16_t>(size, [](auto row) { return row % 11; }),
          // Sorting key.
          makeFlatVector<int32_t>(size, [](auto row) { return row; }),
      });

  createDuckDbTable({data});

  core::PlanNodeId windowId;
  auto plan = PlanBuilder()
                  .values(split(data, 10))
                  .window({"row_number() over (partition by p order by s)"})
                  .capturePlanNodeId(windowId)
                  .planNode();

  auto spillDirectory = TempDirectoryPath::create();
  TestScopedSpillInjection scopedSpillInjection(100);
  auto task =
      AssertQueryBuilder(plan, duckDbQueryRunner_)
          .config(core::QueryConfig::kPreferredOutputBatchBytes, "1024")
          .config(core::QueryConfig::kSpillEnabled, "true")
          .config(core::QueryConfig::kWindowSpillEnabled, "true")
          .spillDirectory(spillDirectory->getPath())
          .assertResults(
              "SELECT *, row_number() over (partition by p order by s) FROM tmp");

  auto taskStats = exec::toPlanStats(task->taskStats());
  const auto& stats = taskStats.at(windowId);

  ASSERT_GT(stats.spilledBytes, 0);
  ASSERT_GT(stats.spilledRows, 0);
  ASSERT_GT(stats.spilledFiles, 0);
  ASSERT_GT(stats.spilledPartitions, 0);
}

TEST_F(WindowTest, spillUnsupported) {
  const vector_size_t size = 1'000;
  auto data = makeRowVector(
      {"d", "p", "s"},
      {
          // Payload.
          makeFlatVector<int64_t>(size, [](auto row) { return row; }),
          // Partition key.
          makeFlatVector<int16_t>(size, [](auto row) { return row % 11; }),
          // Sorting key.
          makeFlatVector<int32_t>(size, [](auto row) { return row; }),
      });

  createDuckDbTable({data});

  core::PlanNodeId windowId;
  auto plan = PlanBuilder()
                  .values(split(data, 10))
                  .window({"row_number() over (order by s)"})
                  .capturePlanNodeId(windowId)
                  .planNode();

  auto spillDirectory = TempDirectoryPath::create();
  TestScopedSpillInjection scopedSpillInjection(100);
  auto task =
      AssertQueryBuilder(plan, duckDbQueryRunner_)
          .config(core::QueryConfig::kPreferredOutputBatchBytes, "1024")
          .config(core::QueryConfig::kSpillEnabled, "true")
          .config(core::QueryConfig::kWindowSpillEnabled, "true")
          .spillDirectory(spillDirectory->getPath())
          .assertResults("SELECT *, row_number() over (order by s) FROM tmp");

  auto taskStats = exec::toPlanStats(task->taskStats());
  const auto& stats = taskStats.at(windowId);

  ASSERT_EQ(stats.spilledBytes, 0);
  ASSERT_EQ(stats.spilledRows, 0);
  ASSERT_EQ(stats.spilledFiles, 0);
  ASSERT_EQ(stats.spilledPartitions, 0);
  auto opStats = toOperatorStats(task->taskStats());
  ASSERT_GT(
      opStats.at("Window").runtimeStats[Operator::kSpillNotSupported].sum, 1);
}

TEST_F(WindowTest, rowBasedStreamingWindowOOM) {
  const vector_size_t size = 1'000'000;
  auto data = makeRowVector(
      {"d", "p", "s"},
      {
          // Payload.
          makeFlatVector<int64_t>(size, [](auto row) { return row; }),
          // Partition key.
          makeFlatVector<int16_t>(size, [](auto row) { return row; }),
          // Sorting key.
          makeFlatVector<int32_t>(size, [](auto row) { return row; }),
      });

  createDuckDbTable({data});

  // Abstract the common values vector split.
  auto valuesSplit = split(data, 10);

  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
  CursorParameters params;
  auto queryCtx = core::QueryCtx::create(executor_.get());
  queryCtx->testingOverrideMemoryPool(memory::memoryManager()->addRootPool(
      queryCtx->queryId(),
      8'388'608 /* 8MB */,
      exec::MemoryReclaimer::create()));

  params.queryCtx = queryCtx;

  auto testWindowBuild = [&](bool useStreamingWindow) {
    if (useStreamingWindow) {
      params.planNode =
          PlanBuilder(planNodeIdGenerator)
              .values(valuesSplit)
              .streamingWindow(
                  {"row_number() over (partition by p order by s)"})
              .project({"d"})
              .singleAggregation({}, {"sum(d)"})
              .planNode();

      readCursor(params);
    } else {
      params.planNode =
          PlanBuilder(planNodeIdGenerator)
              .values(valuesSplit)
              .window({"row_number() over (partition by p order by s)"})
              .project({"d"})
              .singleAggregation({}, {"sum(d)"})
              .planNode();

      VELOX_ASSERT_THROW(readCursor(params), "Exceeded memory pool capacity");
    }
  };
  // RowStreamingWindow will not OOM.
  testWindowBuild(true);
  // SortBasedWindow will OOM.
  testWindowBuild(false);
}

DEBUG_ONLY_TEST_F(WindowTest, aggWindowResultMismatch) {
  auto data = makeRowVector(
      {"id", "order_num"},
      {makeFlatVector<int64_t>(4500, [](auto row) { return row; }),
       makeConstant(1, 4500)});

  createDuckDbTable({data});

  const std::vector<std::string> kClauses = {
      "sum(order_num) over (order by order_num DESC)"};

  auto plan = PlanBuilder()
                  .values({data})
                  .orderBy({"order_num"}, false)
                  .streamingWindow(kClauses)
                  .planNode();

  std::atomic_bool isStreamCreated{false};
  SCOPED_TESTVALUE_SET(
      "facebook::velox::exec::RowsStreamingWindowBuild::RowsStreamingWindowBuild",
      std::function<void(RowsStreamingWindowBuild*)>(
          [&](RowsStreamingWindowBuild* windowBuild) {
            isStreamCreated.store(true);
          }));

  AssertQueryBuilder(plan, duckDbQueryRunner_)
      .config(core::QueryConfig::kPreferredOutputBatchBytes, "1024")
      .config(core::QueryConfig::kPreferredOutputBatchRows, "2")
      .config(core::QueryConfig::kMaxOutputBatchRows, "2")
      .assertResults(
          "SELECT *, sum(order_num) over (order by order_num DESC) FROM tmp");
  ASSERT_TRUE(isStreamCreated.load());
}

DEBUG_ONLY_TEST_F(WindowTest, rankRowStreamingWindowBuild) {
  auto data = makeRowVector(
      {"c1"},
      {makeFlatVector<int64_t>(std::vector<int64_t>{1, 1, 1, 1, 1, 2, 2})});

  createDuckDbTable({data});

  const std::vector<std::string> kClauses = {
      "rank() over (order by c1 rows unbounded preceding)"};

  auto plan = PlanBuilder()
                  .values({data})
                  .orderBy({"c1"}, false)
                  .streamingWindow(kClauses)
                  .planNode();

  std::atomic_bool isStreamCreated{false};
  SCOPED_TESTVALUE_SET(
      "facebook::velox::exec::RowsStreamingWindowBuild::RowsStreamingWindowBuild",
      std::function<void(RowsStreamingWindowBuild*)>(
          [&](RowsStreamingWindowBuild* windowBuild) {
            isStreamCreated.store(true);
          }));

  AssertQueryBuilder(plan, duckDbQueryRunner_)
      .config(core::QueryConfig::kPreferredOutputBatchBytes, "1024")
      .config(core::QueryConfig::kPreferredOutputBatchRows, "2")
      .config(core::QueryConfig::kMaxOutputBatchRows, "2")
      .assertResults(
          "SELECT *, rank() over (order by c1 rows unbounded preceding) FROM tmp");

  ASSERT_TRUE(isStreamCreated.load());
}

DEBUG_ONLY_TEST_F(WindowTest, valuesRowsStreamingWindowBuild) {
  const vector_size_t size = 1'00;

  auto data = makeRowVector(
      {makeFlatVector<int32_t>(size, [](auto row) { return row % 5; }),
       makeFlatVector<int32_t>(size, [](auto row) { return row % 50; }),
       makeFlatVector<int64_t>(
           size, [](auto row) { return row % 3 + 1; }, nullEvery(5)),
       makeFlatVector<int32_t>(size, [](auto row) { return row % 40; }),
       makeFlatVector<int32_t>(size, [](auto row) { return row; })});

  createDuckDbTable({data});

  const std::vector<std::string> kClauses = {
      "rank() over (partition by c0, c2 order by c1, c3)",
      "dense_rank() over (partition by c0, c2 order by c1, c3)",
      "row_number() over (partition by c0, c2 order by c1, c3)",
      "sum(c4) over (partition by c0, c2 order by c1, c3)"};

  auto plan = PlanBuilder()
                  .values({split(data, 10)})
                  .orderBy({"c0", "c2", "c1", "c3"}, false)
                  .streamingWindow(kClauses)
                  .planNode();

  std::atomic_bool isStreamCreated{false};
  SCOPED_TESTVALUE_SET(
      "facebook::velox::exec::RowsStreamingWindowBuild::RowsStreamingWindowBuild",
      std::function<void(RowsStreamingWindowBuild*)>(
          [&](RowsStreamingWindowBuild* windowBuild) {
            isStreamCreated.store(true);
          }));

  AssertQueryBuilder(plan, duckDbQueryRunner_)
      .config(core::QueryConfig::kPreferredOutputBatchBytes, "1024")
      .assertResults(
          "SELECT *, rank() over (partition by c0, c2 order by c1, c3), dense_rank() over (partition by c0, c2 order by c1, c3), row_number() over (partition by c0, c2 order by c1, c3), sum(c4) over (partition by c0, c2 order by c1, c3) FROM tmp");
  ASSERT_TRUE(isStreamCreated.load());
}

DEBUG_ONLY_TEST_F(WindowTest, aggregationWithNonDefaultFrame) {
  const vector_size_t size = 1'00;

  auto data = makeRowVector(
      {makeFlatVector<int32_t>(size, [](auto row) { return row % 5; }),
       makeFlatVector<int32_t>(size, [](auto row) { return row % 50; }),
       makeFlatVector<int64_t>(
           size, [](auto row) { return row % 3 + 1; }, nullEvery(5)),
       makeFlatVector<int32_t>(size, [](auto row) { return row % 40; }),
       makeFlatVector<int32_t>(size, [](auto row) { return row; })});

  createDuckDbTable({data});

  const std::vector<std::string> kClauses = {
      "sum(c4) over (partition by c0, c2 order by c1, c3 range between unbounded preceding and unbounded following)"};

  auto plan = PlanBuilder()
                  .values({split(data, 10)})
                  .orderBy({"c0", "c2", "c1", "c3"}, false)
                  .streamingWindow(kClauses)
                  .planNode();

  std::atomic_bool isStreamCreated{false};
  SCOPED_TESTVALUE_SET(
      "facebook::velox::exec::RowsStreamingWindowBuild::RowsStreamingWindowBuild",
      std::function<void(RowsStreamingWindowBuild*)>(
          [&](RowsStreamingWindowBuild* windowBuild) {
            isStreamCreated.store(true);
          }));

  AssertQueryBuilder(plan, duckDbQueryRunner_)
      .config(core::QueryConfig::kPreferredOutputBatchBytes, "1024")
      .assertResults(
          "SELECT *, sum(c4) over (partition by c0, c2 order by c1, c3 range between unbounded preceding and unbounded following) FROM tmp");

  ASSERT_FALSE(isStreamCreated.load());
}

DEBUG_ONLY_TEST_F(WindowTest, nonRowsStreamingWindow) {
  auto data = makeRowVector(
      {"c1"},
      {makeFlatVector<int64_t>(std::vector<int64_t>{1, 1, 1, 1, 1, 2, 2})});

  createDuckDbTable({data});

  const std::vector<std::string> kClauses = {
      "first_value(c1) over (order by c1 rows unbounded preceding)",
      "nth_value(c1, 1) over (order by c1 rows unbounded preceding)"};

  auto plan = PlanBuilder()
                  .values({data})
                  .orderBy({"c1"}, false)
                  .streamingWindow(kClauses)
                  .planNode();

  std::atomic_bool isStreamCreated{false};
  SCOPED_TESTVALUE_SET(
      "facebook::velox::exec::RowsStreamingWindowBuild::RowsStreamingWindowBuild",
      std::function<void(RowsStreamingWindowBuild*)>(
          [&](RowsStreamingWindowBuild* windowBuild) {
            isStreamCreated.store(true);
          }));

  AssertQueryBuilder(plan, duckDbQueryRunner_)
      .config(core::QueryConfig::kPreferredOutputBatchBytes, "1024")
      .config(core::QueryConfig::kPreferredOutputBatchRows, "2")
      .config(core::QueryConfig::kMaxOutputBatchRows, "2")
      .assertResults(
          "SELECT *, first_value(c1) over (order by c1 rows unbounded preceding), nth_value(c1, 1) over (order by c1 rows unbounded preceding) FROM tmp");
  ASSERT_FALSE(isStreamCreated.load());
}

TEST_F(WindowTest, missingFunctionSignature) {
  auto input = {makeRowVector({
      makeFlatVector<int64_t>({1, 2, 3}),
      makeFlatVector<std::string>({"A", "B", "C"}),
      makeFlatVector<int64_t>({10, 20, 30}),
  })};

  auto runWindow = [&](const core::CallTypedExprPtr& callExpr) {
    core::WindowNode::Frame frame{
        core::WindowNode::WindowType::kRows,
        core::WindowNode::BoundType::kUnboundedPreceding,
        nullptr,
        core::WindowNode::BoundType::kUnboundedFollowing,
        nullptr};

    core::WindowNode::Function windowFunction{callExpr, frame, false};

    CursorParameters params;
    params.planNode =
        PlanBuilder()
            .values(input)
            .addNode([&](auto nodeId, auto source) -> core::PlanNodePtr {
              return std::make_shared<core::WindowNode>(
                  nodeId,
                  std::vector<core::FieldAccessTypedExprPtr>{
                      std::make_shared<core::FieldAccessTypedExpr>(
                          BIGINT(), "c0")},
                  std::vector<core::FieldAccessTypedExprPtr>{}, // sortingKeys
                  std::vector<core::SortOrder>{}, // sortingOrders
                  std::vector<std::string>{"w"},
                  std::vector<core::WindowNode::Function>{windowFunction},
                  false,
                  source);
            })
            .planNode();

    readCursor(params);
  };

  auto callExpr = std::make_shared<core::CallTypedExpr>(
      BIGINT(),
      std::vector<core::TypedExprPtr>{
          std::make_shared<core::FieldAccessTypedExpr>(VARCHAR(), "c1")},
      "sum");

  VELOX_ASSERT_THROW(
      runWindow(callExpr),
      "Window function signature is not supported: sum(VARCHAR). Supported signatures:");

  callExpr = std::make_shared<core::CallTypedExpr>(
      VARCHAR(),
      std::vector<core::TypedExprPtr>{
          std::make_shared<core::FieldAccessTypedExpr>(BIGINT(), "c2")},
      "sum");

  VELOX_ASSERT_THROW(
      runWindow(callExpr),
      "Unexpected return type for window function sum(BIGINT). Expected BIGINT. Got VARCHAR.");
}

TEST_F(WindowTest, duplicateOrOverlappingKeys) {
  auto data = makeRowVector(
      ROW({"a", "b", "c", "d", "e"},
          {
              BIGINT(),
              BIGINT(),
              BIGINT(),
              BIGINT(),
              BIGINT(),
          }),
      10);

  auto plan = [&](const std::vector<std::string>& partitionKeys,
                  const std::vector<std::string>& sortingKeys) {
    std::ostringstream sql;
    sql << "row_number() over (";
    if (!partitionKeys.empty()) {
      sql << " partition by ";
      sql << folly::join(", ", partitionKeys);
    }
    if (!sortingKeys.empty()) {
      sql << " order by ";
      sql << folly::join(", ", sortingKeys);
    }
    sql << ")";

    PlanBuilder().values({data}).window({sql.str()}).planNode();
  };

  VELOX_ASSERT_THROW(
      plan({"a", "a"}, {"b"}),
      "Partitioning keys must be unique. Found duplicate key: a");

  VELOX_ASSERT_THROW(
      plan({"a", "b"}, {"c", "d", "c"}),
      "Sorting keys must be unique and not overlap with partitioning keys. Found duplicate key: c");

  VELOX_ASSERT_THROW(
      plan({"a", "b"}, {"c", "b"}),
      "Sorting keys must be unique and not overlap with partitioning keys. Found duplicate key: b");
}

TEST_F(WindowTest, nagativeFrameArg) {
  const vector_size_t size = 1'000;

  auto sizeAt = [](vector_size_t row) { return row % 5; };
  auto keyAt = [](vector_size_t row) { return row % 11; };
  auto keys = makeArrayVector<float>(size, sizeAt, keyAt);
  auto data = makeRowVector(
      {"c0", "c1", "p0", "p1", "k0", "row_number"},
      {
          // Payload.
          makeFlatVector<float>(size, [](auto row) { return row; }),
          makeFlatVector<float>(size, [](auto row) { return row; }),
          // Partition key.
          keys,
          makeFlatVector<std::string>(
              size, [](auto row) { return fmt::format("{}", row + 20); }),
          makeFlatVector<int32_t>(size, [](auto row) { return row; }),
          // Sorting key.
          makeFlatVector<int64_t>(size, [](auto row) { return row; }),
      });

  createDuckDbTable({data});

  struct {
    std::string fragmentStart;
    std::string fragmentEnd;

    std::string debugString() const {
      if (fragmentStart[0] == '-') {
        return fmt::format(
            "Window frame {} offset must not be negative", fragmentStart);
      } else {
        return fmt::format(
            "Window frame {} offset must not be negative", fragmentEnd);
      }
    }
  } testSettings[] = {
      {"k0", "-1"}, // Negative end
      {"-1", "k0"}, // Negative start
      {"-1", "-3"} // Negative start, negative end
  };
  for (const auto& testData : testSettings) {
    SCOPED_TRACE(testData.debugString());
    const auto& startOffset = testData.fragmentStart;
    const auto& endOffset = testData.fragmentEnd;
    auto plan =
        PlanBuilder()
            .values(split(data, 10))
            .window({fmt::format(
                "regr_count(c0, c1) over (partition by p0, p1 order by row_number ROWS between {} PRECEDING and {} FOLLOWING)",
                startOffset,
                endOffset)})
            .planNode();
    VELOX_ASSERT_USER_THROW(
        AssertQueryBuilder(plan, duckDbQueryRunner_)
            .assertResults(fmt::format(
                "SELECT *, regr_count(c0, c1) over (partition by p0, p1 order by row_number  ROWS between {} PRECEDING and {} FOLLOWING) from tmp",
                startOffset,
                endOffset)),
        testData.debugString());
  }
}

DEBUG_ONLY_TEST_F(WindowTest, frameColumnNullCheck) {
  auto makePlan = [&](const RowVectorPtr& input) {
    return PlanBuilder()
        .values({input})
        .window(
            {"sum(c0) OVER (PARTITION BY p0 ORDER BY s0 RANGE BETWEEN UNBOUNDED PRECEDING AND off0 FOLLOWING)"})
        .planNode();
  };

  // Null values in order-by column 's0' and frame column 'off0' do not match,
  // so exception is expected.
  auto inputThrow = makeRowVector(
      {"c0", "p0", "s0", "off0"},
      {
          makeNullableFlatVector<int64_t>({1, std::nullopt, 1, 2, 2}),
          makeFlatVector<int64_t>({1, 2, 1, 2, 1}),
          makeNullableFlatVector<int64_t>({1, 2, 3, std::nullopt, 5}),
          makeNullableFlatVector<int64_t>({2, std::nullopt, 4, 5, 6}),
      });
  VELOX_ASSERT_THROW(
      AssertQueryBuilder(makePlan(inputThrow)).copyResults(pool()), "");

  // Null values in order-by column 's0' and frame column 'off0' match, so no
  // exception should be thrown.
  auto inputNoThrow = makeRowVector(
      {"c0", "p0", "s0", "off0"},
      {
          makeNullableFlatVector<int64_t>({1, 1, 2, std::nullopt, 2}),
          makeFlatVector<int64_t>({1, 1, 1, 2, 2}),
          makeNullableFlatVector<int64_t>({1, std::nullopt, 2, 3, 5}),
          makeNullableFlatVector<int64_t>({2, std::nullopt, 3, 4, 6}),
      });
  ASSERT_NO_THROW(
      AssertQueryBuilder(makePlan(inputNoThrow)).copyResults(pool()));
}

DEBUG_ONLY_TEST_F(WindowTest, reserveMemorySort) {
  struct {
    bool usePrefixSort;
    bool spillEnabled;
    bool enableSpillPrefixSort;
  } testSettings[] = {
      {false, true, false}, {true, false, true}, {true, true, false}};

  const vector_size_t size = 1'000;
  auto prefixSortData = makeRowVector(
      {"d", "p", "s"},
      {
          // Payload.
          makeFlatVector<int64_t>(size, [](auto row) { return row; }),
          // Partition key.
          makeFlatVector<int16_t>(size, [](auto row) { return row % 11; }),
          // Sorting key.
          makeFlatVector<int32_t>(size, [](auto row) { return row; }),
      });
  auto prefixSortPlan = std::dynamic_pointer_cast<const core::WindowNode>(
      PlanBuilder()
          .values(split(prefixSortData, 10))
          .window({"row_number() over (partition by p order by s)"})
          .planNode());

  const std::vector<std::string> fruits = {
      "apple", "banana", "pear", "grapes", "mango", "grapefruit"};
  auto nonPrefixSortData = makeRowVector(
      {"d", "p", "s"},
      {
          // Payload.
          makeFlatVector<int64_t>(size, [](auto row) { return row; }),
          // Partition key.
          makeFlatVector<int16_t>(size, [](auto row) { return row % 11; }),
          // Sorting key.
          makeFlatVector<StringView>(
              size,
              [&fruits](auto row) {
                return StringView(fruits[row % fruits.size()]);
              }),
      });
  auto nonPrefixSortPlan = std::dynamic_pointer_cast<const core::WindowNode>(
      PlanBuilder()
          .values(split(nonPrefixSortData, 10))
          .window({"row_number() over (partition by p order by s)"})
          .planNode());

  for (const auto [usePrefixSort, spillEnabled, enableSpillPrefixSort] :
       testSettings) {
    SCOPED_TRACE(fmt::format(
        "usePrefixSort: {}, spillEnabled: {}, enableSpillPrefixSort: {}",
        usePrefixSort,
        spillEnabled,
        enableSpillPrefixSort));
    auto spillDirectory = exec::test::TempDirectoryPath::create();
    auto spillConfig =
        getSpillConfig(spillDirectory->getPath(), enableSpillPrefixSort);
    folly::Synchronized<common::SpillStats> spillStats;
    const auto plan = usePrefixSort ? prefixSortPlan : nonPrefixSortPlan;
    velox::common::PrefixSortConfig prefixSortConfig =
        velox::common::PrefixSortConfig{
            std::numeric_limits<int32_t>::max(), 130, 12};
    auto sortWindowBuild = std::make_unique<SortWindowBuild>(
        plan,
        pool_.get(),
        std::move(prefixSortConfig),
        spillEnabled ? &spillConfig : nullptr,
        &nonReclaimableSection_,
        &spillStats);

    TestScopedSpillInjection scopedSpillInjection(0);
    const auto data = usePrefixSort ? prefixSortData : nonPrefixSortData;
    sortWindowBuild->addInput(data);

    std::atomic_bool hasReserveMemory = false;
    // Reserve memory for sort.
    SCOPED_TESTVALUE_SET(
        "facebook::velox::common::memory::MemoryPoolImpl::maybeReserve",
        std::function<void(memory::MemoryPoolImpl*)>(
            ([&](memory::MemoryPoolImpl* pool) {
              hasReserveMemory.store(true);
            })));

    sortWindowBuild->noMoreInput();
    if (spillEnabled) {
      // Reserve memory for sort.
      ASSERT_TRUE(hasReserveMemory);
    } else {
      ASSERT_FALSE(hasReserveMemory);
    }
  }
}

TEST_F(WindowTest, NaNFrameBound) {
  const auto kNan = std::numeric_limits<double>::quiet_NaN();
  auto data = makeRowVector(
      {"c0", "s0", "off0", "off1"},
      {
          makeFlatVector<int64_t>({1, 2, 3, 4}),
          makeFlatVector<double>({1.0, 2.0, 3.0, kNan}),
          makeFlatVector<double>({0.1, 2.0, 1.9, kNan}),
          makeFlatVector<double>({kNan, 2.0, kNan, kNan}),
      });

  const auto makeFrames = [](const std::string& call) {
    std::vector<std::string> frames;

    std::vector<std::string> orders{"asc", "desc"};
    std::vector<std::string> bounds{"preceding", "following"};
    for (const std::string& order : orders) {
      for (const std::string& startBound : bounds) {
        for (const std::string& endBound : bounds) {
          // Frames starting from following and ending at preceding are not
          // allowed.
          if (startBound == "following" && endBound == "preceding") {
            continue;
          }
          frames.push_back(fmt::format(
              "{} over (order by s0 {} range between off0 {} and off1 {})",
              call,
              order,
              startBound,
              endBound));
          frames.push_back(fmt::format(
              "{} over (order by s0 {} range between off1 {} and off0 {})",
              call,
              order,
              startBound,
              endBound));
        }
      }
    }
    return frames;
  };

  auto expected = makeRowVector(
      {makeNullableFlatVector<int64_t>({std::nullopt, 2, std::nullopt, 4})});
  for (const auto& frame : makeFrames("sum(c0)")) {
    auto plan =
        PlanBuilder().values({data}).window({frame}).project({"w0"}).planNode();
    AssertQueryBuilder(plan).assertResults(expected);
  }

  // rank() should not be affected by the frames, so added this test to ensure
  // rank() produces correct results even if the frame bounds contain NaN.
  expected = makeRowVector({makeFlatVector<int64_t>({1, 2, 3, 4})});
  for (const auto& frame : makeFrames("rank()")) {
    auto plan =
        PlanBuilder().values({data}).window({frame}).project({"w0"}).planNode();
    AssertQueryBuilder(plan).assertResults(expected);
  }
}

} // namespace
} // namespace facebook::velox::exec

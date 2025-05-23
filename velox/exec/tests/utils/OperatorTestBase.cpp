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

#include "velox/exec/tests/utils/OperatorTestBase.h"
#include "velox/common/base/PeriodicStatsReporter.h"
#include "velox/common/caching/AsyncDataCache.h"
#include "velox/common/file/FileSystems.h"
#include "velox/common/memory/MallocAllocator.h"
#include "velox/common/memory/SharedArbitrator.h"
#include "velox/common/testutil/TestValue.h"
#include "velox/exec/tests/utils/LocalExchangeSource.h"
#include "velox/functions/prestosql/aggregates/RegisterAggregateFunctions.h"
#include "velox/functions/prestosql/registration/RegistrationFunctions.h"
#include "velox/parse/Expressions.h"
#include "velox/parse/ExpressionsParser.h"
#include "velox/parse/TypeResolver.h"
#include "velox/serializers/CompactRowSerializer.h"
#include "velox/serializers/PrestoSerializer.h"
#include "velox/serializers/UnsafeRowSerializer.h"
#include "velox/vector/tests/utils/VectorMaker.h"

DECLARE_bool(velox_memory_leak_check_enabled);
DECLARE_bool(velox_enable_memory_usage_track_in_default_memory_pool);

using namespace facebook::velox::common::testutil;
using namespace facebook::velox::memory;

namespace facebook::velox::exec::test {

OperatorTestBase::OperatorTestBase() {
  // Overloads the memory pools used by VectorTestBase to work with memory
  // arbitrator.
  rootPool_ = memory::memoryManager()->addRootPool(
      "", memory::kMaxMemory, exec::MemoryReclaimer::create());
  pool_ = rootPool_->addLeafChild("", true, exec::MemoryReclaimer::create());
  vectorMaker_ = velox::test::VectorMaker(pool_.get());

  parse::registerTypeResolver();
}

void OperatorTestBase::registerVectorSerde() {
  velox::serializer::presto::PrestoVectorSerde::registerVectorSerde();
}

OperatorTestBase::~OperatorTestBase() {
  // Wait for all the tasks to be deleted.
  exec::test::waitForAllTasksToBeDeleted();
}

void OperatorTestBase::SetUpTestCase() {
  FLAGS_velox_enable_memory_usage_track_in_default_memory_pool = true;
  FLAGS_velox_memory_leak_check_enabled = true;
  memory::SharedArbitrator::registerFactory();
  resetMemory();
  functions::prestosql::registerAllScalarFunctions();
  aggregate::prestosql::registerAllAggregateFunctions();
  TestValue::enable();
}

void OperatorTestBase::TearDownTestCase() {
  asyncDataCache_->shutdown();
  waitForAllTasksToBeDeleted();
  memory::SharedArbitrator::unregisterFactory();
}

void OperatorTestBase::setupMemory(
    int64_t allocatorCapacity,
    int64_t arbitratorCapacity,
    int64_t arbitratorReservedCapacity,
    int64_t memoryPoolInitCapacity,
    int64_t memoryPoolReservedCapacity,
    int64_t memoryPoolMinReclaimBytes,
    int64_t memoryPoolAbortCapacityLimit) {
  if (asyncDataCache_ != nullptr) {
    asyncDataCache_->clear();
    asyncDataCache_.reset();
  }
  MemoryManagerOptions options;
  options.allocatorCapacity = allocatorCapacity;
  options.arbitratorCapacity = arbitratorCapacity;
  options.arbitratorKind = "SHARED";
  options.checkUsageLeak = true;
  options.arbitrationStateCheckCb = memoryArbitrationStateCheck;

  using ExtraConfig = SharedArbitrator::ExtraConfig;
  options.extraArbitratorConfigs = {
      {std::string(ExtraConfig::kReservedCapacity),
       folly::to<std::string>(arbitratorReservedCapacity) + "B"},
      {std::string(ExtraConfig::kMemoryPoolInitialCapacity),
       folly::to<std::string>(memoryPoolInitCapacity) + "B"},
      {std::string(ExtraConfig::kMemoryPoolReservedCapacity),
       folly::to<std::string>(memoryPoolReservedCapacity) + "B"},
      {std::string(ExtraConfig::kMemoryPoolMinReclaimBytes),
       folly::to<std::string>(memoryPoolMinReclaimBytes) + "B"},
      // For simplicity, we set the reclaim pct to 0, so that the tests will be
      // purely based on kMemoryPoolMinReclaimBytes.
      {std::string(ExtraConfig::kMemoryPoolMinReclaimPct), "0"},
      {std::string(ExtraConfig::kMemoryPoolAbortCapacityLimit),
       folly::to<std::string>(memoryPoolAbortCapacityLimit) + "B"},
      {std::string(ExtraConfig::kGlobalArbitrationEnabled), "true"},
  };

  memory::MemoryManager::testingSetInstance(options);
  asyncDataCache_ =
      cache::AsyncDataCache::create(memory::memoryManager()->allocator());
  cache::AsyncDataCache::setInstance(asyncDataCache_.get());
}

void OperatorTestBase::resetMemory() {
  OperatorTestBase::setupMemory(8L << 30, 6L << 30, 0, 512 << 20, 0, 0, 0);
}

void OperatorTestBase::SetUp() {
  if (!isRegisteredVectorSerde()) {
    this->registerVectorSerde();
  }
  if (!isRegisteredNamedVectorSerde(VectorSerde::Kind::kPresto)) {
    serializer::presto::PrestoVectorSerde::registerNamedVectorSerde();
  }
  if (!isRegisteredNamedVectorSerde(VectorSerde::Kind::kCompactRow)) {
    serializer::CompactRowVectorSerde::registerNamedVectorSerde();
  }
  if (!isRegisteredNamedVectorSerde(VectorSerde::Kind::kUnsafeRow)) {
    serializer::spark::UnsafeRowVectorSerde::registerNamedVectorSerde();
  }
  driverExecutor_ = std::make_unique<folly::CPUThreadPoolExecutor>(3);
  ioExecutor_ = std::make_unique<folly::IOThreadPoolExecutor>(3);
  PeriodicStatsReporter::Options options;
  options.allocator = memory::memoryManager()->allocator();
  options.allocatorStatsIntervalMs = 2'000;
  options.cache = asyncDataCache_.get();
  options.cacheStatsIntervalMs = 2'000;
  options.arbitrator = memory::memoryManager()->arbitrator();
  options.arbitratorStatsIntervalMs = 2'000;
  options.spillMemoryPool = memory::spillMemoryPool();
  options.spillStatsIntervalMs = 2'000;
  startPeriodicStatsReporter(options);
  testingStartLocalExchangeSource();
}

void OperatorTestBase::TearDown() {
  waitForAllTasksToBeDeleted();
  stopPeriodicStatsReporter();
  // There might be lingering exchange source on executor even after all tasks
  // are deleted. This can cause memory leak because exchange source holds
  // reference to memory pool. We need to make sure they are properly cleaned.
  testingShutdownLocalExchangeSource();
  pool_.reset();
  rootPool_.reset();
  resetMemory();
}

std::shared_ptr<Task> OperatorTestBase::assertQuery(
    const core::PlanNodePtr& plan,
    const std::vector<std::shared_ptr<connector::ConnectorSplit>>&
        connectorSplits,
    const std::string& duckDbSql,
    std::optional<std::vector<uint32_t>> sortingKeys) {
  std::vector<exec::Split> splits;
  splits.reserve(connectorSplits.size());
  for (const auto& connectorSplit : connectorSplits) {
    splits.emplace_back(exec::Split(folly::copy(connectorSplit), -1));
  }

  return assertQuery(plan, std::move(splits), duckDbSql, sortingKeys);
}

namespace {
/// Returns the plan node ID of the only leaf plan node. Throws if 'root' has
/// multiple leaf nodes.
core::PlanNodeId getOnlyLeafPlanNodeId(const core::PlanNodePtr& root) {
  const auto& sources = root->sources();
  if (sources.empty()) {
    return root->id();
  }

  VELOX_CHECK_EQ(1, sources.size());
  return getOnlyLeafPlanNodeId(sources[0]);
}

std::function<void(TaskCursor* taskCursor)> makeAddSplit(
    std::unordered_map<core::PlanNodeId, std::vector<exec::Split>>&& splits) {
  return [&](TaskCursor* taskCursor) {
    if (taskCursor->noMoreSplits()) {
      return;
    }
    auto& task = taskCursor->task();
    for (auto& [nodeId, nodeSplits] : splits) {
      for (auto& split : nodeSplits) {
        task->addSplit(nodeId, std::move(split));
      }
      task->noMoreSplits(nodeId);
    }
    taskCursor->setNoMoreSplits();
  };
}
} // namespace

std::shared_ptr<Task> OperatorTestBase::assertQuery(
    const core::PlanNodePtr& plan,
    std::vector<exec::Split>&& splits,
    const std::string& duckDbSql,
    std::optional<std::vector<uint32_t>> sortingKeys) {
  const auto splitNodeId = getOnlyLeafPlanNodeId(plan);
  return assertQuery(
      plan, {{splitNodeId, std::move(splits)}}, duckDbSql, sortingKeys);
}

std::shared_ptr<Task> OperatorTestBase::assertQuery(
    const core::PlanNodePtr& plan,
    std::unordered_map<core::PlanNodeId, std::vector<exec::Split>>&& splits,
    const std::string& duckDbSql,
    std::optional<std::vector<uint32_t>> sortingKeys) {
  return test::assertQuery(
      plan,
      makeAddSplit(std::move(splits)),
      duckDbSql,
      duckDbQueryRunner_,
      sortingKeys);
}

// static
std::shared_ptr<core::FieldAccessTypedExpr> OperatorTestBase::toFieldExpr(
    const std::string& name,
    const RowTypePtr& rowType) {
  return std::make_shared<core::FieldAccessTypedExpr>(
      rowType->findChild(name), name);
}

core::TypedExprPtr OperatorTestBase::parseExpr(
    const std::string& text,
    RowTypePtr rowType,
    const parse::ParseOptions& options) {
  auto untyped = parse::parseExpr(text, options);
  return core::Expressions::inferTypes(untyped, rowType, pool_.get());
}

/*static*/ void OperatorTestBase::deleteTaskAndCheckSpillDirectory(
    std::shared_ptr<Task>& task) {
  const auto spillDirectoryStr = task->spillDirectory();
  // Nothing to do if there is no spilling directory was set.
  if (spillDirectoryStr.empty()) {
    return;
  }

  // Wait for the task to go.
  task.reset();
  waitForAllTasksToBeDeleted();

  // If a spilling directory was set, ensure it was removed after the task is
  // gone.
  auto fs = filesystems::getFileSystem(spillDirectoryStr, nullptr);
  EXPECT_FALSE(fs->exists(spillDirectoryStr));
}

} // namespace facebook::velox::exec::test

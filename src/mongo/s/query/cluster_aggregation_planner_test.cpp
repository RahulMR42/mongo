/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"


#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/db/pipeline/document_source_out_gen.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/stub_mongo_process_interface.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog_cache_test_fixture.h"
#include "mongo/s/query/cluster_aggregation_planner.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

namespace {

const NamespaceString kTestAggregateNss = NamespaceString{"unittests", "cluster_exchange"};
const NamespaceString kTestOutNss = NamespaceString{"unittests", "out_ns"};

/**
 * For the purposes of this test, assume every collection is sharded. Stages may ask this during
 * setup. For example, to compute its constraints, the $out stage needs to know if the output
 * collection is sharded.
 */
class FakeMongoProcessInterface : public StubMongoProcessInterface {
public:
    bool isSharded(OperationContext* opCtx, const NamespaceString& ns) override {
        return true;
    }
};

class ClusterExchangeTest : public CatalogCacheTestFixture {
public:
    void setUp() {
        CatalogCacheTestFixture::setUp();
        _expCtx = new ExpressionContextForTest(operationContext(),
                                               AggregationRequest{kTestAggregateNss, {}});
        _expCtx->mongoProcessInterface = std::make_shared<FakeMongoProcessInterface>();
    }

    boost::intrusive_ptr<ExpressionContext> expCtx() {
        return _expCtx;
    }

    boost::intrusive_ptr<DocumentSource> parse(const std::string& json) {
        auto stages = DocumentSource::parse(_expCtx, fromjson(json));
        ASSERT_EQ(stages.size(), 1UL);
        return stages.front();
    }

    std::vector<ChunkType> makeChunks(const NamespaceString& nss,
                                      const OID epoch,
                                      std::vector<std::pair<ChunkRange, ShardId>> chunkInfos) {
        ChunkVersion version(1, 0, epoch);
        std::vector<ChunkType> chunks;
        for (auto&& pair : chunkInfos) {
            chunks.emplace_back(nss, pair.first, version, pair.second);
            version.incMinor();
        }
        return chunks;
    }

    void loadRoutingTable(NamespaceString nss,
                          const OID epoch,
                          const ShardKeyPattern& shardKey,
                          const std::vector<ChunkType>& chunkDistribution) {
        auto future = scheduleRoutingInfoRefresh(nss);

        // Mock the expected config server queries.
        expectGetDatabase(nss);
        expectGetCollection(nss, epoch, shardKey);
        expectGetCollection(nss, epoch, shardKey);
        expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
            std::vector<BSONObj> response;
            for (auto&& chunk : chunkDistribution) {
                response.push_back(chunk.toConfigBSON());
            }
            return response;
        }());

        future.timed_get(kFutureTimeout).get();
    }

private:
    boost::intrusive_ptr<ExpressionContext> _expCtx;
};

TEST_F(ClusterExchangeTest, ShouldNotExchangeIfPipelineDoesNotEndWithOut) {
    setupNShards(2);
    auto mergePipe =
        unittest::assertGet(Pipeline::create({DocumentSourceLimit::create(expCtx(), 1)}, expCtx()));
    ASSERT_FALSE(cluster_aggregation_planner::checkIfEligibleForExchange(operationContext(),
                                                                         mergePipe.get()));
    mergePipe = unittest::assertGet(
        Pipeline::create({DocumentSourceMatch::create(BSONObj(), expCtx())}, expCtx()));
    ASSERT_FALSE(cluster_aggregation_planner::checkIfEligibleForExchange(operationContext(),
                                                                         mergePipe.get()));
}

TEST_F(ClusterExchangeTest, ShouldNotExchangeIfPipelineEndsWithReplaceCollectionOut) {
    setupNShards(2);

    // For this test pretend 'kTestOutNss' is not sharded so that we can use a "replaceCollection"
    // $out.
    const auto originalMongoProcessInterface = expCtx()->mongoProcessInterface;
    expCtx()->mongoProcessInterface = std::make_shared<StubMongoProcessInterface>();
    ON_BLOCK_EXIT([&]() { expCtx()->mongoProcessInterface = originalMongoProcessInterface; });

    auto mergePipe = unittest::assertGet(Pipeline::create(
        {DocumentSourceOut::create(kTestOutNss, expCtx(), WriteModeEnum::kModeReplaceCollection)},
        expCtx()));
    ASSERT_FALSE(cluster_aggregation_planner::checkIfEligibleForExchange(operationContext(),
                                                                         mergePipe.get()));
}

TEST_F(ClusterExchangeTest, SingleOutStageNotEligibleForExchangeIfOutputDatabaseDoesNotExist) {
    setupNShards(2);
    auto mergePipe = unittest::assertGet(Pipeline::create(
        {DocumentSourceOut::create(kTestOutNss, expCtx(), WriteModeEnum::kModeInsertDocuments)},
        expCtx()));

    auto future = launchAsync([&] {
        ASSERT_THROWS_CODE(cluster_aggregation_planner::checkIfEligibleForExchange(
                               operationContext(), mergePipe.get()),
                           AssertionException,
                           ErrorCodes::NamespaceNotFound);
    });

    // Mock out a response as if the database doesn't exist.
    expectFindSendBSONObjVector(kConfigHostAndPort, []() { return std::vector<BSONObj>{}; }());
    expectFindSendBSONObjVector(kConfigHostAndPort, []() { return std::vector<BSONObj>{}; }());

    future.timed_get(kFutureTimeout);
}

// If the output collection doesn't exist, we don't know how to distribute the output documents so
// cannot insert an $exchange. The $out stage should later create a new, unsharded collection.
TEST_F(ClusterExchangeTest, SingleOutStageNotEligibleForExchangeIfOutputCollectionDoesNotExist) {
    setupNShards(2);
    auto mergePipe = unittest::assertGet(Pipeline::create(
        {DocumentSourceOut::create(kTestOutNss, expCtx(), WriteModeEnum::kModeInsertDocuments)},
        expCtx()));

    auto future = launchAsync([&] {
        ASSERT_FALSE(cluster_aggregation_planner::checkIfEligibleForExchange(operationContext(),
                                                                             mergePipe.get()));
    });

    expectGetDatabase(kTestOutNss);
    // Pretend there are no collections in this database.
    expectFindSendBSONObjVector(kConfigHostAndPort, std::vector<BSONObj>());

    future.timed_get(kFutureTimeout);
}

// A $limit stage requires a single merger.
TEST_F(ClusterExchangeTest, LimitFollowedByOutStageIsNotEligibleForExchange) {
    // Sharded by {_id: 1}, [MinKey, 0) on shard "0", [0, MaxKey) on shard "1".
    setupNShards(2);
    loadRoutingTableWithTwoChunksAndTwoShards(kTestOutNss);

    auto mergePipe = unittest::assertGet(Pipeline::create(
        {DocumentSourceLimit::create(expCtx(), 6),
         DocumentSourceOut::create(kTestOutNss, expCtx(), WriteModeEnum::kModeInsertDocuments)},
        expCtx()));

    auto future = launchAsync([&] {
        ASSERT_FALSE(cluster_aggregation_planner::checkIfEligibleForExchange(operationContext(),
                                                                             mergePipe.get()));
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ClusterExchangeTest, GroupFollowedByOutIsEligbleForExchange) {
    // Sharded by {_id: 1}, [MinKey, 0) on shard "0", [0, MaxKey) on shard "1".
    setupNShards(2);
    loadRoutingTableWithTwoChunksAndTwoShards(kTestOutNss);

    auto mergePipe = unittest::assertGet(Pipeline::create(
        {parse("{$group: {_id: '$x', $doingMerge: true}}"),
         DocumentSourceOut::create(kTestOutNss, expCtx(), WriteModeEnum::kModeInsertDocuments)},
        expCtx()));

    auto future = launchAsync([&] {
        auto exchangeSpec = cluster_aggregation_planner::checkIfEligibleForExchange(
            operationContext(), mergePipe.get());
        ASSERT_TRUE(exchangeSpec);
        ASSERT(exchangeSpec->policy == ExchangePolicyEnum::kRange);
        ASSERT_TRUE(exchangeSpec->shardDistributionInfo);
        const auto& partitions = exchangeSpec->shardDistributionInfo->partitions;
        ASSERT_EQ(partitions.size(), 2UL);  // One for each shard.

        auto shard0Ranges = partitions.find("0");
        ASSERT(shard0Ranges != partitions.end());
        ASSERT_EQ(shard0Ranges->second.size(), 1UL);
        auto shard0Range = shard0Ranges->second[0];
        ASSERT(shard0Range == ChunkRange(BSON("_id" << MINKEY), BSON("_id" << 0)));
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ClusterExchangeTest, RenamesAreEligibleForExchange) {
    // Sharded by {_id: 1}, [MinKey, 0) on shard "0", [0, MaxKey) on shard "1".
    setupNShards(2);
    loadRoutingTableWithTwoChunksAndTwoShards(kTestOutNss);

    auto mergePipe = unittest::assertGet(Pipeline::create(
        {parse("{$group: {_id: '$x', $doingMerge: true}}"),
         parse("{$project: {temporarily_renamed: '$_id'}}"),
         parse("{$project: {_id: '$temporarily_renamed'}}"),
         DocumentSourceOut::create(kTestOutNss, expCtx(), WriteModeEnum::kModeInsertDocuments)},
        expCtx()));

    auto future = launchAsync([&] {
        auto exchangeSpec = cluster_aggregation_planner::checkIfEligibleForExchange(
            operationContext(), mergePipe.get());
        ASSERT_TRUE(exchangeSpec);
        ASSERT(exchangeSpec->policy == ExchangePolicyEnum::kRange);
        ASSERT_TRUE(exchangeSpec->shardDistributionInfo);
        const auto& partitions = exchangeSpec->shardDistributionInfo->partitions;
        ASSERT_EQ(partitions.size(), 2UL);  // One for each shard.

        auto shard0Ranges = partitions.find("0");
        ASSERT(shard0Ranges != partitions.end());
        ASSERT_EQ(shard0Ranges->second.size(), 1UL);
        auto shard0Range = shard0Ranges->second[0];
        ASSERT(shard0Range == ChunkRange(BSON("_id" << MINKEY), BSON("_id" << 0)));

        auto shard1Ranges = partitions.find("1");
        ASSERT(shard1Ranges != partitions.end());
        ASSERT_EQ(shard1Ranges->second.size(), 1UL);
        auto shard1Range = shard1Ranges->second[0];
        ASSERT(shard1Range == ChunkRange(BSON("_id" << 0), BSON("_id" << MAXKEY)));
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ClusterExchangeTest, SortThenGroupIsEligibleForExchange) {
    // Sharded by {_id: 1}, [MinKey, 0) on shard "0", [0, MaxKey) on shard "1".
    setupNShards(2);
    loadRoutingTableWithTwoChunksAndTwoShards(kTestOutNss);

    // This would be the merging half of the pipeline if the original pipeline was
    // [{$sort: {x: 1}},
    //  {$group: {_id: "$x"}},
    //  {$out: {to: "sharded_by_id", mode: "replaceDocuments"}}].
    // No $sort stage appears in the merging half since we'd expect that to be absorbed by the
    // $mergeCursors and AsyncResultsMerger.
    auto mergePipe = unittest::assertGet(Pipeline::create(
        {parse("{$group: {_id: '$x'}}"),
         DocumentSourceOut::create(kTestOutNss, expCtx(), WriteModeEnum::kModeInsertDocuments)},
        expCtx()));

    auto future = launchAsync([&] {
        auto exchangeSpec = cluster_aggregation_planner::checkIfEligibleForExchange(
            operationContext(), mergePipe.get());
        ASSERT_TRUE(exchangeSpec);
        ASSERT(exchangeSpec->policy == ExchangePolicyEnum::kRange);
        ASSERT_TRUE(exchangeSpec->shardDistributionInfo);
        ASSERT_BSONOBJ_EQ(exchangeSpec->shardDistributionInfo->logicalShardKeyAtSplitPoint.toBSON(),
                          BSON("x" << 1));
        const auto& partitions = exchangeSpec->shardDistributionInfo->partitions;
        ASSERT_EQ(partitions.size(), 2UL);  // One for each shard.

        auto shard0Ranges = partitions.find("0");
        ASSERT(shard0Ranges != partitions.end());
        ASSERT_EQ(shard0Ranges->second.size(), 1UL);
        auto shard0Range = shard0Ranges->second[0];
        ASSERT(shard0Range == ChunkRange(BSON("x" << MINKEY), BSON("x" << 0)));

        auto shard1Ranges = partitions.find("1");
        ASSERT(shard1Ranges != partitions.end());
        ASSERT_EQ(shard1Ranges->second.size(), 1UL);
        auto shard1Range = shard1Ranges->second[0];
        ASSERT(shard1Range == ChunkRange(BSON("x" << 0), BSON("x" << MAXKEY)));
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ClusterExchangeTest, ProjectThroughDottedFieldDoesNotPreserveShardKey) {
    // Sharded by {_id: 1}, [MinKey, 0) on shard "0", [0, MaxKey) on shard "1".
    setupNShards(2);
    loadRoutingTableWithTwoChunksAndTwoShards(kTestOutNss);

    auto mergePipe = unittest::assertGet(Pipeline::create(
        {parse("{$group: {"
               "  _id: {region: '$region', country: '$country'},"
               "  population: {$sum: '$population'},"
               "  cities: {$push: {name: '$city', population: '$population'}}"
               "}}"),
         parse(
             "{$project: {_id: '$_id.country', region: '$_id.region', population: 1, cities: 1}}"),
         DocumentSourceOut::create(kTestOutNss, expCtx(), WriteModeEnum::kModeInsertDocuments)},
        expCtx()));

    auto future = launchAsync([&] {
        auto exchangeSpec = cluster_aggregation_planner::checkIfEligibleForExchange(
            operationContext(), mergePipe.get());
        // Because '_id' is populated from '$_id.country', we cannot prove that '_id' is a simple
        // rename. We cannot prove that '_id' is not an array, and thus the $project could do more
        // than a rename.
        ASSERT_FALSE(exchangeSpec);
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ClusterExchangeTest, WordCountUseCaseExample) {
    // Sharded by {_id: 1}, [MinKey, 0) on shard "0", [0, MaxKey) on shard "1".
    setupNShards(2);
    loadRoutingTableWithTwoChunksAndTwoShards(kTestOutNss);

    // As an example of a pipeline that might replace a map reduce, imagine that we are performing a
    // word count, and the shards part of the pipeline tokenized some text field of each document
    // into {word: <token>, count: 1}. Then this is the merging half of the pipeline:
    auto mergePipe = unittest::assertGet(Pipeline::create(
        {parse("{$group: {"
               "  _id: '$word',"
               "  count: {$sum: 1},"
               "  $doingMerge: true"
               "}}"),
         DocumentSourceOut::create(kTestOutNss, expCtx(), WriteModeEnum::kModeInsertDocuments)},
        expCtx()));

    auto future = launchAsync([&] {
        auto exchangeSpec = cluster_aggregation_planner::checkIfEligibleForExchange(
            operationContext(), mergePipe.get());
        ASSERT_TRUE(exchangeSpec);
        ASSERT(exchangeSpec->policy == ExchangePolicyEnum::kRange);
        ASSERT_TRUE(exchangeSpec->shardDistributionInfo);
        const auto& partitions = exchangeSpec->shardDistributionInfo->partitions;
        ASSERT_EQ(partitions.size(), 2UL);  // One for each shard.

        auto shard0Ranges = partitions.find("0");
        ASSERT(shard0Ranges != partitions.end());
        ASSERT_EQ(shard0Ranges->second.size(), 1UL);
        auto shard0Range = shard0Ranges->second[0];
        ASSERT(shard0Range == ChunkRange(BSON("_id" << MINKEY), BSON("_id" << 0)));

        auto shard1Ranges = partitions.find("1");
        ASSERT(shard1Ranges != partitions.end());
        ASSERT_EQ(shard1Ranges->second.size(), 1UL);
        auto shard1Range = shard1Ranges->second[0];
        ASSERT(shard1Range == ChunkRange(BSON("_id" << 0), BSON("_id" << MAXKEY)));
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ClusterExchangeTest, WordCountUseCaseExampleShardedByWord) {
    setupNShards(2);
    const OID epoch = OID::gen();
    ShardKeyPattern shardKey(BSON("word" << 1));
    loadRoutingTable(kTestOutNss,
                     epoch,
                     shardKey,
                     makeChunks(kTestOutNss,
                                epoch,
                                {{ChunkRange{BSON("word" << MINKEY),
                                             BSON("word"
                                                  << "hello")},
                                  ShardId("0")},
                                 {ChunkRange{BSON("word"
                                                  << "hello"),
                                             BSON("word"
                                                  << "world")},
                                  ShardId("1")},
                                 {ChunkRange{BSON("word"
                                                  << "world"),
                                             BSON("word" << MAXKEY)},
                                  ShardId("1")}}));

    // As an example of a pipeline that might replace a map reduce, imagine that we are performing a
    // word count, and the shards part of the pipeline tokenized some text field of each document
    // into {word: <token>, count: 1}. Then this is the merging half of the pipeline:
    auto mergePipe = unittest::assertGet(Pipeline::create(
        {parse("{$group: {"
               "  _id: '$word',"
               "  count: {$sum: 1},"
               "  $doingMerge: true"
               "}}"),
         parse("{$project: {word: '$_id', count: 1}}"),
         DocumentSourceOut::create(kTestOutNss, expCtx(), WriteModeEnum::kModeInsertDocuments)},
        expCtx()));

    auto future = launchAsync([&] {
        auto exchangeSpec = cluster_aggregation_planner::checkIfEligibleForExchange(
            operationContext(), mergePipe.get());
        ASSERT_TRUE(exchangeSpec);
        ASSERT(exchangeSpec->policy == ExchangePolicyEnum::kRange);
        ASSERT_TRUE(exchangeSpec->shardDistributionInfo);
        ASSERT_BSONOBJ_EQ(exchangeSpec->shardDistributionInfo->logicalShardKeyAtSplitPoint.toBSON(),
                          BSON("_id" << 1));
        const auto& partitions = exchangeSpec->shardDistributionInfo->partitions;
        ASSERT_EQ(partitions.size(), 2UL);  // One for each shard.

        auto shard0Ranges = partitions.find("0");
        ASSERT(shard0Ranges != partitions.end());
        ASSERT_EQ(shard0Ranges->second.size(), 1UL);
        auto firstRangeOnShard0 = shard0Ranges->second[0];
        ASSERT(firstRangeOnShard0 == ChunkRange(BSON("_id" << MINKEY),
                                                BSON("_id"
                                                     << "hello")));

        auto shard1Ranges = partitions.find("1");
        ASSERT(shard1Ranges != partitions.end());
        ASSERT_EQ(shard1Ranges->second.size(), 2UL);
        auto firstRangeOnShard1 = shard1Ranges->second[0];
        ASSERT(firstRangeOnShard1 == ChunkRange(BSON("_id"
                                                     << "hello"),
                                                BSON("_id"
                                                     << "world")));
        auto secondRangeOnShard1 = shard1Ranges->second[1];
        ASSERT(secondRangeOnShard1 == ChunkRange(BSON("_id"
                                                      << "world"),
                                                 BSON("_id" << MAXKEY)));
    });

    future.timed_get(kFutureTimeout);
}

// We'd like to test that a compound shard key pattern can be used. Strangely, the only case we can
// actually perform an exchange today on a compound shard key is when the shard key contains fields
// which are all duplicates. This is due to the limitations of tracking renames through dots, see
// SERVER-36787 for an example.
TEST_F(ClusterExchangeTest, CompoundShardKeyThreeShards) {
    const OID epoch = OID::gen();
    ShardKeyPattern shardKey(BSON("x" << 1 << "y" << 1));

    setupNShards(3);
    const std::vector<std::string> xBoundaries = {"a", "g", "m", "r", "u"};
    auto chunks = [&]() {
        std::vector<ChunkType> chunks;
        ChunkVersion version(1, 0, epoch);
        chunks.emplace_back(kTestOutNss,
                            ChunkRange{BSON("x" << MINKEY << "y" << MINKEY),
                                       BSON("x" << xBoundaries[0] << "y" << MINKEY)},
                            version,
                            ShardId("0"));
        for (std::size_t i = 0; i < xBoundaries.size() - 1; ++i) {
            chunks.emplace_back(kTestOutNss,
                                ChunkRange{BSON("x" << xBoundaries[i] << "y" << MINKEY),
                                           BSON("x" << xBoundaries[i + 1] << "y" << MINKEY)},
                                version,
                                ShardId(str::stream() << i % 3));
        }
        chunks.emplace_back(kTestOutNss,
                            ChunkRange{BSON("x" << xBoundaries.back() << "y" << MINKEY),
                                       BSON("x" << MAXKEY << "y" << MAXKEY)},
                            version,
                            ShardId(str::stream() << "1"));
        return chunks;
    }();

    loadRoutingTable(kTestOutNss, epoch, shardKey, chunks);

    auto mergePipe = unittest::assertGet(Pipeline::create(
        {parse("{$group: {"
               "  _id: '$x',"
               "  $doingMerge: true"
               "}}"),
         parse("{$project: {x: '$_id', y: '$_id'}}"),
         DocumentSourceOut::create(kTestOutNss, expCtx(), WriteModeEnum::kModeInsertDocuments)},
        expCtx()));

    auto future = launchAsync([&] {
        auto exchangeSpec = cluster_aggregation_planner::checkIfEligibleForExchange(
            operationContext(), mergePipe.get());
        ASSERT_TRUE(exchangeSpec);
        ASSERT(exchangeSpec->policy == ExchangePolicyEnum::kRange);
        ASSERT_TRUE(exchangeSpec->shardDistributionInfo);
        ASSERT_BSONOBJ_EQ(exchangeSpec->shardDistributionInfo->logicalShardKeyAtSplitPoint.toBSON(),
                          BSON("_id" << 1 << "_id" << 1));
        const auto& partitions = exchangeSpec->shardDistributionInfo->partitions;
        ASSERT_EQ(partitions.size(), 3UL);  // One for each shard.

        // Make sure each shard has the same chunks that it started with, just with the names of the
        // boundary fields translated. For each chunk that we created to begin with, make sure its
        // corresponding/translated chunk is present on the same shard in the same order.
        StringMap<std::size_t> numChunksExaminedOnShard = {{"0", 0}, {"1", 0}, {"2", 0}};
        for (auto&& chunk : chunks) {
            auto shardId = chunk.getShard().toString();
            auto shardRanges = partitions.find(shardId);
            ASSERT(shardRanges != partitions.end());
            auto nextChunkOnShard = numChunksExaminedOnShard[shardId]++;
            ASSERT_LTE(nextChunkOnShard, shardRanges->second.size());
            auto outputChunk = shardRanges->second[nextChunkOnShard];

            auto expectedChunkMin = [&]() {
                ASSERT_EQ(chunk.getMin().nFields(), 2);
                return BSON("_id" << chunk.getMin()["x"] << "_id" << chunk.getMin()["y"]);
            }();
            ASSERT_BSONOBJ_EQ(outputChunk.getMin(), expectedChunkMin);

            auto expectedChunkMax = [&]() {
                ASSERT_EQ(chunk.getMax().nFields(), 2);
                return BSON("_id" << chunk.getMax()["x"] << "_id" << chunk.getMax()["y"]);
            }();
            ASSERT_BSONOBJ_EQ(outputChunk.getMax(), expectedChunkMax);
        }
    });

    future.timed_get(kFutureTimeout);
}
}  // namespace
}  // namespace mongo

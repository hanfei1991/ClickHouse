#pragma once
#include <Interpreters/ActionsDAG.h>
#include <Processors/QueryPlan/QueryPlan.h>

namespace DB
{

class ReadFromMergeTree;

using PartitionIdToMaxBlock = std::unordered_map<String, Int64>;

struct ProjectionDescription;

class MergeTreeDataSelectExecutor;

struct MergeTreeDataSelectAnalysisResult;
using MergeTreeDataSelectAnalysisResultPtr = std::shared_ptr<MergeTreeDataSelectAnalysisResult>;

class IMergeTreeDataPart;
using DataPartPtr = std::shared_ptr<const IMergeTreeDataPart>;
using DataPartsVector = std::vector<DataPartPtr>;

struct StorageInMemoryMetadata;
using StorageMetadataPtr = std::shared_ptr<const StorageInMemoryMetadata>;

struct SelectQueryInfo;

}

namespace DB::QueryPlanOptimizations
{

/// Common checks that projection can be used for this step.
bool canUseProjectionForReadingStep(ReadFromMergeTree * reading);

/// Max blocks for sequential consistency reading from replicated table.
std::shared_ptr<PartitionIdToMaxBlock> getMaxAddedBlocks(ReadFromMergeTree * reading);

/// This is a common DAG which is a merge of DAGs from Filter and Expression steps chain.
/// Additionally, for all the Filter steps, we collect filter conditions into filter_nodes.
/// Flag remove_last_filter_node is set in case if the last step is a Filter step and it should remove filter column.
struct QueryDAG
{
    ActionsDAGPtr dag;
    ActionsDAG::NodeRawConstPtrs filter_nodes;

    bool build(QueryPlan::Node & node);

private:
    void appendExpression(const ActionsDAGPtr & expression);
};

struct ProjectionCandidate
{
    const ProjectionDescription * projection;

    /// The number of marks we are going to read
    size_t sum_marks = 0;

    /// Analysis result, separate for parts with and without projection.
    /// Analysis is done in order to estimate the number of marks we are going to read.
    /// For chosen projection, it is reused for reading step.
    MergeTreeDataSelectAnalysisResultPtr merge_tree_projection_select_result_ptr;
    MergeTreeDataSelectAnalysisResultPtr merge_tree_normal_select_result_ptr;
};

/// This function fills ProjectionCandidate structure for specified projection.
/// It returns false if for some reason we cannot read from projection.
bool analyzeProjectionCandidate(
    ProjectionCandidate & candidate,
    const ReadFromMergeTree & reading,
    const MergeTreeDataSelectExecutor & reader,
    const Names & required_column_names,
    const DataPartsVector & parts,
    const StorageMetadataPtr & metadata,
    const SelectQueryInfo & query_info,
    const ContextPtr & context,
    const std::shared_ptr<PartitionIdToMaxBlock> & max_added_blocks,
    const ActionDAGNodes & added_filter_nodes);

}

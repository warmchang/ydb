#pragma once

#include "dq_opt.h"

#include <ydb/library/yql/dq/common/dq_common.h>
#include <ydb/library/yql/dq/expr_nodes/dq_expr_nodes.h>

#include <yql/essentials/ast/yql_expr.h>
#include <yql/essentials/core/yql_expr_optimize.h>

namespace NYql {
    struct TTypeAnnotationContext;
}

namespace NYql::NDq {

NNodes::TMaybeNode<NNodes::TDqStage> DqPushLambdaToStage(const NNodes::TDqStage &stage,
    const NNodes::TCoAtom& outputIndex, const NNodes::TCoLambda& lambda,
    const TVector<NNodes::TDqConnection>& lambdaInputs, TExprContext& ctx, IOptimizationContext& optCtx);

TExprNode::TPtr DqBuildPushableStage(const NNodes::TDqConnection& connection, TExprContext& ctx);

NNodes::TMaybeNode<NNodes::TDqConnection> DqPushLambdaToStageUnionAll(const NNodes::TDqConnection& connection, const NNodes::TCoLambda& lambda,
    const TVector<NNodes::TDqConnection>& lambdaInputs, TExprContext& ctx, IOptimizationContext& optCtx);

void DqPushLambdasToStagesUnionAll(std::vector<std::pair<NNodes::TDqCnUnionAll, NNodes::TCoLambda>>& items, TExprContext& ctx, IOptimizationContext& optCtx);

NNodes::TExprBase DqPushSkipNullMembersToStage(NNodes::TExprBase node, TExprContext& ctx, IOptimizationContext& optCtx,
    const TParentsMap& parentsMap, bool allowStageMultiUsage = true);

NNodes::TExprBase DqPushPruneKeysToStage(NNodes::TExprBase node, TExprContext& ctx, IOptimizationContext& optCtx,
    const TParentsMap& parentsMap, bool allowStageMultiUsage = true);

NNodes::TExprBase DqPushPruneAdjacentKeysToStage(NNodes::TExprBase node, TExprContext& ctx, IOptimizationContext& optCtx,
    const TParentsMap& parentsMap, bool allowStageMultiUsage = true);

NNodes::TExprBase DqPushExtractMembersToStage(NNodes::TExprBase node, TExprContext& ctx, IOptimizationContext& optCtx,
    const TParentsMap& parentsMap, bool allowStageMultiUsage = true);

NNodes::TExprBase DqPushAssumeDistinctToStage(NNodes::TExprBase node, TExprContext& ctx, IOptimizationContext& optCtx,
    const TParentsMap& parentsMap, bool allowStageMultiUsage = true);

NNodes::TExprBase DqPushAssumeUniqueToStage(NNodes::TExprBase node, TExprContext& ctx, IOptimizationContext& optCtx,
const TParentsMap& parentsMap, bool allowStageMultiUsage = true);

NNodes::TExprBase DqPushOrderedLMapToStage(NNodes::TExprBase node, TExprContext& ctx, IOptimizationContext& optCtx,
    const TParentsMap& parentsMap, bool allowStageMultiUsage = true);

NNodes::TExprBase DqPushLMapToStage(NNodes::TExprBase node, TExprContext& ctx, IOptimizationContext& optCtx,
    const TParentsMap& parentsMap, bool allowStageMultiUsage = true);

NNodes::TExprBase DqBuildPureFlatmapStage(NNodes::TExprBase node, TExprContext& ctx);

NNodes::TExprBase DqBuildFlatmapStage(NNodes::TExprBase node, TExprContext& ctx, IOptimizationContext& optCtx,
    const TParentsMap& parentsMap, bool allowStageMultiUsage = true);

NNodes::TExprBase DqPushFlatmapToStage(NNodes::TExprBase node, TExprContext& ctx, IOptimizationContext& optCtx,
    const TParentsMap& parentsMap, bool allowStageMultiUsage = true);

NNodes::TExprBase DqPushCombineToStage(NNodes::TExprBase node, TExprContext& ctx, IOptimizationContext& optCtx,
    const TParentsMap& parentsMap, bool allowStageMultiUsage = true);

NNodes::TExprBase DqPushCombineToStageDependsOnOtherStage(NNodes::TExprBase node, TExprContext& ctx, IOptimizationContext& optCtx,
    const TParentsMap& parentsMap, bool allowStageMultiUsage = true);

NNodes::TExprBase DqPushAggregateCombineToStage(NNodes::TExprBase node, TExprContext& ctx, IOptimizationContext& optCtx,
    const TParentsMap& parentsMap, bool allowStageMultiUsage = true);

NNodes::TExprBase DqBuildPartitionsStage(
    NNodes::TExprBase node,
    TExprContext& ctx,
    IOptimizationContext& optCtx,
    const TParentsMap& parentsMap,
    bool allowStageMultiUsage = true,
    TTypeAnnotationContext* typeCtx = nullptr,
    bool enableShuffleElimination = false
);

NNodes::TExprBase DqBuildPartitionStage(
    NNodes::TExprBase node,
    TExprContext& ctx,
    IOptimizationContext& optCtx,
    const TParentsMap& parentsMap,
    bool allowStageMultiUsage = true,
    TTypeAnnotationContext* typeCtx = nullptr,
    bool enableShuffleElimination = false
);

NNodes::TExprBase DqBuildShuffleStage(
    NNodes::TExprBase node,
    TExprContext& ctx,
    IOptimizationContext& optCtx,
    const TParentsMap& parentsMap,
    bool allowStageMultiUsage = true,
    TTypeAnnotationContext* typeCtx = nullptr,
    bool enableShuffleElimination = false
);

NNodes::TExprBase DqBuildFinalizeByKeyStage(NNodes::TExprBase node, TExprContext& ctx,
    const TParentsMap& parentsMap, bool allowStageMultiUsage = true);

NNodes::TExprBase DqBuildAggregationResultStage(NNodes::TExprBase node, TExprContext& ctx,
    IOptimizationContext& optCtx);

NNodes::TExprBase DqBuildTopStageRemoveSort(NNodes::TExprBase node, TExprContext& ctx, IOptimizationContext& optCtx,
    TTypeAnnotationContext& typeCtx, const TParentsMap& parentsMap, bool allowStageMultiUsage = true);

NNodes::TExprBase DqBuildTopStage(NNodes::TExprBase node, TExprContext& ctx, IOptimizationContext& optCtx,
    const TParentsMap& parentsMap, bool allowStageMultiUsage = true);

NNodes::TExprBase DqBuildTopSortStage(NNodes::TExprBase node, TExprContext& ctx, IOptimizationContext& optCtx,
    const TParentsMap& parentsMap, bool allowStageMultiUsage = true);

NNodes::TExprBase DqBuildSortStage(NNodes::TExprBase node, TExprContext& ctx, IOptimizationContext& optCtx,
    const TParentsMap& parentsMap, bool allowStageMultiUsage = true);

NNodes::TExprBase DqBuildSkipStage(NNodes::TExprBase node, TExprContext& ctx, IOptimizationContext& optCtx,
    const TParentsMap& parentsMap, bool allowStageMultiUsage = true);

NNodes::TExprBase DqBuildTakeStage(NNodes::TExprBase node, TExprContext& ctx, IOptimizationContext& optCtx,
    const TParentsMap& parentsMap, bool allowStageMultiUsage = true);

NNodes::TExprBase DqBuildTakeSkipStage(NNodes::TExprBase node, TExprContext& ctx, IOptimizationContext& optCtx,
    const TParentsMap& parentsMap, bool allowStageMultiUsage = true);

NNodes::TExprBase DqRewriteLengthOfStageOutput(NNodes::TExprBase node, TExprContext& ctx, IOptimizationContext& optCtx,
    const TParentsMap& parentsMap, bool allowStageMultiUsage);

NNodes::TExprBase DqRewriteRightJoinToLeft(const NNodes::TExprBase node, TExprContext& ctx);

NNodes::TExprBase DqRewriteLeftPureJoin(const NNodes::TExprBase node, TExprContext& ctx,
    const TParentsMap& parentsMap, bool allowStageMultiUsage = true);

bool DqValidateJoinInputs(
    const NNodes::TExprBase& left, const NNodes::TExprBase& right, const TParentsMap& parentsMap,
    bool allowStageMultiUsage);

NNodes::TMaybeNode<NNodes::TDqJoin> DqFlipJoin(const NNodes::TDqJoin& join, TExprContext& ctx);

TMaybe<std::pair<NNodes::TExprBase, NNodes::TDqConnection>>  ExtractPureExprStage(TExprNode::TPtr input,
    TExprContext& ctx);

NNodes::TExprBase DqBuildPureExprStage(NNodes::TExprBase node, TExprContext& ctx);

NNodes::TExprBase DqBuildOrderedLMapOverMuxStage(NNodes::TExprBase node, TExprContext& ctx, IOptimizationContext& optCtx, const TParentsMap& parentsMap);

NNodes::TExprBase DqBuildLMapOverMuxStage(NNodes::TExprBase node, TExprContext& ctx, IOptimizationContext& optCtx, const TParentsMap& parentsMap);

NNodes::TExprBase DqBuildExtendStage(NNodes::TExprBase node, TExprContext& ctx, bool enableParallelUnionAllConnections = false);

NNodes::TExprBase DqBuildPrecompute(NNodes::TExprBase node, TExprContext& ctx);

NNodes::TExprBase DqBuildHasItems(NYql::NNodes::TExprBase node, NYql::TExprContext& ctx, IOptimizationContext& optCtx,
    const TParentsMap& parentsMap, bool allowStageMultiUsage = true);

NNodes::TExprBase DqBuildSqlIn(NNodes::TExprBase node, TExprContext& ctx, IOptimizationContext& optCtx,
    const TParentsMap& parentsMap, bool allowStageMultiUsage);

NYql::NNodes::TExprBase DqBuildScalarPrecompute(NYql::NNodes::TExprBase node, NYql::TExprContext& ctx,
    NYql::IOptimizationContext& optCtx, const TParentsMap& parentsMap, bool allowStageMultiUsage);

NYql::NNodes::TExprBase DqPrecomputeToInput(const NYql::NNodes::TExprBase& node, TExprContext& ctx);

NYql::NNodes::TExprBase DqPropagatePrecomuteTake(NYql::NNodes::TExprBase node, NYql::TExprContext& ctx,
    NYql::IOptimizationContext& optCtx, const NYql::TParentsMap& parentsMap, bool allowStageMultiUsage);

NYql::NNodes::TExprBase DqPropagatePrecomuteFlatmap(NYql::NNodes::TExprBase node, NYql::TExprContext& ctx,
    NYql::IOptimizationContext& optCtx, const NYql::TParentsMap& parentsMap, bool allowStageMultiUsage);

TVector<NYql::NNodes::TCoArgument> PrepareArgumentsReplacement(const NYql::NNodes::TExprBase& node, const TVector<NYql::NNodes::TDqConnection>& newInputs,
    NYql::TExprContext& ctx, NYql::TNodeOnNodeOwnedMap& replaceMap);

NNodes::TExprBase DqBuildStageWithSourceWrap(NNodes::TExprBase node, TExprContext& ctx);

NNodes::TExprBase DqBuildStageWithReadWrap(NNodes::TExprBase node, TExprContext& ctx);

NNodes::TExprBase DqPushUnorderedToStage(NNodes::TExprBase node, TExprContext& ctx, IOptimizationContext& optCtx,
    const TParentsMap& parentsMap, bool allowStageMultiUsage);

NNodes::TMaybeNode<NNodes::TExprBase> DqUnorderedOverStageInput(NNodes::TExprBase node, TExprContext& ctx, IOptimizationContext& optCtx,
    const TTypeAnnotationContext& typeAnnCtx, const TParentsMap& parentsMap, bool allowStageMultiUsage);


} // namespace NYql::NDq

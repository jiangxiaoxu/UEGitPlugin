// Copyright (c) 2026
//
// 仓库级 Changed Assets status snapshot. 调用方应在 worker thread 执行 CaptureSnapshot,
// 并仅在成功返回后把值快照交给 Game Thread.

#pragma once

#include "GitChangedAssetsModel.h"

/** 无 provider 的 repository-wide porcelain-v2 snapshot builder. */
class GITSOURCECONTROL_API FGitChangedAssetsStatus final
{
public:
	/**
	 * 从带 branch.oid 的 status 固定 HEAD, 再以一次 rev-parse 校验. HEAD 变化时不发布部分或过期 snapshot.
	 * 仅查询当前 repository, 不扫描 Asset Registry, 不加载 package, 不进行网络访问.
	 */
	static bool CaptureSnapshot(const FString& InGitBinary, const FString& InRepositoryRoot, uint64 InGeneration,
		FGitChangedAssetSnapshot& OutSnapshot, FString& OutError);

	/** 供低成本自动化测试及 status snapshot 使用的 NUL-safe porcelain-v2 parser. */
	static bool ParsePorcelainV2(const TArray<uint8>& InOutput, const FString& InRepositoryRoot,
		TArray<FGitChangedAssetEntry>& OutEntries, FString& OutError);
};

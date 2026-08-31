# UEGitPlugin 研究报告

## 结论

当前插件是 Unreal Editor 的 standalone local Git asset tool, 不注册 Unreal `ISourceControlProvider`. 它只在用户明确触发的 Git repository snapshot 和 exact path 上工作, 不承担 remote, branch 或团队协作流程. Changed Assets 与 mutation 以 `.uasset` 或 `.umap` primary package 为单位, sidecar 作为同 package artifact 处理; 不自动扩大到未选 OFPA、BuiltData 或其他 package.

## 当前实现

- `FGitSourceControlModule` 注册 standalone Content Browser UI 和 `GitChangedAssets` Nomad tab. Startup 异步且恰好执行一次 Git capability gate, 发现 Git executable 并验证版本 2.53.0+, 但不探测 repository; Content Browser 资产菜单始终注册以保持 discoverability, `Pending`/`Unavailable` 时点击只显示 actionable diagnostic 且不执行 Git, `Available` 时才执行 action. status bar 和 panel 在门禁未通过时只显示 actionable diagnostic 并禁止交互. Git Changes 用户入口仅为 Level Editor 右下角替换默认 Source Control 控件的 status bar 按钮, layout restore 或 programmatic tab invocation 仍受 gate 约束. 模块仍不创建 provider、`DirectoryWatcher`、state cache 或后台 status polling.
- `UGitLocalSourceControlOperation` 是 AngelScript-only `UObject`, 不暴露 Blueprint, 不提供 public `Tick`. module-owned `FGCObject` registry 保活所有 non-terminal operation; ticker 在 module startup 注册, module shutdown 移除, 自动 pump Game Thread 且 idle 时快速返回. `OnProgress` 和 `OnCompleted` 是事件式通知, completion 在 package reload/recovery 后恰好一次; reload failure 进入 `Failed` phase 并报告 partial-disk 状态. `Cancel` 与 phase/result readback 保持显式 API.
- `GitLocalSourceControl` 和菜单操作在 startup gate `Available` 后解析 nearest repository root. 每个 job 使用 worker 和 Game Thread completion, 临时 repository context 在 job 结束后释放. 当前 Editor session 不重探 Git; 安装或升级后需重启.
- `GitSourceControlUtils` 负责 exact path status preflight, repository-wide porcelain-v2 snapshot, fixed-HEAD history, exact blob 和 LFS materialization. Changed Assets status 仅通过窄接口触发一次全仓查询, 空路径的 legacy 操作不会升级为 repository-wide scan.
- `FGitChangedAssetsMetadataResolver` 以磁盘 package header 作为现存 changed `.uasset` 的名称、类型和 object path 真值; map 使用 package identity/short name 和 `World` 类型. Asset Registry 只用于 owner/DataLayer topology; `FGitChangedAssetsController` 负责 generation, 异步 enrich, refresh phase/progress, 过滤结果刷新和 revert 完成后的状态重查. WorldDataLayers topology 继续使用 Asset Registry 和既有 owner-resolution 路径.
- `FGitSourceControlMenu` 提供普通资产和 OFPA actor History, 三种 Diff, same-path Force Restore 和 tracked Discard. Changed Assets 使用独立的 project-wide snapshot/list UI, 不注册 native changelist 或 Content Browser badge.
- `FGitSourceControlAssetOperations` 是 Content Browser legacy path 使用的 UI 无关 mutation service, 仍只处理 standalone `.uasset`. map 由 `GitMapPackageSet` 构造逻辑 package artifact set, 再交给 `FGitChangedAssetOperations` 执行 selection-driven mutation, 包括 OFPA owner/lifecycle gate.
- `FGitSourceControlRevision` 是 standalone Diff 使用的 private `ISourceControlRevision` adapter。每个实例自带 Git binary、repository root、commit、historical path 和 current path, 不回查 module provider。

## History 和 Diff

History 是按文件显式请求的 local `git log`, 先捕获 `HEAD` 再查询。默认 `CurrentPath` 不使用 `--follow`, 因此只扫描当前 Git path。`ExactRenames` 仅在每段最早 commit 为 single-parent `R100 old-path -> new-path` 时切换到 parent 和 old path; merge、copy、modified rename、redirector 和 delete/re-add 均停止, 不推断 Unreal asset identity。

插件自有 History window 持有固定 snapshot, 支持 revision-workspace、revision-previous 和任意两 revision Diff。workspace 使用 live in-memory asset; historical revision 使用唯一 temp package, 再交给 Unreal type-specific asset Diff。revision path 与 current path 不同时仍可 History、Diff 和 Fetch, 但不能 Force Restore。

OFPA actor 的 Level Editor Actor context menu 只在恰好一个已加载, 已保存, Editor-world external main actor 时显示 `Git (Local) > View Git History...`; 普通 actor, PIE/transient/unsaved/child actor, 多选/混选和 unloaded actor均 fail closed. Actor 只查询 `CurrentPath`, 不提供 `ExactRenames`; 后者对普通 asset 仅表示 Git 已证明的 committed single-parent `R100` path transition, 不证明 Unreal identity. Actor Diff 通过 `SDetailsDiff` 比较同 class actor 的 reflected properties, workspace侧保留当前加载对象的未保存修改, 不实现完整 package/component graph diff.

Actor History 的 Restore 复用 HistoricalRestore transaction 及 owner, dirty, PIE, LevelInstance, LFS, fingerprint/index, rollback 和 reload safety. 顶层 Actor context menu 不提供 Discard. 菜单生成阶段只做 selection/world/package path 解析, Git capability gate 在点击后执行.

## LFS 和外部 Git client

Git LFS 3.7.1+ capability 只在首次需要 LFS object 的显式操作时 lazy 检查, 只缓存成功结果. 缺失、版本过低或瞬时检查失败不缓存, 当前动作失败且下次显式 LFS 动作重试, 无需重启 Editor. pointer 优先从本地 storage verify/materialize. Diff、Restore、Fetch 或 Discard 的 object 缺失时, 插件只 fetch 对应 full commit + historical path. remote 只接受当前 branch upstream 或 repository 唯一 remote; 多 remote 且无 upstream 时在 network command 前失败, 不根据 `origin` 猜测. 下载可取消, 完成后必须通过 SHA-256 和 size 校验.

插件不执行 branch checkout、remote status polling 或 server coordination。项目仍在外部 Git client 中完成 fetch、pull、push、commit、merge、conflict resolution、asset delete 和 LFS lock。Content Browser 浏览及 Engine asset lifecycle 不触发 Git work。

## 安全与一致性语义

### Changed Assets

Changed Assets 在显式打开或 Refresh 时执行一次 `git status --porcelain=v2 -z` 并固定 `HEAD`; 结果按 `.uasset` 或 `.umap` primary package 聚合为 Modified, Deleted, Added, Untracked, Renamed 或 Conflicted. staged 与 worktree 状态合并为相对 HEAD 的总体状态, sidecar 保留为 artifact 但不显示独立行, 不提供 staging/unstaging. 刷新阶段依次为 `Git status`、当前文件 metadata、固定 `HEAD` metadata 和 owner/DataLayer fallback; 所有阶段完成前持续报告 phase/progress 并禁用 Refresh/Revert, generation 只用于丢弃过期异步 metadata 结果. 刷新无 DirectoryWatcher, 后台轮询或长期 status cache.

性能约束: 每次刷新只启动一个 repository status process, 不逐行启动 Git, 不递归扫描未变更 package, 不联网. 现存 changed `.uasset` 的名称、类型和 object path 以磁盘 package header 为真值, map 显示 package identity/short name 和 `World`, 不强制刷新全局 Asset Registry; Asset Registry 只用于 owner/DataLayer topology. 行列表使用虚拟化 Slate, metadata 解析按批次合并回 Game Thread, activity-only 更新不会重建 rows. WorldDataLayers topology 继续沿用 Asset Registry/既有 owner-resolution 路径.

现存资产通过 package header 渲染友好名称、类型和 object path, owner level 仍通过 Asset Registry/OFPA `OptionalOuterPath` 与 actor descriptor 解析; owner 无法唯一解析时显示 unresolved 并 fail closed. Changed Assets 接受 `.uasset`/`.umap` primary, 每个 selected primary 与 exact same-stem sidecar 组成 artifact group.

`Revert to HEAD` 是显式, 不可 Undo 的 all-or-nothing 事务, 同时丢弃 staged 与 unstaged. 每个 selected primary 会展开为 exact sidecars; 单选 map 只回退 map artifacts, map+selected OFPA 在同一事务中处理, 未选 OFPA/BuiltData 不 mutation 但其 dirty/lifecycle closure 参与安全检查. tracked Modified/Deleted 恢复 HEAD, Added/Untracked 精确删除, Rename 成对原子恢复, Conflict、owner 无法证明、PIE/non-Editor world 或不完整 WorldPartition closure fail closed. 事务前复核 HEAD, index, fingerprint 和 package 校验, 禁止 `git clean`, 目录删除和模糊 pathspec.

1. 输入路径先转为绝对路径并解析 nearest Git root, 跨 repository selection 直接拒绝。
2. 命令使用 exact pathspec 和 NUL-safe 参数。空列表、directory target 和无法解析 repository 的输入不执行 mutation; map 走 logical package service, 不进入 standalone `.uasset` service.
3. Discard 和 Force Restore 在确认前保存文件 fingerprint、HEAD 和 index snapshot。外部 Git 操作或文件变化会使请求失效, 需要重新执行。
4. Mutation 前创建临时 safety backup。Git 或文件系统步骤失败时恢复 index 和 worktree; 无法恢复的 backup 会保留并报告路径。
5. 历史 Unreal package 校验 package tag 和 topology。LFS object 同时验证 pointer SHA-256 OID 与 size。

## Editor automation API

AngelScript 通过 typed `GitLocalSourceControl` API 控制 `.uasset`/`.umap` 的 History/Diff/Restore/Discard 能力; 该 surface 不暴露给 Blueprint. `StartLoadHistory(AssetObjectPath, EGitLocalSourceControlHistoryMode)` 显式选择 `CurrentPath` 或 `ExactRenames`; map path 必须由 package identity 唯一解析, 不猜测扩展名. LFS fetch 和 revision Restore 始终以 `ExactRenames` 解析 historical path, Restore 仍要求 current/historical exact same path. 该 public API 不提供 Changed Assets status refresh 或 untracked delete; Changed Assets tab 保持 private/providerless. operation 由 module-owned `FGCObject` registry 保活并仅在存在 managed operation 时注册 ticker 自动 pump, 不提供 public `Tick`; AS workflow 绑定 `OnProgress`/`OnCompleted`, 以 `Cancel`, `IsTerminal`, `GetPhase` 和 `GetResult` 控制及 readback. `OnCompleted` 只在 package reload/recovery 后派发一次. `GetProviderInfo(AssetObjectPath)` 按资产路径解析 nearest repository. 输入为 asset object path, 不暴露 raw Git argv.

## 构建与测试入口

从项目根目录运行:

```text
npm run build:regular
npm run test:unreal:automation -- UEGitPlugin
npm run as:diagnostics
```

automation suite 保留 16 个 focused tests, 统一使用 `UEGitPlugin.*` 前缀。测试使用 isolated temporary Git repository 和 local bare LFS remote, 只覆盖安全核心:

- status snapshot、primary package 聚合、fixed `HEAD` 和普通资产 multi-hop `R100` exact rename;
- LFS lazy capability、目标 commit/path fetch、OID/size 校验和 ambiguous remote fail closed;
- Git index/worktree rollback、map + selected OFPA all-or-nothing transaction、exact sidecars、reload/dirty owner/historical conflict safety;
- `Standalone.GameThreadUiLifetime` 中的 operation UI state lifetime、Game Thread destruction、pre-commit cancel、post-commit safe shutdown, 以及 Actor target resolver.

Actor menu 可见性、Actor `CurrentPath`-only 入口、`SDetailsDiff` 窗口、Diff/Restore 按钮交互以及 DetailsDiff window/artifact cleanup 与 module shutdown lifecycle 由人工 GUI 验收并结合 production lifecycle review, 不属于 automation coverage. 测试需要 Git 和 Git LFS, 不连接 external server.

## 已知限制

- standalone plugin 不提供 remote/branch 协作状态、commit/push/pull 流程、conflict resolution、LFS lock 管理或 Content Browser status badge。
- Changed Assets 与 mutation 支持 `.uasset`/`.umap` primary package 及其 exact sidecars; map 不会自动回退未选 OFPA、BuiltData 或其他 package. OFPA actor/object package 可显示, 但 owner unresolved、dirty owner map、PIE/non-Editor world 或无法证明 lifecycle closure 时不可 Revert.
- 跨 rename revision 可以 Diff, 但不能 Force Restore 到当前 path, 因为 Git path identity 不证明 Unreal package identity。
- LFS miss 在多 remote 且无 current branch upstream 时失败。
- Git 缺失或低于 2.53.0 时 startup gate 为 `Unavailable`, 资产右键菜单仍保持可见但点击只显示诊断, 面板入口只显示诊断; 安装/升级后不重启不会重新探测.
- Git LFS 缺失、低于 3.7.1 或瞬时检查失败时当前 LFS 操作失败且不缓存失败结果, 下一次显式 LFS 操作重试, 无需重启 Editor; 普通 Changed Assets 不受影响.
- 插件不提供 precompiled binary, 构建需要项目 Unreal Editor target 和可用 C++ toolchain。
- AngelScript operation 不提供 Blueprint 节点或 public `Tick`; `Task_GitAssetRestoreViaApi` 和 `Task_GitAssetHistoryPerformance` 两个 workflow 必须使用 event-driven progress/completion. ticker 仅在 managed operation 存在时注册, 全部 terminal 后移除.

## Attribution and license

代码起源于 [UE4GitPlugin by Sebastien Rombauts](https://github.com/SRombauts/UE4GitPlugin), 后续包含 Project Borealis 的 production changes。许可证为 MIT, 详见同目录 [LICENSE.txt](../LICENSE.txt)。

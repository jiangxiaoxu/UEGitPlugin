# Unreal Engine Git Plugin

`GitSourceControl` 是 Unreal Editor 的 standalone local Git asset tool. 它不注册 Unreal `ISourceControlProvider`, 不维护 Content Browser 状态徽标, 也不参与 Engine asset lifecycle. 模块启动后会异步执行一次 Git executable 与版本门禁检查, 只验证 Git 是否可用且版本为 2.53.0 或更高, 不探测 repository 状态. Content Browser 资产右键菜单始终注册以保持 discoverability; 门禁为 `Pending` 或 `Unavailable` 时点击只显示可操作的诊断并禁止 Git 交互, `Available` 时才执行 Git action. Level Editor 状态栏和 Git Changes 面板在门禁未通过时同样禁止交互. 普通 create/move/copy/save/rename/delete 仍不执行 Git.

面向自动化的 `GitLocalSourceControl` API 是 AngelScript-only surface, History/Diff/Fetch/Restore/Discard typed entry points 支持 `.uasset` 和 `.umap` package path. `UGitLocalSourceControlOperation` 不再暴露给 Blueprint, 不提供 public `Tick`; module-owned `FGCObject` registry 会保活所有 non-terminal operation, 并仅在存在 managed operation 时注册 Game Thread ticker 自动 pump. 全部 operation terminal 后 ticker 自动移除, module shutdown 会先停止 ticker 再同步排空允许的 cleanup. operation 通过 `OnProgress` 和 `OnCompleted` 通知, `Cancel` 与 phase/result readback 保持显式 API; completion 只在 package reload 或 recovery 完成后发出. `GetProviderInfo` 按 asset object path 解析 nearest repository, 不依赖全局 provider.

## 文档

- [Standalone Asset Workflow](Documentation/Standalone-Asset-Workflow.md): 用户入口、History、Diff、Restore、Discard、LFS 和错误处理。
- [Architecture Contract](Documentation/Architecture.md): providerless 边界、线程模型、revision adapter、asset scope、LFS 规则和 release gates。
- [研究报告](Source/UEGitPlugin_Research_Report.md): 实现现状、限制、安全语义和测试入口。

## 当前能力

- `Git Changes` 提供项目级 primary package 变更列表, 支持 `.uasset` 和 `.umap`, 汇总相对固定 `HEAD` 的 Modified, Deleted, Added, Untracked, Renamed 和 Conflicted 状态, 并显示资产名, 所属关卡, object path 与类型. staged 与 unstaged 合并显示, 不提供 staging/unstaging. package sidecar 保留在 snapshot 中但不单独显示.
- Level Editor 右下角状态栏保留 Unsaved Assets 指示, 并以 `Git Changes` 直达按钮替换默认 Source Control 控件. 这是 Git Changes 的唯一用户入口; layout restore 或 programmatic tab invocation 仍受 Git executable 门禁约束.
- Git executable 门禁状态为 `Pending`, `Available` 或 `Unavailable`. Content Browser 的 Git 资产菜单始终可见以便发现; `Pending`/`Unavailable` 时点击只显示诊断, `Available` 时才执行 action. 状态栏和面板入口保持可见以便给出诊断, 但门禁未通过时不会启动 refresh 或 mutation.
- Changed Assets 的 `Revert to HEAD` 按明确 selection 构造一个 package transaction. 单选 `.umap` 只 mutation 该 map 的 primary package 和同 package sidecars; `.umap` 与已选 OFPA 行一起时, map 与这些 OFPA package 在同一个 transaction 中处理. 未选 OFPA 不 mutation, 但其 dirty 状态和 owner/lifecycle closure 仍参与安全检查. tracked 修改/删除恢复 HEAD, Added/Untracked 精确删除, Rename 原子恢复, Conflict 或无法证明 owner 时 fail closed.
- OFPA 行优先使用 Asset Registry `OptionalOuterPath`/actor descriptor 解析所属关卡. dirty owner map, 无法唯一解析 owner, PIE 或非 Editor world, 以及不完整的 WorldPartition closure 都禁止 Revert.
- Level Editor Actor 右键菜单仅对恰好一个已加载, 已保存, Editor-world 的 OFPA external main actor提供 `Git (Local) > View Git History...`; 普通/PIE/transient/unsaved/child/multi/unloaded actor不显示. Actor 只支持 `CurrentPath`, 不提供 `ExactRenames`; Diff 使用 `SDetailsDiff` 的 reflected property view, 不承诺完整 package/component graph diff. Actor Restore 复用 HistoricalRestore transaction 的 owner, dirty, PIE, LevelInstance, LFS, rollback 和 reload safety, 顶层 Actor菜单不提供 Discard. 菜单生成不启动 Git process.
- Changed Assets 刷新是显式, 准确, 异步的一次性流水线: `Git status` -> 当前文件 metadata -> 固定 `HEAD` metadata -> owner/DataLayer fallback. 直到所有阶段完成才发布完整 snapshot; 阶段和进度在面板中可见, `Refresh` 与 `Revert` 在整个流水线期间禁用.
- 现存 changed `.uasset` 的名称、类型和 object path 以磁盘 package header 为显示真值; map 使用 package identity/short name, type 为 `World`. 不强制刷新全局 Asset Registry. Asset Registry 只用于 owner level 和 DataLayer topology, metadata 回写按批次进行, activity-only 更新不会重建 rows.
- 刷新按 changed primary package 数量处理, 每次只启动一个 status process; 不逐行执行 Git, 不扫描全项目 package, 不访问网络, 不使用 DirectoryWatcher、后台轮询或跨刷新 status cache. WorldDataLayers topology 继续使用 Asset Registry 和既有 owner-resolution 路径.
- History 默认使用固定 `HEAD` 的 `CurrentPath` 查询; `.uasset` 和 `.umap` 均支持, `ExactRenames` 仅追踪 committed、single-parent、`R100` rename, 不使用 `--follow`. revision rename 或 path mismatch 不进入 Restore.
- standalone History window 提供 revision-workspace、revision-previous 和 selected-revisions 三种 Diff, 以及 selection-driven Restore、Refresh、Close 按钮. Diff 和 Restore 对 map 使用完整 package artifact set.
- Diff 失败或取消时立即清理临时导出; 成功打开的 Diff 导出及其 map sidecars 保留到当前 Editor session 退出, 仅清理插件专用的 `Diff/UEGitPlugin/UEGit-Diff-*` 文件。
- LFS object 优先使用本地 cache; miss 时只针对目标 commit/path fetch, remote 必须是 branch upstream 或唯一 remote。
- Force Restore 会明确丢弃目标 `.uasset` 或 `.umap` package 的 worktree、index 和 loaded in-memory changes, 同步处理该 package 的 exact sidecars, reset index path 到 `HEAD`, 原子写入 revision, 并在失败时 rollback. revision path mismatch, owner/lifecycle 无法证明或 PIE 使用中的 map fail closed. 它没有 Undo.
- Operation manager 只在存在 managed operation 时注册 ticker; package reload/recovery 后才广播 completion. reload 失败会进入 `Failed` phase, 同时报告已完成的磁盘 mutation 和需要人工检查的 partial-disk 状态。

## 外部 Git client 边界

branch、remote、commit、merge、push、pull、conflict resolution、asset delete 和 LFS lock 由项目的外部 Git GUI 或 command line client 负责。插件只对用户选中的路径执行 local Git 操作, 不执行目录级 `git clean`。

## 安装与验证

将插件放入项目 `Plugins/` 或 Engine `Plugins/` 后, 使用项目 Unreal Editor target 构建. 插件启动时异步检查 Editor process `PATH` 中的 Git executable 与最低版本 2.53.0, 每个 Editor session 只检查一次. 安装或升级 Git 后重启 Editor 才会重新探测; 不在运行中自动重探.

```text
npm run build:regular
npm run test:unreal:automation -- UEGitPlugin
npm run as:diagnostics
```

automation suite 使用 `UEGitPlugin.*` 前缀.

当前 suite 保留 16 个 focused automation tests, 只覆盖 status/fixed `HEAD`、普通资产 multi-hop `R100`、LFS、Git index/worktree rollback、map + selected OFPA + exact sidecars、reload/dirty owner/historical conflict、`Standalone.GameThreadUiLifetime` 中的 operation UI state lifetime、Game Thread destruction、pre-commit cancel、post-commit safe shutdown, 以及 Actor target resolver 等安全核心.

Actor menu 可见性、Actor `CurrentPath`-only 行为、`SDetailsDiff` 窗口、Diff/Restore 按钮交互以及 DetailsDiff window/artifact cleanup 与 module shutdown lifecycle 列为人工 GUI 验收, 并结合 production lifecycle review, 不宣称由 automation 覆盖. 发布前运行完整 suite 并执行 `git diff --check`.

还需验证 AS operation manager 的 FGCObject 保活、operation-driven ticker 注册/terminal 后移除及 shutdown drain、无 public `Tick`、progress/completion 事件只广播一次、取消边界、reload/recovery 后 completion 以及 reload 失败时的 `Failed`/partial-disk diagnostic.

插件需要 Git 2.53.0 或更新版本. 使用 Git LFS 的项目还需要 Git LFS 3.7.1 或更新版本; LFS capability 只在首次确实需要 LFS object 的显式操作时 lazy 检查, 只缓存成功的 3.7.1+ 结果, 不阻塞普通 Git Changes. 缺失、版本过低或瞬时检查失败只使当前 LFS 动作失败, 下次显式 LFS 动作会重试, 无需重启 Editor. 插件不提供 Git、Git LFS 或预编译 binary.

## Attribution and license

本插件源自 [UE4GitPlugin by Sebastien Rombauts](https://github.com/SRombauts/UE4GitPlugin), 并包含 Project Borealis 的 production changes。许可证为 MIT, 详见 [LICENSE.txt](LICENSE.txt)。

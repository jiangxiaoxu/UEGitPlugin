# Standalone Architecture Contract

本文档是 `GitSourceControl` 的维护契约, 用于判断新功能是否会重新引入 Unreal Source Control 集成或静默 Git 开销。

## Providerless and idle behavior

- 不注册 Unreal `ISourceControlProvider`, 不调用 `RegisterModularFeature("SourceControl", ...)`, 不使用 Unreal native History window、status badge、filter、changelist 或 Source Control settings。
- 不注册 `DirectoryWatcher`, 不维护跨会话 repository/status cache, history cache, provider handoff 或后台轮询. Changed Assets tab 只在显式打开/Refresh 时创建一次性的 snapshot generation, 操作完成后丢弃.
- Module startup 异步且恰好执行一次 Git capability gate, 发现可执行文件并读取 `git version`, 要求 Git 2.53.0 或更高; 不执行 `rev-parse` 或 repository probe. gate 状态为 `Pending`, `Available` 或 `Unavailable`. Content Browser 资产菜单始终注册以保持 discoverability; `Pending`/`Unavailable` 时点击只显示 actionable diagnostic 且不执行 Git, `Available` 时才执行 action. status bar 和 Changed Assets panel 在 `Pending`/`Unavailable` 时显示 actionable diagnostic 并禁止交互. Git Changes 用户入口仅为 Level Editor status bar; layout restore 或 programmatic tab invocation 仍受 gate 约束. 当前 Editor session 不重探, 安装或升级 Git 后需重启 Editor.
- Content Browser asset lifecycle 的 create、move、copy、save、rename、delete 不得触发 Git command。
- Level Editor 右下角 status bar 以 owner-scoped ToolMenus entry 替换默认 Source Control 组合控件, 保留 Unsaved Assets 指示并提供 `Git Changes` 直达按钮. 这是 Git Changes 的唯一用户入口; layout restore 或 programmatic tab invocation 仍受 startup gate 约束. 原 entry 在 shutdown 时恢复; module 禁止 dynamic reload, 避免 ToolMenus/Slate 缓存持有已卸载 DLL delegate.

`UGitLocalSourceControlOperation` 是 AngelScript-only `UObject`, 不标记为 Blueprint surface, 也不提供 public `Tick`. module-owned `FGCObject` registry 持有所有 non-terminal operation, 防止异步期间被 GC. registry 只在存在 managed operation 时注册 ticker 驱动 operation 的 Game Thread readback 和事件派发, 全部 operation terminal 后自动移除; module shutdown 会先停止 ticker, 再同步排空允许的 cleanup. `OnProgress` 在 phase/progress 变化时派发, `OnCompleted` 在 package reload 或 recovery 完成后恰好派发一次. `Cancel`, `IsTerminal`, `GetPhase` 和 `GetResult` 是 readback/control API; worker 只写线程安全 state, 不触碰 UObject 或 Slate.

`ScheduleDiscardTrackedAfterCompletion` 只接受 Restore/Discard mutation operation, 可在 parent 尚未进入 mutation 时预先 arm, 但只在 parent 实际 commit 了磁盘 mutation 后启动一次 child discard. read-only history/LFS success 不能启动 child; parent failure 或 rollback 不启动 child; parent reload failure 不改变已成功的磁盘 mutation 判断. internal child failure 会记录资产路径和人工恢复指引, 随后由 manager 释放。

## Explicit asynchronous jobs

每个 Changed Assets refresh/revert, History, Diff, LFS Fetch, Restore 或 Discard 都是显式 async job, 且必须先通过 startup Git gate. Git/LFS process 和 file I/O 在 worker, 所有 Slate, asset load/unload/reload 和 operation event/completion callback 在 Game Thread. operation 生命周期由 module-owned `FGCObject` registry 和 operation-driven ticker 管理, 不依赖调用方持续持有 UObject, 也不向 AS 暴露手动 `Tick`. completion 必须晚于 package reload/recovery; reload 失败进入 `Failed` phase, 并带有 partial-disk diagnostic. Git LFS 3.7.1+ 只在首次需要 LFS object 的显式操作时 lazy gate, 只缓存成功结果; 缺失、版本过低或瞬时失败不缓存, 当前动作失败且下次显式 LFS 动作重试, 无需重启 Editor. 未触发 LFS 时不执行 LFS version probe.

Read-only job 可由用户取消, window close 和 module shutdown 必须终止并等待其 process。Mutation 在 commit point 前可取消, 进入 commit point 后必须完成或 rollback。任何 callback 都必须先验证 window/module lifetime, 禁止在 worker thread 访问 UObject、Slate 或 Editor subsystem。

## History and revision adapter

- History 先捕获固定 `HEAD`, `CurrentPath` 使用普通 path-scoped `git log`, 永不使用 `--follow`。
- `ExactRenames` 只追踪 committed、single-parent、`R100` path transitions, 最多 250 条。merge、copy、modified rename、delete/re-add、redirector、root 和重复节点停止链路。
- 每条 revision 保存 historical path, current local filename 仍保持当前 asset path。
- `FGitSourceControlRevision` 只作为 private `ISourceControlRevision` Diff adapter。实例必须自带 Git binary、repository root、commit、historical path 和 current path, 不得回查 module/provider。
- historical package 使用唯一 temp identity。失败或取消的 temp file 立即清理; 成功打开 Diff 的导出保留到当前 Editor session 的 `FCoreDelegates::OnPreExit`, 避免 Diff editor 延迟读取时文件消失。普通 module unload/hot reload 不删除成功导出; pre-exit 只扫描并删除插件专用 `Diff/UEGitPlugin/UEGit-Diff-*` 路径, 不依赖旧 DLL 的 static registry, 也不触碰其他 Diff 文件。
- Fixed-HEAD 结果可以在 HEAD 改变后显示, 但必须提示用户 Refresh; 不得把新 HEAD 混入旧 snapshot。

## Asset scope and safety

Changed Assets 与 mutation scope 只处理单个 `.uasset` 文件. `.umap`, `.uexp`, `.ubulk`, `.uptnl`, `.upayload` 等 sidecar 或其他 package 不在本期范围, 不应通过猜测扩展名加入同一操作. 普通 `.uasset`, Blueprint, Animation, DataAsset 以及 OFPA external actor/object 的 `.uasset` 均可进入项目级列表.

Changed Assets tab 是 providerless, 显式触发的 repository-wide view: 一次 `git status --porcelain=v2 -z` snapshot 聚合相对固定 `HEAD` 的 `Modified`, `Deleted`, `Added`, `Untracked`, `Renamed` 和 `Conflicted`. 同一路径的 staged 与 worktree 差异合并为一条总体状态, 不暴露 staging UI. 当前 snapshot 仅存于 tab/job 生命周期, generation 用于丢弃过期异步结果.

列表的 Slate selection 是 checkbox, 行高亮, 计数和 Revert 输入的唯一真值. 普通点击切换单项, Shift/Ctrl 使用当前过滤结果的连续范围语义; filter 或 generation 变化必须剔除不可见/过期选择并同步 Slate navigation anchor, 防止隐藏资产进入 Revert.

刷新性能契约是每次只启动一个 repository status process, 解析复杂度随 changed `.uasset` 数量增长, 不执行逐行 Git 查询, 目录递归扫描或网络访问. 刷新 generation 按以下阶段串行推进: `Git status` 固定 repository snapshot, 当前文件 metadata, 固定 `HEAD` metadata, 最后 owner/DataLayer fallback. 所有阶段完成前都保持 `IsRefreshing`, 面板显示当前 phase/progress, `Refresh` 与 `Revert` 均禁用; 旧 generation 的结果直接丢弃. UI 行使用虚拟化列表, metadata 结果按批次回到 Game Thread 更新, activity-only 通知不会触发 row reconcile/filter/sort 重建.

现存 changed `.uasset` 的名称、类型和 object path 以磁盘 package header 为真值; 不为显示 metadata 强制刷新全局 Asset Registry. Asset Registry 只作为 owner level 与 DataLayer topology 的查询源, OFPA 仍优先使用 `OptionalOuterPath`/actor descriptor. 无法唯一解析 owner 时保留原始 path 并禁用 Revert, dirty owner map 同样阻止 OFPA Revert. WorldDataLayers topology 继续使用 Asset Registry 和既有 owner-resolution 路径.

`Revert to HEAD` 必须复核 workspace fingerprint, HEAD, index snapshot 和单文件 `.uasset` 校验, 并在 repository mutex 内执行 all-or-nothing mutation. `Modified`/`Deleted` 恢复 HEAD 的 index 与 worktree; `Added`/`Untracked` 精确删除文件并清除 index; `Renamed` 成对原子恢复; `Conflicted` 或 owner unresolved 禁用. 操作同时丢弃 staged 与 unstaged 内容, 不执行 `git clean`, 目录删除或模糊 pathspec, 也不提供 Undo. 失败时 rollback 并保留无法恢复的 backup 路径.

## LFS and remote resolution

LFS materialization 是 Diff、Restore、Fetch 和 Discard 共用的显式服务。先验证本地 object; miss 才执行 commit/path-scoped targeted fetch。remote 只允许当前 branch upstream, 或 repository 恰好唯一的 remote; 多 remote 且无 upstream 时必须在 network process 前失败, 不猜测 `origin`。

## Repository allowlist

Repository discovery 只接受用户显式选中的 asset path, 解析 nearest repository root, 并拒绝 directory target、mixed-root selection 和无法解析的 path。所有 Git argv 使用 exact pathspec 与 NUL-safe 参数。插件不承担 branch、remote、commit、merge、push、pull、conflict resolution、asset delete 或 LFS lock 管理。

## Tests and release gates

变更至少应覆盖以下边界:

- startup Git capability gate 恰好执行一次, `Pending`/`Unavailable` 时所有 Git action 均 fail closed 并给出 actionable diagnostic, 同时保留 Content Browser 菜单 discoverability; 安装或升级 Git 后必须重启 Editor 才重新探测。
- Git gate 完成后的 module idle 和普通 asset lifecycle 不再启动额外 Git process, 且没有 Unreal Source Control modular feature。
- AS operation manager 仅在 active operation 存在时注册 ticker, terminal 后自动移除; active operation 由 module `FGCObject` registry 保活, 无 public `Tick`, `OnProgress`/`OnCompleted` 只在 Game Thread 派发且 completion 只发生一次并晚于 reload/recovery. Cancel、terminal readback、reload failure `Failed` phase 和 partial-disk diagnostic 均需覆盖。
- CurrentPath、multi-hop `R100`、fixed HEAD、250 条上限和无 `--follow`。
- Diff 三种选择模式、跨 rename path、类型不兼容和 temp cleanup。
- LFS capability 在首次需要 LFS 的显式操作中 lazy 探测, 只缓存成功的 3.7.1+ 结果; 缺失、版本过低或瞬时失败不缓存, 当前动作失败且下次显式 LFS 动作重试. cache hit 零 network fetch; miss 只进行目标 commit/path fetch; ambiguous remote 在网络前失败.
- Restore/Discard 的 dirty、staged、conflict、untracked、index-added、package load/reload、rollback 和 index invariants。
- Changed Assets 的 repository-wide status parser, `.uasset` state aggregation, Added/Untracked 删除, Rename 原子回退, OFPA owner unresolved/dirty-map gate, generation cancellation 和 mutation guard.
- Changed Assets refresh phase/progress 从 status 持续到 current/HEAD/owner metadata 完成, 期间 Refresh/Revert 禁用; 现存文件使用 package-header truth, activity-only 更新不重建 rows, WDL topology 沿用 Asset Registry/既有 owner-resolution 路径, 且无 DirectoryWatcher、后台 polling 或全局 Asset Registry refresh.
- window close、cancel、module shutdown 时无遗留 Git process、notification 或 temp package。

文档和发布检查还必须确认 `UGitLocalSourceControlOperation` 不再出现在 Blueprint surface, `Task_GitAssetRestoreViaApi` 和 `Task_GitAssetHistoryPerformance` 两个 AngelScript workflow 只绑定 progress/completion events, 不手动 Tick; `GetProviderInfo(AssetObjectPath)` 与 nearest repository 解析一致.

交付前运行 `npm run build:regular`, 相关 Unreal automation filters, `npm run as:diagnostics` 和 `git diff --check`。需要 C++/AS API 变化时验证真实 generated surface。最终必须有独立 reviewer 审核 providerless 边界、零隐式 Git、LFS/Restore safety、API surface、测试证据和文档结论。

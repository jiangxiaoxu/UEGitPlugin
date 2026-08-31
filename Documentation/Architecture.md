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

## OFPA Actor menu and Diff

- Level Editor Viewport 与 Scene Outliner 的 Actor context menu 只为恰好一个已加载, 已保存, Editor-world 的 OFPA external main actor 注册 `Git (Local) > View Git History...`. 普通 actor, PIE/transient/unsaved actor, child actor, 多选, 混选和 unloaded actor 均不显示入口.
- Actor menu 只使用 `CurrentPath`; 不提供 `ExactRenames`. Exact rename 对普通 asset 仅表示 Git 已证明的 committed single-parent `R100` path transition, 不代表 Unreal asset identity; OFPA actor package 不据此推断历史归属.
- Actor History 的 Diff 使用 `SDetailsDiff` 比较 reflected properties. 它不是完整 package diff, 也不展开 component graph 或 subobject graph. workspace 一侧使用当前加载的 actor, 因此包含未保存的内存修改.
- Actor History 的 Restore 复用现有 HistoricalRestore transaction, 包括 owner/dirty/PIE/LevelInstance/LFS/fingerprint/index/rollback/reload safety. Actor context menu 不提供顶层 Discard; History window 仅在 capability 和 preflight 满足时启用 Restore.
- context menu 生成只解析 selection, world 和 package path, 不启动 Git process; Git capability gate 仅在用户点击 action 后生效.

## Asset scope and safety

Changed Assets 与 mutation scope 以单个 primary package 为单位, 支持 `.uasset` 和 `.umap`. 每个选中的 primary package 显式展开为同 package 的现有/status-visible sidecars; 不通过目录或猜测扩展名扩大范围. OFPA external actor/object package 仍作为独立 primary, 只有被选中时才进入 mutation.

Changed Assets tab 是 providerless, 显式触发的 repository-wide view: 一次 `git status --porcelain=v2 -z` snapshot 聚合相对固定 `HEAD` 的 `Modified`, `Deleted`, `Added`, `Untracked`, `Renamed` 和 `Conflicted`. 同一路径的 staged 与 worktree 差异合并为一条总体状态, 不暴露 staging UI. 当前 snapshot 仅存于 tab/job 生命周期, generation 用于丢弃过期异步结果.

列表的 Slate selection 是 checkbox, 行高亮, 计数和 Revert 输入的唯一真值. 普通点击切换单项, Shift/Ctrl 使用当前过滤结果的连续范围语义; filter 或 generation 变化必须剔除不可见/过期选择并同步 Slate navigation anchor, 防止隐藏资产进入 Revert.

刷新性能契约是每次只启动一个 repository status process, 解析复杂度随 changed primary package 数量增长, 不执行逐行 Git 查询, 目录递归扫描或网络访问. 刷新 generation 按以下阶段串行推进: `Git status` 固定 repository snapshot, 当前文件 metadata, 固定 `HEAD` metadata, 最后 owner/DataLayer fallback. 所有阶段完成前都保持 `IsRefreshing`, 面板显示当前 phase/progress, `Refresh` 与 `Revert` 均禁用; 旧 generation 的结果直接丢弃. UI 行使用虚拟化列表, metadata 结果按批次回到 Game Thread 更新, activity-only 通知不会触发 row reconcile/filter/sort 重建.

现存 changed `.uasset` 的名称、类型和 object path 以磁盘 package header 为真值; map 使用 package identity/short name, type 为 `World`. 不为显示 metadata 强制刷新全局 Asset Registry. Asset Registry 只作为 owner level 与 DataLayer topology 的查询源, OFPA 仍优先使用 `OptionalOuterPath`/actor descriptor. 无法唯一解析 owner 时保留原始 path 并 fail closed, dirty owner map 同样阻止 OFPA Revert. WorldDataLayers topology 继续使用 Asset Registry 和既有 owner-resolution 路径.

`Revert to HEAD` 必须复核 workspace fingerprint, HEAD, index snapshot 和 package 校验, 并在 repository mutex 内执行 all-or-nothing mutation. selection 只决定 primary package 和已选 OFPA; 单选 map 只 mutation map package artifacts, map+selected OFPA 在一个 transaction 中处理. 未选 OFPA 或 BuiltData 不 mutation, 但未选 OFPA 的 dirty 状态及 owner/lifecycle closure 必须纳入检查. `Modified`/`Deleted` 恢复 HEAD 的 index 与 worktree; `Added`/`Untracked` 精确删除文件并清除 index; `Renamed` 成对原子恢复; `Conflicted`, owner 无法证明, PIE/非 Editor world 或不完整 WorldPartition closure 均 fail closed. sidecar 与 primary 一起处理, 不执行 `git clean`, 目录删除或模糊 pathspec, 也不提供 Undo. 失败时 rollback 并保留无法恢复的 backup 路径.

## LFS and remote resolution

LFS materialization 是 Diff、Restore、Fetch 和 Discard 共用的显式服务。先验证本地 object; miss 才执行 commit/path-scoped targeted fetch。remote 只允许当前 branch upstream, 或 repository 恰好唯一的 remote; 多 remote 且无 upstream 时必须在 network process 前失败, 不猜测 `origin`。

## Repository allowlist

Repository discovery 只接受用户显式选中的 asset path, 解析 nearest repository root, 并拒绝 directory target、mixed-root selection 和无法解析的 path。所有 Git argv 使用 exact pathspec 与 NUL-safe 参数。插件不承担 branch、remote、commit、merge、push、pull、conflict resolution、asset delete 或 LFS lock 管理。

## Tests and release gates

当前 automation suite 保留 16 个 focused tests, 统一使用 `UEGitPlugin.*` 前缀。自动化只覆盖安全核心:

- repository-wide status、primary package 聚合、fixed `HEAD` 和普通资产的 multi-hop `R100` exact rename.
- LFS lazy capability、目标 commit/path fetch、OID/size 校验和 ambiguous remote fail closed。
- Git index/worktree rollback、dirty/staged/conflict/untracked rejection, 以及 map + selected OFPA 的 all-or-nothing transaction 和 exact sidecars。
- package reload、dirty owner、historical conflict、World Partition lifecycle gate 和失败后的 rollback 诊断。
- `Standalone.GameThreadUiLifetime` 中的 operation UI state lifetime、Game Thread destruction、pre-commit cancel、post-commit safe shutdown, 以及 Actor target resolver.

以下项目只作为人工 GUI 验收, 不宣称由 automation 覆盖:

- Viewport/Scene Outliner 中 Actor menu 的可见性、selection gate 和 zero-Git menu generation。
- Actor History 的 `CurrentPath`-only 行为, 不显示 `ExactRenames`。
- History window 的 `SDetailsDiff` 窗口、Diff 按钮启用状态和 `Restore Selected...` capability/preflight 交互; DetailsDiff window/artifact cleanup 与 module shutdown lifecycle 结合 production lifecycle review 验收.

文档和发布检查还必须确认 `UGitLocalSourceControlOperation` 不再出现在 Blueprint surface, `Task_GitAssetRestoreViaApi` 和 `Task_GitAssetHistoryPerformance` 两个 AngelScript workflow 只绑定 progress/completion events, 不手动 Tick; `GetProviderInfo(AssetObjectPath)` 与 nearest repository 解析一致.

交付前运行 `npm run build:regular`, `npm run test:unreal:automation -- UEGitPlugin`, `npm run as:diagnostics` 和 `git diff --check`。需要 C++/AS API 变化时验证真实 generated surface。最终必须有独立 reviewer 审核 providerless 边界、零隐式 Git、LFS/Restore safety、API surface、测试证据和文档结论。

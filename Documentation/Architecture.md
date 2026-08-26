# Standalone Architecture Contract

本文档是 `GitSourceControl` 的维护契约, 用于判断新功能是否会重新引入 Unreal Source Control 集成或静默 Git 开销。

## Providerless and idle behavior

- 不注册 Unreal `ISourceControlProvider`, 不调用 `RegisterModularFeature("SourceControl", ...)`, 不使用 Unreal native History window、status badge、filter、changelist 或 Source Control settings。
- 不注册 `DirectoryWatcher`, 不维护 repository generation、status cache、history cache、provider handoff 或 post-mutation status refresh。
- Module startup 不执行 Git discovery、`git version`、`rev-parse` 或 repository probe, 不创建 module-global ticker。仅显式操作时按需发现 Git binary 和 nearest repository。
- Content Browser asset lifecycle 的 create、move、copy、save、rename、delete 不得触发 Git command。

## Explicit asynchronous jobs

每个 History、Diff、LFS Fetch、Restore 或 Discard 都是显式 async job。Git/LFS process 和 file I/O 在 worker, 所有 Slate、asset load/unload/reload 和 completion callback 在 Game Thread。窗口或 operation handle 持有临时 job context, job 结束即释放; 不建立插件持久缓存。

Read-only job 可由用户取消, window close 和 module shutdown 必须终止并等待其 process。Mutation 在 commit point 前可取消, 进入 commit point 后必须完成或 rollback。任何 callback 都必须先验证 window/module lifetime, 禁止在 worker thread 访问 UObject、Slate 或 Editor subsystem。

## History and revision adapter

- History 先捕获固定 `HEAD`, `CurrentPath` 使用普通 path-scoped `git log`, 永不使用 `--follow`。
- `ExactRenames` 只追踪 committed、single-parent、`R100` path transitions, 最多 250 条。merge、copy、modified rename、delete/re-add、redirector、root 和重复节点停止链路。
- 每条 revision 保存 historical path, current local filename 仍保持当前 asset path。
- `FGitSourceControlRevision` 只作为 private `ISourceControlRevision` Diff adapter。实例必须自带 Git binary、repository root、commit、historical path 和 current path, 不得回查 module/provider。
- historical package 使用唯一 temp identity。失败或取消的 temp file 立即清理; 成功打开 Diff 的导出保留到当前 Editor session 的 `FCoreDelegates::OnPreExit`, 避免 Diff editor 延迟读取时文件消失。普通 module unload/hot reload 不删除成功导出; pre-exit 只扫描并删除插件专用 `Diff/UEGitPlugin/UEGit-Diff-*` 路径, 不依赖旧 DLL 的 static registry, 也不触碰其他 Diff 文件。
- Fixed-HEAD 结果可以在 HEAD 改变后显示, 但必须提示用户 Refresh; 不得把新 HEAD 混入旧 snapshot。

## Asset scope and safety

当前 mutation scope 只有 tracked `.uasset`。`.umap` 不是当前可变更对象, 未来如支持必须单独定义 world、World Partition 和 external package 规则。OFPA roadmap 采用 current-map `Changed Actors` 视图: 每行显示 actor label、GUID、Git status 和 checkbox, 支持多选 Modified / Deleted / Added actor 后执行 `Reset Selected to HEAD`。Tracked Modified / Deleted external actor package 恢复 HEAD index + worktree; HEAD 中不存在的 Added / untracked actor 在明确确认后删除 exact package。整个 batch 只 reload 当前 map 一次, 根 `.umap` 不参与 reset, 也不尝试热替换单个 actor。external package、World Partition、非 `.uasset` package 和无法通过 package 校验的输入必须拒绝, 不应通过猜测扩展名绕过。

Restore 必须复核 workspace fingerprint、HEAD、index snapshot、package topology 和 LFS object, reset exact index path 到 HEAD 后原子替换 worktree, 失败时 rollback。Discard 只处理 tracked `.uasset`, 恢复 index 和 worktree 到 HEAD, 不执行 `git clean` 或删除 untracked 文件。任何 mutation 都不提供 Undo。

## LFS and remote resolution

LFS materialization 是 Diff、Restore、Fetch 和 Discard 共用的显式服务。先验证本地 object; miss 才执行 commit/path-scoped targeted fetch。remote 只允许当前 branch upstream, 或 repository 恰好唯一的 remote; 多 remote 且无 upstream 时必须在 network process 前失败, 不猜测 `origin`。

## Repository allowlist

Repository discovery 只接受用户显式选中的 asset path, 解析 nearest repository root, 并拒绝 directory target、mixed-root selection 和无法解析的 path。所有 Git argv 使用 exact pathspec 与 NUL-safe 参数。插件不承担 branch、remote、commit、merge、push、pull、conflict resolution、asset delete 或 LFS lock 管理。

## Tests and release gates

变更至少应覆盖以下边界:

- module idle 和 asset lifecycle 的 Git process count 为零, 且没有 Unreal Source Control modular feature。
- CurrentPath、multi-hop `R100`、fixed HEAD、250 条上限和无 `--follow`。
- Diff 三种选择模式、跨 rename path、类型不兼容和 temp cleanup。
- LFS cache hit 零 network fetch; miss 只进行目标 commit/path fetch; ambiguous remote 在网络前失败。
- Restore/Discard 的 dirty、staged、conflict、untracked、index-added、package load/reload、rollback 和 index invariants。
- window close、cancel、module shutdown 时无遗留 Git process、notification 或 temp package。

交付前运行 `npm run build:regular`, 相关 Unreal automation filters, `npm run as:diagnostics` 和 `git diff --check`。需要 C++/AS API 变化时验证真实 generated surface。最终必须有独立 reviewer 审核 providerless 边界、零隐式 Git、LFS/Restore safety、API surface、测试证据和文档结论。

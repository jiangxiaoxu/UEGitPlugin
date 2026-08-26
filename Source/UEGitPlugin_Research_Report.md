# UEGitPlugin 研究报告

## 结论

当前插件是 Unreal Editor 的 standalone local Git asset tool, 不注册 Unreal `ISourceControlProvider`。它只在用户明确选择的 Git worktree 和 index path 上工作, 不承担 remote、branch 或团队协作流程。当前可变更范围只有 tracked `.uasset`; `.umap` 留待未来单独设计。

## 当前实现

- `FGitSourceControlModule` 只注册 standalone Content Browser UI。Startup 不发现 Git, 不创建 provider、worker queue、`DirectoryWatcher`、state cache 或 global ticker。
- `GitLocalSourceControl` 和菜单操作在显式交互后才解析 Git executable 与 nearest repository root。每个 job 使用 worker 和 Game Thread completion, 临时 repository context 在 job 结束后释放。
- `GitSourceControlUtils` 负责 exact path status preflight、fixed-HEAD history、exact blob 和 LFS materialization。空路径不会升级为 repository-wide scan。
- `FGitSourceControlMenu` 提供 History、三种 Diff、same-path Force Restore 和 tracked Discard, 不提供 status refresh、untracked delete、checkout、changelist 或 Content Browser badge。
- `FGitSourceControlAssetOperations` 是 UI 无关的 mutation service。它拒绝 directory、mixed-root、map 和 external package, 在 commit point 前复核 fingerprint、HEAD、index 和 package topology, 并在失败时 rollback。
- `FGitSourceControlRevision` 是 standalone Diff 使用的 private `ISourceControlRevision` adapter。每个实例自带 Git binary、repository root、commit、historical path 和 current path, 不回查 module provider。

## History 和 Diff

History 是按文件显式请求的 local `git log`, 先捕获 `HEAD` 再查询。默认 `CurrentPath` 不使用 `--follow`, 因此只扫描当前 Git path。`ExactRenames` 仅在每段最早 commit 为 single-parent `R100 old-path -> new-path` 时切换到 parent 和 old path; merge、copy、modified rename、redirector 和 delete/re-add 均停止, 不推断 Unreal asset identity。

插件自有 History window 持有固定 snapshot, 支持 revision-workspace、revision-previous 和任意两 revision Diff。workspace 使用 live in-memory asset; historical revision 使用唯一 temp package, 再交给 Unreal type-specific asset Diff。revision path 与 current path 不同时仍可 History、Diff 和 Fetch, 但不能 Force Restore。

## LFS 和外部 Git client

Git LFS pointer 优先从本地 storage verify/materialize。Diff、Restore、Fetch 或 Discard 的 object 缺失时, 插件只 fetch 对应 full commit + historical path。remote 只接受当前 branch upstream 或 repository 唯一 remote; 多 remote 且无 upstream 时在 network command 前失败, 不根据 `origin` 猜测。下载可取消, 完成后必须通过 SHA-256 和 size 校验。

插件不执行 branch checkout、remote status polling 或 server coordination。项目仍在外部 Git client 中完成 fetch、pull、push、commit、merge、conflict resolution、asset delete 和 LFS lock。Content Browser 浏览及 Engine asset lifecycle 不触发 Git work。

## 安全与一致性语义

1. 输入路径先转为绝对路径并解析 nearest Git root, 跨 repository selection 直接拒绝。
2. 命令使用 exact pathspec 和 NUL-safe 参数。空列表、directory target、map、`.umap` 和无法解析 repository 的输入不执行 mutation。
3. Discard 和 Force Restore 在确认前保存文件 fingerprint、HEAD 和 index snapshot。外部 Git 操作或文件变化会使请求失效, 需要重新执行。
4. Mutation 前创建临时 safety backup。Git 或文件系统步骤失败时恢复 index 和 worktree; 无法恢复的 backup 会保留并报告路径。
5. 历史 Unreal package 校验 package tag 和 topology。LFS object 同时验证 pointer SHA-256 OID 与 size。

## Editor automation API

AngelScript 和 Blueprint automation 通过 typed `GitLocalSourceControl` API 控制当前能力。`StartLoadHistory(AssetObjectPath, EGitLocalSourceControlHistoryMode)` 显式选择 `CurrentPath` 或 `ExactRenames`; LFS fetch 和 revision Restore 始终以 `ExactRenames` 解析 historical path。API 提供显式 repository info、history、LFS fetch、Discard 和 revision Restore, 不提供 status refresh 或 untracked delete。Operation handle 仅在 Game Thread 被 Tick、Cancel 和 readback, Git/LFS I/O 在 worker。输入为 asset object path, 不暴露 raw Git argv。

## 构建与测试入口

从项目根目录运行:

```text
npm run build:regular
npm run test:unreal:automation -- Cthulhu.GitSourceControl
npm run test:unreal:automation -- Cthulhu.GitSourceControl.Integration.HistoryDiffRestore
npm run as:diagnostics
```

测试使用 isolated temporary Git repository 和 local bare LFS remote, 覆盖 fixed-HEAD history、exact rename、LFS hit/miss、standalone Diff selection、Restore/Discard safety、index invariant、provider absence、startup process snapshot 以及 asset lifecycle 中的 zero Git process。测试需要 Git 和 Git LFS, 不连接 external server。

## 已知限制

- standalone plugin 不提供 remote/branch 协作状态、commit/push/pull 流程、conflict resolution、LFS lock 管理或 Content Browser status badge。
- 当前只支持 tracked `.uasset` mutation。`.umap`、World Partition、external package 和其他非 `.uasset` package 需要未来独立设计。
- 跨 rename revision 可以 Diff, 但不能 Force Restore 到当前 path, 因为 Git path identity 不证明 Unreal package identity。
- LFS miss 在多 remote 且无 current branch upstream 时失败。
- 插件不提供 precompiled binary, 构建需要项目 Unreal Editor target 和可用 C++ toolchain。

## Attribution and license

代码起源于 [UE4GitPlugin by Sebastien Rombauts](https://github.com/SRombauts/UE4GitPlugin), 后续包含 Project Borealis 的 production changes。许可证为 MIT, 详见同目录 [LICENSE.txt](../LICENSE.txt)。

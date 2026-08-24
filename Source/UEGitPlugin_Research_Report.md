# UEGitPlugin 研究报告

## 结论

当前插件是 Unreal Editor 的 local Git Source Control provider. 它只在本地 Git working tree 和 index 上执行明确路径操作, 不承担 remote 或 branch workflow. 远端同步, 分支管理和团队协作由项目使用的外部 Git GUI 或命令行客户端完成.

## 当前实现

- `FGitSourceControlModule` 注册 `Connect`, `UpdateStatus` 和 `Delete` worker, 注册 Source Control provider 和 Content Browser 菜单.
- `FGitSourceControlProvider` 负责 Git executable 和 repository 检查, 状态缓存, 异步命令调度, generation 防陈旧结果, 目录监听和防抖刷新.
- `GitSourceControlUtils` 使用本地 Git 命令解析 porcelain-v2 status, rename/copy 对, special path, history 和 exact blob. 空路径不会升级为 repository-wide scan.
- `FGitSourceControlMenu` 提供选中资产的 discard, delete new asset files, history restore 和 local status refresh.
- `FGitSourceControlAssetOperations` 是 UI 无关的本地变更服务. 它拒绝目录目标和 mixed-root 请求, 对 mutation 执行确认, fingerprint, repository generation 和 index snapshot 复核, 并在失败时尝试 backup rollback.
- `FGitSourceControlRevision` 和 History UI 支持单文件历史与 revision export. 导出不使用 filters 或 LFS smudge, 以避免把提示文本混入二进制数据.

## 支持的 Editor 行为

### Local status

Provider 返回选定路径的 staged, working-tree, untracked, ignored, deleted, copied 和 renamed 状态. `UsesCheckout`, `UsesChangelists` 和 `UsesLocalReadOnlyState` 均为 false, 因此 UI 不模拟 checkout, changelist 或 server-side read-only state.

### Asset mutations

- **Discard tracked changes**: 对明确选择的 tracked files 执行 `git restore --source=HEAD --staged --worktree`, 覆盖 staged 与 worktree 变更, 并处理 rename 的 old/new pair.
- **Delete new asset files**: 仅处理明确选择的 untracked 或 index-added files, 先更新 index 再删除磁盘文件, 不调用 directory-level `git clean`.
- **Restore history revision**: 从指定 commit/path 导出临时 blob, 校验 Unreal package header 后替换当前 worktree 文件. Git index, branch 和 remotes 保持不变.

所有 mutation 都要求用户确认, 变更前重新检查目标文件和 index. 已加载 package 的 reload 由 Editor callback 请求, 失败会在结果中报告.

### History and local LFS

History 是按文件显式请求的 local `git log`. Git LFS pointer 优先从本地 LFS storage materialize; 用户主动执行历史 Diff/Restore 且 object 缺失时, 插件只 fetch 对应 full commit + historical path. 下载阶段可取消并有 90 秒 timeout, 完成后必须通过 SHA-256 和 size 校验.

Editor automation 通过 AS-visible `GitLocalSourceControl` typed API 控制当前能力. API 提供 provider info, status, history, LFS fetch, discard, untracked delete 和 revision restore; operation handle 仅在 Game Thread 被 Tick/Cancel/readback, Git/LFS I/O 在 worker. 输入为 asset object path, 不暴露 raw Git argv. Mutation 拒绝 Map, loaded, dirty 或 open package, 在写入前重新验证 package/HEAD/index/generation/fingerprint, 并按 repository 串行执行.

## 外部 Git client 边界

插件不执行 branch checkout, remote status polling 或 server coordination. 项目仍在外部 Git client 中完成常规 fetch, pull, push, commit, merge, conflict resolution 和 LFS lock 等协作步骤. 唯一网络例外是用户主动历史 Diff/Restore 所需的精准 Git LFS fetch; status, watcher, Content Browser 浏览和普通 workspace mutation 不联网. 外部变更通过 directory watcher 触发防抖 invalidation; 必要时可在菜单中手动刷新 selected 或 cached local status.

## 安全与一致性语义

1. 输入路径先转为绝对路径并解析 nearest Git root. 跨 repository 选择直接拒绝.
2. 命令使用 exact pathspec 和 NUL-safe 参数. 空列表, directory target 和无法解析的 repository 不执行 mutation.
3. Discard 和 historical restore 在确认前保存文件 fingerprint, index snapshot 和 repository generation. 外部 Git 操作或文件变化会使请求失效, 需要 refresh 后重试.
4. Mutation 前创建临时 safety backup. Git 或文件系统步骤失败时恢复 index 和 worktree; 无法恢复的 backup 会保留并报告路径.
5. 历史 Unreal package 校验 `PACKAGE_FILE_TAG` 或 swapped tag. LFS 只检查 pointer 声明的 object size, 不执行 network fetch.

## 构建与测试入口

从项目根目录运行:

```text
npm run build:regular
npm run test:unreal:automation -- Cthulhu.GitSourceControl
```

`GitSourceControlTests` 在 system temp 下创建隔离的 local Git repository, 覆盖 status, rename pair, history/blob, capability 和 asset operation. 测试需要可执行的 Git, 不连接 remote.

## 已知限制

- Provider 不提供 remote/branch 协作状态, commit/push/pull 流程, conflict resolution 或 LFS lock 管理; 这些属于外部 Git client 边界.
- 历史 LFS revision 依赖本地 object. 缺少 object 时必须先在外部获取, 插件不会自动补齐.
- LFS materialization 当前按 pointer 声明的 size 检查 object, 不在 Engine 内执行可靠的 SHA-256 OID 校验.
- Asset mutation 依赖 Unreal package 当前加载状态. 磁盘变更成功后 reload callback 仍可能失败, 需要按 Editor 提示处理.
- 插件不提供 precompiled binary. 构建需要项目 Unreal Editor target 和可用的 C++ toolchain.

## Attribution and license

代码起源于 [UE4GitPlugin by Sebastien Rombauts](https://github.com/SRombauts/UE4GitPlugin), 后续包含 Project Borealis 的 production changes. 许可证为 MIT, 详见同目录 [LICENSE.txt](../LICENSE.txt).

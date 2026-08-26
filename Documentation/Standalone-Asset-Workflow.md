# Standalone Asset Workflow

本文档描述 `GitSourceControl` 在 Unreal Editor 中的用户行为。插件不接入 Unreal Source Control provider, 所有 Git 操作都必须由用户显式触发。

## 支持范围

- 当前只处理项目 Git repository 中的 `.uasset` 文件。
- `.umap` 只作为未来扩展方向, 当前 History、Diff、Restore 和 Discard 均不修改 map。
- Content Browser 只有选中 `.uasset` 时才显示 `Git (Local)` 菜单区; 选中 `.umap` 或其他文件不会显示 Git actions。
- 插件不会把 Unreal asset identity 当成 Git rename identity。跨 rename 只表示 Git 能证明的路径移动。
- remote、branch、commit、merge、push、pull、conflict resolution 和 asset delete 由外部 Git client 负责。

## 入口与 History

1. 在 Content Browser 选中一个 `.uasset`。
2. 打开资产菜单中的 `Git (Local)` 区域并选择 `View Git History...` 或 `View Git History Across Exact Renames...`。
3. History window 使用固定的 `HEAD` snapshot, 以表格显示 commit、date、author、action 和 description。
4. 选中 revision 后, 窗口底部只显示当前可用的按钮。无需依赖 revision 右键菜单。

`CurrentPath` 是默认模式, 只查询当前 Git path, 不使用 `git log --follow`。`ExactRenames` 只串联 committed、single-parent、`R100` rename, 最多 250 条; merge、copy、非 `R100`、delete/re-add、redirector、root 或重复 `(commit,path)` 会停止链路。

查询期间如果 repository `HEAD` 发生变化, 窗口仍显示捕获的 snapshot, 并提示重新打开或点击 `Refresh`。History 查询和 revision 导出是异步的, `Cancel` 只取消尚未进入 mutation 的任务。

## Diff

History window 提供三种 Diff:

- `Diff against Workspace`: 将选中 revision 与当前 workspace 中的 live asset 比较, 包含未保存的内存修改。
- `Diff against Previous`: 将选中 revision 与 snapshot 中紧邻的更旧 revision 比较。最旧 revision 没有 previous, 按钮会禁用。
- `Diff Selected Revisions`: 选择两个 revision 后, 按 snapshot 顺序比较 older/newer, 不依赖点击顺序。

插件会为 historical package 使用唯一临时路径, 再调用 Unreal 对应 asset type 的 Diff editor。revision 不可加载、类型不兼容或 package 校验失败时只显示错误, 不改 workspace。失败或取消会立即清理导出文件; 成功打开的 Diff 导出会保留到当前 Editor session 的 `OnPreExit`, 以保证 Diff editor 延迟读取时仍有文件。模块热重载不会提前删除这些成功导出, Editor 退出时只清理插件专用 `Diff/UEGitPlugin/UEGit-Diff-*` 路径。

## Force Restore

`Restore Selected...` 是破坏性操作, 不是普通 checkout。点击后会先弹出确认, 明确说明将丢弃哪些内容。

- 只允许当前 path 与 historical path 相同的 tracked `.uasset`。
- 操作前会检查文件 fingerprint、repository `HEAD`、index 条目、package topology 和 LFS object。
- 确认后会将目标 index path reset 到 `HEAD`, 再把选中的 revision 原子写回 worktree。
- dirty、staged、conflicted、untracked、ignored、index-added 或已加载但有未保存修改的内容都会被丢弃; 操作没有 Undo。
- 已加载 package 会先关闭相关 editor、unload, 替换文件并 reload。失败时先 rollback, 不能安全恢复时保留 backup 并报告路径。
- branch 和 remote 不会改变。跨 rename revision 可以 History/Diff/Fetch, 但不能 Restore 到当前 path。

如果用户不希望丢弃工作区改动, 选择 `No` 并先在外部 Git client 中 commit 或保存副本。

## Discard

`Discard Tracked...` 将明确选中的 tracked `.uasset` 的 index 和 worktree 恢复到当前 `HEAD`。它同样要求显式确认、单 repository 和 mutation 前复核。插件不执行目录级 `git clean`, 不删除 untracked asset, 也不改变 branch。

## Git LFS

History 查询只读取 commit metadata。Diff、Restore、Fetch 或 Discard 需要 blob 时按以下顺序处理:

1. 先验证本地 LFS object 的 OID 和 size; cache hit 不访问网络。
2. cache miss 时, 只 fetch 目标 commit + historical path 对应的 LFS object, 完成后再次验证。
3. remote 优先使用当前 branch 的 upstream; 没有 upstream 时, 仅当 repository 恰好有一个 remote 才使用它。
4. 多 remote 且无 upstream 时, 在启动网络命令前失败, 不猜测 `origin`。

LFS 下载显示可取消进度。取消或失败不会进入 asset mutation。

## 异步、取消与关闭

启动插件、浏览 Content Browser、create/move/copy/save/rename/delete asset 都不会执行 Git。显式操作才会创建异步 job; Git/LFS I/O 在 worker, Slate 和 asset/package 操作在 Game Thread。

关闭 History window 会取消未完成的 read-only job。模块 shutdown 会取消并等待 read-only/network job; 已跨过 mutation commit point 的 Restore/Discard 必须完成或 rollback, 不能被中途取消。

## 常见错误

- `Git executable or repository not found`: 当前选中的 asset 不在可识别的 Git repository, 或 Editor process `PATH` 找不到 Git。
- `History is empty`: 当前 path 在捕获的 `HEAD` 中没有 commit, 或 exact rename 链在 Git 规则边界停止。
- `LFS remote is ambiguous`: 多个 remote 且当前 branch 没有 upstream, 需在外部 Git client 配置 upstream。
- `Unable to load asset for Diff`: revision 不是可加载的 `.uasset`, 或当前 Editor asset type 不支持该 Diff。
- `Historical restore requires the same path`: 选中的 revision 位于旧 Git path, 只能 History/Diff/Fetch, 不能 Restore。
- `Force restore requires confirmation`: Restore 会丢弃工作区、index 和内存修改; 取消确认不会写入文件。
- `Asset operation failed and was rolled back`: 写入或 reload 失败, 插件已尝试恢复原文件和 index; 按提示保留 backup 并人工检查。

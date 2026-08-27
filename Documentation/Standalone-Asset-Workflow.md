# Standalone Asset Workflow

本文档描述 `GitSourceControl` 在 Unreal Editor 中的用户行为。插件不接入 Unreal Source Control provider. 模块启动后会异步执行一次 Git 2.53.0+ capability gate; 除此之外, 所有 repository Git 操作都必须由用户显式触发.

## 支持范围

- 当前所有 Git actions 只处理单个 `.uasset` 文件. 支持普通资产, Blueprint, Animation, DataAsset, Plugin Content 以及 OFPA external actor/object 的 `.uasset`.
- `.umap`, `.uexp`, `.ubulk`, `.uptnl`, `.upayload` 等 sidecar 或其他 package 不在本期范围, 不参与 Changed Assets 或 Revert.
- Content Browser 选中 `.uasset` 时始终显示 `Git (Local)` 菜单区以保持 discoverability; `Pending` 或 `Unavailable` 时点击 action 只显示 startup capability diagnostic 且不执行 Git, `Available` 时才执行 action. 选中 `.umap` 或其他文件不会显示 Git actions.
- Level Editor 右下角状态栏和已打开的 Git Changes 面板在 gate `Pending`/`Unavailable` 时保留入口用于说明状态, 但按钮和列表交互均禁用. Git Changes 用户入口仅为 Level Editor 右下角状态栏的替换 Source Control 按钮; layout restore 或 programmatic tab invocation 仍受 gate 约束. 点击会显示包含安装、升级和重启 Editor 指引的诊断, 不会启动 Git job.
- 插件不会把 Unreal asset identity 当成 Git rename identity。跨 rename 只表示 Git 能证明的路径移动。
- remote、branch、commit、merge、push、pull、conflict resolution 和 asset delete 由外部 Git client 负责。

## Changed Assets

1. 当 startup gate 为 `Available` 时, 点击 Level Editor 右下角状态栏替换默认 Source Control 控件的 `Git Changes` 按钮. 状态栏仍保留 Unsaved Assets 指示. 面板先显示当前 snapshot 或 loading 状态; repository-wide `git status --porcelain=v2 -z` 只在打开, 用户点击 `Refresh` 或操作完成后显式启动, 并在 worker 上运行. gate 为 `Pending`/`Unavailable` 时点击只显示 actionable diagnostic, 面板不执行 Git 操作.
2. 列表按固定 `HEAD` 汇总每个变更 `.uasset` 的总体状态: `Modified`, `Deleted`, `Added`, `Untracked`, `Renamed` 或 `Conflicted`. 同一路径 staged 与 unstaged 改动合并显示, 不提供 staging/unstaging 操作.
3. 每行显示友好名称, 所属关卡, asset/object path, 类型和状态; 原始 Git path 仅在详情或 Tooltip 中显示. 普通资产使用 Asset Registry, OFPA 使用 actor descriptor metadata. owner 无法唯一解析的行仍可查看, 但 Revert 会禁用.
4. 普通单击或 Ctrl+单击会切换单行选择并保留其他选择; Shift 选择当前过滤结果中的连续范围, Ctrl+Shift 追加范围. checkbox 与行高亮使用同一选择状态. 不可回退行可以被选中, 但混合选择会按 all-or-nothing 规则禁用整批 Revert.
5. 选择一行或多行后执行 `Revert Selected to HEAD...`. 该操作会同时丢弃所选项 staged 与 unstaged 内容; `Modified`/`Deleted` 恢复 HEAD, `Added`/`Untracked` 精确删除, `Renamed` 原子恢复旧/新路径, `Conflicted` 不可回退. OFPA 的 dirty owner map 或 unresolved owner 同样阻止操作. 确认后没有 Undo.

Changed Assets 不使用 DirectoryWatcher, 后台轮询或跨刷新 status cache. 刷新结果按 generation 合并; 新一代 snapshot 会使旧异步 metadata 结果失效. 刷新失败保留上一次成功列表并显示错误.
一次刷新只执行一个 repository-wide status process; 列表不为每一行启动 Git, 不联网, 也不扫描未变更目录.

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

`Discard Tracked...` 将明确选中的 tracked `.uasset` 的 index 和 worktree 恢复到当前 `HEAD`. Changed Assets 的 `Revert to HEAD` 覆盖相同安全检查, 并额外支持 Added/Untracked 精确删除与 Rename 原子恢复; 插件不执行目录级 `git clean`, 也不改变 branch.

## Git LFS

History 查询只读取 commit metadata. Git LFS 3.7.1+ capability 只在首次确实需要 LFS object 的 Diff、Restore、Fetch 或 Discard 中 lazy 检查, 只缓存成功结果; 普通 Changed Assets 不触发 LFS probe. 缺失、版本过低或瞬时检查失败时当前动作在 mutation 前失败, 下次显式 LFS 动作会重试, 无需重启 Editor.

通过 LFS gate 后, Diff、Restore、Fetch 或 Discard 需要 blob 时按以下顺序处理:

1. 先验证本地 LFS object 的 OID 和 size; cache hit 不访问网络。
2. cache miss 时, 只 fetch 目标 commit + historical path 对应的 LFS object, 完成后再次验证。
3. remote 优先使用当前 branch 的 upstream; 没有 upstream 时, 仅当 repository 恰好有一个 remote 才使用它。
4. 多 remote 且无 upstream 时, 在启动网络命令前失败, 不猜测 `origin`。

LFS 下载显示可取消进度。取消或失败不会进入 asset mutation。

## 异步、取消与关闭

启动插件会执行一次异步 Git executable/version gate, 但不会执行 repository status. 浏览 Content Browser、create/move/copy/save/rename/delete asset 都不会执行额外 Git. 仅在 gate `Available` 后的显式操作才会创建异步 job; Git/LFS I/O 在 worker, Slate 和 asset/package 操作在 Game Thread. Git 安装或升级后需重启 Editor, 当前 session 不会自动重探.

关闭 History window 会取消未完成的 read-only job。模块 shutdown 会取消并等待 read-only/network job; 已跨过 mutation commit point 的 Restore/Discard 必须完成或 rollback, 不能被中途取消。

## 常见错误

- `Git executable or repository not found`: 当前选中的 asset 不在可识别的 Git repository, 或 Editor process `PATH` 找不到 Git。
- `Git capability is pending`: 插件仍在执行启动时的 Git 检查. 稍候重试; 不会重复启动检查.
- `Git 2.53.0 or newer is required`: Git 缺失或版本过低. 安装/升级 Git 后重启 Editor.
- `Git LFS 3.7.1 or newer is required`: 当前操作需要 LFS object, 但 Git LFS 缺失或版本过低. 安装/升级 Git LFS 后重试当前或下一次显式 LFS 操作, 无需重启 Editor.
- `History is empty`: 当前 path 在捕获的 `HEAD` 中没有 commit, 或 exact rename 链在 Git 规则边界停止。
- `LFS remote is ambiguous`: 多个 remote 且当前 branch 没有 upstream, 需在外部 Git client 配置 upstream。
- `Unable to load asset for Diff`: revision 不是可加载的 `.uasset`, 或当前 Editor asset type 不支持该 Diff。
- `Historical restore requires the same path`: 选中的 revision 位于旧 Git path, 只能 History/Diff/Fetch, 不能 Restore。
- `Force restore requires confirmation`: Restore 会丢弃工作区、index 和内存修改; 取消确认不会写入文件。
- `Asset operation failed and was rolled back`: 写入或 reload 失败, 插件已尝试恢复原文件和 index; 按提示保留 backup 并人工检查。

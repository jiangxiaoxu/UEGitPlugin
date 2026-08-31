# Standalone Asset Workflow

本文档描述 `GitSourceControl` 在 Unreal Editor 中的用户行为。插件不接入 Unreal Source Control provider. 模块启动后会异步执行一次 Git 2.53.0+ capability gate; 除此之外, 所有 repository Git 操作都必须由用户显式触发.

## 支持范围

- Git actions 支持 `.uasset` 和 `.umap` primary package. `.uexp`, `.ubulk`, `.uptnl`, `.m.ubulk`, `.upayload` 等同 stem sidecar 不单独显示, 但随已选 primary package 进入 exact artifact transaction.
- 支持普通资产, Blueprint, Animation, DataAsset, Plugin Content, map 以及 OFPA external actor/object package. 选中 map 不会自动纳入其 OFPA、BuiltData 或其他未选 package.
- Level Editor Viewport 与 Scene Outliner 的 Actor 右键菜单只对恰好一个已加载, 已保存, Editor-world 的 OFPA external main actor显示 `Git (Local) > View Git History...`. 普通 actor, PIE/transient/unsaved actor, child actor, 多选, 混选和 unloaded actor不显示入口. Actor菜单不提供 `View Git History Across Exact Renames...`.
- Content Browser 选中 `.uasset` 或 `.umap` 时显示 `Git (Local)` 菜单区以保持 discoverability; `Pending` 或 `Unavailable` 时点击 action 只显示 startup capability diagnostic 且不执行 Git, `Available` 时才执行 action. 非 Unreal package 文件不显示 Git actions.
- Level Editor 右下角状态栏和已打开的 Git Changes 面板在 gate `Pending`/`Unavailable` 时保留入口用于说明状态, 但按钮和列表交互均禁用. Git Changes 用户入口仅为 Level Editor 右下角状态栏的替换 Source Control 按钮; layout restore 或 programmatic tab invocation 仍受 gate 约束. 点击会显示包含安装、升级和重启 Editor 指引的诊断, 不会启动 Git job.
- 插件不会把 Unreal asset identity 当成 Git rename identity。跨 rename 只表示 Git 能证明的路径移动。
- remote、branch、commit、merge、push、pull、conflict resolution 和 asset delete 由外部 Git client 负责。

## Changed Assets

1. 当 startup gate 为 `Available` 时, 点击 Level Editor 右下角状态栏替换默认 Source Control 控件的 `Git Changes` 按钮. 状态栏仍保留 Unsaved Assets 指示. 面板先显示当前 snapshot 或 loading 状态; repository-wide `git status --porcelain=v2 -z` 只在打开, 用户点击 `Refresh` 或操作完成后显式启动, 并在 worker 上运行. gate 为 `Pending`/`Unavailable` 时点击只显示 actionable diagnostic, 面板不执行 Git 操作.
2. 列表按固定 `HEAD` 汇总每个变更 primary package (`.uasset` 或 `.umap`) 的总体状态: `Modified`, `Deleted`, `Added`, `Untracked`, `Renamed` 或 `Conflicted`. 同一路径 staged 与 unstaged 改动合并显示, 不提供 staging/unstaging 操作; sidecar 只保留为 artifact.
3. 每行显示友好名称, 所属关卡, asset/object path, 类型和状态; `.umap` 使用 map package identity/short name, 类型为 `World`; OFPA 使用 actor descriptor metadata. owner 无法唯一解析的行仍可查看, 但 Revert 会禁用.
4. 普通单击或 Ctrl+单击会切换单行选择并保留其他选择; Shift 选择当前过滤结果中的连续范围, Ctrl+Shift 追加范围. checkbox 与行高亮使用同一选择状态. 不可回退行可以被选中, 但混合选择会按 all-or-nothing 规则禁用整批 Revert.
5. 选择一行或多行后执行 `Revert Selected to HEAD...`. 每个 selected primary 会展开为自身 exact sidecar set; 单选 `.umap` 只回退 map package artifacts, map 与 selected OFPA 在一个 all-or-nothing transaction 中处理. 未选 OFPA 不会因 map 自动纳入 mutation, 但 dirty/lifecycle closure 仍参与 safety check. `Modified`/`Deleted` 恢复 HEAD, `Added`/`Untracked` 精确删除, `Renamed` 原子恢复旧/新路径, `Conflicted`、owner 无法证明、PIE/non-Editor world 或不完整 WorldPartition closure fail closed. 确认后没有 Undo.

Changed Assets 刷新按 `Git status` -> 当前文件 metadata -> 固定 `HEAD` metadata -> owner/DataLayer fallback 的阶段推进. 面板持续显示当前 phase/progress; 直到 owner fallback 完成才认为刷新完成, `Refresh` 与 `Revert` 在整个过程中禁用. 刷新结果按 generation 合并; 新一代 snapshot 会使旧异步 metadata 结果失效. 刷新失败保留上一次成功列表并显示错误.
现存 changed `.uasset` 的名称、类型和 object path 以磁盘 package header 为显示真值; map 使用 package identity/short name 和 `World` 类型. 不强制刷新全局 Asset Registry. Asset Registry 只用于 owner level 与 DataLayer topology, metadata 批量回写时 activity-only 更新不会重建 rows. WorldDataLayers topology 继续使用 Asset Registry 和既有 owner-resolution 路径.
一次刷新只执行一个 repository-wide status process; 列表不为每一行启动 Git, 不联网, 也不扫描未变更目录. 插件不使用 DirectoryWatcher、后台轮询或跨刷新 status cache.

## 入口与 History

1. 在 Content Browser 选中一个 `.uasset` 或 `.umap`.
2. 打开资产菜单中的 `Git (Local)` 区域并选择 `View Git History...` 或 `View Git History Across Exact Renames...`。
3. 对符合条件的 OFPA actor, 从 Level Editor Actor 右键菜单选择 `Git (Local) > View Git History...`; 该入口只查询 actor external package 的当前路径.
4. History window 使用固定的 `HEAD` snapshot, 以表格显示 commit、date、author、action 和 description。
5. 选中 revision 后, 窗口底部只显示当前可用的按钮。无需依赖 revision 右键菜单。

`CurrentPath` 是默认模式, 只查询当前 Git path, 不使用 `git log --follow`. `.umap`, `.uasset` 和 OFPA actor `.uasset` 均可查询. 普通 asset 的 `ExactRenames` 只串联 committed, single-parent, `R100` rename, 最多 250 条; 它对确实发生过 Git 路径移动的普通 asset 有意义, 但不推断 Unreal asset identity. OFPA actor 不提供该模式. merge, copy, 非 `R100`, delete/re-add, redirector, root 或重复 `(commit,path)` 会停止链路.

查询期间如果 repository `HEAD` 发生变化, 窗口仍显示捕获的 snapshot, 并提示重新打开或点击 `Refresh`。History 查询和 revision 导出是异步的, `Cancel` 只取消尚未进入 mutation 的任务。

## Diff

History window 提供三种 Diff:

- `Diff against Workspace`: 将选中 revision 与当前 workspace 中的 live asset 比较, 包含未保存的内存修改。
- `Diff against Previous`: 将选中 revision 与 snapshot 中紧邻的更旧 revision 比较。最旧 revision 没有 previous, 按钮会禁用。
- `Diff Selected Revisions`: 选择两个 revision 后, 按 snapshot 顺序比较 older/newer, 不依赖点击顺序。

插件会为 historical package 使用唯一临时路径并 materialize 完整 package artifact set, 再调用 Unreal 对应 asset type 的 Diff editor. revision 不可加载、类型不兼容或 package 校验失败时只显示错误, 不改 workspace. 失败或取消会立即清理导出文件; 成功打开的 Diff 导出会保留到当前 Editor session 的 `OnPreExit`, 以保证 Diff editor 延迟读取时仍有文件. 模块热重载不会提前删除这些成功导出, Editor 退出时只清理插件专用 `Diff/UEGitPlugin/UEGit-Diff-*` 路径.

OFPA actor 的 Diff 使用 `SDetailsDiff` 比较同 class actor 的 reflected properties; workspace 一侧为当前加载的 actor, 可见未保存的内存修改. 该视图不承诺完整 package, component 或 subobject graph diff. Actor History 仍可使用与资产相同的 `Restore Selected...` capability, 但顶层 Actor 右键菜单不提供 Discard.

## Force Restore

`Restore Selected...` 是破坏性操作, 不是普通 checkout。点击后会先弹出确认, 明确说明将丢弃哪些内容。

- 只允许当前 path 与 historical path 相同的 tracked primary package (`.uasset` 或 `.umap`). map 必须通过 exact package identity 解析, 不猜测 `.uasset`/`.umap` 扩展名.
- 操作前会检查文件 fingerprint、repository `HEAD`、index 条目、package topology 和 LFS object。
- 确认后会将目标 package artifact 的 index path reset 到 `HEAD`, 再把选中的 revision 完整 artifact set 原子写回 worktree.
- dirty、staged、conflicted、untracked、ignored、index-added 或已加载但有未保存修改的内容都会被丢弃; 操作没有 Undo。
- 已加载 package 会先关闭相关 editor, unload, 替换文件并 reload. map 会检查 Editor world lifecycle; PIE, non-Editor world, 无法证明 owner 或不完整 WorldPartition closure 直接失败. 失败时先 rollback, 不能安全恢复时保留 backup 并报告路径.
- branch 和 remote 不会改变。跨 rename revision 可以 History/Diff/Fetch, 但不能 Restore 到当前 path。

如果用户不希望丢弃工作区改动, 选择 `No` 并先在外部 Git client 中 commit 或保存副本。

## Discard

`Discard Tracked...` 将明确选中的 tracked primary package (`.uasset` 或 `.umap`) 及其 exact sidecars 的 index 和 worktree 恢复到当前 `HEAD`. Changed Assets 的 `Revert to HEAD` 覆盖相同安全检查, 并额外支持 Added/Untracked 精确删除与 Rename 原子恢复; 插件不执行目录级 `git clean`, 也不改变 branch.

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

`GitLocalSourceControl` automation API 仅供 AngelScript 使用, History/Diff/Fetch/Restore/Discard 的 typed entry points 接受 `.uasset` 或 `.umap` package path. `UGitLocalSourceControlOperation` 不暴露 Blueprint, 没有 public `Tick`; module-owned `FGCObject` registry 保活所有 non-terminal operation, 仅在存在 managed operation 时注册 ticker 自动 pump Game Thread, 全部 operation terminal 后自动移除. module shutdown 会停止 ticker 并同步排空允许的 cleanup. workflow 通过 `OnProgress` 观察 phase/progress, 通过 `OnCompleted` 收取终态; completion 在 package reload 或 recovery 完成后才发送且每个 operation 只发送一次. `Cancel`, `IsTerminal`, `GetPhase` 和 `GetResult` 用于控制和 readback. `GetProviderInfo(AssetObjectPath)` 按资产路径解析 nearest repository.

关闭 History window 会取消未完成的 read-only job。模块 shutdown 会取消并等待 read-only/network job; 已跨过 mutation commit point 的 Restore/Discard 必须完成或 rollback, 不能被中途取消。

## 自动化与 GUI 验收

当前 automation suite 保留 16 个 focused tests, 统一使用 `UEGitPlugin.*` 前缀。自动化契约只覆盖:

- status snapshot、primary package 聚合、fixed `HEAD` 和普通资产 multi-hop `R100` exact rename;
- LFS lazy capability、目标 commit/path fetch、完整性校验和 ambiguous remote fail closed;
- Git index/worktree rollback、map + selected OFPA all-or-nothing transaction、exact sidecars、reload/dirty owner/historical conflict safety;
- `Standalone.GameThreadUiLifetime` 中的 operation UI state lifetime、Game Thread destruction、pre-commit cancel、post-commit safe shutdown, 以及 Actor target resolver.

以下行为由人工在关闭其他自动化干扰后通过 GUI 验收: Actor menu 可见性与 selection gate、Actor `CurrentPath`-only 入口、History window 的 `SDetailsDiff` 窗口和 Diff/Restore 按钮交互, 以及 DetailsDiff window/artifact cleanup 与 module shutdown lifecycle. 这些行为不属于 automation coverage; DetailsDiff window/artifact cleanup 与 module shutdown lifecycle 还需结合 production lifecycle review.

发布验证统一运行 `npm run test:unreal:automation -- UEGitPlugin`, 不使用已删除的独立 filter.

## 常见错误

- `Git executable or repository not found`: 当前选中的 asset 不在可识别的 Git repository, 或 Editor process `PATH` 找不到 Git。
- `Git capability is pending`: 插件仍在执行启动时的 Git 检查. 稍候重试; 不会重复启动检查.
- `Git 2.53.0 or newer is required`: Git 缺失或版本过低. 安装/升级 Git 后重启 Editor.
- `Git LFS 3.7.1 or newer is required`: 当前操作需要 LFS object, 但 Git LFS 缺失或版本过低. 安装/升级 Git LFS 后重试当前或下一次显式 LFS 操作, 无需重启 Editor.
- `History is empty`: 当前 path 在捕获的 `HEAD` 中没有 commit, 或 exact rename 链在 Git 规则边界停止。
- `LFS remote is ambiguous`: 多个 remote 且当前 branch 没有 upstream, 需在外部 Git client 配置 upstream。
- `Unable to load asset for Diff`: revision 不是可加载的 Unreal package, 或当前 Editor asset type 不支持该 Diff.
- `Historical restore requires the same path`: 选中的 revision 位于旧 Git path, 只能 History/Diff/Fetch, 不能 Restore。
- `Actor history is unavailable`: 当前 selection 不是恰好一个已加载、已保存、Editor-world 的 OFPA external main actor, 或 package owner/lifecycle 无法安全解析。
- `Force restore requires confirmation`: Restore 会丢弃工作区、index 和内存修改; 取消确认不会写入文件。
- `Asset operation failed and was rolled back`: 写入或 reload 失败, 插件已尝试恢复原文件和 index; 按提示保留 backup 并人工检查。

# UEGitPlugin 研究报告

## 总体概览
- 插件模块: GitSourceControl. 核心职责是实现 UE SourceControl Provider, 并通过 Git CLI 完成状态查询, 提交, 同步, LFS 锁等操作.
- 主要对象: FGitSourceControlModule 负责模块生命周期和菜单扩展, FGitSourceControlProvider 负责 ISourceControlProvider, IGitSourceControlWorker 系列负责具体命令执行, GitSourceControlUtils 封装 Git 命令与解析逻辑.
- 线程模型: 命令可异步执行, 通过 FGitSourceControlCommand 派发到线程池并在 Tick 中回收结果; 另外有一个 FGitSourceControlRunner 定时后台拉取并刷新状态.

## 文件结构与职责
- 模块入口: GitSourceControlModule.h/.cpp
- Provider 实现: GitSourceControlProvider.h/.cpp
- 命令与执行: GitSourceControlCommand.h/.cpp
- Git 工具与解析: GitSourceControlUtils.h/.cpp
- 操作 Worker: GitSourceControlOperations.h/.cpp
- 状态与历史: GitSourceControlState.h/.cpp, GitSourceControlRevision.h/.cpp
- Changelist 支持: GitSourceControlChangelist.h/.cpp, GitSourceControlChangelistState.h/.cpp
- UI 设置面板: SGitSourceControlSettings.h/.cpp
- 菜单扩展: GitSourceControlMenu.h/.cpp
- 后台刷新线程: GitSourceControlRunner.h/.cpp
- 控制台命令: GitSourceControlConsole.h/.cpp

## 核心流程梳理

### 初始化与连接
- FGitSourceControlModule::StartupModule 注册 Worker, 加载设置, 绑定 Provider, 绑定 ContentBrowser 事件.
- FGitSourceControlProvider::CheckGitAvailability 解析设置或自动查找 Git 路径, 随后调用 CheckRepositoryStatus.
- CheckRepositoryStatus 会解析用户配置, 分支, 远端, 以及 LFS lockable 状态, 并做首次 RunUpdateStatus.

### 状态查询与缓存
- 状态缓存存在 FGitSourceControlProvider::StateCache 中, 通过 GetStateInternal 写入.
- RunUpdateStatus 调用 Git status 并解析结果, 结果写入 FGitSourceControlState.
- 更新后会 AddFileToIgnoreForceCache 避免短时间内重复强制查询.

### 提交与推送
- FGitCheckInWorker 负责 commit, 并在成功后自动 push, 如失败尝试 fetch + pull + push, 同时处理 LFS unlock.
- 会使用 git diff 或 git log 获取未推送变更文件, 并用于更新状态.

### 同步与拉取
- FGitSyncWorker 先 fetch, 再 pull, 再更新状态, 并获取 commit 信息.
- GitSourceControlUtils::PullOrigin 会对 lockable 文件执行包卸载和重载, 并强制 pull --rebase --autostash.

### LFS 文件锁
- bUsingGitLfsLocking 决定 checkout 和 read-only 语义, 结合 FGitLockedFilesCache 更新本地只读标志.

### uasset 历史版本与 diff 流程
- 入口在 ContentBrowser 右键菜单: "Diff against status branch". 调用 FGitSourceControlModule::DiffAssetAgainstGitOriginBranch -> DiffAgainstOriginBranch.
- DiffAgainstOriginBranch 取当前资源的包路径, 通过 GitSourceControlUtils::GetOriginRevisionOnBranch 获取指定分支(状态分支)上的最新提交信息.
- GetOriginRevisionOnBranch 内部执行 `git show <BranchName>` 解析提交信息, 产出 FGitSourceControlRevision, 并修正 Filename 为相对路径.
- FGitSourceControlRevision::Get 使用 `git cat-file --filters <CommitId>:<RelativePath>` 将指定版本导出为临时文件(会走 filters, 对 LFS 资产可触发 smudge).
- 导出的临时包被 LoadPackage 以 LOAD_ForDiff|LOAD_DisableCompileOnLoad 加载, 再由 AssetTools::DiffAssets 将旧版本 UObject 与当前 UObject 做 diff.
- 另一条历史链路来自 UpdateStatus 的 ShouldUpdateHistory: RunGetHistory 执行 `git log --follow --date=raw --name-status --pretty=medium`, 再用 `git ls-tree --long` 取 blob hash 与大小, 写入 FGitSourceControlState::History 供 UI 展示.
- 现象与原因: 对 LFS 资产, `git ls-tree --long` 只能拿到 pointer blob 的大小, 所以历史列表会显示指针大小; 超出本地 LFS 缓存的旧版本在 `git cat-file --filters` 时也可能只得到 pointer 或失败.
- 修复策略: 使用 `git ls-tree <commit> -- <path>` 获取 blob hash, 先 `git cat-file -p <blob>` 判断是否为 LFS pointer; 如果是 LFS, 先执行 `git lfs fetch --include=<path> --all` 拉取历史对象, 再用 `git cat-file --filters <commit>:<path>` 导出. 该导出仍走 stdout, 但由于对象已在本地不会触发下载, 因此不会混入下载进度文本; 若导出结果仍是 LFS pointer, 则视为失败并触发 LFS 拉取重试; 在 history 中对 LFS pointer 解析 `size <n>` 并覆盖 FileSize 为真实大小; fetch 或导出失败时弹对话框提示手动拉取.
- 已发现 bug: 当 `git cat-file --filters` 触发 LFS 下载时, git-lfs 会输出 "Downloading ..." 进度到 stdout, 该文本被拼入二进制输出导致 uasset 文件头损坏, UE 报 "Unable to load assets to diff" 或 summary invalid.
- 调试策略: 在 UE 日志中输出导出路径, 文件头 hex, 是否走 LFS 路径, 以及失败原因, 用于快速定位 diff 失败或文件损坏问题.
 - 防御措施: 导出后校验 uasset 文件头魔数, 不合法则删除临时文件并报错, 避免复用损坏文件.

### UI 与菜单
- FGitSourceControlMenu 在状态栏菜单中加入 Push, Pull, Revert, Refresh, 并处理通知条与 stash.
- SGitSourceControlSettings 提供 Git 路径, LFS 开关, 初始化仓库功能. UE5 分支中初始化相关功能被隐藏.

### 后台自动刷新
- FGitSourceControlRunner 每 30 秒执行一次 FGitFetch 并更新状态.

## 关键数据结构与状态模型
- FGitState 组合 EFileState, ETreeState, ELockState, ERemoteState 来描述文件.
- FGitSourceControlState::GetGitState 将多状态折叠成 UI 友好的优先级状态.
- Changelist 在 UE5 中抽象为 Working 与 Staged, 并映射到 git add 或 git restore --staged.

## 潜在问题与风险点
- 线程安全: FGitSourceControlRunner 使用普通 bool 标志位, 在多线程环境中没有原子保护, 有竞态风险.
- 命令可用性与路径: FGitSourceControlModule::StartupModule 中 RequiredRepositoryAccessURL 检查直接调用 git 命令, 未使用设置中的 Git 路径, 在非 PATH 场景会失败.
- LFS Lock 与只读状态: FGitLockedFilesCache::OnFileLockChanged 直接修改文件只读属性, 若锁状态与远端不同步可能导致误导.
- Revert 行为: FGitRevertWorker 中 reset/checkout 逻辑被注释, 实际使用 git restore -SW, 但文件选择逻辑较复杂, 可能导致状态未更新.
- 提交后自动推送: FGitCheckInWorker 在提交后自动 push, 且在冲突时自动 pull, 多人协作时可能引发 UI 意外或阻塞.

## 优化机会与扩展点
- 状态刷新策略: TicksUntilNextForcedUpdate 由 ContentBrowser 事件触发强制刷新, 大项目可能带来频繁状态扫描. 可引入节流或分批更新.
- Git 命令拼接: RunCommandInternalRaw 构建命令时未统一转义策略, 当前依赖上层传入安全参数. 可考虑统一转义并记录原始参数以便调试.
- Changelist 体验: 仅支持 Working 与 Staged, UI 显示和描述比较基础. 可扩展 staging 视图或增加对 shelved 的提示.
- LFS 锁信息: GetAllLocks 返回全量锁, 缓存刷新策略依赖 fetch. 可考虑按需刷新或在状态更新时仅刷新相关路径.

## 建议的后续工作方向
1) Bug 修复: 先明确症状或日志, 从 GitSourceControlOperations.cpp 与 GitSourceControlUtils.cpp 做最小化定位.
2) 性能优化: 明确项目规模与卡顿点, 优先评估 RunUpdateStatus 触发频率与 TicksUntilNextForcedUpdate 逻辑.
3) 功能扩展: 明确目标功能后, 基于 FGitSourceControlProvider::RegisterWorker 与菜单扩展点设计改造方案.

## 经验性总结: uasset diff 与 Git LFS stdout 污染
- 现象: uasset diff 临时文件头出现 `warning: current Git remote contains credentials`, 导致 LoadPackage 失败.
- 根因: Git 自身把 warning 输出到 stdout, 与 `git cat-file --filters` 的二进制输出混在一起.
- 关键结论: `git lfs fetch --all` 只影响 LFS 对象可用性, 不会阻止 warning 输出, 也不能保证 stdout 纯净.
- 防御策略: 导出后在二进制缓冲区内扫描 `PACKAGE_FILE_TAG` / `PACKAGE_FILE_TAG_SWAPPED`, 将其作为二进制起始位置并截断前导文本; 扫描上限建议 16KB.
- 失败策略: 若前 16KB 未命中 tag, 直接判定导出失败并记录日志, 避免写入坏文件; `RunDumpToFile` 会返回失败以触发上层重试或报错.
- 弱校验局限: 当前 `IsValidPackageSummary` 的 offset/count 校验只保证值在范围内, 不能拦截 "offset 合法但 count 极大" 这类逻辑异常; 如需更严格应加入 count 上限或 offset+count*entrySize 检查.
- 调试策略: 输出被截断文本, 便于定位来自 Git 或 git-lfs 的提示内容.
- 缓存策略: 若已存在临时文件且无效, 应删除并触发重新导出; 避免持续复用坏文件.
- LFS fetch 触发点: 保留在导出失败或导出结果仍为 LFS pointer 时再执行 fetch + 重试, 可减少无效的预先 fetch.
- 根因治理建议: 将 remote URL 中的凭据移除, 采用 credential helper 或 SSH, 从源头避免该 warning.

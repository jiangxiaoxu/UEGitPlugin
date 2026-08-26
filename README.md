# Unreal Engine Git Plugin

`GitSourceControl` 是 Unreal Editor 的 standalone local Git asset tool。它不注册 Unreal `ISourceControlProvider`, 不维护 Content Browser 状态徽标, 也不参与 Engine asset lifecycle。启动、静置和普通 create/move/copy/save/rename/delete 不执行 Git; 用户显式触发 History、Diff、LFS Fetch、Force Restore 或 Discard 后才创建异步 job。

## 文档

- [Standalone Asset Workflow](Documentation/Standalone-Asset-Workflow.md): 用户入口、History、Diff、Restore、Discard、LFS 和错误处理。
- [Architecture Contract](Documentation/Architecture.md): providerless 边界、线程模型、revision adapter、asset scope、LFS 规则和 release gates。
- [研究报告](Source/UEGitPlugin_Research_Report.md): 实现现状、限制、安全语义和测试入口。

## 当前能力

- 当前 mutation scope 只有 tracked `.uasset`; `.umap` 计划未来单独支持。
- History 默认使用固定 `HEAD` 的 `CurrentPath` 查询; `ExactRenames` 仅追踪 committed、single-parent、`R100` rename, 不使用 `--follow`。
- standalone History window 提供 revision-workspace、revision-previous 和 selected-revisions 三种 Diff, 以及 selection-driven Restore、Refresh、Close 按钮。
- Diff 失败或取消时立即清理临时导出; 成功打开的 Diff 导出保留到当前 Editor session 退出, 仅清理插件专用的 `Diff/UEGitPlugin/UEGit-Diff-*` 文件。
- LFS object 优先使用本地 cache; miss 时只针对目标 commit/path fetch, remote 必须是 branch upstream 或唯一 remote。
- Force Restore 会明确丢弃目标 `.uasset` 的 worktree、index 和 loaded in-memory changes, reset index path 到 `HEAD`, 原子写入 revision, 并在失败时 rollback。它没有 Undo。

## 外部 Git client 边界

branch、remote、commit、merge、push、pull、conflict resolution、asset delete 和 LFS lock 由项目的外部 Git GUI 或 command line client 负责。插件只对用户选中的路径执行 local Git 操作, 不执行目录级 `git clean`。

## 安装与验证

将插件放入项目 `Plugins/` 或 Engine `Plugins/` 后, 使用项目 Unreal Editor target 构建。Git 从显式操作开始时的 Editor process `PATH` 解析, module startup 不执行 Git probe。

```text
npm run build:regular
npm run test:unreal:automation -- Cthulhu.GitSourceControl
npm run as:diagnostics
```

插件需要可执行的 Git; 使用 Git LFS 的项目还需要 Git LFS 能通过该 Git executable 访问本地 object store。插件不提供 Git、Git LFS 或预编译 binary。

## Attribution and license

本插件源自 [UE4GitPlugin by Sebastien Rombauts](https://github.com/SRombauts/UE4GitPlugin), 并包含 Project Borealis 的 production changes。许可证为 MIT, 详见 [LICENSE.txt](LICENSE.txt)。

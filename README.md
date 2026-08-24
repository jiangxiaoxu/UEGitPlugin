# Unreal Engine Git Plugin

`GitSourceControl` is a local Git provider for Unreal Editor. It reads and changes the selected worktree and index paths through a local Git executable. Remote and branch workflows stay in the external Git client used by the project.

## Current implementation

- Resolves Git from the Editor process `PATH`, then validates the executable, repository root, and local capabilities without contacting a remote.
- Reports path-scoped local status, including staged, working-tree, untracked, ignored, deleted, copied, and renamed paths.
- Provides Unreal Source Control status, file history, and single-file revision export. Checkout, changelist, and remote-state emulation are not part of this provider contract.
- Adds explicit Content Browser actions for selected assets:
  - Discard tracked changes from `HEAD`, including staged changes and tracked deletions.
  - Delete selected untracked or index-added asset files.
  - Restore a selected historical revision into the worktree while leaving the Git index unchanged.
  - Refresh selected or cached local status.
- Uses and verifies local Git LFS objects. An explicit historical Diff/Restore may fetch only its missing commit/path object with a cancelable 90-second timeout.
- Watches project and repository paths and debounces local status invalidation after Editor or external-client changes.

## External Git client boundary

Status, watching, browsing and workspace mutations are local-only. Use the team's Git GUI or command-line client for branch and collaboration operations such as fetch, pull, push, commit, merge, conflict resolution, and LFS lock management. The only network exception is an explicit historical Diff/Restore whose required LFS object is missing locally; the provider performs a path- and commit-scoped fetch after showing cancelable progress.

## Installation and configuration

Install this plugin in the project's `Plugins/` directory, or install it in the engine's `Plugins/` directory. The plugin does not ship precompiled binaries; build it with the project's Unreal Editor target.

The settings page only displays the resolved Git executable and local repository root. It does not persist a binary override. Configure the Editor process `PATH` when a different Git installation should be selected.

For repositories that use Git LFS for Unreal assets, keep explicit attributes for `*.uasset` and `*.umap` in `.gitattributes`, and install Git LFS so the resolved Git executable can run `git lfs pointer` and targeted `git lfs fetch`. The plugin does not ship a Git LFS executable. Configure authentication and remote policy in the external Git client.

## Safety semantics

- Mutating actions require explicit file selections. Directory-wide requests and selections spanning different repositories are rejected.
- Read-only preflight and LFS download show cancelable progress. After confirmation, mutation and package reload remain modal and non-cancelable until completion.
- Discard and historical restore perform a confirmation, file fingerprint, repository-generation, and index recheck before mutation.
- Mutations use exact pathspecs, create temporary safety backups, and attempt rollback when a filesystem or Git step fails. They never invoke directory-level `git clean`.
- Discard restores both the selected worktree and index entries from `HEAD`. Historical restore writes only the selected worktree file and leaves the index, branch, and remotes unchanged.
- Unreal package revisions are validated before replacement. Package reload is requested after a successful disk mutation.

## Editor automation API

AngelScript and Blueprint automation can use the typed `GitLocalSourceControl` API. It exposes provider info plus async status, history, LFS fetch, discard, untracked delete, and revision restore operations. Each start function returns a `GitLocalSourceControlOperation`; call `Tick`, `Cancel`, `IsTerminal`, `GetPhase`, and `GetResult` on the Game Thread.

The API accepts asset object paths rather than raw Git arguments. Mutation calls reject Map, loaded, dirty, or open assets, recheck package and Git state immediately before writing, serialize mutations per repository, and become non-cancelable after the mutation boundary. Module shutdown cancels read-only work and joins active mutations.

## Build and tests

From the repository root:

```text
npm run build:regular
npm run test:unreal:automation -- Cthulhu.GitSourceControl
```

The automation tests create isolated temporary Git repositories and a local bare LFS remote. They require Git and Git LFS but do not contact an external server.

## Known limitations

- Remote state, branch coordination, server-side conflict handling, and LFS locking are outside the Editor provider and must be handled externally.
- Historical LFS revisions require Git LFS through the resolved Git executable and a resolvable upstream, `origin`, or sole remote when the object is missing. Downloads are commit/path scoped, cancelable, time-bounded, and verified by SHA-256 and size.
- Asset mutations and package reloads depend on the Unreal Editor's package state; save or reload prompts may still be required by the surrounding Editor workflow.

## Attribution and license

This plugin is a refactor of the [UE4GitPlugin by Sebastien Rombauts](https://github.com/SRombauts/UE4GitPlugin), with production-oriented changes from Project Borealis. It is distributed under the MIT License; see [LICENSE.txt](LICENSE.txt).

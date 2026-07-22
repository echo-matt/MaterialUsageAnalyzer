# Material Usage Analyzer

> ⚠️ **This plugin is entirely vibecoded.** It was built through AI-assisted (Claude) iteration, not hand-audited line by line. Test it on a throwaway copy of your project before trusting it on real content. See [Known Issues](#known-issues--missing-features) below.

> This plugin was created after seeing the Unreal Fest presentation https://www.youtube.com/watch?v=wobQ8ZKQpbc&t=586s&pp=0gcJCZsLAYcqIYzv, as I needed that material analyzer cool thing they showed but since I am working in 5.7 I had to backport the changes they made in 5.8, so this won't work unless you do the same.

Editor-only Unreal Engine plugin that analyzes **per-material-instance usage flag** efficiency (`bUsedWithStaticLighting`, `bUsedWithSkeletalMesh`, `bUsedWithNiagaraSprites`, etc.) across a material's full instance hierarchy, tells you which flags are wasted shader permutation cost, and lets you fix it in bulk.

## Why

Every "Used with X" flag on a material generates extra static permutations. Those flags default ON at the base `UMaterial` and are inherited down the whole `UMaterialInstanceConstant` chain — so a single flag flip on the root material can silently bloat shader compile times and PSO cache size for every instance under it, even instances that are only ever placed on a plain static mesh in a level.

This tool walks the asset registry, figures out what a material instance is actually *used with* (via its referencers — static mesh, skeletal mesh, Niagara emitter, landscape, instanced/foliage placement, etc.), compares that against the currently effective usage flags, and flags the difference as **wasteful** (flag ON but nothing needs it) or **missing** (flag OFF but something needs it).

## ⚠️ Requires a non-stock engine change

This plugin calls `UMaterialEditingLibrary::SetMaterialUsageOverride()` and reads custom `UsageFlags` / `UsageFlagsOverrideMask` asset registry tags that **do not exist in stock UE 5.7**. Per-material-instance usage flag overrides only ship natively starting in **UE 5.8**. On this branch they were manually hand-ported from 5.8 into the engine (14 modified engine files: `MaterialInterface`, `Material`, `MaterialInstance`, `BasePropertyOverrides`, `MaterialInstanceDynamic`, `MaterialInstanceConstant`, `MaterialShared`, `MaterialEditingLibrary`, plus the MI details customization panel), together with local-only asset-registry tag additions.

**This means the plugin will not compile against a vanilla engine.** If you're on 5.8+, you still need to verify the asset-registry tags this plugin reads (`UsageFlags`, `UsageFlagsOverrideMask`) are something your engine actually writes — they were a local addition here, not stock even in 5.8. Treat this repo as a reference implementation / starting point, not a drop-in plugin, unless you're running the same backport.

## Features

Console command: **`MaterialUsageAnalyzer`** toggles the tool window (three tabs).

### Tab 1 — MI Hierarchy Analysis
- Pick a root `UMaterial`, scans every `UMaterialInstanceConstant` under it (registry-only, via the `Parent` tag — no asset loading required for the base pass).
- Infers real usage requirements per flag from each MI's referencers (static mesh placement, skeletal mesh, Niagara, landscape, UI/widget, etc.), including instancing indirection (ISM/HISM/foliage/Packed Level Actor — these reference the *static mesh*, not the MI directly, and are handled as a second-hop lookup).
- Per-cell click-to-cycle override editor: inherit → ON → OFF per flag per instance.
- Filter builder (flag + condition), batch-edit row to stamp an override across the current filtered set, "Save All" writes overrides via `SetMaterialUsageOverride` and resaves.
- Optional **Deep Scan**: loads levels/packages to catch usage that's only detectable at runtime (construction-script HISM, dynamically spawned components) — async, batched, cancelable, GC-aware.
- Optional slow-flag resolution (clothing / Nanite / Niagara / Cascade material-flag requirements) — also async/batched so it doesn't stall the editor on large hierarchies.

### Tab 2 — Material Batch Analysis
- Bulk-analyze an arbitrary list of root materials (single pick or recursive directory scan).
- Worst-offender sort, per-flag wasted/missing counts, CSV export.

### Tab 3 — Graph Analysis
- Deep-dive on one loaded material/instance: walks the expression graph (recursing material functions and material attribute layers), reports texture sample counts/duplicates, static switches, custom HLSL nodes, function calls, WPO/PDO/two-sided relevance flags.
- Static switch table: which path is active, whether it's inline or graph-driven, per-branch texture cost, redundant-switch detection.
- **Scan Instance Variance**: walks every instance under the root material and tallies whether a given static switch actually varies across instances. If it never varies, the tool tells you to delete the switch and hardwire the branch instead of paying the static-permutation cost for a decision that's already made.
- Shader stats (instruction/sampler/texture counts) via `FMaterialStatsUtils::ExtractMatertialStatsInfo`.

## Requirements

- `SlateIM`, `Niagara`, `MaterialEditor` plugin/module dependencies (see `MaterialUsageAnalyzer.Build.cs`).
- Editor build only (`Type: Editor`).
- The engine backport described above (5.8+ native support strongly recommended over hand-porting it yourself).

## Known Issues / Missing Features

- **Not fully verified in-editor end to end** on this branch: window open, cell-click editing, auto-record-on-save behavior, the actual shader-map shrink from an OFF override, and registry tag appearance after resave have each been spot-checked but not exhaustively regression-tested.
- Usage that's set via a runtime **construction-script HISM in an arbitrary Blueprint** is invisible to the registry pass and to Deep Scan's static component walk — only opening the level once (which triggers engine auto-record) or a full Deep Scan catches it reliably.
- Deep Scan and slow-flag resolution both load packages asynchronously and can take a while on large projects; there's a cancel button but no resume/checkpoint.
- SlateIM re-entrancy: native modal dialogs (file/directory pickers, CSV export) and blocking editor pumps had to be deferred through a queue + `FTSTicker` to avoid crashing the SlateIM root — if you extend this tool with new modals, follow that pattern rather than calling `FSlateApplication` modal APIs directly from inside `Draw()`.
- No automated tests.
- `Graph Analysis` shader-stats extraction reads engine-private struct layout via `reinterpret_cast` (`FMaterialStatsUtils::ExtractMatertialStatsInfo` output) — this **will break silently** if `MaterialStats.h`'s internal layout changes between engine versions. Check that struct on upgrade.
- Only tested against relatively small/medium material hierarchies; no data on performance at AAA-scale asset counts.

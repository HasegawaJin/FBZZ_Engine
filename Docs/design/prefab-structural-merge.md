<!-- @file prefab-structural-merge.md -->
<!-- @brief Prefab structural merge identity, conflicts, review and checked history contract. -->
<!-- @author Hasegawa Jin -->
<!-- @date 2026-10-24 -->

# Prefab structural updates

`PrefabStructureMerge` compares the last successfully applied template (`prefabSourceSnapshot`), the current instance and the current source file. It uses the existing `prefabSourceId`, `instanceId` and `parentInstanceId` schema. It does not infer identity from names or array order. Source IDs are required in both template revisions. Legacy unique parent names can be resolved to existing source IDs; ambiguous names are rejected.

The supported scope is one stable root and its descendants. Adding, removing and reparenting template children works alongside compatible local additions, removals and parent overrides. The root keeps its placement and external parent. Existing instance GUIDs survive; template additions receive fresh GUIDs. Instance-only additions receive temporary merge keys and retain an empty `prefabSourceId` after publication, so the next template revision still recognizes them as local additions.

Retained objects keep their relative instance array order. Incoming-only objects append in source array order, independently of the lookup map's source-ID ordering. Sibling reorder changes are not merged as a separate three-way operation; an incoming insertion position among retained siblings is not imposed.

For each stable source ID:

| Baseline | Instance | Template | Result |
|---|---|---|---|
| absent | absent | present | Add template object. |
| absent | present | absent | Keep local object. |
| present | absent | unchanged | Keep local deletion. |
| present | unchanged | absent | Remove template object. |
| present | edited | absent | Conflict: deletion would discard local edits. |
| present | absent | edited | Conflict: local deletion and template edit. |
| present | present | present | Merge parent and property changes. |

An instance parent differing from the baseline is retained when the template parent is unchanged or agrees. Different simultaneous parent changes conflict. The final hierarchy is validated again; combined cycles, missing parents and local children left beneath removed parents conflict. ID collisions, multiple roots, changed root identity, malformed data, missing source files and explicit nested Prefab links also conflict. Nested expansion and reparenting across instance boundaries require a separate workflow. An inherited child moved outside the connected subtree has no persisted owner-instance mapping in the current schema; this workflow does not claim to recover that ownership.

Property values differing from the baseline are local overrides and win over incoming leaf values. Component or property removal against edits on the other side conflicts. Unchanged fields receive template values. Asset reference hints and automatic instance name suffixes do not create false local edits. Existing Prefab reference normalization is used to compare and rebuild internal GUID references; retained serialized properties pointing at removed source IDs conflict rather than silently binding elsewhere. The existing engine replacement path remaps saved non-owning scene references, and invalidates references to deleted objects. Arbitrary C++ local pointers are outside that contract.

After success the root baseline becomes the actual current template, not the merged instance. Compatible local deletions, additions and overrides therefore remain local on subsequent updates. A conflict retains the old instance and old baseline. Old instances without a baseline keep the existing conservative property-only refresh path; structural changes cannot be inferred safely for them.

## Review and publication

The Inspector's **Preview Structure Update** shows structural changes and conflicts. **Apply Structure Update** applies one reviewed instance with Undo enabled. The operator equivalents are `prefab.structure.preview` with an exact root GUID and `prefab.structure.apply` with the returned receipt. A new operator preview replaces its previous receipt; successful Apply consumes it. Preview does not write assets or invoke save callbacks.

Apply compares the source bytes, the passive Scene snapshot and the recomputed merge against the review. A changed source, Scene or review rejects publication. It then calls `ReplacePrefabInstanceDocument`, a narrow adapter to the existing Prefab prepare/append/reference-remap/destroy path. The adapter accepts an in-memory document, retains the true source baseline and checks preserved-GUID ownership. No temporary asset file or second hierarchy mutation framework is used. Failed hierarchy validation or capacity/preparation checks retain the original hierarchy; append failure uses the existing rollback path.

`RefreshInstanceKeepingOverrides`, override-preserving propagation and `Revert` with preserved overrides retain the existing conservative policy: local child additions, deletions or parent changes refuse automatic replacement, retaining the existing objects, references and baseline. `ApplyAndPropagate` uses overrides captured before the asset write and skips those same instances. A saved baseline does not opt an instance into an implicit structural three-way merge. Review and explicit Apply provide that new behavior; after automatic refusal the same unchanged instance can still be reviewed and merged.

When local topology matches its baseline, automatic propagation continues to apply incoming template additions, removals and parents through the existing safe replacement path while preserving local property values. This was already supported before structural three-way merge. Legacy instances without a baseline compare against the current source and retain their prior conservative behavior. Explicit **Revert** continues to discard instance changes. Writing an instance back to its source asset remains the separate existing Prefab Apply operation.

## Undo and recovery

Explicit Apply uses `CheckedLambdaCommand` and existing passive Scene serialization. The complete before/after snapshots restore deleted objects and external references together. Restore fully prepares the replacement Scene in memory, validates its object count, and compares a fresh passive capture against the requested snapshot using canonical TOML formatting before moving it into the existing Scene object. The initial recovery Scene passes the same check. A restore callback changing serialized authoring values is therefore refused even when the object count is unchanged. Undo or Redo first compares the current snapshot to the expected side; a later unrelated edit refuses the transition and retains the history cursor. Redo restores the captured result and does not rerun a merge against a changed template. If the final post-Apply snapshot fails, the validated before-Scene is used for recovery.

Commands reject Play, Prefab asset edit mode and Scenes containing objects omitted by the recovery snapshot. Preview, Apply and history also reject the known non-restorable case of a MeshRenderer with a live mesh pointer and an empty meshPath anywhere in the Scene; the existing serializer only warns and cannot reconstruct that mesh. This bounded guard does not redefine all runtime component snapshot semantics. Native EntityIDs and cached pointers can change on replacement and snapshot restore, as in existing Scene snapshot history. Selection and active UI canvas are invalidated when restoring a whole Scene. Snapshot cost scales with the Scene; this first implementation makes no performance improvement claim. It uses the existing component/script restore semantics and does not claim rollback of arbitrary external effects from user constructors or callbacks.

## Regression verification

`PrefabStructureMergeTests.cpp` contains pure merge cases and tiny `EditorFixture` cases. They cover template add/remove/reparent with local properties and additions; compatible local deletion/reparent; delete/edit and divergent-parent conflicts; combined cycles; missing parents and IDs; nested links and removed-target references; GUID references; save/load; repeated Apply; two template revisions; deleted-object/external-reference Undo/Redo; stale history; stale source/Scene reviews; no-op sibling order and stable appended additions; non-restorable unrelated mesh rejection; and same-count restore callback changes during initial recovery and Undo. Compatibility regressions verify automatic refusal through propagation, direct refresh, preserved Revert and Apply-and-propagate, retaining object addresses, external references, baselines and existing history before a successful explicit merge and Undo/Redo. Incoming-only topology updates also remain supported automatically. The original three `PrefabSaveTest.AutomaticPropagationKeepsLocally*Children` assertions remain unchanged. Fixture asset writes are restricted to `TestKit` temporary projects. The callback fixture requires an initially empty ScriptFactory registry, registers only its temporary test type and unregisters it in teardown.

After a change, use `Tools/AgentBuild.ps1 check`, build `FBZZTestsEditorAuto`, and run the `PrefabStructureMerge` and existing Prefab suites to catch regressions in shared replacement and legacy property behavior. GPU and full Editor verification remain separate. Staging alone does not establish build or runtime success; the integration report records completed verification.

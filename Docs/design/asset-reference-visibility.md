# Asset Browser reference visibility

Asset Browser's **Find References...** presents an owned snapshot of direct saved reference evidence for the selected asset. Each owner row shows its asset type, project-relative path, TOML field and source line. Selecting an owner uses the existing reveal/select/ping request; it does not open a scene or replace an unsaved editor document. The modal is rendered once per panel so grid, list and tree context menus share the same behavior, including a filtered or empty grid.

## Evidence and scope

The scan reuses `AssetSearch::Entries()` as a value-owned candidate snapshot and records `AssetSearch::Revision()` and `AssetDatabase::Revision()`. It does not build a second filesystem index. The supported owner formats under indexed project `Assets/` are `.scene`, `.mat`, `.prefab`, `.animcontroller`, `.vfx`, `.terrain`, `.fzdata`, `.terrainpaint`, and `.meshinstances`. Their TOML string values are visited recursively through tables and arrays. Owner labels describe their file format, not their provenance or purpose.

Evidence is an exact complete string value, never a substring or stem match. Comments are ignored by TOML parsing. Existing `guid:<32hex>[subasset suffix]|<path hint>` values retain GUID authority. A different registered GUID with a matching stale hint is not reported as a match. An unresolved GUID with a matching path hint is shown as an **unverified hint candidate**, never a verified GUID match. Numeric submesh and `::sprite::` suffixes are retained for an explicitly selected subasset. Selecting the parent includes its subasset values. Image paths using the legacy `<image>.meta` alias resolve to their source according to the existing `TexDescSerializer::ResolveSourcePath` contract; arbitrary metadata filenames do not establish asset relationships.

The UI intentionally calls these serialized reference values. A user string can contain a whole asset reference without exercising it at runtime. Script-created paths, script/code text, binary or unsupported formats, `.meta` contents, owners in external/shared roots, generated owner dependencies, transitive use and unsaved scene contents are not scanned. A direct generated target can be matched using its existing AssetDatabase derived GUID or explicit `Library/...` path, but the scan does not infer that every generated asset belongs to a selected source model. AssetSearch itself excludes generated directories and caps its index; no completeness claim is made beyond the recorded candidate snapshot.

**No reference detected does not establish that deletion is safe.** This feature neither deletes nor imports assets, changes metadata, cleans caches nor changes packaging behavior.

## Provenance and verification purpose

The inspected existing sidecars record GUIDs and import settings, but do not provide a common source-of-addition or verification-purpose schema. The modal therefore displays **Unknown** for added source/provenance and verification/sample/production role for both target and owners. It never derives those labels from a folder, filename, GUID or source DCC setting.

Existing `[model].source_dcc`, `importer` and `importer_version` values are shown verbatim as recorded import settings. `Auto` remains `Auto`; it does not prove which DCC created an asset. Missing, malformed, oversized or unreadable metadata does not become a provenance classification.

No new `asset_info` schema is introduced. Static inspection found that `FbxMetaSerializer::SaveOptions` loads the existing root and replaces `model`, while `SaveCacheInfo` additionally removes the legacy `cache` table. In contrast, `TexDescSerializer::Save` reconstructs the root from `vector_field`, `meta` and `texture`, so a new arbitrary root table would not survive that writer. Durable, editable provenance metadata is a separate format contract and is not implied by this UI.

## Cost, lifetime and refresh

Opening Find References or pressing **Refresh saved references** starts a new generation and replaces the previous result. A scan advances through at most four owner files per displayed frame only while that requested scan is active. Each owner file is limited to 2 MiB and each metadata read to 64 KiB. A scan is limited to 8,192 owner candidates, 64 MiB of owner text, 4,096 evidence items, depth 64 and 65,536 visited TOML nodes per owner. Hitting a limit is visible as a partial result. Unreadable/changing files, oversize files and invalid TOML have separate counters.

Reads use the file size as an allocation/read bound, reject extra bytes and reject a modification-time change during the read. A completed scan performs no more content reads when drawn again. Candidate values remain owned when the search index reallocates. Starting a new request clears the previous generation synchronously, so there is no late worker publication. Changing the active project suppresses old owners and requires closing/reopening for the current project.

Index/GUID generation changes label the snapshot stale but do not silently start a new scan. AssetSearch's path index does not advance its revision for content-only modifications. The UI therefore always labels the result an on-demand saved-file snapshot and asks the user to save and refresh after edits. Files read at different moments are not an atomic project snapshot. Metadata is read only when the request begins or an owner is detected, not during normal completed-result rendering.

## GUID を起点にたどる MCP 操作

`asset_dependencies_scan(guid, maxDepth)` で一度調査し、返された `job` を
`asset_dependencies_status(job, direction, offset, limit)` に渡す。どちらも read 権限で使える。
GUID は32桁の16進数で `guid:` を付けてもよい。既に参照先が失われた GUID も起点にできる。
`finished=true` までステータスを取得し、同じジョブで `direction` を切り替えられる。

- `outgoing` は保存済みの参照先をたどる。シーンから Prefab を経由して Material に至る場合、Material への辺は `distance=2`。
- `incoming` は保存済みの参照元を逆にたどる。変更する Material を起点にすると、直接参照する Prefab と、その Prefab を参照するシーンが分かる。
- 辺には `source`、`target`、`field`、`line`、`referenceKind` がある。循環や同じアセットを指す複数のフィールドも根拠として残し、各ノードの探索は一度だけ行う。
- `target.state` の `missingGuid` と `missingPath` が参照切れ。読み取り権限などで存在を確定できない場合は `unverified`。GUID 付きの `hint` は保存時の情報として返す。移動後も GUID で解決し、GUID が消えた場合にヒントのファイルをたどることはない。

`totalEdges` はページ全体の件数、`relatedCount` は起点を除いた到達ノード数、
`brokenCount` は到達した参照切れの辺数。幅優先で近い辺から返し、`offset` で続きに進む。
深さは既定4、最大16。深さ上限の先に辺がある場合は `depthLimited` を返す。
`stale=true` のジョブは辺を返さない。保存・移動・削除後は新しい調査を開始する。

対象は上記の保存済み TOML と索引済みの `Assets/`。パスだけを持つ旧形式の保存値も扱う。
GUID で解決した共有・生成アセットも参照先に表示できる。参照元の本文はプロジェクト内の対応ファイルだけから読む。
スキャン上限、読み取り失敗、構文エラーは `limited`、`incomplete` と `coverage` で確認する。
動的・未保存・バイナリ内の参照は調査範囲外。サブメッシュ番号や Sprite の接尾辞は `suffix` に返し、
存在確認はアセット本体まで行う。

## Validation status

CPU regression code is provided in `AssetReferenceScanTests.cpp`, using the same private scanner as the production modal and existing TestKit fixtures. Cases cover actual `EncodeGuidRefs` serialization, nested field/line evidence, comments and substring rejection, script/dynamic scope, GUID authority versus unresolved hints, image-sidecar aliases, exact sprite suffixes, unknown versus recorded import settings, incremental/finished scan behavior, explicit refresh and generation changes, malformed/missing/oversize files, no metadata creation and existing derived GUID mappings for generated targets. No public test-only API is added.

This isolated change has not been compiled or executed. Native integration, `AgentBuild.ps1 check`, the registered CPU tests and any Editor visual/interaction checks remain pending under the current execution restrictions. Future checks should exercise grid/list/tree invocation, same-name owners in different folders, reveal/select behavior, refresh after save, malformed/large input, project changes while the modal is open and scrolling through a bounded large result.

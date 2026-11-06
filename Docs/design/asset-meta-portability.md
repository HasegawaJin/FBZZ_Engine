<!-- @file    asset-meta-portability.md
@brief   端末間でアセットメタデータのGUIDと保存内容を維持する契約。
@author  Hasegawa Jin
@date    2026-11-06 -->
# アセットメタデータの保存

`.meta` は共有する制作データであり、既存のGUID・型mapping・Sprite ID・アセット参照を引き継ぐ。原本の絶対パス・更新時刻など、端末やcloneで変わるインポート記録は `Library/ImportCache.toml` に置く。

Gitの `*.meta text eol=lf` とEditorConfigの `[*.meta]` により、作業ツリーでもUTF-8・LFを使う。`core.autocrlf` に依存させない。既存ファイルをLFへ移すときはTOMLを再生成せず、CRLFだけを置換する。

Engineの `asset::MetaFile::Save` は保存候補と既存ファイルをTOMLとして比較する。同値なら既存のコメント・書式を保ち、既にLFなら書き込まない。CRLFの既存ファイルは同じ内容のままLFへ移行する。値が変わったときはLFの候補を原子的に保存する。読取・解析・保存が失敗した既存ファイルは上書きしない。

モデル、テクスチャ、速度場、GUID発行、C#型mapping、SceneのEditor設定はこの保存処理を使う。C#型mappingの複数ファイル保存が失敗した場合のロールバックは、変更前の原本バイト列を復元する。

Sceneの `[editor].game_objects` は `instance_id` 順に保存し、非表示・ロック対象のIDも昇順にする。各GameObjectの `component_order` は作者が指定した表示順を保持する。共有メタデータの並びを `unordered_map` の挿入順や再ハッシュに依存させない。

回帰テストは `Projects/Tests/Editor/Auto/MetaFilePersistenceTests.cpp`。同値保存時の更新時刻維持、CRLF移行、GUID・参照・Sprite IDの維持、変更した設定の反映、インポート記録の分離、Scene保存順、解析失敗時の原本保護を検証する。

<!-- @file    shader-capabilities.md -->
<!-- @brief   シェーダー能力の明示宣言と実行版への固定。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-02 -->
# シェーダーの能力契約

ファイル名、フォルダー名、GUID の表示ヒントから能力を推測しない。原本の `.hlsl.meta` に `[shader]` を記載し、改名・移動では原本と `.meta` を一緒に移す。GUID は索引の現在の実体だけへ解決する。解決できない GUID を、たまたま同じ名前のヒント先で認定しない。

標準 PBR の剛体メッシュ用の宣言は次のとおり。

```toml
[shader]
version = 1
vertex = 'standard_surface_v1'
surface = 'metallic_roughness_v1'
opacity = 'alpha_clip_v1'
variants = 'standard_surface_v1'
```

`vertex` は `standard_surface_v1` / `standard_skinned_v1` / `custom`。標準セットは Engine の頂点変換、UV とスキニングを共有し、独自の頂点変形を含まない。`variants` は一致する標準 GBuffer の static / instanced / skinned 変種の組、または `none`。形状や alpha clip が異なる独自シェーダーに標準セットを付けない。

草のシェーダーで `vertex = 'fiber_blade_v1'` と `variants = 'fiber_blade_instanced_v1'` を宣言すると、描画パスがリボン単位のインスタンス化を選べる。`fiberLod.w` が 1 のときは `SV_InstanceID / 2` が根元番号、下位ビットがリボン番号、`SV_VertexID` がリボン内の頂点番号になる。0 のときは従来どおり `SV_VertexID` から根元・リボン・面の頂点を求める。

`standard_surface_v1` のカスタム不透明・カットアウト材質は、`variants = 'resident_forward_v1'` を宣言できる。`Example.hlsl` と同じフォルダーの `Example.Resident.hlsl` は `FBZZ_INSTANCED` と `FBZZ_RESIDENT_INSTANCES` を有効にして原本を含め、`ObjectInstance.hlsli` から個体を読む。色の材質定数とテクスチャは原本と共通。変種または個体資源の準備に失敗した場合は原本による個別描画に戻る。

`surface` は `metallic_roughness_v1` / `lambert_v1` / `unlit_v1` / `water_v1` / `terrain_v1` / `custom`。`opacity` は `alpha_clip_v1` / `opaque_v1` / `custom`。GBuffer は標準 PBR、標準 alpha clip、一致する頂点・変種セットをすべて要求する。Lambert と Unlit は PBR と同等ではないため Forward へ送る。空欄、未宣言、不正な型、未知の版、矛盾する変種セットも Forward へ送る。

RT の形状、被覆、BSDF は別に評価する。標準の不透明形状は遮蔽者になれても、PBR の表面契約が無ければ標準 BSDF として評価しない。スキンドでは現在フレームの変形済み形状も要求する。定数材質、テクスチャ、追加ローブや誘電体に関する既存の被覆検証は引き続き適用する。

ResourceManager は成功した shader load / reload の能力をハンドルと世代に固定する。PBR 宣言は、ロードされた PS に標準化可能な albedo、metallic、roughness、UV、alpha cutoff の float 定数があることも検証する。コンパイルや reload の準備に失敗した場合は旧 shader と旧能力を保持する。全体 reload は全候補を準備してから一括で公開する。

プレビュー入力も `preview` に明示できる。値は `surface` / `skinned` / `water` / `terrain` / `ui` / `particle` / `trail` / `decal` / `post_process` / `mesh_trail` / `gpu_particle` / `fiber_shell` / `fiber_fin` / `fiber_blade`。省略時は宣言済みの標準頂点と Water / Terrain 表面からのみ補う。GPU Particle と MeshTrail は通常の材質プレビュー用頂点では描けないため非対応表示にする。Material の既存 `render_path` と `mesh_type` も尊重し、名前による救済は行わない。

`.meta` の CPU 読み取りは更新時刻でキャッシュする。本体の存在と `.meta` の更新時刻は各照会で確認し、通知なしの変更・削除・復旧も次の照会へ反映する。再利用するのは参照の解析、GUID 索引の版に対応するパス、UTF-8 から native path への変換結果だけとする。保存域はスレッドごとに件数を制限し、GUID 索引の変更・初期化・終了で解決先を引き直す。通常のパス参照は毎回解決し、新しい project 側ファイルが共有 Engine asset より優先される規則を保つ。

実際の shader コードが宣言した数学的意味を満たすことは作者の契約であり、反射のレイアウト検査だけで任意のコードの意味を証明できるとはしない。新しい変形・BSDF・独自 GBuffer の追加は、対応する variant と検証を別途実装してから契約の版を追加する。

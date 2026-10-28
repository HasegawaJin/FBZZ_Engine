# 描画エラー時のフォールバック

Material の未割当・読込失敗・空 shader・shader 読込失敗は、スロット単位でマゼンタの Unlit 診断材質へ切り替える。MaterialComponent の disabled とスロットの visible=false は引き続き非表示であり、読込失敗と同一視しない。主スロットの失敗で他のスロットを非表示にしない。

診断材質は Graphics の ResourceManager が通常頂点用とスキンド頂点用を独立して所有する。Shader の識別子は builtin:ErrorSurface / builtin:ErrorSkinned。DX12 の VS / PS はビルド時に固定 DXC で生成して DLL に埋め込む。実行時の .mat / .meta / GUID / HLSL / CSO / include / shader reflection は使わない。ビルド時の生成対象は FBZZEmbeddedShaders で、変更単位コンパイル前にも単独で生成できる。

CPU Vertex / SkinnedVertex の offsetof で入力レイアウトを設定する。Camera b0 の viewProjection offset=128、Object b1 の world offset=0、Skinning b7 の boneMatrices[128] は既存の描画契約と一致させる。PS は (1,0,1,1) を直接返す。診断材質は opaque・両面・geometry queue・depth bias=0 とし、元材質の透過やインスタンス上書きに依存しない。Deferred でも Forward の診断経路を使い、PBR / GBuffer 材質と誤認しない。GPU の生成・記録そのものの失敗は診断材質で回復できるとは保証しない。

元の Material 参照・上書き・シーン保存データには代替を書き戻さない。AssetManager の失敗キャッシュを再インポートで解除した後は再解決する。初回 shader 失敗も再試行し、成功済み shader の reload 失敗は既存の最後の正常版保持契約に従う。ResourceManager Reset 後は次回取得で診断 shader を再生成する。

SpriteFallback / VFXMeshFallback / ProceduralMeshFallback / ParticleFallback は未指定時の用途別既定材質であり、エラー診断用とは別物。旧 Fallback.mat / FallbackSkinned.mat は既存シーンの明示参照との互換のため残すが、エンジンの最終エラー表示はそれらを読まない。

Particle Material の読込失敗は白テクスチャ・既定 particle 設定へ戻し、前材質の customShader・materialParamsCB・materialTint・6-way 補助画像と変更検出キーをすべて解除する。customShader 読込失敗と GPU reset 後の無効化は同じ path でも再試行する。

検証は CPU fake backend で未割当・欠損・shader 失敗・復帰・スロット分離・無効化・reset を固定する。GPU は外部 shader resolver を必ず失敗させた状態で通常／スキンドを描画し、色とボーン変形を読み戻す。

## 2026-10-05 の検証

- 変更 C++ 7 単位の compile check と、埋め込み生成条件の修正後の DX12Shader 再 check がエラー・警告 0 件。
- AgentLint は変更した 11 ファイルを全体検査し、エラー・警告 0 件。
- 関連 CPU テスト 64 件（追加した Material / Particle の回帰 11 件を含む）が合格。記録は `build/agent/test-20261005-134634-68480.log`。
- `GraphicsStandaloneTest.EmbeddedErrorShadersDrawWithoutExternalFilesAndRespectSkinning` が実 DX12 デバイスで合格。通常・スキンドのマゼンタと、ボーン変形で画面外へ移した場合の黒を線形 RGBA 読戻しで確認した。外部 shader resolver は常に空パスを返す。Graphics 単独ビルドもエラー・警告 0 件（`build/agent/build-20261005-134652-44212.log`）。
- 全体再ビルドは並行して変更されている ManagedHost の C# API 不一致で失敗した（`build/agent/build-20261005-134341-54184.log`）。この変更では C# 基盤を修正せず、エディターの再起動と Playtest シナリオは未実施。

@see [Graphics ライブラリ](graphics-library.md)、[Deferred と Forward](pipeline-boundary.md)、[DXC オプション](https://github.com/microsoft/DirectXShaderCompiler/blob/main/include/dxc/Support/HLSLOptions.td)

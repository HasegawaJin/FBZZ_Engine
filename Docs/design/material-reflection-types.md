# Material Reflection の型契約

Script、Inspector、AI バス、プレビュー、本描画は `ShaderVarDesc` と
`MaterialParamBinding` の同じ型・値数・転送規則を使う。

| HLSL | Script の入口 | 保存する値 |
|---|---|---|
| float / int / uint / bool | SetFloat / SetInt / SetUInt / SetBool | 1 個。bool は 0/1 |
| float2 / float3 / float4 | SetVector2 / SetVector3 / SetVector4 | 成分順 |
| float4x4 | SetMatrix | C++ の Matrix4 を行優先のまま渡す |
| その他の 32 bit 数値ベクトル・行列・配列 | SetValues | 配列要素順、その中は行→列。パディングなし |
| struct / struct 配列 | 各 leaf へ上記 API | `surface.tint`、`layers[0].gain` の完全名 |
| 16 bit / 64 bit 型・その他の型 | 未対応として拒否 | shader.inspect の unsupportedReason に理由 |

`SetValues(MaterialPropertyId, span<const double>)` の double は転送用の値表現であり、
HLSL double への対応ではない。int32 / uint32 の全域を正確に渡せる。
配列長・行列サイズは Reflection が決める。型や値数の不一致、整数の範囲外、
NaN/Inf は false と診断ログを返し、既存の override を変更しない。
Reflection を取得できない段階でも成功を返さない。

```cpp
const auto instance = material.Instance();
instance.SetUInt(MaterialPropertyId("mask"), UINT32_MAX);
instance.SetVector2(MaterialPropertyId("uvTiling"), {2.0f, 3.0f});
const double weights[] = {0.2, 0.3, 0.5};
instance.SetValues(MaterialPropertyId("weights"), weights);
instance.SetFloat(MaterialPropertyId("layers[0].gain"), 0.8f);
```

`.mat` の `[params]` は名前と数値・フラット配列を保存する。整数を float に丸めないため
`integerParams` を持つ。同名キーは `params` と排他的に更新する。
読み込み時は float の整数精度内に収まる値を既存 `params` へ戻し、従来の数値参照を保つ。
整数としての GPU 転送は保存領域に依存せず、シェーダーの型で決める。
旧ファイルの短い float ベクトルは従来どおり未指定成分の既定値を保持する。
新しい Script/AI の書き込みは完全な要素数を要求する。

行列は Reflection の行/列優先情報、配列は要素 stride を保持する。
CPU の値列を GPU の配置へ変換し、パディングや隣の変数には書き込まない。
根拠は [DXC Buffer Packing](https://github.com/microsoft/DirectXShaderCompiler/wiki/Buffer-Packing)。
構造体は Reflection のメンバー offset を使用する。

`shader.inspect` は `writable`、`unsupportedReason`、`type`、`components`、
`rows`、`columns`、`elements`、`rowMajor`、`valueOrder` を返す。
`material.override` も同じ検証を通り、整数精度と Undo を保持する。

DataAsset と Scene は KeyCode、型付きアセット参照とリストの TOML codec を共有する。
旧 DataAsset の path 文字列も読み込む。多態参照は type と fields を保存し、
未登録型の fields は再保存時にも保持する。Scene の Entity 参照解決は Scene 側に残す。

回帰テストは `MaterialReflectionTest` と `MaterialScriptIntegrationTest`。

# Renderer / Shader

HLSL シェーダーの管理・コンパイル・キャッシュ。

---

## ShaderManager

```cpp
namespace fbzz::renderer {

class ShaderManager {
public:
    // パスからシェーダーをロード。2 回目以降はキャッシュを返す
    static std::shared_ptr<IShader> Load(const std::string& path);

    // キャッシュを破棄 (開発中のホットリロード用)
    static void ClearCache();
    static void Reload(const std::string& path);

private:
    static std::unordered_map<std::string, std::shared_ptr<IShader>> m_cache;
};

} // namespace fbzz::renderer
```

### 使用例

```cpp
auto shader = ShaderManager::Load("assets/shaders/Phong.hlsl");
```

---

## コンパイル方式

DX11 では実行時コンパイル (`D3DCompileFromFile`) を採用する。
開発中にシェーダーを編集してもリビルドが不要になる。

```cpp
// DX11Shader.cpp 内
HRESULT hr = D3DCompileFromFile(
    wPath.c_str(),
    nullptr,
    D3D_COMPILE_STANDARD_FILE_INCLUDE,
    "VSMain", "vs_5_0",
    compileFlags, 0,
    &vsBlob, &errorBlob
);
```

将来の DX12 移行時は事前コンパイル (DXIL) に切り替える。
上位レイヤーは `ShaderManager::Load()` のみを使うため変更不要。

---

## HLSL ファイル構成

1 ファイルに頂点シェーダー (VS) とピクセルシェーダー (PS) を記述する。

```hlsl
// assets/shaders/Phong.hlsl

cbuffer CameraConstants : register(b0) {
    float4x4 viewProjection;
    float3   cameraPos;
    float    _pad;
};

cbuffer ObjectConstants : register(b1) {
    float4x4 world;
};

struct VSInput {
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD;
};

struct PSInput {
    float4 position : SV_POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD;
    float3 worldPos : TEXCOORD1;
};

PSInput VSMain(VSInput input) {
    PSInput output;
    float4 worldPos = mul(float4(input.position, 1.0), world);
    output.position = mul(worldPos, viewProjection);
    output.normal   = mul(input.normal, (float3x3)world);
    output.uv       = input.uv;
    output.worldPos = worldPos.xyz;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET {
    // ... ライティング計算
}
```

---

## 定数バッファのスロット規則

| スロット | 用途 |
|----------|------|
| `b0` | CameraConstants (VP 行列, カメラ位置) |
| `b1` | ObjectConstants (ワールド行列) |
| `b2` | MaterialConstants (色, テクスチャフラグ) |
| `b3` | LightConstants (ライト情報) |

---

## シェーダーファイルの配置

```
assets/
└── shaders/
    ├── Phong.hlsl          Phong ライティング
    ├── Unlit.hlsl          ライティングなし (単色・テクスチャ)
    └── DebugLine.hlsl      デバッグ線描画
```

---

## ファイル構成

```
engine/
├── include/engine/
│   └── Renderer/
│       └── ShaderManager.hpp
└── src/
    └── Renderer/
        └── ShaderManager.cpp
```

---

## 参考ドキュメント

- [D3DCompileFromFile](https://learn.microsoft.com/ja-jp/windows/win32/api/d3dcompiler/nf-d3dcompiler-d3dcompilefromfile) — HLSL ファイルの実行時コンパイル
- [HLSL リファレンス](https://learn.microsoft.com/ja-jp/windows/win32/direct3dhlsl/dx-graphics-hlsl-reference) — HLSL 言語仕様全体
- [定数バッファ (cbuffer)](https://learn.microsoft.com/ja-jp/windows/win32/direct3dhlsl/dx-graphics-hlsl-constants) — cbuffer / register の使い方
- [シェーダーセマンティクス](https://learn.microsoft.com/ja-jp/windows/win32/direct3dhlsl/dx-graphics-hlsl-semantics) — POSITION / NORMAL / SV_TARGET 等
- [Shader Model 5.0](https://learn.microsoft.com/ja-jp/windows/win32/direct3dhlsl/d3d11-graphics-reference-sm5) — vs_5_0 / ps_5_0 の機能一覧

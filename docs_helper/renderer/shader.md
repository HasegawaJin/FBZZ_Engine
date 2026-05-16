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

**dxc.exe** (DirectX Shader Compiler) でビルド時に事前コンパイルする。
実行時コンパイル (`D3DCompileFromFile` / fxc) は使わない。

| 項目 | 内容 |
|------|------|
| コンパイラ | `dxc.exe` (Windows SDK 同梱) |
| DX11 出力 | DXBC (`.cso`) — `-Fo output.cso` |
| DX12 出力 | DXIL (`.cso`) — `-Fo output.cso` (同じ拡張子、中身が DXIL) |
| シェーダーモデル | `vs_6_0` / `ps_6_0` (DX11・DX12 共通) |

### dxc コンパイルコマンド例

```bat
:: VS コンパイル
dxc -T vs_6_0 -E VSMain -Fo assets/shaders/compiled/Phong.vs.cso assets/shaders/Phong.hlsl

:: PS コンパイル
dxc -T ps_6_0 -E PSMain -Fo assets/shaders/compiled/Phong.ps.cso assets/shaders/Phong.hlsl
```

### DX11Shader::Init() での読み込み

ランタイムはバイトコードをファイルから読み込むだけ。

```cpp
// .cso ファイルをバイナリ読み込み
std::ifstream vsFile("assets/shaders/compiled/Phong.vs.cso", std::ios::binary);
std::vector<char> vsBlob((std::istreambuf_iterator<char>(vsFile)), {});

device->CreateVertexShader(vsBlob.data(), vsBlob.size(), nullptr, m_vs.GetAddressOf());
device->CreateInputLayout(layout, _countof(layout), vsBlob.data(), vsBlob.size(), m_inputLayout.GetAddressOf());
```

### DX11 / DX12 の違い

DX11 は DXBC、DX12 は DXIL だが `dxc.exe` が内部で自動選択する。
**HLSL ファイルとコンパイルコマンドは共通**、`CreateVertexShader` 等の API 呼び出しのみ実装が異なる。

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

## シェーダーファイルの配置 (コンパイル済み)

```
assets/
└── shaders/
    ├── Phong.hlsl              ソース (バージョン管理対象)
    ├── Unlit.hlsl
    ├── DebugLine.hlsl
    └── compiled/               ビルド時生成 (.gitignore 推奨)
        ├── Phong.vs.cso
        ├── Phong.ps.cso
        ├── Unlit.vs.cso
        ├── Unlit.ps.cso
        ├── DebugLine.vs.cso
        └── DebugLine.ps.cso
```

---

## 参考ドキュメント

- [dxc (DirectX Shader Compiler)](https://github.com/microsoft/DirectXShaderCompiler) — dxc ソース・リリース
- [dxc コマンドラインオプション](https://github.com/microsoft/DirectXShaderCompiler/blob/main/docs/UsingDxc.rst) — `-T`, `-E`, `-Fo` 等のオプション一覧
- [HLSL リファレンス](https://learn.microsoft.com/ja-jp/windows/win32/direct3dhlsl/dx-graphics-hlsl-reference) — HLSL 言語仕様全体
- [定数バッファ (cbuffer)](https://learn.microsoft.com/ja-jp/windows/win32/direct3dhlsl/dx-graphics-hlsl-constants) — cbuffer / register の使い方
- [シェーダーセマンティクス](https://learn.microsoft.com/ja-jp/windows/win32/direct3dhlsl/dx-graphics-hlsl-semantics) — POSITION / NORMAL / SV_TARGET 等
- [Shader Model 6.0](https://learn.microsoft.com/ja-jp/windows/win32/direct3dhlsl/shader-model-6-0) — vs_6_0 / ps_6_0 の機能一覧

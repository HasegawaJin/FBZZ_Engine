# Renderer / Texture

テクスチャの読み込み実装。`DX11Texture` が `ITexture` を実装する。
読み込みには **DirectXTex** を採用する。

---

## 採用ライブラリの判断

| 選択肢 | 対応形式 | 依存 | 判断 |
|--------|---------|------|------|
| DirectXTex | PNG / JPG / BMP / DDS / TGA / HDR | Microsoft 公式ライブラリ | ✅ 採用 |
| WICTextureLoader | PNG / JPG / BMP / GIF | DirectXTK スタンドアロン | NG (DDS 非対応、ミップマップ制御が弱い) |
| DDSTextureLoader | DDS のみ | DirectXTK スタンドアロン | NG (汎用性不足) |
| stb_image | PNG / JPG / BMP 等 | 外部ヘッダ | NG (外部依存を増やさない) |

DirectXTex は Microsoft 公式の画像処理ライブラリ。DDS / PNG / JPG / HDR を統一 API で扱え、
ミップマップ生成・圧縮・変換も提供する。サードパーティ導入済みリストに追加する。

---

## DX11Texture

```cpp
namespace fbzz::renderer {

class DX11Texture : public ITexture {
public:
    bool Init(ID3D11Device* device, ID3D11DeviceContext* context,
              const std::string& path);
    void Shutdown();

    uint32_t GetWidth()  const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }

    // DX11Shader が使う内部アクセス
    ID3D11ShaderResourceView* GetSRV() const { return m_srv.Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv;
    uint32_t m_width  = 0;
    uint32_t m_height = 0;
};

} // namespace fbzz::renderer
```

### Init の実装イメージ

```cpp
bool DX11Texture::Init(ID3D11Device* device, ID3D11DeviceContext* context,
                       const std::string& path) {
    std::wstring wpath(path.begin(), path.end());

    DirectX::ScratchImage image;
    DirectX::TexMetadata  metadata;

    // 拡張子で読み込み関数を切り替え
    HRESULT hr;
    if (path.ends_with(".dds") || path.ends_with(".DDS")) {
        hr = DirectX::LoadFromDDSFile(wpath.c_str(), DirectX::DDS_FLAGS_NONE,
                                      &metadata, image);
    } else {
        hr = DirectX::LoadFromWICFile(wpath.c_str(), DirectX::WIC_FLAGS_NONE,
                                      &metadata, image);
    }
    FBZZ_HR_CHECK(hr);

    hr = DirectX::CreateShaderResourceView(device,
                                           image.GetImages(),
                                           image.GetImageCount(),
                                           metadata,
                                           m_srv.GetAddressOf());
    FBZZ_HR_CHECK(hr);

    m_width  = static_cast<uint32_t>(metadata.width);
    m_height = static_cast<uint32_t>(metadata.height);

    FBZZ_LOG_INFO("テクスチャ読み込み完了: %s (%ux%u)", path.c_str(), m_width, m_height);
    return true;
}
```

---

## DX11Renderer::Submit でのバインド

テクスチャのバインドは `IShader` ではなく `DX11Renderer::Submit()` が `DrawCall.textures[]` を見て行う。
`DX11Texture` へのダウンキャストは `DX11Renderer` の内部に閉じており、上位レイヤーには漏れない。

```cpp
// DX11Renderer::Submit() 内部イメージ
void DX11Renderer::Submit(const DrawCall& call) {
    // ...
    for (uint32_t i = 0; i < call.textures.size(); ++i) {
        if (!call.textures[i]) continue;
        auto* dx11Tex = static_cast<DX11Texture*>(call.textures[i].get());
        ID3D11ShaderResourceView* srv = dx11Tex->GetSRV();
        m_context->PSSetShaderResources(i, 1, &srv);
    }
    // ...
}
```

---

## 対応ファイル形式

| 形式 | 拡張子 | 備考 |
|------|--------|------|
| DDS | `.dds` | ミップマップ・圧縮テクスチャ対応 |
| PNG | `.png` | アルファチャンネル対応 |
| JPEG | `.jpg`, `.jpeg` | 非可逆圧縮 |
| BMP | `.bmp` | |
| TIFF | `.tif`, `.tiff` | |
| HDR | `.hdr` | 将来の IBL 対応用 |

---

## ファイル構成

```
engine/
└── src/
    └── Renderer/
        └── Platform/
            └── DX11/
                ├── DX11Texture.hpp / .cpp

third_party/
└── DirectXTex/           Microsoft 公式ライブラリ (vcpkg または手動配置)
```

---

## 参考ドキュメント

- [DirectXTex (GitHub)](https://github.com/microsoft/DirectXTex) — Microsoft 公式ソース・ビルド手順
- [DirectXTex Wiki](https://github.com/microsoft/DirectXTex/wiki) — API リファレンス
- [LoadFromWICFile](https://github.com/microsoft/DirectXTex/wiki/LoadFromWICFile) — PNG/JPG 読み込み
- [LoadFromDDSFile](https://github.com/microsoft/DirectXTex/wiki/LoadFromDDSFile) — DDS 読み込み
- [CreateShaderResourceView](https://github.com/microsoft/DirectXTex/wiki/CreateShaderResourceView) — SRV 生成
- [ID3D11ShaderResourceView](https://learn.microsoft.com/ja-jp/windows/win32/api/d3d11/nn-d3d11-id3d11shaderresourceview) — DX11 公式リファレンス

[Fix] マテリアル/シェーダーと DX11 レンダラーの不具合修正

## 実装内容
- `assets/shaders/Mesh.hlsl` を修正（シェーダーの不具合修正）
- `engine/include/engine/Renderer/Material.hpp` / `engine/src/Renderer/Material.cpp` を修正（マテリアル適用ロジックの修正）
- `engine/include/engine/Renderer/SamplerMode.hpp` を調整（サンプラ設定の修正）
- `engine/src/Renderer/Platform/DX11/DX11Renderer.hpp` / `engine/src/Renderer/Platform/DX11/DX11Renderer.cpp` を修正（DX11 パイプラインの不具合修正）
- `engine/src/Scene/Systems/RenderSystem.cpp` を修正（レンダリング経路の不具合修正）
- `sandbox/src/main.cpp` を更新（動作確認用起動設定の修正）

## 動作確認
- [ ] Debug ビルドが通る
- [ ] Release ビルドが通る
- [ ] サンドボックス起動でレンダリングが期待通りに表示される

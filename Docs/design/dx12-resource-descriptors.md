<!-- @file    dx12-resource-descriptors.md -->
<!-- @brief   DX12 の buffer descriptor staging の所有権と再利用契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-11 -->

# Buffer descriptor staging

DX12StructuredBuffer の SRV/UAV と DX12Buffer の GPU writable vertex UAV は、DX12Context が所有する CPU-only CBV_SRV_UAV heap の共有ページに置く。1ページは4096 descriptor とし、各 buffer は descriptor の添字だけを所有する。生成・参照・解放は描画スレッドに限定する。描画スレッドへの移送は既存の ThreadedRenderer を使う。

1000個体の独立した骨格を3 frame lane で更新すると、palette だけで約3000 buffer が生存する。buffer ごとに小さい native descriptor heap を作る従来方式は、bindless の使用量が容量以下でも CPU heap の生成に失敗する実機条件があった。ページ共有は個体の bone matrices、位相、frame lane、出力 buffer を共有せず、この native heap 数の増加を避ける。

CPU staging の添字は GPU bindless の添字と別の索引である。descriptor は CopyDescriptorsSimple で shader-visible heap へ即時にコピーされ、GPU はコピー先を参照する。buffer 破棄時に CPU staging 枠を即時再利用できる。GPU bindless 枠と resource 本体は従来通り fence まで解放を遅延する。ページそのものは Context の Shutdown まで保持する。device reset ではページと空き添字をまとめて破棄し、旧添字を再利用しない。

ページ確保に失敗した場合はログと INVALID_BINDLESS_INDEX を返し、buffer の生成を失敗させる。既に確保した descriptor は失敗した buffer の破棄時に回収する。公開 IRenderer、C# API、保存データの形式は変えない。

GPU 回帰では1000個体の異なる行列を6フレームにわたり提出し、各フレーム1000 dispatch を確認する。同じ pose の2つ目の view は再 dispatch しない。先頭・末尾の GPU 頂点出力を交互に読み、各個体と各フレームの変形が一致することを数値で確認する。

参照: [Descriptor heaps](https://learn.microsoft.com/en-us/windows/win32/direct3d12/creating-descriptor-heaps)、[CopyDescriptorsSimple](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-copydescriptorssimple)。

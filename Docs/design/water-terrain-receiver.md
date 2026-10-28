<!-- @file    water-terrain-receiver.md -->
<!-- @brief   Fixed Terrain bottoms and receiver-only atlas boundary precision. -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-11 -->

# Terrain の底と水中受光

WaterMediumAtlas は最終 world XZ で Terrain の保存深度を読む。保存深度は波がない基準面から測るため、波による Y 変位を深度へ足す。`surfaceY - depth` は固定 Terrain の高さとなる。Spline の authored depth、Terrain 非対応の一様水深、未被覆 texel の fallback は別の契約として保持する。

Terrain の描画三角形、保存深度の UNORM16、view-local atlas は異なる解像度と補間を持つ。厳密な volume 判定を、そのまま底にある受光面の分類に使うと、微小な差で incoming RGB extinction がオン・オフになり、静止時にも大きな受光模様が出る。

受光面だけに `FBZZ_QueryWaterReceiverMedium[WithContext]` を使う。同じ owner の bilinear footprint 内の底高さの範囲、保存深度の半量子化幅、float丸め幅の和を、Terrain 境界の誤差範囲とする。最近傍 owner の必須判定と有効 neighbor の規則を維持する。Terrain の variable depth を持つ Rectangle/Horizon のみを対象にし、受光面の照会深度をこの範囲だけ許容する。照会結果に返す depth 自体は元の物理深度である。

水深上限を越える補正は行わない。ゼロ水深、上面、XZ coverage、priority、dry exclusion box は元の厳密な条件を使う。Spline の authored depth と一様水深は補正対象外。Composite・SSR・Caustics・Volumetric・物理・任意の source Beer trace は厳密な体積照会を保持する。入射光と環境光の RGB Beer、Emission、通常の shadow は保持する。

GPU table body row0.w の正値は従来の certified wet interior、負値は Terrain UNORM16 深度の半量子化幅である。signed depth range を `r` とすれば半幅は `r / 65535`。旧 table の w=0 は量子化補正なしで読み、既存の高速経路は負値を認定値として扱わない。table schema、resource slot、公開 C# API とアセット形式は変えない。

GPU 回帰は本番の atlas pass と PBR receiver の出力で検証する。平坦・傾斜した Terrain に水平・垂直の波変位を与え、2位相で底が固定されることを確認する。微小な底の誤差では受光が連続し、通常の volume 照会・dry box・水深上限・Spline・一様水深は拡張されないことを確認する。実シーンは OceanEcosystem の固定露出、上下カメラ、波 ON/OFF、Caustics 0、Sun shadow ON/OFF、Terrain ON/OFF、Sun 90°/50°/10°/0を保存せず比較する。

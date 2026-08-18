// FBZZ Engine
// InspectorMaterial.hpp | fbzz::editor
// Material Component の Inspector 描画
//
// WHY 何も公開しないか:
//   マテリアルの割り当てと .mat の中身の編集は、どちらも MaterialComponent の
//   セクション内で完結する。以前 Renderer 側から呼べるよう一部を公開していたが、
//   スロット配列の実体は MaterialComponent が持っているため、編集 UI を別
//   コンポーネントのセクションへ出すと「そちらの Undo トラッカーは違う
//   コンポーネントを見ている」という食い違いを回避コードで埋める必要が出る。
//   データの持ち主が編集 UI も持つ、という形に戻した。
#pragma once

#include "InspectorCommon.hpp"

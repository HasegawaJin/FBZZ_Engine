/// @file    InspectorMaterial.hpp
/// @brief   Material Component の Inspector 描画。
/// @author  Hasegawa Jin
/// @date    2026-06-07
///
/// @note 公開関数を増やさない。割り当てと .mat 編集は MaterialComponent のセクション内で完結させる。
///       スロット配列の実体は MaterialComponent にあり、別コンポーネントへ編集 UI を出すと
///       Undo トラッカーの食い違いを回避コードで埋める必要が出る。
#pragma once

#include "InspectorCommon.hpp"

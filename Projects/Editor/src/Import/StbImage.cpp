// FBZZ Engine
// StbImage.cpp | fbzz::editor
// Editor 内で共有する stb_image 実装の唯一の翻訳単位

// stb_image の宣言を利用する全パネル・インポーターへ外部シンボルを提供する。
// WHY: STB_IMAGE_STATIC を各利用側で定義すると、未定義の利用側から参照できず、
//      利用箇所ごとの実装生成はコンパイル時間とバイナリサイズも増加させるため。
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

# Third-Party Notices

FBZZ Engine 本体は MIT License ([LICENSE](LICENSE)) で配布する。
このファイルは、リポジトリに**ベンダーしている**サードパーティ製ソフトウェアと、その
ライセンス全文をまとめたもの。BSD-3-Clause と MIT はどちらもバイナリ配布時の告知同梱を条件にしているため、
スタンドアロンパッケージを配るときは `LICENSE` とこのファイルを必ず添えること。

各ライブラリのライセンス全文は `ThirdParty/<Name>/LICENSE` にも置いてある。ここはその索引と写し。

---

## 一覧

| ライブラリ | 版 | SPDX | 全文 |
|---|---|---|---|
| [Assimp](https://github.com/assimp/assimp) | 6.0.5 | `BSD-3-Clause` | [`ThirdParty/Assimp/LICENSE`](ThirdParty/Assimp/LICENSE) |
| [DirectXTex](https://github.com/microsoft/DirectXTex) | 2.0.2 (`DIRECTX_TEX_VERSION 202`) | `MIT` | [`ThirdParty/DirectXTex/LICENSE`](ThirdParty/DirectXTex/LICENSE) |
| [WinPixEventRuntime](https://www.nuget.org/packages/WinPixEventRuntime/1.0.240308001) | 1.0.240308001 | `MIT` | [`ThirdParty/WinPixEventRuntime/LICENSE`](ThirdParty/WinPixEventRuntime/LICENSE) |
| [DirectX 12 Agility SDK](https://www.nuget.org/packages/Microsoft.Direct3D.D3D12/1.619.6) | 1.619.6 | Microsoft binary / MIT code terms | [`ThirdParty/AgilitySDK/LICENSE`](ThirdParty/AgilitySDK/LICENSE) |
| [DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.9.2609) | 1.9.2609 | MIT / LLVM / Microsoft binary terms | [`ThirdParty/DXC/LICENSE`](ThirdParty/DXC/LICENSE) |
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.92.8 (docking) | `MIT` | [`ThirdParty/ImGui/LICENSE.txt`](ThirdParty/ImGui/LICENSE.txt) |
| [ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo) | — | `MIT` | [`ThirdParty/ImGuizmo/LICENSE`](ThirdParty/ImGuizmo/LICENSE) |
| [imnodes](https://github.com/Nelarius/imnodes) | — | `MIT` | [`ThirdParty/ImNodes/LICENSE`](ThirdParty/ImNodes/LICENSE) |
| [toml++](https://github.com/marzer/tomlplusplus) | 3.4.0 | `MIT` | [`ThirdParty/TomlPlusPlus/LICENSE`](ThirdParty/TomlPlusPlus/LICENSE) |
| [stb](https://github.com/nothings/stb) | image 2.30 / truetype 1.26 / rect_pack 1.01 | `MIT OR Unlicense` | [`ThirdParty/Stb/LICENSE`](ThirdParty/Stb/LICENSE) |
| [TinyEXR](https://github.com/syoyo/tinyexr) | — | `BSD-3-Clause` | [`ThirdParty/TinyExr/LICENSE`](ThirdParty/TinyExr/LICENSE) |
| ↳ [miniz](https://github.com/richgel999/miniz) (TinyEXR が同梱) | 3.0.0 | `MIT` | 同上 |
| [GoogleTest / GoogleMock](https://github.com/google/googletest) | 1.15.2 | `BSD-3-Clause` | [`ThirdParty/GoogleTest/LICENSE`](ThirdParty/GoogleTest/LICENSE) |

版が `—` のものは、上流にバージョン番号の付いたタグまたはヘッダー定数が無く、
特定の時点のソースを取り込んでいる。

### ベンダーしていない依存

次のものはリポジトリに含まれておらず、ビルド環境または OS から供給される。

| 名前 | 供給元 | 備考 |
|---|---|---|
| DirectX 11 / DirectX 12 | Windows SDK | ライセンスは Windows SDK の使用許諾に従う |
| XAudio2 | Windows SDK | 同上 |
| Microsoft::WRL (ComPtr) | Windows SDK | 同上 |
| FXC (`fxc.exe`) | Windows SDK | DX11 用シェーダーコンパイラー。ビルド時のみ使用 |
| Electron / React / Node.js 依存 (GameHub) | npm | `Projects/GameHub/package.json` と `node_modules` 配下の各ライセンスに従う |
| MCP SDK ほか (EditorMcp) | npm | `Projects/EditorMcp/package.json` と `node_modules` 配下の各ライセンスに従う |

### 外部アセット

| アセット | 出所 | ライセンス |
|---|---|---|
| 一部テレインテクスチャ | [ambientCG](https://ambientcg.com/) | CC0 1.0 (パブリックドメイン) |
| `Assets/Models/DebugCharacter/` のモデルとアニメーション | [Mixamo](https://www.mixamo.com/) | Adobe 無償ライセンス |

`GreenWare/` のキャラクター・ボス・武器・マップは Blender で自作したもので、本体と同じ MIT License の対象。

---

## Assimp — BSD-3-Clause

取り込み範囲: `include/` (ヘッダー) と `lib/` `dll/` (ビルド済みバイナリ)。上流のソースツリーとビルドスクリプトは取り込んでいない。

```
Open Asset Import Library (assimp)

Copyright (c) 2006-2026, assimp team
All rights reserved.

Redistribution and use of this software in source and binary forms,
with or without modification, are permitted provided that the
following conditions are met:

* Redistributions of source code must retain the above
  copyright notice, this list of conditions and the
  following disclaimer.

* Redistributions in binary form must reproduce the above
  copyright notice, this list of conditions and the
  following disclaimer in the documentation and/or other
  materials provided with the distribution.

* Neither the name of the assimp team, nor the names of its
  contributors may be used to endorse or promote products
  derived from this software without specific prior
  written permission of the assimp team.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

Assimp が同梱する Poly2Tri の告知を含む全文は [`ThirdParty/Assimp/LICENSE`](ThirdParty/Assimp/LICENSE) にある。

---

## DirectXTex — MIT

取り込み範囲: `include/` と `lib/x64/`。

```
Copyright (c) Microsoft Corporation.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## WinPixEventRuntime — MIT

Included files: five official SDK headers, the desktop x64 import library and
runtime DLL. Package/version/hash provenance is recorded in
[`ThirdParty/WinPixEventRuntime/VERSION`](ThirdParty/WinPixEventRuntime/VERSION).
The package's informational PIX tool-suite notices are also preserved verbatim in
[`ThirdParty/WinPixEventRuntime/ThirdPartyNotices.txt`](ThirdParty/WinPixEventRuntime/ThirdPartyNotices.txt).

```text
Copyright (c) Microsoft Corporation.

MIT License

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED *AS IS*, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Dear ImGui — MIT

取り込み範囲: コア 4 ファイルと `backends/` の Win32 / DX11 / DX12。docking ブランチ。

```
The MIT License (MIT)

Copyright (c) 2014-2026 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## ImGuizmo — MIT

```
The MIT License (MIT)

Copyright (c) 2016 Cedric Guillemet

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## imnodes — MIT

```
MIT License

Copyright (c) 2019 Johann Muszynski

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## toml++ — MIT

```
MIT License

Copyright (c) Mark Gillard <mark.gillard@outlook.com.au>

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

---

## stb — MIT または Public Domain (選択可)

取り込み範囲: `stb_image.h` (v2.30) / `stb_truetype.h` (v1.26) / `stb_rect_pack.h` (v1.01)。
3 ファイルとも末尾に同一の告知を持つ。

```
This software is available under 2 licenses -- choose whichever you prefer.
ALTERNATIVE A - MIT License
Copyright (c) 2017 Sean Barrett
Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
ALTERNATIVE B - Public Domain (www.unlicense.org)
This is free and unencumbered software released into the public domain.
Anyone is free to copy, modify, publish, use, compile, sell, or distribute this
software, either in source code form or as a compiled binary, for any purpose,
commercial or non-commercial, and by any means.
In jurisdictions that recognize copyright laws, the author or authors of this
software dedicate any and all copyright interest in the software to the public
domain. We make this dedication for the benefit of the public at large and to
the detriment of our heirs and successors. We intend this dedication to be an
overt act of relinquishment in perpetuity of all present and future rights to
this software under copyright law.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

---

## TinyEXR — BSD-3-Clause

TinyEXR 本体に加えて、OpenEXR 由来の部分と、同梱の miniz にそれぞれ別の告知が付く。
3 つの全文は [`ThirdParty/TinyExr/LICENSE`](ThirdParty/TinyExr/LICENSE) にある。ここには本体分を写す。

```
Copyright (c) 2014 - 2021, Syoyo Fujita and many contributors.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the Syoyo Fujita nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

---

## GoogleTest / GoogleMock — BSD-3-Clause

取り込み範囲: `googletest/{include,src}` と `googlemock/{include,src}`。上流の CMake / bazel / docs / CI は取り込んでいない (理由は [`ThirdParty/GoogleTest/VERSION`](ThirdParty/GoogleTest/VERSION))。テストのみで使用し、配布物には含まれない。

```
Copyright 2008, Google Inc.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

    * Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above
copyright notice, this list of conditions and the following disclaimer
in the documentation and/or other materials provided with the
distribution.
    * Neither the name of Google Inc. nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

---

## ライブラリを足すとき

`ThirdParty/` へベンダーするたびに、次の 3 つを揃える (規約は [`AGENTS.md`](AGENTS.md))。

1. `ThirdParty/<Name>/LICENSE` — 上流のライセンス全文をそのまま置く
2. `ThirdParty/<Name>/VERSION` — 取得元 URL・版・取得日・取り込み範囲・更新手順 ([`ThirdParty/GoogleTest/VERSION`](ThirdParty/GoogleTest/VERSION) が手本)
3. このファイルの一覧と全文へ 1 件追記する

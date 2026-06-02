#!/usr/bin/env python3
# FBZZ Engine
# Tools/FontAtlasGen/gen_font_atlas.py
# TTF フォントを RGBA PNG アトラスと .fnt メタデータファイルに変換する。
#
# 必要ライブラリ: pip install Pillow
#
# 使い方:
#   python gen_font_atlas.py <ttf_or_dir> [--size 48]
#   python gen_font_atlas.py Assets/Fonts/Kenney/Future.ttf --size 48
#   python gen_font_atlas.py Assets/Fonts/Kenney/ --size 48   # ディレクトリで一括変換
#
# 出力: <ttf_stem>.png と <ttf_stem>.fnt を TTF と同じディレクトリに生成する。
#
# FNT ファイル形式:
#   line_height <px>         アトラスレンダリング時の行高さ
#   base        <px>         上端からベースラインまでの距離 (= ascent)
#   cell_w      <px>         アトラス内の 1 グリフセル幅
#   glyph <code> <u0> <v0> <u1> <v1> <advance>
#     code    : ASCII コード (32〜126)
#     u0..v1  : 正規化 UV (0.0〜1.0)
#     advance : 次のグリフ原点までの水平距離 (アトラスピクセル単位)

import argparse
import math
import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    print("ERROR: Pillow が見つかりません。pip install Pillow を実行してください。")
    sys.exit(1)

# 対象 ASCII 範囲 (スペース 0x20 〜 チルダ 0x7E)
FIRST_CHAR = 0x20
LAST_CHAR  = 0x7E
CHARS = [chr(c) for c in range(FIRST_CHAR, LAST_CHAR + 1)]


def next_pow2(n: int) -> int:
    """n 以上の最小の 2 の冪を返す。"""
    p = 1
    while p < n:
        p <<= 1
    return p


def generate_atlas(ttf_path: str, size: int) -> tuple:
    """
    TTF を読み込み、PNG アトラス (PIL.Image) とメタデータ dict を返す。

    メタデータ:
        line_height : int   行高さ (px)
        base        : int   上端からベースラインまでの距離 (px)
        cell_w      : int   セル幅 (px)
        glyphs      : dict  { char: (u0, v0, u1, v1, advance) }
    """
    font = ImageFont.truetype(ttf_path, size)

    # フォントの基本メトリクス
    ascent, descent = font.getmetrics()
    line_height = ascent + descent   # セル高さ = 行高さ

    # 全グリフの最大 advance を測定してセル幅を決める。
    # WHY: セル幅を揃えることで UV 計算が単純になり、行全体を一定ピッチで並べられる。
    #      グリフの実ピクセルはセル内に収まる; 余白は透明なのでレンダリングに影響しない。
    max_advance = 1
    advances = {}
    for c in CHARS:
        adv = font.getlength(c)
        advances[c] = adv
        if adv > max_advance:
            max_advance = adv

    cell_w = int(math.ceil(max_advance)) + 2   # +2 px パディング
    cell_h = line_height + 2                   # +2 px パディング

    # 10 列固定でアトラスを並べる (95 文字 → 10 列 × 10 行)
    cols = 10
    rows = math.ceil(len(CHARS) / cols)

    atlas_w = next_pow2(cols * cell_w)
    atlas_h = next_pow2(rows * cell_h)

    # グレースケールアトラスを生成する。
    # WHY: RGBA で保存すると WIC (DirectXTex) が alpha チャンネルを失うことがある。
    #      グレースケール "L" モードにすることで、グリフの coverage が R チャンネルに入り
    #      UIText.hlsl が .r で読み取るパスと一致する。アンチエイリアス情報も保持される。
    atlas = Image.new("L", (atlas_w, atlas_h), 0)
    draw  = ImageDraw.Draw(atlas)

    glyphs = {}
    for idx, c in enumerate(CHARS):
        col = idx % cols
        row = idx // cols
        ox  = col * cell_w + 1   # 1 px パディング
        oy  = row * cell_h + 1   # 1 px パディング

        # ベースラインを oy + ascent に合わせて描画する (anchor="ls" = left, baseline)
        draw.text((ox, oy + ascent), c, font=font, fill=255, anchor="ls")

        # セル全体を UV 範囲とする。透明部分はシェーダーで破棄される。
        u0 = ox / atlas_w
        v0 = oy / atlas_h
        u1 = (ox + cell_w - 2) / atlas_w
        v1 = (oy + cell_h - 2) / atlas_h

        glyphs[c] = (u0, v0, u1, v1, advances[c])

    meta = {
        "line_height": line_height,
        "base":        ascent,
        "cell_w":      cell_w - 2,
        "glyphs":      glyphs,
    }
    return atlas, meta


def write_fnt(fnt_path: str, meta: dict):
    """メタデータを .fnt テキストファイルに書き出す。"""
    with open(fnt_path, "w", encoding="utf-8") as f:
        f.write(f"line_height {meta['line_height']}\n")
        f.write(f"base {meta['base']}\n")
        f.write(f"cell_w {meta['cell_w']}\n")
        for c, (u0, v0, u1, v1, adv) in meta["glyphs"].items():
            code = ord(c)
            f.write(f"glyph {code} {u0:.8f} {v0:.8f} {u1:.8f} {v1:.8f} {adv:.4f}\n")


def process_one(ttf_path: Path, size: int):
    """1 つの TTF を処理する。"""
    out_dir  = ttf_path.parent
    png_path = out_dir / f"{ttf_path.stem}.png"
    fnt_path = out_dir / f"{ttf_path.stem}.fnt"

    print(f"  {ttf_path.name}  (size={size}px) ...", end="", flush=True)
    atlas, meta = generate_atlas(str(ttf_path), size)
    atlas.save(str(png_path))
    write_fnt(str(fnt_path), meta)
    print(f" {atlas.width}x{atlas.height}  {len(meta['glyphs'])} glyphs -> {png_path.name}")


def main():
    parser = argparse.ArgumentParser(description="FBZZ Font Atlas Generator")
    parser.add_argument("path",   help="TTF ファイルまたはディレクトリのパス")
    parser.add_argument("--size", type=int, default=48, help="レンダリングサイズ (px) [default: 48]")
    args = parser.parse_args()

    target = Path(args.path)

    if target.is_dir():
        # Windows はファイルシステムが大文字小文字を区別しないため set で重複排除する
        seen = set()
        ttfs = []
        for p in sorted(target.glob("*.ttf")) + sorted(target.glob("*.TTF")):
            if p.name.lower() not in seen:
                seen.add(p.name.lower())
                ttfs.append(p)
        if not ttfs:
            print(f"ERROR: {target} に TTF ファイルが見つかりません。")
            sys.exit(1)
        print(f"Generating font atlases in {target}/ ...")
        for ttf in ttfs:
            process_one(ttf, args.size)
    elif target.is_file():
        process_one(target, args.size)
    else:
        print(f"ERROR: {target} が見つかりません。")
        sys.exit(1)


if __name__ == "__main__":
    main()

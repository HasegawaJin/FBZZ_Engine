"""FBZZ Engine — Doxygen XML から AI 向けの圧縮 API リファレンス (Markdown) を作る。

使い方 (リポジトリ直下で):
    doxygen Docs/Doxyfile                    # build/docs/xml/ を作る
    python Projects/DevTools/ApiReference/ApiReference.py             # build/docs/api/ へ書く
    python Projects/DevTools/ApiReference/ApiReference.py --xml build/docs/xml --out build/docs/api

出力:
    build/docs/api/index.md                  ヘッダー一覧 (モジュール別・@brief・型名)
    build/docs/api/<Module>/<Header>.md      ヘッダーごと。型・メンバー・マクロの宣言と契約 (@brief/@param/@return/@pre/@note/@warning/@see) だけ

方針: AI エージェントがヘッダーを開く代わりに読むための「契約の一覧」。処理の説明・include 図・継承図は載せない。
規約: Docs/conventions/comments.md
"""
from __future__ import annotations

import argparse
import os
import re
import sys
import xml.etree.ElementTree as ET
from collections import defaultdict
from dataclasses import dataclass, field

SKIP_SIMPLESECT = {"author", "date", "copyright", "version", "since"}
SIMPLESECT_LABEL = {
    "return": "return", "pre": "pre", "post": "post", "note": "note",
    "warning": "warning", "see": "see", "attention": "warning", "remark": "note", "par": "note",
}


# ---------------------------------------------------------------------------
# XML → テキスト
# ---------------------------------------------------------------------------

def inline_text(node: ET.Element | None) -> str:
    """段落内の要素を 1 行のテキストへ畳む。<computeroutput> はバッククォート、<ref> は名前だけ。"""
    if node is None:
        return ""
    parts: list[str] = []

    def walk(el: ET.Element) -> None:
        tag = el.tag
        if tag in ("parameterlist", "simplesect", "xrefsect"):
            return
        if tag == "computeroutput":
            parts.append("`" + "".join(el.itertext()).strip() + "`")
            if el.tail:
                parts.append(el.tail)
            return
        if tag == "sp":
            parts.append(" ")
        elif tag == "linebreak":
            parts.append(" ")
        elif tag == "itemizedlist" or tag == "orderedlist":
            items = [inline_text(li) for li in el.findall("listitem")]
            parts.append(" / ".join(i for i in items if i))
            if el.tail:
                parts.append(el.tail)
            return
        elif tag == "programlisting":
            code = " ".join("".join(cl.itertext()).strip() for cl in el.findall("codeline"))
            parts.append("`" + code + "`")
            if el.tail:
                parts.append(el.tail)
            return
        if el.text:
            parts.append(el.text)
        for child in el:
            walk(child)
        if el.tail:
            parts.append(el.tail)

    if node.text:
        parts.append(node.text)
    for child in node:
        walk(child)
    text = "".join(parts)
    return re.sub(r"\s+", " ", text).strip()


@dataclass
class Description:
    brief: str = ""
    detail: list[str] = field(default_factory=list)
    params: list[tuple[str, str]] = field(default_factory=list)
    tparams: list[tuple[str, str]] = field(default_factory=list)
    sects: list[tuple[str, str]] = field(default_factory=list)

    def has_docs(self) -> bool:
        return bool(self.brief or self.detail or self.params or self.sects)


def parse_description(member: ET.Element) -> Description:
    d = Description()
    d.brief = inline_text(member.find("briefdescription"))
    detailed = member.find("detaileddescription")
    if detailed is None:
        return d
    for para in detailed.findall("para"):
        text = inline_text(para)
        if text:
            d.detail.append(text)
        for plist in para.findall("parameterlist"):
            target = d.tparams if plist.get("kind") == "templateparam" else d.params
            for item in plist.findall("parameteritem"):
                names = [inline_text(n) for n in item.findall("parameternamelist/parametername")]
                desc = " ".join(inline_text(p) for p in item.findall("parameterdescription/para"))
                target.append((", ".join(names), desc))
        for sect in para.findall("simplesect"):
            kind = sect.get("kind", "")
            if kind in SKIP_SIMPLESECT:
                continue
            label = SIMPLESECT_LABEL.get(kind, kind)
            body = " ".join(inline_text(p) for p in sect.findall("para"))
            if kind == "par":
                title = inline_text(sect.find("title"))
                body = f"{title}: {body}" if title else body
            if body:
                d.sects.append((label, body))
    # detaileddescription の para は簡潔説明の重複を含みうる
    d.detail = [t for t in d.detail if t and t != d.brief]
    return d


# ---------------------------------------------------------------------------
# 宣言の 1 行化
# ---------------------------------------------------------------------------

def template_prefix(node: ET.Element) -> str:
    tpl = node.find("templateparamlist")
    if tpl is None:
        return ""
    params = []
    for p in tpl.findall("param"):
        t = inline_text(p.find("type"))
        n = inline_text(p.find("declname")) or inline_text(p.find("defname"))
        dv = inline_text(p.find("defval"))
        s = f"{t} {n}".strip() if n else t
        if dv:
            s += f" = {dv}"
        params.append(s)
    return f"template<{', '.join(params)}> "


def member_signature(m: ET.Element) -> str:
    kind = m.get("kind")
    name = inline_text(m.find("name"))
    typ = inline_text(m.find("type"))
    args = inline_text(m.find("argsstring"))
    init = inline_text(m.find("initializer"))
    prefix = template_prefix(m)
    if m.get("static") == "yes":
        prefix += "static "
    if m.get("constexpr") == "yes" and "constexpr" not in typ:
        prefix += "constexpr "
    virt = m.get("virt", "non-virtual")
    if virt != "non-virtual" and "virtual" not in typ:
        prefix += "virtual "
    if kind == "function":
        sig = f"{prefix}{typ} {name}{args}".strip()
        if virt == "pure-virtual" and "= 0" not in sig:
            sig += " = 0"
        return re.sub(r"\s+", " ", sig)
    if kind == "variable":
        sig = f"{prefix}{typ} {name}{args}".strip()
        if init:
            sig += f" {init}" if init.startswith("=") else f" = {init}"
        return re.sub(r"\s+", " ", sig)
    if kind == "typedef":
        definition = inline_text(m.find("definition"))
        if definition.startswith("using") or definition.startswith("typedef"):
            return re.sub(r"\s+", " ", prefix + definition)
        return f"{prefix}using {name} = {typ}".strip()
    if kind == "enum":
        strong = " class" if m.get("strong") == "yes" else ""
        under = f" : {typ}" if typ else ""
        return f"enum{strong} {name}{under}"
    if kind == "define":
        params = [inline_text(p.find("defname")) for p in m.findall("param")]
        return f"#define {name}({', '.join(params)})" if m.findall("param") else f"#define {name}"
    if kind == "friend":
        return ""
    return f"{typ} {name}{args}".strip()


def format_docs(d: Description, indent: str) -> list[str]:
    lines: list[str] = []
    for text in d.detail:
        lines.append(f"{indent}{text}")
    for name, desc in d.tparams:
        lines.append(f"{indent}tparam `{name}`: {desc}")
    for name, desc in d.params:
        lines.append(f"{indent}param `{name}`: {desc}")
    for label, body in d.sects:
        lines.append(f"{indent}{label}: {body}")
    return lines


def member_lines(m: ET.Element, include_private: bool) -> list[str]:
    prot = m.get("prot", "public")
    if prot == "private" and not include_private:
        return []
    kind = m.get("kind")
    if kind == "friend":
        return []
    sig = member_signature(m)
    if not sig:
        return []
    d = parse_description(m)
    head = f"- `{sig}`"
    if prot == "protected":
        head += " (protected)"
    if d.brief:
        head += f" — {d.brief}"
    lines = [head]
    lines += format_docs(d, "  - ")
    if kind == "enum":
        for ev in m.findall("enumvalue"):
            if ev.get("prot") == "private" and not include_private:
                continue
            name = inline_text(ev.find("name"))
            init = inline_text(ev.find("initializer"))
            brief = inline_text(ev.find("briefdescription")) or " ".join(
                inline_text(p) for p in ev.findall("detaileddescription/para"))
            item = f"  - `{name}{(' ' + init) if init else ''}`"
            if brief:
                item += f" — {brief}"
            lines.append(item)
    return lines


def location_line(node: ET.Element) -> int:
    loc = node.find("location")
    if loc is None:
        return 0
    try:
        return int(loc.get("line", "0"))
    except ValueError:
        return 0


# ---------------------------------------------------------------------------
# コンパウンド (ファイル / クラス)
# ---------------------------------------------------------------------------

def sectiondef_members(compound: ET.Element, include_private: bool) -> list[str]:
    """sectiondef を宣言行順に並べて出す。@name グループ (user-defined) は見出しを付ける。"""
    lines: list[str] = []
    sections = list(compound.findall("sectiondef"))
    plain: list[tuple[int, ET.Element]] = []
    grouped: list[tuple[str, list[ET.Element]]] = []
    for sec in sections:
        kind = sec.get("kind", "")
        if kind == "user-defined":
            header = inline_text(sec.find("header"))
            grouped.append((header, list(sec.findall("memberdef"))))
            continue
        if kind.startswith("private") and not include_private:
            continue
        for m in sec.findall("memberdef"):
            plain.append((location_line(m), m))
    plain.sort(key=lambda t: t[0])
    for _, m in plain:
        lines += member_lines(m, include_private)
    for header, members in grouped:
        block: list[str] = []
        for m in sorted(members, key=location_line):
            block += member_lines(m, include_private)
        if block:
            lines.append(f"- **{header}**" if header else "- **(group)**")
            lines += ["  " + l if l.startswith("-") else l for l in block]
    return lines


def compound_block(compound: ET.Element, include_private: bool) -> list[str]:
    kind = compound.get("kind")
    name = inline_text(compound.find("compoundname"))
    d = parse_description(compound)
    tpl = template_prefix(compound).strip()
    title = f"{tpl} {kind} {name}".strip() if tpl else f"{kind} {name}"
    line = location_line(compound)
    lines = [f"## {title}" + (f"  <sub>L{line}</sub>" if line else "")]
    bases = [inline_text(b) for b in compound.findall("basecompoundref") if b.get("prot") != "private"]
    if bases:
        lines.append(f"derives: {', '.join(bases)}")
    if d.brief:
        lines.append(d.brief)
    lines += format_docs(d, "")
    members = sectiondef_members(compound, include_private)
    if members:
        lines.append("")
        lines += members
    return lines


def load_compound(xml_dir: str, refid: str) -> ET.Element | None:
    path = os.path.join(xml_dir, refid + ".xml")
    if not os.path.exists(path):
        return None
    root = ET.parse(path).getroot()
    return root.find("compounddef")


def render_file(xml_dir: str, file_compound: ET.Element, include_private: bool) -> tuple[str, str, list[str], str]:
    """@return (相対パス, 本文 Markdown, 型名一覧, @brief)"""
    rel = file_compound.find("location").get("file") if file_compound.find("location") is not None else inline_text(file_compound.find("compoundname"))
    d = parse_description(file_compound)
    out = [f"# {rel}"]
    if d.brief:
        out.append(d.brief)
    out += format_docs(d, "")
    type_names: list[str] = []

    inner: list[tuple[int, list[str]]] = []
    for ic in file_compound.findall("innerclass"):
        if ic.get("prot") == "private" and not include_private:
            continue
        comp = load_compound(xml_dir, ic.get("refid", ""))
        if comp is None:
            continue
        cname = inline_text(comp.find("compoundname"))
        if "::detail::" in cname or cname.endswith("::detail"):
            continue
        type_names.append(cname.split("::")[-1])
        inner.append((location_line(comp), compound_block(comp, include_private)))
    inner.sort(key=lambda t: t[0])

    free = sectiondef_members(file_compound, include_private)
    if free:
        out += ["", "## file scope"] + free
    for _, block in inner:
        out += [""] + block
    return rel, "\n".join(out).rstrip() + "\n", type_names, d.brief


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--xml", default="build/docs/xml", help="Doxygen の XML 出力 (既定 build/docs/xml)")
    ap.add_argument("--out", default="build/docs/api", help="Markdown の出力先 (既定 build/docs/api)")
    ap.add_argument("--include-private", action="store_true", help="private メンバーも載せる")
    args = ap.parse_args(argv)

    index_path = os.path.join(args.xml, "index.xml")
    if not os.path.exists(index_path):
        print(f"ERROR {index_path} が無い。先に `doxygen Docs/Doxyfile` を実行する", file=sys.stderr)
        return 2
    index = ET.parse(index_path).getroot()
    file_refids = [c.get("refid") for c in index.findall("compound") if c.get("kind") == "file"]

    os.makedirs(args.out, exist_ok=True)
    by_module: dict[str, list[tuple[str, str, list[str]]]] = defaultdict(list)
    written = 0
    for refid in file_refids:
        comp = load_compound(args.xml, refid)
        if comp is None:
            continue
        rel, body, type_names, brief = render_file(args.xml, comp, args.include_private)
        if not rel.endswith(".hpp"):
            continue
        md_rel = rel[:-4] + ".md"
        dest = os.path.join(args.out, md_rel)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "w", encoding="utf-8", newline="\n") as f:
            f.write(body)
        written += 1
        module = rel.split("/")[0]
        by_module[module].append((rel, brief, type_names))

    lines = ["# FBZZ Engine — API リファレンス索引",
             "",
             "生成: `doxygen Docs/Doxyfile` → `python Projects/DevTools/ApiReference/ApiReference.py`。各ヘッダーの契約は同名の `.md` にある (例: `Engine/Scene/GameObject.md`)。",
             "規約: `Docs/conventions/comments.md`",
             ""]
    for module in sorted(by_module):
        lines.append(f"## {module}")
        for rel, brief, type_names in sorted(by_module[module]):
            md_rel = rel[:-4] + ".md"
            entry = f"- [{rel}]({md_rel})"
            if brief:
                entry += f" — {brief}"
            if type_names:
                entry += f" ({', '.join(type_names)})"
            lines.append(entry)
        lines.append("")
    with open(os.path.join(args.out, "index.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    print(f"API reference: {written} headers -> {args.out} (index.md + per-header .md)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

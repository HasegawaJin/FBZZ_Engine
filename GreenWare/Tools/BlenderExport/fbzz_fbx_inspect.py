# FBZZ Engine
# fbzz_fbx_inspect.py | 依存なしの FBX バイナリ検査ツール
#
# WHY: エクスポート結果が FBZZ の FbxImportTool / AnimSubExporter の期待どおりか、
#      Editor を起動せずに検証したい。Assimp を用意せずに済むよう FBX 7.x の
#      バイナリノード構造だけを最小実装で読む。
#
# 検査項目:
#   - AnimationStack (テイク) 名 … `<FBX名>@<テイク名>.anim` のテイク名になる
#   - Model ノードの名前と種別 … LimbNode 数 = ボーン数 (MAX_SKINNING_BONES=128 未満か)
#   - Root_Motion ノードの有無と AnimationCurveNode 接続 … Root Motion 認識の前提
#
# 使い方: python fbzz_fbx_inspect.py <file.fbx> [...]

import json
import struct
import sys
import zlib

_SCALAR = {"Y": ("h", 2), "C": ("?", 1), "I": ("i", 4),
           "F": ("f", 4), "D": ("d", 8), "L": ("q", 8)}
_ARRAY_ITEM = {"f": ("f", 4), "d": ("d", 8), "l": ("q", 8), "i": ("i", 4), "b": ("?", 1)}


class Node:
    __slots__ = ("name", "props", "children")

    def __init__(self, name, props, children):
        self.name = name
        self.props = props
        self.children = children


def _read_property(data, offset):
    kind = chr(data[offset])
    offset += 1
    if kind in _SCALAR:
        fmt, size = _SCALAR[kind]
        value = struct.unpack_from("<" + fmt, data, offset)[0]
        return value, offset + size
    if kind in _ARRAY_ITEM:
        length, encoding, compressed_len = struct.unpack_from("<III", data, offset)
        offset += 12
        payload = data[offset:offset + compressed_len]
        offset += compressed_len
        if encoding == 1:
            payload = zlib.decompress(payload)
        fmt, size = _ARRAY_ITEM[kind]
        return list(struct.unpack_from("<" + str(length) + fmt, payload, 0)), offset
    if kind in ("S", "R"):
        length = struct.unpack_from("<I", data, offset)[0]
        offset += 4
        raw = data[offset:offset + length]
        offset += length
        return (raw.decode("utf-8", "replace") if kind == "S" else raw), offset
    raise ValueError(f"unknown property type {kind!r} at {offset}")


def _read_node(data, offset, wide):
    # 7500 以降はオフセット類が 64bit
    if wide:
        end_offset, num_props, _prop_len = struct.unpack_from("<QQQ", data, offset)
        offset += 24
    else:
        end_offset, num_props, _prop_len = struct.unpack_from("<III", data, offset)
        offset += 12
    name_len = data[offset]
    offset += 1
    name = data[offset:offset + name_len].decode("utf-8", "replace")
    offset += name_len

    if end_offset == 0:
        return None, offset  # null レコード = 兄弟リストの終端

    props = []
    for _ in range(num_props):
        value, offset = _read_property(data, offset)
        props.append(value)

    children = []
    while offset < end_offset:
        child, offset = _read_node(data, offset, wide)
        if child is None:
            break
        children.append(child)
    return Node(name, props, children), end_offset


def parse(path):
    with open(path, "rb") as handle:
        data = handle.read()
    if not data.startswith(b"Kaydara FBX Binary"):
        raise ValueError("not a binary FBX")
    version = struct.unpack_from("<I", data, 23)[0]
    wide = version >= 7500
    offset = 27
    roots = []
    while offset < len(data) - 16:
        node, offset = _read_node(data, offset, wide)
        if node is None:
            break
        roots.append(node)
    return version, roots


def inspect(path):
    version, roots = parse(path)
    objects = next((n for n in roots if n.name == "Objects"), None)
    connections = next((n for n in roots if n.name == "Connections"), None)

    takes, models, curve_nodes = [], {}, {}
    if objects:
        for node in objects.children:
            if node.name == "AnimationStack":
                takes.append(str(node.props[1]).split("\x00")[0])
            elif node.name == "Model":
                name = str(node.props[1]).split("\x00")[0]
                models[node.props[0]] = (name, str(node.props[2]))
            elif node.name == "AnimationCurveNode":
                curve_nodes[node.props[0]] = str(node.props[1]).split("\x00")[0]

    # AnimationCurveNode → Model の接続を数え、どのノードが動いているか調べる
    animated_models = {}
    if connections:
        for conn in connections.children:
            if len(conn.props) < 3:
                continue
            source, destination = conn.props[1], conn.props[2]
            if source in curve_nodes and destination in models:
                animated_models.setdefault(models[destination][0], []).append(
                    curve_nodes[source])

    kinds = {}
    for name, kind in models.values():
        kinds[kind] = kinds.get(kind, 0) + 1

    return {
        "file": path.replace("\\", "/").rsplit("/", 1)[-1],
        "fbx_version": version,
        "takes": takes,
        "model_kinds": kinds,
        "bone_count": kinds.get("LimbNode", 0),
        "has_root_motion_node": "Root_Motion" in [m[0] for m in models.values()],
        "root_motion_channels": sorted(animated_models.get("Root_Motion", [])),
        "animated_node_count": len(animated_models),
    }


if __name__ == "__main__":
    for target in sys.argv[1:]:
        print(json.dumps(inspect(target), ensure_ascii=False))


# serpent_map_build.py --- 蛇ボス専用のアリーナを組む
#
# WHY モデルで作るか (シーンで箱を並べないか):
#   ボス 1 の場が Assets/Models/Map/Boss_Arena_Map.fbx として «モデル» で入って
#   いる。命名も ARENA_* (見た目) / COL_* (当たり専用) / SPAWN_* (位置の目印) /
#   M_AR_* (材質) で揃っていて、シーン側はそれを置くだけ。同じ形にしておけば
#   取り込みも当たりの付け方もボス 1 の手順がそのまま使える。
#
# WHY 段で作るか (壁を 1 枚の筒にしないか):
#   ボス 1 の壁は 0〜10 / 10〜16 / 16〜26 の 3 段で、段ごとに «板 → 肋 → 光る帯 →
#   警告灯 → 危険帯 → 段の笠木» が繰り返される。1 枚の筒にすると高さの手がかりが
#   消えて、26 m の壁が «近くの低い塀» に見える。蛇は鎌首をもたげる分だけ高さが
#   読めないと困るので、こちらも段で作る。
#
# WHY 面を増やさないか:
#   ボス 1 のアリーナは床 10 面・壁 38 面・肋 150 面で、全部合わせても 1800 面ほど。
#   «磨く» を分割数を上げることだと読むと、語彙が同じでも別の場に見える。
#   円は 48 角形 (床) と 24 角形 (壁) までに留めて、増やすのはパーツの «種類» の方。
#
# WHY 穴ごとにオブジェクトを割るか:
#   予兆が «穴の縁が光る» で、しかも 14 口のうちどれが開くかが手そのものになる。
#   1 メッシュに畳むと «どの口か» を名指しできない。蛇の節を材質ごとに割ったのと
#   同じ理由 ─ 光らせる単位・動かす単位でオブジェクトを切る。
import bpy, bmesh, math, os, random
from mathutils import Matrix, Vector

# --- 寸法 ---------------------------------------------------------------------
# 出どころは Assets/Docs/boss-serpent.md の「アリーナと開口」。
# ここを直したら doc と serpent_arena.py も直すこと
# (あちらは «この寸法で胴が渡れるか» を検算する側)。
ARENA_R = 22.0     # 床の実効半径 (壁の内側)
RINGS   = [(6, 8.0, 0.0), (10, 16.0, math.pi / 10.0)]   # (口数, 半径, 位相)

# WHY この 3 つの半径か:
#   胴が渡れる «穴と穴の間隔» には上下の窓がある。狭いと折れ角が上限 (12度/関節)
#   を超え、広いと弧が伸びきって胴が床スラブの中に埋まる ─ 実測で **8.0 〜 11.5 m**。
#   場を広げるとき輪の半径を素直に比例させると外輪の隣どうしが窓を突き抜けるので、
#   半径ではなく «口数» で間隔を調整する (外輪 8 口 → 10 口)。
#   内→内 8.00 m / 内→外 8.75 m / 外→外 9.89 m。3 通りとも窓の中。
HOLE_R  = 2.2      # 開口の半径。胴が要るのは 1.3 m なので余裕がある
FLOOR_T = 0.60     # 床スラブの厚み (ボス 1 と同じ)
PIT_D   = 7.0      # 床下の深さ。潜った胴が «消える» 所

# 壁の段。ボス 1 は 80 m 幅に対して 26 m (比 0.33)。こちらは 36 m 幅で 15.6 m
# (比 0.43)。ボス 1 より «縦長» なのは鎌首をもたげる相手だから ─ ただし比を
# 0.5 まで上げると «井戸» に見えて、床の 14 口を見渡す場でなくなる。
TIER1   = 7.0
TIER2   = 12.5
TRUSS_Z = 13.8
ROOF_Z  = 15.6

# 別々の部品どうしが «同じ面» を共有しないための最小の隔たり。
#
# WHY 要るか:
#   同じ位置に同じ向きの面が 2 枚あると、どちらが手前かを深度バッファが
#   決められずにフレームごとに入れ替わる (Z ファイティング)。開口まわりは
#   縁・筐体・カラー・羽が同心で重なるので、半径も高さも «段» で切って
#   必ずこの隙間を空ける。
#
#   実測で 1050 面ペアが隔たり 0.0000 m で重なっていた。内訳はすべて開口:
#     カラー x 筐体 288 / カラー x 羽 498 / 縁 x 筐体 260 / 床 x 縁 4
GAP = 0.02

# 開口まわりの半径の段。内側から: 羽 → カラー → 筐体の座 → 光る縁。
# どの段も隣と GAP 以上あける。床の穴の壁 (r = HOLE_R) とも重ねない。
R_COLLAR = (2.23, 2.43)    # 回る輪
R_SEAT   = (2.45, 2.72)    # 筐体の座 (動かない)
R_RIM    = (2.76, 2.98)    # 光る縁 (動かない)

SEG      = 48      # 床の円周分割
SEG_WALL = 24      # 壁の円周分割 (24 角形。ボス 1 の八角形と同じ «平板» の語彙)
SEG_HOLE = 16      # 開口の分割

BUTTRESS = 8       # 通し柱の本数


def holes():
    """[(id, Vector)] 開口の中心。id は I1..I6 / O1..O8。"""
    out = []
    for ring, (count, radius, phase) in enumerate(RINGS):
        tag = "I" if ring == 0 else "O"
        for i in range(count):
            a = phase + 2.0 * math.pi * i / count
            out.append(("%s%d" % (tag, i + 1),
                        Vector((radius * math.cos(a), radius * math.sin(a), 0.0))))
    return out


# --- 下ごしらえ ---------------------------------------------------------------
TEX_DIR = (r"C:\Users\jinhs\Downloads\FBZZ_Engine\GreenWare"
           r"\Assets\Models\Map\Textures")
UV_TILE = 4.0      # 1 タイルが覆うワールドの寸法 [m]

# 材質 → (テクスチャの接頭辞, 持っているチャンネル, 自発光の色と強さ)
MAT_TABLE = {
    "M_AR_Deck":      ("T_AR_Deck",      "amrn", None),
    "M_AR_WallPanel": ("T_AR_WallPanel", "amrn", None),
    "M_AR_Steel":     ("T_AR_Steel",     "amrn", None),
    "M_AR_Hazard":    ("T_AR_Hazard",    "a",    None),
    "M_AR_StripGlow": ("T_AR_StripGlow", "a",    ((1.00, 0.72, 0.30), 3.0)),
    # 開口の縁。絵は壁の帯と同じだが材質を分ける。
    #
    # WHY 分けるか: 予兆は «その口だけ» を灯す。壁の帯と同じ材質にすると、
    #     材質のパラメータを触った瞬間にアリーナ中の帯が一緒に光って、
    #     どの口が開くのか読めなくなる ─ 予兆が予兆でなくなる。
    #     エンジン側は口ごとに材質インスタンスを持つこと。
    "M_AR_RimGlow":   ("T_AR_StripGlow", "a",    ((1.00, 0.72, 0.30), 3.0)),
    "M_AR_WarnLight": ("T_AR_WarnLight", "a",    ((1.00, 0.40, 0.16), 2.4)),
    "M_AR_CoreRing":  ("T_AR_CoreRing",  "a",    ((0.42, 0.86, 1.00), 1.6)),
}


def box_uv(me, tile=UV_TILE):
    """ワールド座標の箱投影で UV を張る。

    WHY 展開しないか:
        この場は «平らな板と輪» だけで、継ぎ目や段差の意匠はモデル側で作って
        ある。テクスチャは «材質の肌» を敷くだけなので、面の向きで投影して
        同じ縮尺で並べば足りる。部品ごとに展開すると縮尺がばらけて、床と壁で
        鋼の目の細かさが変わる ─ 同じ施設に見えなくなる。

    WHY ローカルではなくワールドか:
        書き出しの時点で全パーツが原点 (position 0,0,0) にメッシュのワールド
        座標を持っているので、ローカル = ワールド。隣り合う部品どうしで
        タイルの位相が揃う。
    """
    uv = me.uv_layers.new(name="UVMap")
    for poly in me.polygons:
        n = poly.normal
        ax = max(range(3), key=lambda i: abs(n[i]))
        for li in poly.loop_indices:
            co = me.vertices[me.loops[li].vertex_index].co
            if ax == 2:
                u, v = co.x, co.y
            elif ax == 0:
                u, v = co.y, co.z
            else:
                u, v = co.x, co.z
            uv.data[li].uv = (u / tile, v / tile)


def _image(fname, non_color=False):
    img = bpy.data.images.get(fname)
    if img is None:
        path = os.path.join(TEX_DIR, fname)
        if not os.path.exists(path):
            return None
        img = bpy.data.images.load(path, check_existing=True)
        img.name = fname
    img.colorspace_settings.name = "Non-Color" if non_color else "sRGB"
    return img


def material(name):
    """材質を «テクスチャ付きで» 用意する。既にあれば繋ぎ直す。

    WHY Deck / WallPanel / Hazard を作り直さないか:
        ボス 1 の場が既に同名のタイルを持っていて、同じ施設の別室という設定。
        作り直すと «同じ床のはずなのに別の床» になる。新しく起こしたのは
        Steel 一式と発光系 3 つ ─ アリーナで一番面積を持つのに、ボス 1 でも
        テクスチャが無かったもの。
    """
    m = bpy.data.materials.get(name)
    if m is None:
        m = bpy.data.materials.new(name)
    m.use_nodes = True
    spec = MAT_TABLE.get(name)
    if spec is None:
        return m
    prefix, channels, glow = spec
    nt = m.node_tree
    for nd in list(nt.nodes):
        if nd.type in {"TEX_IMAGE", "NORMAL_MAP"}:
            nt.nodes.remove(nd)
    bsdf = next((nd for nd in nt.nodes if nd.type == "BSDF_PRINCIPLED"), None)
    if bsdf is None:
        bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
        out = next((nd for nd in nt.nodes if nd.type == "OUTPUT_MATERIAL"), None)
        if out:
            nt.links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])

    def place(fname, y, non_color):
        img = _image(fname, non_color)
        if img is None:
            return None
        nd = nt.nodes.new("ShaderNodeTexImage")
        nd.image = img
        nd.location = (-560, y)
        return nd

    if "a" in channels:
        nd = place(prefix + "_Albedo.png", 300, False)
        if nd:
            nt.links.new(nd.outputs["Color"], bsdf.inputs["Base Color"])
            if glow:
                # 自発光も同じ絵で。灯る所と灯らない所が一致する
                nt.links.new(nd.outputs["Color"], bsdf.inputs["Emission Color"])
    if "m" in channels:
        nd = place(prefix + "_Metallic.png", 40, True)
        if nd:
            nt.links.new(nd.outputs["Color"], bsdf.inputs["Metallic"])
    if "r" in channels:
        nd = place(prefix + "_Roughness.png", -220, True)
        if nd:
            nt.links.new(nd.outputs["Color"], bsdf.inputs["Roughness"])
    if "n" in channels:
        nd = place(prefix + "_Normal.png", -480, True)
        if nd:
            nm = nt.nodes.new("ShaderNodeNormalMap")
            nm.location = (-280, -480)
            nt.links.new(nd.outputs["Color"], nm.inputs["Color"])
            nt.links.new(nm.outputs["Normal"], bsdf.inputs["Normal"])
    if glow:
        bsdf.inputs["Emission Strength"].default_value = glow[1]
        bsdf.inputs["Metallic"].default_value = 0.0
        bsdf.inputs["Roughness"].default_value = 0.35
    return m


def _free_name(name):
    """その名前を «誰も使っていない» 状態にする。

    WHY 要るか:
        Blender は名前が衝突すると黙って .001 を足す。組み直しのたびに古い
        メッシュデータが残っていると、新しい方が ARENA_Steel.003 になる。
        書き出し側はオブジェクト名ではなく **メッシュ名** を FBX の Geometry
        名として書き、取り込み側の selected_meshes もそれを読む ─ 結果、
        エンジンには ARENA_Rim_I1.003 という名前で入り、«口ごとに名指しする»
        という約束が丸ごと壊れる (実際に 92 個全部が .003 になっていた)。
    """
    for coll in (bpy.data.objects, bpy.data.meshes):
        while True:
            it = coll.get(name)
            if it is None:
                break
            if coll is bpy.data.objects:
                bpy.data.objects.remove(it, do_unlink=True)
            else:
                if it.users:
                    it.name = name + "__stale"
                    break
                bpy.data.meshes.remove(it)
    # 連番付きの残骸も片づける (ARENA_Steel.001 など)
    for me in list(bpy.data.meshes):
        if me.users == 0 and me.name.split(".")[0] == name:
            bpy.data.meshes.remove(me)


def _put(name, matname, bm):
    _free_name(name)
    # 5 角以上だけ三角化する。
    #
    # WHY 要るか: 法線マップを効かせるにはタンジェントが要り、書き出し側は
    #     n-gon のタンジェントを計算できない。床のブーリアン結果と羽の半円の
    #     蓋がこれに当たる。
    # WHY 全部三角化しないか: 四角のままなら箱投影の UV が素直に並ぶし、
    #     面数も増えない。割るのは割らないと困る面だけでよい。
    ngons = [f for f in bm.faces if len(f.verts) > 4]
    if ngons:
        bmesh.ops.triangulate(bm, faces=ngons)

    me = bpy.data.meshes.new(name)
    bm.to_mesh(me); bm.free()
    box_uv(me)
    me.materials.append(material(matname))
    for p in me.polygons:
        p.use_smooth = False
    ob = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(ob)
    return ob


def ring_verts(bm, r, z, n, cx=0.0, cy=0.0, phase=0.0):
    return [bm.verts.new((cx + r * math.cos(phase + 2 * math.pi * i / n),
                          cy + r * math.sin(phase + 2 * math.pi * i / n), z))
            for i in range(n)]


def bridge(bm, a, b, flip=False):
    n = len(a)
    for i in range(n):
        j = (i + 1) % n
        f = (a[i], a[j], b[j], b[i])
        bm.faces.new(f[::-1] if flip else f)


def annulus(bm, r_in, r_out, z0, z1, n=SEG, cx=0.0, cy=0.0,
            cap_in=True, phase=0.0):
    """厚みのある輪。上下の面と内外の壁を張る。"""
    ti = ring_verts(bm, r_in,  z1, n, cx, cy, phase)
    to = ring_verts(bm, r_out, z1, n, cx, cy, phase)
    bi = ring_verts(bm, r_in,  z0, n, cx, cy, phase)
    bo = ring_verts(bm, r_out, z0, n, cx, cy, phase)
    bridge(bm, ti, to)
    bridge(bm, bi, bo, flip=True)
    bridge(bm, to, bo)
    if cap_in:
        bridge(bm, bi, ti)
    return ti, to, bi, bo


def hole_footprint():
    """開口の機構が床を占めている半径。ここへ意匠を描くと口の上に乗る。"""
    return HOLE_R + 0.85


def in_hole(x, y, margin=0.0):
    r = hole_footprint() + margin
    for _hid, c in holes():
        if (x - c.x) ** 2 + (y - c.y) ** 2 < r * r:
            return True
    return False


def flat_ring(bm, r_in, r_out, z, n=SEG, phase=0.0, avoid_holes=True):
    """厚みの無い輪。床へ «描く» 帯 (ボス 1 の CoreRing と同じ作り)。

    WHY 口を避けるか:
        輪の意匠は «開口の在り処» を示すために開口と同じ半径へ引く。素通しで
        描くと口の真上を横切り、シャッターが開いた瞬間に線だけが虚空に残る。
        口の footprint に掛かる区間は最初から描かない。
    """
    for i in range(n):
        a0 = phase + 2.0 * math.pi * i / n
        a1 = phase + 2.0 * math.pi * (i + 1) / n
        mid = (a0 + a1) * 0.5
        rm = (r_in + r_out) * 0.5
        if avoid_holes and in_hole(rm * math.cos(mid), rm * math.sin(mid)):
            continue
        vs = [bm.verts.new((r * math.cos(a), r * math.sin(a), z))
              for r, a in ((r_in, a0), (r_out, a0), (r_out, a1), (r_in, a1))]
        bm.faces.new(vs)


def slab_disc(bm, r, z0, z1, n=SEG, cx=0.0, cy=0.0):
    """厚みのある円板。中心を潰さずに上下を張る。"""
    t = ring_verts(bm, r, z1, n, cx, cy)
    b = ring_verts(bm, r, z0, n, cx, cy)
    ct = bm.verts.new((cx, cy, z1)); cb = bm.verts.new((cx, cy, z0))
    for i in range(n):
        j = (i + 1) % n
        bm.faces.new((ct, t[i], t[j]))
        bm.faces.new((cb, b[j], b[i]))
    bridge(bm, t, b)
    return t, b


def box(bm, size, at, rot=None):
    m = Matrix.Translation(at)
    if rot:
        m = m @ Matrix.Rotation(rot, 4, 'Z')
    m = m @ Matrix.Diagonal((size[0], size[1], size[2], 1.0))
    return bmesh.ops.create_cube(bm, size=1.0, matrix=m)


def quad(bm, pts, z):
    vs = [bm.verts.new((p.x, p.y, z)) for p in pts]
    bm.faces.new(vs)


# --- 床 -----------------------------------------------------------------------
def _after_boolean(ob):
    """ブーリアンの後始末。n-gon を割り直し、UV を張り直す。

    WHY ここでもう一度やるか: 三角化と箱投影は _put の中で済ませているが、
        床だけはそのあとにブーリアンで穴を抜く。抜いた縁の面は新しく出来た
        面なので、n-gon のまま残ってタンジェントが計算できず、UV も
        ブーリアンの補間任せになる。
    """
    md = ob.modifiers.new("Tri", 'TRIANGULATE')
    md.min_vertices = 5
    with bpy.context.temp_override(object=ob, active_object=ob,
                                   selected_objects=[ob],
                                   selected_editable_objects=[ob]):
        bpy.ops.object.modifier_apply(modifier="Tri")
    while ob.data.uv_layers:
        ob.data.uv_layers.remove(ob.data.uv_layers[0])
    box_uv(ob.data)


def build_floor():
    """開口を抜いた床。円板を作ってから 14 本の円柱で削る。

    WHY ブーリアンで抜くか: 開口が同心円 2 重で位相もずれているので、扇形を
        手で張ると «穴の周りだけ三角形が詰まった» 面になり、法線が暴れる。
        Exact ソルバに任せた方が結果が読める。
    """
    bm = bmesh.new()
    slab_disc(bm, ARENA_R + 0.5, -FLOOR_T, 0.0, SEG)
    floor = _put("ARENA_Floor", "M_AR_Deck", bm)

    cb = bmesh.new()
    for _id, c in holes():
        bmesh.ops.create_cone(cb, cap_ends=True, cap_tris=False, segments=SEG_HOLE,
                              radius1=HOLE_R, radius2=HOLE_R, depth=FLOOR_T * 4.0,
                              matrix=Matrix.Translation((c.x, c.y, 0.0)))
    cutter = _put("_CUTTER", "M_AR_Steel", cb)
    md = floor.modifiers.new("Holes", 'BOOLEAN')
    md.operation = 'DIFFERENCE'; md.solver = 'EXACT'; md.object = cutter
    with bpy.context.temp_override(object=floor, active_object=floor,
                                   selected_objects=[floor],
                                   selected_editable_objects=[floor]):
        bpy.ops.object.modifier_apply(modifier="Holes")
    bpy.data.objects.remove(cutter, do_unlink=True)
    _after_boolean(floor)
    return floor


def build_core_rings():
    """開口の輪をなぞる 2 本の線。

    WHY 要るか: シャッターが閉じている間、床は «ただの円盤» になる。口の在り処が
        線として残っていれば、予兆が光った瞬間に «あの輪のあそこか» と結びつく。
        ボス 1 の CoreRing と同じ役 ─ 場に «中心からの距離» の目盛りを与える。
    """
    bm = bmesh.new()
    # WHY 高さを分けるか: 床の意匠は «厚みの無い板» なので、同じ高さに置くと
    #     交差したところで必ず食い合う。放射の継ぎ目 (0.012) より上へ逃がす。
    for _count, radius, _phase in RINGS:
        flat_ring(bm, radius - 0.16, radius + 0.16, 0.020, SEG)
    return _put("ARENA_CoreRing", "M_AR_CoreRing", bm)


def build_floor_plates():
    """床の意匠。放射の継ぎ目と、外周を回る点検帯。"""
    bm = bmesh.new()
    flat_ring(bm, ARENA_R - 1.6, ARENA_R - 1.4, 0.012, SEG)
    inner_r = RINGS[0][1] - HOLE_R - 1.0
    for i in range(SEG_WALL):
        a = 2.0 * math.pi * i / SEG_WALL
        d = Vector((math.cos(a), math.sin(a), 0.0))
        n = Vector((-d.y, d.x, 0.0))
        w = 0.09
        # 口に掛かるところだけ抜く。1 本の長い板のままだと開いた口の上に
        # 橋が架かる。
        #
        # WHY 連続した区間をまとめるか: 細かく割ったまま 1 区間 1 枚で出すと、
        #     24 本 x 40 分割で 1272 三角になる。掛かっていない区間は繋がって
        #     いるので、走査して «run» ごとに 1 枚にすれば 10 分の 1 で済む。
        steps = 40
        run = None
        for k in range(steps + 1):
            t0 = inner_r + (ARENA_R - 0.4 - inner_r) * k / steps
            t1 = inner_r + (ARENA_R - 0.4 - inner_r) * min(k + 1, steps) / steps
            blocked = k >= steps or in_hole(d.x * (t0 + t1) * 0.5,
                                            d.y * (t0 + t1) * 0.5)
            if blocked:
                if run is not None:
                    p0, p1 = d * run[0], d * run[1]
                    quad(bm, [p0 + n * w, p0 - n * w, p1 - n * w, p1 + n * w], 0.012)
                    run = None
            else:
                run = (t0, t1) if run is None else (run[0], t1)
    # 点検口。開口ではない «ただの板» を混ぜると、本物の口が際立つ
    for i in range(SEG_WALL // 2):
        a = 2.0 * math.pi * (i + 0.5) / (SEG_WALL // 2)
        c = Vector((math.cos(a), math.sin(a), 0.0)) * (ARENA_R - 3.0)
        n = Vector((-c.y, c.x, 0.0)).normalized()
        d = Vector((c.x, c.y, 0.0)).normalized()
        if in_hole(c.x, c.y, 1.0):
            continue
        quad(bm, [c + n * 0.9 + d * 0.6, c - n * 0.9 + d * 0.6,
                  c - n * 0.9 - d * 0.6, c + n * 0.9 - d * 0.6], 0.028)
    return _put("ARENA_FloorPlates", "M_AR_Steel", bm)


def build_floor_hazard():
    """開口の «真上に立つな» を床へ描く楔。閉じていても危険地帯だと分かる。"""
    bm = bmesh.new()
    for _hid, c in holes():
        d = Vector((c.x, c.y, 0.0))
        d = d.normalized() if d.length > 1e-6 else Vector((1.0, 0.0, 0.0))
        n = Vector((-d.y, d.x, 0.0))
        for s in (1.0, -1.0):
            base = c + n * (s * (HOLE_R + 0.42))
            quad(bm, [base + d * 0.95, base - d * 0.95,
                      base - d * 0.55 + n * (s * 0.34),
                      base + d * 0.55 + n * (s * 0.34)], 0.036)
    return _put("ARENA_FloorHazard", "M_AR_Hazard", bm)


def build_hazard_kick():
    """壁と床の継ぎ目を回る危険帯。場の «縁» を足元で示す。"""
    bm = bmesh.new()
    annulus(bm, ARENA_R - 0.34, ARENA_R, 0.02, 1.10, SEG_WALL)
    return _put("ARENA_HazardKick", "M_AR_Hazard", bm)


# --- 開口まわり ---------------------------------------------------------------
def build_rims():
    """開口 1 口ぶんの «光る縁»。予兆はここを灯す。

    WHY 機構の «外» に置くか:
        最初は開口のすぐ外 (HOLE_R+0.02) に回したが、そこは回るカラーの居場所で、
        カラーに覆われて予兆がまったく見えなくなった。光る帯は «遠くから
        どの口が開くか» を読ませるためのものなので、機構の一番外側に出す。

    WHY 高さを 0.06 に抑えるか:
        床は閉じている間 «歩ける» ことになっている。0.1 を超えると足元で
        段差として当たり、閉じた口が障害物になる。ボス 1 の床の意匠も 0.02。
    """
    out = []
    for hid, c in holes():
        bm = bmesh.new()
        annulus(bm, R_RIM[0], R_RIM[1], 0.006, 0.066, SEG_HOLE, c.x, c.y)
        out.append(_put("ARENA_Rim_" + hid, "M_AR_RimGlow", bm))
    return out


def _hole_frame(c):
    """口のローカル軸。tan = 輪の接線 / nrm = そこに直交する水平。"""
    tan = Vector((-c.y, c.x, 0.0))
    if tan.length < 1e-6:
        tan = Vector((1.0, 0.0, 0.0))
    tan.normalize()
    return tan, Vector((-tan.y, tan.x, 0.0))


def build_shutters():
    """開口を塞ぐ 2 枚の羽。閉じているときは床として歩ける。

    A は +法線側、B は -法線側。開くまでの手順は 3 段で、どれも剛体の
    平行移動か «口の中心まわりの» 回転しかない ─ リグもクリップも要らない。

        1. 解錠   ARENA_Collar_<口> を口の中心まわりに +22.5 度   (6F)
        2. 沈む   羽を (0, 0, -0.30)                             (4F)
        3. 開く   羽を 法線 * ±2.53                              (8F)

    閉じるときは逆順。予兆 (ARENA_Rim_<口> を灯す 18F) が 1 の前に入る。

    WHY 沈めてから滑らせるか: 羽は床スラブの厚みの «中» に納めてある。
        真横へ滑らせるとスラブと交差するので、先に床下へ抜く。

    WHY 弦に歯を出すか: 2 枚が «突き合わせ» だと、閉じた口が床に引いた 1 本の
        線にしか見えない。互い違いの歯にすると噛み合って見え、開いた瞬間に
        «外れた» が読める。A と B で歯の位置を半ピッチずらす。

    半径は開口より 0.02 m 小さい。縁 (ARENA_Rim_*) が 0.10 m 出ているので、
    その下へ潜り込ませれば継ぎ目は見えない。開口と同じか大きくすると、
    床に «乗った» 状態になって閉じている間ずっと Z ファイティングする。
    """
    out = []
    for hid, c in holes():
        tan, nrm = _hole_frame(c)
        for leaf, sgn in (("A", 1.0), ("B", -1.0)):
            bm = bmesh.new()
            R = HOLE_R - 0.02
            n = SEG_HOLE
            top, bot = [], []
            for i in range(n + 1):
                a = math.pi * i / n
                p = c + tan * (R * math.cos(a)) + nrm * (sgn * R * math.sin(a))
                top.append(bm.verts.new((p.x, p.y, -0.02)))
                bot.append(bm.verts.new((p.x, p.y, -0.22)))
            for i in range(n):
                bm.faces.new((top[i], top[i + 1], bot[i + 1], bot[i]))
            bm.faces.new(top if sgn > 0 else top[::-1])
            bm.faces.new(bot[::-1] if sgn > 0 else bot)

            # 弦の歯。A は 0/2/4、B は 1/3/5 の位置へ出して互い違いにする
            for k in range(6):
                if (k % 2 == 0) != (sgn > 0):
                    continue
                u = (k + 0.5) / 6.0 * 2.0 - 1.0          # -1..1
                p = c + tan * (u * R * 0.86) + nrm * (sgn * 0.15)
                box(bm, (0.34, 0.30, 0.18), (p.x, p.y, -0.12),
                    rot=math.atan2(tan.y, tan.x))

            # 外周の爪。カラーの歯がこの上から噛む。歯と同じ 45 度おき
            for k in range(4):
                a = math.radians(22.5) + math.radians(45.0) * k
                d = tan * math.cos(a) + nrm * (sgn * math.sin(a))
                # 板の上へ出る耳。カラーの歯はこの «外» を通るので重ならない。
                p = c + d * (R - 0.12)
                box(bm, (0.32, 0.34, 0.075), (p.x, p.y, -0.0225),
                    rot=math.atan2(d.y, d.x))

            # 板の意匠。«背に載せた梁» にすると床から鉄骨が生えて見えるので、
            # 面と同じ高さの帯にして «一枚板ではない» とだけ示す。
            box(bm, (R * 1.30, 0.13, 0.045),
                (c.x + nrm.x * sgn * 0.62, c.y + nrm.y * sgn * 0.62, -0.018),
                rot=math.atan2(tan.y, tan.x))
            box(bm, (R * 0.80, 0.13, 0.045),
                (c.x + nrm.x * sgn * 1.32, c.y + nrm.y * sgn * 1.32, -0.018),
                rot=math.atan2(tan.y, tan.x))
            out.append(_put("ARENA_Shutter%s_%s" % (leaf, hid), "M_AR_Steel", bm))
    return out


def build_rim_frames():
    """縁の外を囲む鋼の筐体。光る帯だけだと «床に描いた円» に見える。

    ここは **動かない** 部分 ─ カラーの座、繰り出し機構の腕、床へのアンカー。
    16 口ぶんを 1 メッシュに畳んでよい (口ごとに名指しするのは動く物だけ)。
    """
    bm = bmesh.new()
    for _hid, c in holes():
        # カラーの座
        annulus(bm, R_SEAT[0], R_SEAT[1], 0.0, 0.10, SEG_HOLE, c.x, c.y)
        # 繰り出しの腕。«床の下に機械がある» を示すぶんだけ。長く出すと
        # 蜘蛛の脚に見えて、床の意匠ではなく置物になる
        for k in range(4):
            a = math.pi * 0.25 + math.pi * 0.5 * k
            d = Vector((math.cos(a), math.sin(a), 0.0))
            p = c + d * (HOLE_R + 0.96)
            box(bm, (0.56, 0.62, 0.11), (p.x, p.y, 0.055), rot=a)
    return _put("ARENA_RimFrames", "M_AR_Steel", bm)


def build_collars():
    """口ごとの «回る輪»。歯が羽を噛んでいて、回ると噛み合いが外れる。

    WHY 回転を先に置くか:
        羽がいきなり沈んで滑ると «板が消えた» にしか見えない。先に輪が回って
        歯が抜ける絵があると、そのあとの動きが «外れたから開いた» になる。
        予兆 (縁が光る) → 解錠 (回る) → 開く、と読む順番も作れる。

    WHY 歯を 8 枚・回転を 22.5 度にするか:
        羽の外周にも同じ 8 か所へ爪を出してある。45 度おきの歯を半分 (22.5 度)
        だけ回せば、歯がちょうど爪の «隙間» へ来て抜ける。角度を数字で覚えなくても
        «歯 1 つぶん» で決まる。

    WHY 原点を口の中心に置かないか:
        ボス 1 の場は全パーツが原点 (position 0,0,0) でメッシュにワールド座標が
        焼かれている。こちらだけ原点を動かすと取り込み後の扱いが変わる。
        回転はエンジン側が «口の中心まわり» で解く (T(P)·R·T(-P))。
        中心は ARENA_Rim_<口> の位置から引けるので、余計な情報は要らない。
    """
    out = []
    for hid, c in holes():
        bm = bmesh.new()
        annulus(bm, R_COLLAR[0], R_COLLAR[1], -0.04, 0.085, SEG_HOLE, c.x, c.y)
        tan, nrm = _hole_frame(c)
        for k in range(8):
            a = math.radians(22.5) + math.pi * 0.5 * k * 0.5
            d = tan * math.cos(a) + nrm * math.sin(a)
            # 歯。«輪の上» へ立てる。
            #
            # WHY 内側へ突き出さないか: 羽 (r <= 2.18) の真上へ張り出すと、
            #     歯と羽の爪が同じ高さで重なって面が食い合う。実測で 498 面ペア。
            #     輪の上へ立てれば «回った» は同じだけ読めて、誰とも重ならない。
            # WHY 外側の耳をやめたか: 筐体の座 (2.45〜2.72) と半径が被っていた。
            #     上へ立てた歯で回転は読めるので、耳は要らない。
            p = c + d * ((R_COLLAR[0] + R_COLLAR[1]) * 0.5)
            box(bm, (R_COLLAR[1] - R_COLLAR[0] - 0.02, 0.34, 0.065),
                (p.x, p.y, 0.1175), rot=math.atan2(d.y, d.x))
        out.append(_put("ARENA_Collar_" + hid, "M_AR_Steel", bm))
    return out


def build_shafts():
    """開口の下へ続く筒。潜った胴が «消える» のではなく «入っていく» ように。"""
    bm = bmesh.new()
    for _hid, c in holes():
        annulus(bm, HOLE_R, HOLE_R + 0.26, -PIT_D, -FLOOR_T, SEG_HOLE, c.x, c.y)
        # WHY 肋を入れないか: 深さの手掛かりとして内側へ肋を回していたが、
        #     16 口ぶんで 2048 三角あって «開いている口からしか見えない»。
        #     深さは筒の口の影で足りる。
    return _put("ARENA_Shafts", "M_AR_Steel", bm)


# --- 壁 -----------------------------------------------------------------------
def _tier_wall(bm, z0, z1):
    annulus(bm, ARENA_R, ARENA_R + 0.9, z0, z1, SEG_WALL)


def build_wall():
    bm = bmesh.new()
    _tier_wall(bm, -PIT_D, TIER1)
    return _put("ARENA_Wall", "M_AR_WallPanel", bm)


def build_wall_upper():
    bm = bmesh.new()
    _tier_wall(bm, TIER1, TIER2)
    return _put("ARENA_WallUpper", "M_AR_WallPanel", bm)


def _ribs(bm, z0, z1, count, w=0.34, d=0.30):
    for i in range(count):
        a = 2.0 * math.pi * i / count
        c = Vector((math.cos(a), math.sin(a), 0.0))
        p = c * (ARENA_R - d * 0.5)
        box(bm, (d, w, z1 - z0), (p.x, p.y, (z0 + z1) * 0.5), rot=a)


def build_wall_ribs():
    bm = bmesh.new(); _ribs(bm, 0.0, TIER1, SEG_WALL)
    return _put("ARENA_WallRibs", "M_AR_Steel", bm)


def build_wall_ribs_upper():
    bm = bmesh.new(); _ribs(bm, TIER1, TIER2, SEG_WALL // 2, w=0.44, d=0.34)
    return _put("ARENA_WallRibsUpper", "M_AR_Steel", bm)


def build_wall_seam():
    bm = bmesh.new()
    annulus(bm, ARENA_R - 0.18, ARENA_R + 0.02, 3.60, 4.05, SEG_WALL)
    return _put("ARENA_WallSeam", "M_AR_Steel", bm)


def build_strips():
    bm = bmesh.new()
    annulus(bm, ARENA_R - 0.24, ARENA_R - 0.02, 5.62, 5.90, SEG_WALL)
    return _put("ARENA_WallStrips", "M_AR_StripGlow", bm)


def build_strips_upper():
    bm = bmesh.new()
    annulus(bm, ARENA_R - 0.24, ARENA_R - 0.02, 9.90, 10.18, SEG_WALL)
    return _put("ARENA_WallStripsUpper", "M_AR_StripGlow", bm)


def build_warn():
    """壁へ埋める警告灯。帯ではなく «箱» にすると数えられる明滅になる。"""
    bm = bmesh.new()
    for i in range(SEG_WALL // 2):
        a = 2.0 * math.pi * (i + 0.5) / (SEG_WALL // 2)
        c = Vector((math.cos(a), math.sin(a), 0.0))
        p = c * (ARENA_R - 0.16)
        box(bm, (0.22, 0.70, 0.42), (p.x, p.y, 5.90), rot=a)
    return _put("ARENA_WarnLights", "M_AR_WarnLight", bm)


def build_upper_hazard():
    bm = bmesh.new()
    annulus(bm, ARENA_R - 0.20, ARENA_R + 0.02, TIER1 - 0.62, TIER1 - 0.18, SEG_WALL)
    return _put("ARENA_UpperHazard", "M_AR_Hazard", bm)


def build_mid_trim():
    bm = bmesh.new()
    annulus(bm, ARENA_R - 0.42, ARENA_R + 0.06, TIER1 - 0.10, TIER1 + 0.50, SEG_WALL)
    return _put("ARENA_MidTrim", "M_AR_Steel", bm)


def build_top_trim():
    bm = bmesh.new()
    annulus(bm, ARENA_R - 0.42, ARENA_R + 0.06, TIER2 - 0.10, TIER2 + 0.50, SEG_WALL)
    return _put("ARENA_TopTrim", "M_AR_Steel", bm)


def build_buttress():
    """通し柱。段の笠木を貫いて立つと «段» が積み上げに見える。"""
    bm = bmesh.new()
    for i in range(BUTTRESS):
        a = 2.0 * math.pi * i / BUTTRESS
        c = Vector((math.cos(a), math.sin(a), 0.0))
        p = c * (ARENA_R - 0.42)
        box(bm, (0.86, 1.05, ROOF_Z + 0.4), (p.x, p.y, (ROOF_Z + 0.4) * 0.5), rot=a)
        # 足元の台座
        p2 = c * (ARENA_R - 0.62)
        box(bm, (1.20, 1.45, 1.30), (p2.x, p2.y, 0.65), rot=a)
    return _put("ARENA_Buttress", "M_AR_Steel", bm)


def build_trusses():
    """屋根を吊る梁。上を «開いた空» にしないための天井の骨。"""
    bm = bmesh.new()
    for i in range(BUTTRESS // 2):
        a = 2.0 * math.pi * i / BUTTRESS
        c = Vector((math.cos(a), math.sin(a), 0.0))
        box(bm, (ARENA_R * 2.0, 0.55, 0.75), (0.0, 0.0, TRUSS_Z), rot=a)
    annulus(bm, ARENA_R * 0.42, ARENA_R * 0.42 + 0.6, TRUSS_Z - 0.5, TRUSS_Z + 0.5,
            SEG_WALL)
    return _put("ARENA_Trusses", "M_AR_Steel", bm)


def build_roof():
    bm = bmesh.new()
    slab_disc(bm, ARENA_R + 0.9, ROOF_Z, ROOF_Z + 0.7, SEG_WALL)
    return _put("ARENA_Roof", "M_AR_Steel", bm)


def build_top_hazard():
    bm = bmesh.new()
    annulus(bm, ARENA_R - 0.20, ARENA_R + 0.02, ROOF_Z - 0.80, ROOF_Z - 0.38, SEG_WALL)
    return _put("ARENA_TopHazard", "M_AR_Hazard", bm)


def build_rubble():
    """壁際の瓦礫。«使い込まれた機械» の年齢を出すのと、寸法の物差しになる。

    WHY 壁際だけか: 床の真ん中に置くと、蛇が這う面と当たり判定の話が増える。
        胴が渡らない外周 (開口の外側) にだけ落としておく。
    """
    rnd = random.Random(20260831)
    bm = bmesh.new()
    for _ in range(46):
        a = rnd.uniform(0.0, 2.0 * math.pi)
        r = rnd.uniform(ARENA_R - 2.4, ARENA_R - 0.7)
        s = rnd.uniform(0.22, 0.72)
        box(bm, (s * rnd.uniform(0.7, 1.6), s * rnd.uniform(0.7, 1.6),
                 s * rnd.uniform(0.3, 0.9)),
            (r * math.cos(a), r * math.sin(a), s * 0.25),
            rot=rnd.uniform(0.0, math.pi))
    return _put("ARENA_Rubble", "M_AR_Deck", bm)


def build_pit():
    """床下の底。落ちた破片が消える所。"""
    bm = bmesh.new()
    slab_disc(bm, ARENA_R, -PIT_D - 0.4, -PIT_D, SEG_WALL)
    return _put("ARENA_Pit", "M_AR_Hazard", bm)


# --- 目印 ---------------------------------------------------------------------
def build_markers():
    """ボス 1 の場と同じく、位置の目印を «モデルの中» に持たせる。

    WHY モデルに入れるか: シーンへ手で置くと、場を作り直すたびにずれる。
        FBX に EMPTY として入れておけば、寸法を直したとき目印も一緒に動く。
    """
    spec = [("SPAWN_Player",  Vector((0.0, 0.0, 0.0))),
            ("SPAWN_Serpent", holes()[0][1].copy())]
    out = []
    for name, at in spec:
        o = bpy.data.objects.get(name)
        if o is None:
            o = bpy.data.objects.new(name, None)
            o.empty_display_type = 'PLAIN_AXES'
            bpy.context.scene.collection.objects.link(o)
        o.empty_display_size = 1.2
        o.location = at
        out.append(o)
    return out


# --- 当たり -------------------------------------------------------------------
def build_collision():
    """COL_* は «当たり専用»。見た目より粗く、意匠も面取りも持たない。"""
    out = []
    bm = bmesh.new()
    slab_disc(bm, ARENA_R + 0.5, -FLOOR_T, 0.0, 24)
    col = _put("COL_Arena_Floor", "M_AR_Steel", bm)
    cb = bmesh.new()
    for _id, c in holes():
        bmesh.ops.create_cone(cb, cap_ends=True, cap_tris=False, segments=12,
                              radius1=HOLE_R, radius2=HOLE_R, depth=FLOOR_T * 4.0,
                              matrix=Matrix.Translation((c.x, c.y, 0.0)))
    cutter = _put("_CUTTER2", "M_AR_Steel", cb)
    md = col.modifiers.new("Holes", 'BOOLEAN')
    md.operation = 'DIFFERENCE'; md.solver = 'EXACT'; md.object = cutter
    with bpy.context.temp_override(object=col, active_object=col,
                                   selected_objects=[col],
                                   selected_editable_objects=[col]):
        bpy.ops.object.modifier_apply(modifier="Holes")
    bpy.data.objects.remove(cutter, do_unlink=True)
    _after_boolean(col)
    out.append(col)

    bm = bmesh.new()
    annulus(bm, ARENA_R, ARENA_R + 0.9, -PIT_D, ROOF_Z, 24)
    out.append(_put("COL_Arena_Wall", "M_AR_Steel", bm))

    bm = bmesh.new()
    slab_disc(bm, ARENA_R + 0.9, ROOF_Z, ROOF_Z + 0.7, 24)
    out.append(_put("COL_Arena_Roof", "M_AR_Steel", bm))

    # 開口 1 口ぶんの蓋。閉じている間だけ有効にして、開けるときに切る。
    for hid, c in holes():
        bm = bmesh.new()
        slab_disc(bm, HOLE_R - 0.02, -0.24, -0.04, 12, c.x, c.y)
        out.append(_put("COL_Shutter_" + hid, "M_AR_Steel", bm))
    return out


# 材質ごとにまとめる静止パーツ。まとめ先 → 元の名前。
#
# WHY まとめるか:
#   ドローコールは «オブジェクト x 材質» で立つ。この場は 1 部屋で全部が同時に
#   画面へ入るので、部品を分けておいても視錐台カリングが効かない ─ 分けた数
#   だけ素直に描画が増える。動かない・名指しもしないものは 1 つに畳む。
#
# WHY 全部まとめないか:
#   口ごとの縁・カラー・羽 (80 個) は 1 口ずつ光らせて動かすのが手そのもの。
#   WarnLights は締め上げで明滅させる。CoreRing / FloorPlates / Floor /
#   Rubble / Pit は材質が違うか、後で別々に触りたい。
MERGE = {
    "ARENA_Steel":      ["ARENA_RimFrames", "ARENA_Shafts", "ARENA_WallRibs",
                         "ARENA_WallRibsUpper", "ARENA_WallSeam", "ARENA_MidTrim",
                         "ARENA_TopTrim", "ARENA_Buttress", "ARENA_Trusses",
                         "ARENA_Roof", "ARENA_FloorPlates"],
    "ARENA_Wall":       ["ARENA_Wall", "ARENA_WallUpper"],
    "ARENA_Hazard":     ["ARENA_HazardKick", "ARENA_FloorHazard",
                         "ARENA_UpperHazard", "ARENA_TopHazard"],
    "ARENA_WallStrips": ["ARENA_WallStrips", "ARENA_WallStripsUpper"],
}


def merge_static(made):
    """MERGE の表どおりに静止パーツを 1 つへ畳む。"""
    out = list(made)
    for dst, srcs in MERGE.items():
        objs = [o for o in out if o.name in srcs and o.type == 'MESH']
        if len(objs) < 2:
            continue
        # 名前が衝突すると join 先が .001 になる。先に退避してから畳む
        head = objs[0]
        for o in objs:
            out.remove(o)
        with bpy.context.temp_override(object=head, active_object=head,
                                       selected_objects=objs,
                                       selected_editable_objects=objs):
            bpy.ops.object.join()
        head.name = dst
        # join 先のメッシュ名は元の名前のままなので付け直す。
        # 先に同名の残骸を退かしてからでないと .001 が付く。
        _free_name(dst)
        head.name = dst
        head.data.name = dst
        out.append(head)
    return out


def build():
    for o in [o for o in bpy.data.objects
              if o.name.startswith(("ARENA_", "COL_", "SPAWN_", "_CUTTER"))]:
        bpy.data.objects.remove(o, do_unlink=True)
    # 孤児になったメッシュデータを先に捨てる。残すと次の名前に .001 が付く。
    for me in list(bpy.data.meshes):
        if me.users == 0 and me.name.startswith(("ARENA_", "COL_", "_CUTTER")):
            bpy.data.meshes.remove(me)
    made = [build_floor(), build_core_rings(), build_floor_plates(),
            build_floor_hazard(), build_hazard_kick()]
    made += build_rims()
    made += [build_rim_frames()]
    made += build_collars()
    made += build_shutters()
    made += [build_shafts(),
             build_wall(), build_wall_ribs(), build_wall_seam(),
             build_strips(), build_warn(), build_upper_hazard(), build_mid_trim(),
             build_wall_upper(), build_wall_ribs_upper(), build_strips_upper(),
             build_top_trim(), build_buttress(),
             build_trusses(), build_top_hazard(), build_roof(),
             build_rubble(), build_pit()]
    made += build_markers()
    made += build_collision()
    made = merge_static(made)

    # 名前の検算。ここが崩れるとエンジン側の «口ごとに名指し» が全部外れる。
    bad = [o.name for o in made
           if o.name != (o.data.name if o.type == 'MESH' else o.name)
           or "." in o.name]
    if bad:
        raise RuntimeError("名前に連番が付いた: %s" % bad[:6])
    return made


if __name__ == "__main__":
    m = build()
    print("objects", len(m), "faces",
          sum(len(o.data.polygons) for o in m if o.type == 'MESH'))

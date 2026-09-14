// FBZZ Engine
// ClusteredLights.hlsli | Rendering
// 点光源 / スポットライトの走査を 1 か所へ集約する層
//
// 各マテリアルシェーダーは「点光源ループ」と「スポットループ」を別々に手書きしていたが、
// 中身は全ファイルで同一だった。ここへ畳んでおくことで、ライトの供給元
// (b3 の固定長 cbuffer / StructuredBuffer / クラスタリスト) を差し替えても
// 呼び出し側のシェーダーを書き換えずに済む。
//
// NOTE: Lighting.hlsli の末尾から include される。LightAttenuation / SpotConeWeight /
//       SafeNormalize の定義より後でなければならない (HLSL は前方宣言を持たないため)。
#ifndef CLUSTERED_LIGHTS_HLSLI
#define CLUSTERED_LIGHTS_HLSLI

#include "Common/ClusterConstants.hlsli"
#include "Rendering/PunctualShadow.hlsli"

// ピクセルシェーダーが読むライトデータ。
// 束縛されていないフレームでは clusterLightMode == LEGACY になるため、これらは読まれない。
StructuredBuffer<PunctualLight> gPunctualLights : register(SB_PUNCTUAL_LIGHTS);
StructuredBuffer<uint>          gClusterIndices : register(SB_CLUSTER_INDICES);

// 1 本ぶんの評価結果。intensity には距離減衰とスポットコーンを織り込み済み。
struct PunctualSample
{
    float3 L;
    float3 color;
    float  intensity;
    // 光源の大きさが反射ローブを広げる量。呼び出し側が roughness へ足して使う。
    // 点光源では 0。
    //
    // WHY intensity へ畳み込まないか: 大きな光源は「暗くなる」のではなく
    //     「同じ総量が広い範囲へ散る」。係数として intensity に掛けると拡散にも
    //     効いてしまい、光源を大きくするほど面全体が暗くなる逆の破綻を生む。
    // NOTE: 物理ベースのシェーダー (PBR / Terrain / Water) だけが
    //       これを読む。Toon / RimLight のような非物理モデルは無視してよい。
    float  roughnessBias;

    // dot(N, L) を掛けない呼び出し側 (フロクセル霧のような体積の標本) 用の強度。
    //
    // WHY 2 本持つか: 大きさを持つ光源は L を「鏡面の代表点への方向」に取る。拡散の
    //     コサインはその方向で取ると誤りなので、intensity 側には正しいコサインを織り込み、
    //     呼び出し側が掛け直す dot(N, L) を先に割って打ち消してある。サーフェスでは
    //     これで辻褄が合うが、コサインを掛けない呼び出し側がそのまま使うと、割った分が
    //     残って最大 20 倍明るくなる。そちらは必ずこちらを読むこと。
    // NOTE: 点光源 / スポットでは両者は同じ値。
    float  intensityNoCosine;
};

// 大きさを持つ光源の評価。PunctualSample を使うので、この定義より後でなければならない。
#include "Rendering/AreaLight.hlsli"
#include "Rendering/SphereTubeLight.hlsli"

// -------------------------------------------------------------------------
// 走査範囲の決定
//   count : このピクセルで評価するライト本数
//   base  : gClusterIndices 内の読み出し開始位置 (CLUSTERED 以外では未使用)
// -------------------------------------------------------------------------
void FBZZ_PunctualRange(float2 svXY, float3 worldPos, out uint count, out uint base)
{
    count = 0;
    base  = 0;

    if (clusterLightMode == FBZZ_LIGHT_MODE_CLUSTERED)
    {
        // ビュー空間深度 → Z スライス。view 行列は b0 (CameraConstants)。
        const float viewZ = mul(float4(worldPos, 1.0f), view).z;
        const uint3 coord = uint3(
            (uint)clamp((int)(svXY.x / max(clusterTilePx.x, 1e-4f)), 0, FBZZ_CLUSTER_GRID_X - 1),
            (uint)clamp((int)(svXY.y / max(clusterTilePx.y, 1e-4f)), 0, FBZZ_CLUSTER_GRID_Y - 1),
            FBZZ_ClusterSliceFromViewZ(viewZ));

        base  = FBZZ_ClusterIndex(coord) * FBZZ_CLUSTER_STRIDE;
        count = min(gClusterIndices[base], (uint)FBZZ_MAX_LIGHTS_PER_CLUSTER);
        base += 1; // 先頭要素は個数なので、ライト番号はその次から
    }
    else if (clusterLightMode == FBZZ_LIGHT_MODE_LINEAR)
    {
        count = min(punctualLightCount, (uint)FBZZ_MAX_PUNCTUAL_LIGHTS);
    }
    else // FBZZ_LIGHT_MODE_LEGACY
    {
        // 点光源 → スポット → 大きさを持つ光源 の順に連結した 1 本のインデックス空間。
        // WHY 連結するか: 呼び出し側のループを 1 重で書けるようにするため。
        //     点とスポットの加算順序は従来どおりなので、Area / Sphere / Tube を
        //     置いていないシーンでは丸め誤差まで以前と一致する。
        count = (uint)max(pointLightCount, 0)
              + (uint)max(spotLightCount, 0)
              + (uint)max(legacyShapedLightCount, 0);
    }
}

// 走査位置 i のライトを取り出す。
PunctualLight FBZZ_PunctualAt(uint base, uint i)
{
    // WHY ゼロ初期化: 全分岐でフィールドを漏れなく埋めているが、FXC は
    //     メンバー単位の代入を分岐をまたいで追跡しきれず X4000 を出す。
    PunctualLight lt = (PunctualLight)0;

    if (clusterLightMode == FBZZ_LIGHT_MODE_CLUSTERED)
    {
        lt = gPunctualLights[gClusterIndices[base + i]];
    }
    else if (clusterLightMode == FBZZ_LIGHT_MODE_LINEAR)
    {
        lt = gPunctualLights[i];
    }
    else // FBZZ_LIGHT_MODE_LEGACY — b3 の固定長配列から詰め替える
    {
        const uint pointCount = (uint)max(pointLightCount, 0);
        const uint spotCount  = (uint)max(spotLightCount, 0);

        // Area / Sphere / Tube は b3 に型が無いので b12 の実体配列から組み立てる。
        if (i >= pointCount + spotCount)
        {
            const uint a    = (i - pointCount - spotCount) * FBZZ_LEGACY_SHAPED_STRIDE;
            const float4 r0 = legacyShapedLight[a + 0u];
            const float4 r1 = legacyShapedLight[a + 1u];
            const float4 r2 = legacyShapedLight[a + 2u];
            const float4 r3 = legacyShapedLight[a + 3u];
            const float4 r4 = legacyShapedLight[a + 4u];
            const float4 r5 = legacyShapedLight[a + 5u];

            lt.position    = r0.xyz;  lt.range      = r0.w;
            lt.color       = r1.rgb;  lt.intensity  = r1.w;
            lt.direction   = r2.xyz;
            lt.innerCos    = 0.0f;
            // Area では outerCos が両面フラグの運搬に使われる (1 = 両面)。
            lt.outerCos    = r5.y;
            lt.type        = (uint)r5.x;
            lt.shadowIndex = -1;
            lt.cookieIndex = -1;
            lt.tangent     = r3.xyz;  lt.halfWidth  = r3.w;
            lt.bitangent   = r4.xyz;  lt.halfHeight = r4.w;
        }
        // WHY else で包むか: 分岐の途中で return すると、FXC は戻り値を X4000 で咎める。
        else
        {
            // b12 の legacyPunctualSlots は「点 8 本ぶんの後にスポット 4 本」という
            // 固定の並びなので、走査位置 i (点→スポットの連結) とは添字がずれる。
            uint slotIndex;
            if (i < pointCount)
            {
                lt.position  = pointLights[i].position;
                lt.range     = pointLights[i].range;
                lt.color     = pointLights[i].color;
                lt.intensity = pointLights[i].intensity;
                lt.direction = float3(0.0f, -1.0f, 0.0f);
                lt.innerCos  = 0.0f;
                lt.outerCos  = 0.0f;
                lt.type      = FBZZ_LIGHT_TYPE_POINT;
                slotIndex    = i;
            }
            else
            {
                const uint s = i - pointCount;
                lt.position  = spotLights[s].position;
                lt.range     = spotLights[s].range;
                lt.color     = spotLights[s].color;
                lt.intensity = spotLights[s].intensity;
                lt.direction = spotLights[s].direction;
                lt.innerCos  = spotLights[s].innerCos;
                lt.outerCos  = spotLights[s].outerCos;
                lt.type      = FBZZ_LIGHT_TYPE_SPOT;
                slotIndex    = FBZZ_LEGACY_SPOT_SLOT_BASE + s;
            }
            // 影 / Cookie のスロット番号と光源半径は b3 に持てないので b12 から引く。
            // b12 が束縛されていないパスでは全ゼロで読まれるが、それは
            // 「スロット 0」ではなく punctualShadowCount == 0 が優先されるため影は出ない。
            const float4 slots = (slotIndex < 12u)
                               ? legacyPunctualSlots[slotIndex]
                               : float4(-1.0f, -1.0f, 0.0f, 0.0f);
            lt.shadowIndex = (int)slots.x;
            lt.cookieIndex = (int)slots.y;
            lt.tangent     = float3(1.0f, 0.0f, 0.0f);
            lt.bitangent   = float3(0.0f, 1.0f, 0.0f);
            lt.halfWidth   = slots.z;   // 光源半径。ハイライトの広がりにだけ効く
            lt.halfHeight  = 0.0f;
        }
    }
    return lt;
}

// 距離減衰とスポットコーンを解く。
// WHY 減衰式をここで再実装しないか: Lighting.hlsli の LightAttenuation / SpotConeWeight を
//     そのまま呼ぶことが、「レガシー経路と見た目が完全一致する」ことの根拠そのものになる。
//     式を書き写すと、片方だけ直したときに静かにズレる。
PunctualSample FBZZ_EvalPointSpotLight(PunctualLight lt, float3 worldPos, float3 N)
{
    const float3 toLight = lt.position - worldPos;
    const float  dist    = length(toLight);

    PunctualSample s;
    s.L             = SafeNormalize(toLight, N);
    s.color         = lt.color;
    s.intensity     = lt.intensity * LightAttenuation(dist, lt.range);
    // Point / Spot も形状は点のままだが、光源半径ぶんハイライトは広がる。
    s.roughnessBias = FBZZ_SourceRadiusRoughnessBias(lt.halfWidth, max(dist, 1e-3f));
    if (lt.type == FBZZ_LIGHT_TYPE_SPOT)
        s.intensity *= SpotConeWeight(s.L, lt.direction, lt.innerCos, lt.outerCos);

    // 影は減衰やコーンと同じ「このライトがこの点へどれだけ届くか」の係数なので、
    // 別の戻り値にせず intensity へ畳み込む。
    // WHY: 呼び出し側の Lighting_*_Direct は 24 か所すべて lightIntensity を 1 つ
    //      受け取る形をしている。影を独立した引数で足すと全シグネチャが変わるが、
    //      ここで織り込めば呼び出し側は 1 行も変わらない。
    // 減衰やコーンで既に 0 なら PCF ループを回す意味がない。
    if (s.intensity > 0.0f)
    {
        s.intensity *= FBZZ_PunctualShadowFactor(lt.type, lt.shadowIndex,
                                                 lt.position, worldPos, N, s.L);
        // Cookie は色を持てるので intensity ではなく color 側へ掛ける。
        // 白黒マスクなら結果は同じだが、ステンドグラスのような色付きゴボが作れる。
        if (lt.cookieIndex >= 0)
            s.color *= FBZZ_SampleLightCookie(lt.cookieIndex, worldPos);
    }
    // 点光源 / スポットは L が光源そのものへの方向なので、コサインの補正が要らない。
    // 影とコーンは畳み込んだ後の値を渡す (霧にも影を効かせる)。
    s.intensityNoCosine = s.intensity;
    return s;
}

// 大きさを持つ光源は減衰も方向も別の作りなので、丸ごと専用の評価へ渡す。
// WHY V をここで作るか: 呼び出し側 (FBZZ_PUNCTUAL_BEGIN) は V を渡してこない。
//     cameraPos は b0 の CameraConstants にあり、この層へ到達する全シェーダー
//     (Terrain / Water が独自宣言する版も含む) が必ず持っている。
// WHY 出口を 1 つにするか: 種類ごとに早期 return すると、FXC は戻り値を X4000 で咎める。
PunctualSample FBZZ_EvalPunctual(PunctualLight lt, float3 worldPos, float3 N)
{
    PunctualSample s;
    if (lt.type == FBZZ_LIGHT_TYPE_AREA)
        s = FBZZ_EvalAreaLight(lt, worldPos, N, SafeNormalize(cameraPos - worldPos, N));
    else if (lt.type == FBZZ_LIGHT_TYPE_SPHERE || lt.type == FBZZ_LIGHT_TYPE_TUBE)
        s = FBZZ_EvalSphereTubeLight(lt, worldPos, N, SafeNormalize(cameraPos - worldPos, N));
    else
        s = FBZZ_EvalPointSpotLight(lt, worldPos, N);
    return s;
}

// -------------------------------------------------------------------------
// 呼び出し側のイテレータ
//
//   FBZZ_PUNCTUAL_BEGIN(worldPos, svPosition.xy, N)
//       result += Lighting_PBR_Direct(N, V, ps.L, col, met, rough, ps.color, ps.intensity);
//   FBZZ_PUNCTUAL_END
//
// ループ変数はマクロ内で完結させ、呼び出し側の名前と衝突しないよう _fbzz 接頭辞を付ける。
// デバッグヒートマップもここで処理し、Forward / Deferred の両方で同じ可視化結果にする。
// -------------------------------------------------------------------------
// 走査するライトを決める位置と、実際に評価する位置を分けられる版。
//
// WHY 分けたい経路があるか: フロクセル霧はスライス内のサンプル位置を毎フレームずらす。
//     ずらした位置でクラスタを引くと、クラスタ境界にいるフロクセルだけライトの顔ぶれが
//     フレームごとに入れ替わり、ちらつきになる。位置をばらす目的は密度と影の標本化に
//     あって、ライトの取捨選択ではないので、そこだけ固定位置で決められるようにする。
//
// ループ本体では ps に加えて _fbzzLight (評価中のライトの生データ) も参照できる。
// 体積の標本のように「減衰の結果を光源の素の値から算出した上限で頭打ちにしたい」
// 呼び出し側が使う。
#define FBZZ_PUNCTUAL_BEGIN_AT(_lookupPos, _worldPos, _svXY, _N)                 \
    {                                                                            \
        const float3 _fbzzWorldPos = (_worldPos);                                \
        const float2 _fbzzSvXY     = (_svXY);                                    \
        uint _fbzzCount, _fbzzBase;                                              \
        FBZZ_PunctualRange(_fbzzSvXY, (_lookupPos), _fbzzCount, _fbzzBase);      \
        [loop] for (uint _fbzzIdx = 0; _fbzzIdx < _fbzzCount; ++_fbzzIdx)        \
        {                                                                        \
            const PunctualLight _fbzzLight =                                     \
                FBZZ_PunctualAt(_fbzzBase, _fbzzIdx);                            \
            PunctualSample ps = FBZZ_EvalPunctual(                               \
                _fbzzLight, _fbzzWorldPos, (_N));

// 走査位置と評価位置が同じ通常版。ラスタ経路はすべてこちら。
#define FBZZ_PUNCTUAL_BEGIN(_worldPos, _svXY, _N)                                \
    FBZZ_PUNCTUAL_BEGIN_AT(_worldPos, _worldPos, _svXY, _N)

// FBZZ_PUNCTUAL_NO_DEBUG を include より前に定義すると、ヒートマップの早期 return を
// 落とした版になる。
// WHY 必要か: デバッグ表示は float4 を返して打ち切る作りなので、コンピュートシェーダー
//     (void) やフロクセル霧のように「色を返さない」呼び出し側では文法的に通らない。
//     画面に出ない経路でヒートマップを出す意味も無い。
#ifdef FBZZ_PUNCTUAL_NO_DEBUG
#define FBZZ_PUNCTUAL_END                                                        \
        }                                                                        \
    }
#else
#define FBZZ_PUNCTUAL_END                                                        \
        }                                                                        \
        if (clusterDebugMode != 0)                                                \
            return float4(FBZZ_ClusterDebugColor(_fbzzSvXY, _fbzzWorldPos), 1.0f); \
    }
#endif

// クラスタ占有のヒートマップ色。clusterDebugMode != 0 のとき共通イテレータが最終色へ差し替える。
// 緑 (空いている) → 黄 → 赤 (上限で切り捨てが起きている)。
float3 FBZZ_ClusterDebugColor(float2 svXY, float3 worldPos)
{
    uint count, base;
    FBZZ_PunctualRange(svXY, worldPos, count, base);
    const float t = saturate((float)count / (float)FBZZ_MAX_LIGHTS_PER_CLUSTER);
    return lerp(float3(0.0f, 1.0f, 0.0f), float3(1.0f, 0.0f, 0.0f), t)
         + float3(0.0f, 0.0f, count >= FBZZ_MAX_LIGHTS_PER_CLUSTER ? 1.0f : 0.0f);
}

#endif // CLUSTERED_LIGHTS_HLSLI

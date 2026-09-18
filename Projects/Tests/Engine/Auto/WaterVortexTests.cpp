/// @file    WaterVortexTests.cpp
/// @brief   流れの場が水面へ出る 3 つの道 — 形・輪の帯分け・浮いた物を押す流速。
/// @author  Hasegawa Jin
/// @date    2026-09-17
///
/// 描画 (VS) と CPU (浮力・水中判定) は同じ式を二重に持っている。ここがずれると
/// «見えている穴の縁で物だけが平らに浮く» という形で表に出る。
/// @see Docs/design/water-waves.md 「流れの場が水面に出る 3 つの道」
#include <TestKit/TestKit.hpp>

#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Fields/FlowFieldEval.hpp>
#include <Engine/Scene/Systems/WaterSystem.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Fluid/VectorFieldAsset.hpp>

#include <Math/MathUtils.hpp>
#include <cmath>
#include <vector>

namespace fbzz::tests {
namespace {

/// 波を持たない «鏡の水面»。流れの場と輪の寄与だけを見るため。
scene::WaterComponent FlatWater()
{
    scene::WaterComponent water;
    water.enableGerstnerWaves = false;
    /// @note 20 m 四方。128 テクセルの波紋テクスチャで 1 テクセルが 0.16 m になり、輪の太さが
    ///       «テクセルで下支えされた値» ではなく素の設計値で出る大きさ。
    water.extentX = 20.0f;
    water.extentZ = 20.0f;
    return water;
}

scene::Transform IdentityTransform()
{
    scene::Transform transform;
    transform.worldPosition = math::Vector3::ZERO;
    transform.worldScale    = { 1.0f, 1.0f, 1.0f };
    return transform;
}

/// 中心を持つ場 1 本。半径の内側は falloffPower で減衰する。
scene::ActiveFlowField CenteredField(scene::FlowFieldType type, float speed, float radius,
                                     float falloffPower = 2.0f)
{
    scene::ActiveFlowField field{};
    field.position     = math::Vector3::ZERO;
    field.radius       = radius;
    field.direction    = { 0.0f, 1.0f, 0.0f };
    field.strength     = speed;
    field.type         = type;
    field.falloffPower = falloffPower;
    field.channels     = 0xFFFFFFFFu;
    return field;
}

/// 鉛直軸の渦 1 本。
scene::ActiveFlowField UprightVortex(float speed, float radius, float falloffPower = 2.0f)
{
    return CenteredField(scene::FlowFieldType::Vortex, speed, radius, falloffPower);
}

/// Bernoulli の素の値 [m]。v^2 / 2g。
float Bernoulli(float speed) { return speed * speed / 19.6f; }

/// XZ 一様な速度場を手で組む。中身が定数なのでトリリニア補間はどこでも同じ値を返す。
fluid::VectorFieldAsset UniformXZField(float speed)
{
    fluid::VectorFieldAsset asset;
    asset.sizeX = 2;
    asset.sizeY = 2;
    asset.sizeZ = 2;
    asset.boundsMin = { -1.0f, -1.0f, -1.0f };
    asset.boundsMax = {  1.0f,  1.0f,  1.0f };
    asset.maxMagnitude = (std::max)(std::abs(speed), 1.0f);
    asset.data.assign(8, math::Vector3{ speed, 0.0f, 0.0f });
    return asset;
}

/// 焼いた場 1 枚を «箱で貼った» 状態に解決したもの。回転なし。
scene::ActiveFlowField BakedField(const fluid::VectorFieldAsset& asset, math::Vector3 extents)
{
    scene::ActiveFlowField field{};
    field.position    = math::Vector3::ZERO;
    /// @note Baked は extents が効く範囲を決めるので radius は使わない (FlowFieldEval と同じ)。
    field.radius      = 0.0f;
    field.direction   = { 0.0f, 1.0f, 0.0f };
    field.strength    = 1.0f;
    field.type        = scene::FlowFieldType::Baked;
    field.channels    = 0xFFFFFFFFu;
    field.vectorField = &asset;
    field.extents     = extents;
    return field;
}

} // namespace

class WaterVortexTest : public ::testing::Test {};

/// @name 形

TEST_F(WaterVortexTest, TheDepressionIsExactlyTheDepthAtTheCenterAndGoneAtTheRadius)
{
    /// @note 描画の VS も同じ式で頂点を下げる。中心で height、影響半径の外で 0 でなければ、
    ///       見えている形の縁に «段» ができる。
    scene::WaterComponent water = FlatWater();
    scene::WaterSurfaceFlow flow;
    flow.kind   = scene::FlowFieldType::Vortex;
    flow.center = { 12.0f, -5.0f };
    flow.radius = 4.0f;
    flow.height = -0.4f;
    water.surfaceFlows[0]  = flow;
    water.surfaceFlowCount = 1;

    EXPECT_NEAR(water.GetSurfaceHeightAt(12.0f, -5.0f, 3.0f), -0.4f, 1.0e-5f);
    EXPECT_NEAR(water.GetSurfaceHeightAt(12.0f + 4.0f, -5.0f, 3.0f), 0.0f, 1.0e-3f);
    EXPECT_NEAR(water.GetSurfaceHeightAt(12.0f, -5.0f - 6.0f, 3.0f), 0.0f, 1.0e-3f);
}

TEST_F(WaterVortexTest, TheDepressionIsMonotoneFromTheCenterOutward)
{
    /// @note (1 - r/R)^p だと r = R で折れる。Gaussian にしたのは C¹ 連続にするためで、
    ///       単調に浅くなることまでは形の最低条件。
    scene::WaterComponent water = FlatWater();
    water.surfaceFlows[0].radius = 6.0f;
    water.surfaceFlows[0].height = -2.0f;
    water.surfaceFlowCount = 1;

    float previous = -2.0f;
    for (int i = 1; i <= 30; ++i) {
        const float r = static_cast<float>(i) * 0.2f;
        const float height = water.GetSurfaceHeightAt(r, 0.0f, 0.0f);
        EXPECT_GT(height, previous) << "r = " << r;
        EXPECT_LE(height, 0.0f);
        previous = height;
    }
}

TEST_F(WaterVortexTest, SinkDigsAndSourceLiftsWithTheSameBernoulli)
{
    /// @note 排水口は «穴»、湧き上がりは «山»。符号だけが違い、大きさは同じ v^2/2g。
    scene::WaterComponent water = FlatWater();
    const scene::Transform transform = IdentityTransform();

    scene::ResolveWaterSurfaceFlows(water, transform,
                                    { CenteredField(scene::FlowFieldType::Sink, 3.0f, 5.0f) });
    ASSERT_EQ(water.surfaceFlowCount, 1);
    EXPECT_NEAR(water.GetSurfaceHeightAt(0.0f, 0.0f, 0.0f), -Bernoulli(3.0f), 1.0e-5f);
    EXPECT_NEAR(water.GetSurfaceHeightAt(5.0f, 0.0f, 0.0f), 0.0f, 1.0e-3f);

    scene::ResolveWaterSurfaceFlows(water, transform,
                                    { CenteredField(scene::FlowFieldType::Source, 3.0f, 5.0f) });
    ASSERT_EQ(water.surfaceFlowCount, 1);
    EXPECT_NEAR(water.GetSurfaceHeightAt(0.0f, 0.0f, 0.0f), Bernoulli(3.0f), 1.0e-5f);
    EXPECT_NEAR(water.GetSurfaceHeightAt(0.0f, 5.0f, 0.0f), 0.0f, 1.0e-3f);
}

TEST_F(WaterVortexTest, UniformAndCurlNeverShapeTheSurface)
{
    /// @note 局所の風はうねりを育てない (吹送距離と時間が要る)。乱流には «高さ» が定義できない。
    ///       どちらも流れと質感にだけ出るので、枠は取るが GetSurfaceHeightAt は動かさない。
    scene::WaterComponent water = FlatWater();
    scene::ActiveFlowField wind = CenteredField(scene::FlowFieldType::Uniform, 6.0f, 8.0f);
    wind.direction = { 1.0f, 0.0f, 0.0f };
    const scene::ActiveFlowField curl = CenteredField(scene::FlowFieldType::Curl, 6.0f, 8.0f);

    scene::ResolveWaterSurfaceFlows(water, IdentityTransform(), { wind, curl });
    ASSERT_EQ(water.surfaceFlowCount, 2);
    for (int i = 0; i < water.surfaceFlowCount; ++i)
        EXPECT_FLOAT_EQ(water.surfaceFlows[static_cast<size_t>(i)].height, 0.0f);

    for (int i = -8; i <= 8; ++i) {
        const float x = static_cast<float>(i);
        EXPECT_FLOAT_EQ(water.GetSurfaceHeightAt(x, 0.0f, 0.0f), 0.0f);
        EXPECT_FLOAT_EQ(water.GetSurfaceHeightAt(0.0f, x, 0.0f), 0.0f);
    }
}

TEST_F(WaterVortexTest, ABakedFieldDigsBernoulliInsideItsBoxAndNothingOutside)
{
    /// @note 焼いた場は XZ 成分だけを Bernoulli へ通す。箱の外は «場が無い» ので平ら。
    ///       HLSL の WaterBakedDisplacement もこの式でなければ、絵と浮力が別の水面になる。
    const fluid::VectorFieldAsset asset = UniformXZField(2.0f);
    scene::WaterComponent water = FlatWater();
    scene::ResolveWaterSurfaceFlows(water, IdentityTransform(),
                                    { BakedField(asset, { 5.0f, 2.0f, 5.0f }) });
    ASSERT_EQ(water.surfaceFlowCount, 1);
    /// @note 上限は kMaxWaterSurfaceDisplacement。刻めるので WaveMeshFade は 1。
    EXPECT_FLOAT_EQ(water.surfaceFlows[0].height, -scene::kMaxWaterSurfaceDisplacement);

    EXPECT_NEAR(water.GetSurfaceHeightAt(0.0f, 0.0f, 0.0f), -Bernoulli(2.0f), 1.0e-5f);
    EXPECT_NEAR(water.GetSurfaceHeightAt(4.0f, -4.0f, 0.0f), -Bernoulli(2.0f), 1.0e-5f);
    EXPECT_FLOAT_EQ(water.GetSurfaceHeightAt(8.0f, 0.0f, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(water.GetSurfaceHeightAt(0.0f, 9.0f, 0.0f), 0.0f);
}

TEST_F(WaterVortexTest, ABakedFieldThatDoesNotReachTheSurfacePlaneIsDropped)
{
    /// @note 箱が水面より下 (または上) に沈んでいれば、基準面での標本は箱の外になる。
    const fluid::VectorFieldAsset asset = UniformXZField(2.0f);
    scene::ActiveFlowField field = BakedField(asset, { 5.0f, 2.0f, 5.0f });
    field.position = { 0.0f, -40.0f, 0.0f };

    scene::WaterComponent water = FlatWater();
    scene::ResolveWaterSurfaceFlows(water, IdentityTransform(), { field });
    EXPECT_EQ(water.surfaceFlowCount, 0);
}

/// @name 帯の原則

TEST_F(WaterVortexTest, ACoarseVertexGridSwallowsTheVortexEntirely)
{
    /// @note 頂点で刻めない形は «もっと大きい別の起伏» に化ける。波と同じ尺で寝かせること。
    scene::WaterComponent water = FlatWater();
    const scene::Transform transform = IdentityTransform();
    const std::vector<scene::ActiveFlowField> fields = { UprightVortex(5.0f, 4.0f) };

    /// @note 判定なし = 刻めるものとして扱う
    water.cellSize = math::Vector2::ZERO;
    scene::ResolveWaterSurfaceFlows(water, transform, fields);
    ASSERT_EQ(water.surfaceFlowCount, 1);
    EXPECT_NEAR(water.surfaceFlows[0].height, -Bernoulli(5.0f), 1.0e-4f);

    /// @note 1 セル 8 m では形の差し渡し (8 m) がちょうど 1 セル = Nyquist の下。消える。
    water.cellSize = { 8.0f, 8.0f };
    scene::ResolveWaterSurfaceFlows(water, transform, fields);
    EXPECT_EQ(water.surfaceFlowCount, 0);
}

TEST_F(WaterVortexTest, TheDepthIsCappedSoTheSurfaceCannotTurnInsideOut)
{
    /// @note Bernoulli をそのまま採ると speed 10 m/s が 5 m の穴を掘り、«底» が見える。
    scene::WaterComponent water = FlatWater();
    scene::ResolveWaterSurfaceFlows(water, IdentityTransform(), { UprightVortex(30.0f, 10.0f) });
    ASSERT_EQ(water.surfaceFlowCount, 1);
    EXPECT_FLOAT_EQ(water.surfaceFlows[0].height, -scene::kMaxWaterSurfaceDisplacement);

    /// @note 盛り上がりにも同じ上限が効く。片方だけだと湧き出しが天へ伸びる。
    scene::ResolveWaterSurfaceFlows(water, IdentityTransform(),
                                    { CenteredField(scene::FlowFieldType::Source, 30.0f, 10.0f) });
    ASSERT_EQ(water.surfaceFlowCount, 1);
    EXPECT_FLOAT_EQ(water.surfaceFlows[0].height, scene::kMaxWaterSurfaceDisplacement);
}

TEST_F(WaterVortexTest, LyingVorticesAndUnboundedOnesDoNotShapeTheSurface)
{
    /// @note 横倒しの渦は水面を «掘る» のではなく撫でる。半径の無い要素は中心からの距離で
    ///       減衰しないので、形の大きさが決まらない。どちらも流速としては効いたままにする。
    scene::WaterComponent water = FlatWater();
    scene::ActiveFlowField lying = UprightVortex(5.0f, 4.0f);
    lying.direction = { 1.0f, 0.0f, 0.0f };
    scene::ActiveFlowField unbounded = UprightVortex(5.0f, 0.0f);

    scene::ResolveWaterSurfaceFlows(water, IdentityTransform(), { lying, unbounded });
    EXPECT_EQ(water.surfaceFlowCount, 0);
}

TEST_F(WaterVortexTest, OnlyVorticesThatReachTheRectangleAreKept)
{
    /// @note 20 x 20、中心は原点
    scene::WaterComponent water = FlatWater();
    scene::ActiveFlowField far = UprightVortex(5.0f, 4.0f);
    far.position = { 200.0f, 0.0f, 0.0f };

    scene::ResolveWaterSurfaceFlows(water, IdentityTransform(), { far });
    EXPECT_EQ(water.surfaceFlowCount, 0);
}

TEST_F(WaterVortexTest, TheStrongestEightWin)
{
    scene::WaterComponent water = FlatWater();
    std::vector<scene::ActiveFlowField> fields;
    for (int i = 0; i < 10; ++i)
        fields.push_back(UprightVortex(1.0f + static_cast<float>(i), 4.0f));

    scene::ResolveWaterSurfaceFlows(water, IdentityTransform(), fields);
    ASSERT_EQ(water.surfaceFlowCount, scene::kWaterSurfaceFlowCount);
    /// @note 残るのは speed 3〜10 の 8 本。いちばん浅い枠でも speed 2 (= 0.204 m) より深い。
    for (int i = 0; i < scene::kWaterSurfaceFlowCount; ++i) {
        EXPECT_GE(std::abs(water.surfaceFlows[static_cast<size_t>(i)].height),
                  Bernoulli(3.0f) - 1.0e-4f);
    }
}

TEST_F(WaterVortexTest, AFlowWithAShapeOutranksOneWithout)
{
    /// @note 形は «その場に無いとシルエットが変わる»。質感は 1 本落ちても画として崩れない。
    scene::WaterComponent water = FlatWater();
    std::vector<scene::ActiveFlowField> fields;
    for (int i = 0; i < scene::kWaterSurfaceFlowCount; ++i) {
        scene::ActiveFlowField wind = CenteredField(scene::FlowFieldType::Uniform, 9.0f, 6.0f);
        wind.direction = { 1.0f, 0.0f, 0.0f };
        fields.push_back(wind);
    }
    fields.push_back(UprightVortex(2.0f, 4.0f));

    scene::ResolveWaterSurfaceFlows(water, IdentityTransform(), fields);
    ASSERT_EQ(water.surfaceFlowCount, scene::kWaterSurfaceFlowCount);
    int shaped = 0;
    for (int i = 0; i < water.surfaceFlowCount; ++i)
        if (water.surfaceFlows[static_cast<size_t>(i)].height != 0.0f) ++shaped;
    EXPECT_EQ(shaped, 1);
}

/// @name 逆写像には入れない

TEST_F(WaterVortexTest, TheDepressionDoesNotEnterTheInverseMapping)
{
    /// @note 形は水平変位を持たないので «変位前の位置» を解く対象ではない。入れると Gerstner の
    ///       位相まで穴のぶんずれ、渦の周りだけ波の位相が飛ぶ。
    scene::WaterComponent water;
    water.waves[0] = { { 1.0f, 0.0f }, 0.5f, 8.0f, 0.5f };
    for (int i = 1; i < 4; ++i) water.waves[static_cast<size_t>(i)].amplitude = 0.0f;

    const math::Vector2 before = water.SolveUndisplacedXZ(3.0f, -2.0f, 1.25f);

    water.surfaceFlows[0].radius = 5.0f;
    water.surfaceFlows[0].height = -2.0f;
    water.surfaceFlowCount = 1;
    const math::Vector2 after = water.SolveUndisplacedXZ(3.0f, -2.0f, 1.25f);

    EXPECT_FLOAT_EQ(before.x, after.x);
    EXPECT_FLOAT_EQ(before.y, after.y);
}

/// @name 波紋の帯分け

TEST_F(WaterVortexTest, TheRippleProfileIsOddAroundItsRing)
{
    /// @note 輪は «山と谷» を対で持つ。輪の上 (ring = 0) はちょうどゼロ交差。
    using W = scene::WaterComponent;
    constexpr float ringRadius = 3.0f;
    constexpr float width = 0.4f;
    EXPECT_NEAR(W::WaterRippleProfile(ringRadius, ringRadius, width), 0.0f, 1.0e-6f);
    for (int i = 1; i <= 12; ++i) {
        const float offset = static_cast<float>(i) * 0.1f;
        EXPECT_NEAR(W::WaterRippleProfile(ringRadius + offset, ringRadius, width),
                    -W::WaterRippleProfile(ringRadius - offset, ringRadius, width), 1.0e-5f);
    }
}

TEST_F(WaterVortexTest, ARingTheGridCannotCarveAddsNoHeight)
{
    /// @note 帯の原則: 頂点に乗らない輪は «法線だけ» の帯へ落ちる。高さに残ると、平らに見える
    ///       水面の上で浮いている物だけが跳ねる。
    scene::WaterComponent water = FlatWater();
    scene::WaterRipple ripple;
    ripple.center    = { 1.0f, 2.0f };
    ripple.radius    = 1.5f;
    ripple.width     = 0.4f;
    ripple.amplitude = 1.0f;
    ripple.meshFade  = 0.0f;
    water.ripples.push_back(ripple);

    /// @note 山のあたりを一通り見て、どこにも高さが出ないこと。
    for (int i = 0; i < 20; ++i) {
        const float x = 1.0f + static_cast<float>(i) * 0.25f;
        EXPECT_FLOAT_EQ(water.GetSurfaceHeightAt(x, 2.0f, 0.0f), 0.0f);
    }

    /// @note 刻めるようになれば同じ輪が高さを持つ。
    water.ripples[0].meshFade = 1.0f;
    float peak = 0.0f;
    for (int i = 0; i < 40; ++i) {
        const float x = 1.0f + static_cast<float>(i) * 0.125f;
        peak = (std::max)(peak, std::abs(water.GetSurfaceHeightAt(x, 2.0f, 0.0f)));
    }
    EXPECT_GT(peak, 0.01f);
}

TEST_F(WaterVortexTest, TheEmittedRingIsMeasuredInMetresAndStartsAtZeroRadius)
{
    scene::WaterComponent water = FlatWater();
    const scene::Transform transform = IdentityTransform();
    scene::EmitWaterRipple(water, transform, { 10.0f, 0.0f, -4.0f }, 1.0f);

    ASSERT_EQ(water.ripples.size(), 1u);
    EXPECT_FLOAT_EQ(water.ripples[0].center.x, 10.0f);
    EXPECT_FLOAT_EQ(water.ripples[0].center.y, -4.0f);
    EXPECT_FLOAT_EQ(water.ripples[0].radius, 0.0f);
    EXPECT_NEAR(water.ripples[0].width, 0.60f, 1.0e-5f);

    /// @note 寿命は m/s で進む。1 秒で speed [m] ぶん広がる。
    const float speed = water.ripples[0].speed;
    scene::UpdateWaterRipples(water, transform, 1.0f);
    ASSERT_EQ(water.ripples.size(), 1u);
    EXPECT_NEAR(water.ripples[0].radius, speed, 1.0e-5f);
}

/// @name 浮いた物を押す流速

TEST_F(WaterVortexTest, TheFlowVelocityIsTheCurrentPlusTheVortexTangent)
{
    /// @note FluidVolume の flowVelocity がこれを返す。場所の関数になって初めて、浮いた物が
    ///       flowCoupling を立てなくても渦の周りを回る。
    scene::WaterComponent water = FlatWater();
    water.current = { 1.0f, 0.0f };
    /// @note falloffPower 0 で半径の内側は減衰なし。接線の向きだけを確かめる。
    const std::vector<scene::ActiveFlowField> fields = { UprightVortex(2.0f, 10.0f, 0.0f) };

    const math::Vector3 flow =
        scene::WaterFlowVelocityAt(water, fields, 0.0f, { 3.0f, 7.0f, 0.0f }, 0.0f);

    /// @note cross((0,1,0), (3,0,0)) = (0,0,-3) → 正規化して strength 倍で (0,0,-2)。
    EXPECT_NEAR(flow.x, 1.0f, 1.0e-5f);
    EXPECT_FLOAT_EQ(flow.y, 0.0f);
    EXPECT_NEAR(flow.z, -2.0f, 1.0e-5f);
}

TEST_F(WaterVortexTest, TheFlowVelocityCarriesEveryFieldType)
{
    /// @note 形に出ない型 (Uniform / Curl / Source) も流速としては必ず効く。ここが抜けると
    ///       «見えているさざ波は流れているのに浮いた物が止まっている» になる。
    scene::WaterComponent water = FlatWater();
    water.current = { 0.5f, 0.0f };
    scene::ActiveFlowField wind = CenteredField(scene::FlowFieldType::Uniform, 3.0f, 10.0f, 0.0f);
    wind.direction = { 0.0f, 0.0f, 1.0f };
    const scene::ActiveFlowField source =
        CenteredField(scene::FlowFieldType::Source, 4.0f, 10.0f, 0.0f);

    const math::Vector3 flow =
        scene::WaterFlowVelocityAt(water, { wind, source }, 0.0f, { 2.0f, 0.0f, 0.0f }, 0.0f);

    /// @note Source は (2,0,0) 方向 = +X へ 4 m/s。Uniform は +Z へ 3 m/s。
    EXPECT_NEAR(flow.x, 0.5f + 4.0f, 1.0e-5f);
    EXPECT_FLOAT_EQ(flow.y, 0.0f);
    EXPECT_NEAR(flow.z, 3.0f, 1.0e-5f);
}

TEST_F(WaterVortexTest, TheFlowVelocityIsPurelyHorizontal)
{
    /// @note 鉛直成分を返すと、浮いた体が浮力と綱引きして水面の上で震える。
    scene::WaterComponent water = FlatWater();
    scene::ActiveFlowField updraft;
    updraft.position  = math::Vector3::ZERO;
    updraft.radius    = 0.0f;
    updraft.direction = { 0.0f, 1.0f, 0.0f };
    updraft.strength  = 9.0f;
    updraft.type      = scene::FlowFieldType::Uniform;
    updraft.channels  = 0xFFFFFFFFu;

    const math::Vector3 flow =
        scene::WaterFlowVelocityAt(water, { updraft }, 0.0f, { 0.0f, 0.0f, 0.0f }, 0.0f);
    EXPECT_FLOAT_EQ(flow.y, 0.0f);
}

} // namespace fbzz::tests

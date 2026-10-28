/// @file    FiberMaterialFields.hlsli
/// @brief   Stable 128-byte engine-owned material prefix for project Fiber shaders.
/// @author  Hasegawa Jin
/// @date    2026-10-21
    float4 rootColor;
    float4 tipColor;
    float fiberLength;
    float fiberDensity;
    float fiberThickness;
    float fiberTaper;
    float fiberFrequency;
    float windResponse;
    float maxBend;
    float gravityBend;
    float roughness;
    float specularStrength;
    float transmission;
    float rootOcclusion;
    float planarShading;
    float worldMapping;
    float finSilhouetteWidth;
    /// @note 主ハイライトを毛先へ、副ハイライトを根元へずらす量 (接線を法線方向へ押す量)。
    float specularShift;
    /// @note 毛の色を帯びる副ハイライト (TRT) の強さ。0 で主ハイライトだけ。
    float secondarySpecular;
    /// @note 毛 1 本ごとの明度のばらつき [0,1]。
    float colorVariation;
    /// @note 毛先を束の中心へ寄せる強さ [0,1]。0 で束を作らない。
    float clumping;
    /// @note 束が根元から毛先までにねじれる回転数。向きは束ごとに乱数で決める。
    float clumpTwist;
    /// @note fiberMask (.mat の tex5) の bindless 添字。未設定でも CPU が白 1×1 の添字を入れるので常に有効。
    FBZZ_MATERIAL_TEX_5
    float _fiberPadding0;
    float _fiberPadding1;
    float _fiberPadding2;

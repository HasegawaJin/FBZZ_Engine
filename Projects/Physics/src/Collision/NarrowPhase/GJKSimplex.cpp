/// @file    GJKSimplex.cpp
/// @brief   原点に対する GJK 単体の縮約と次の探索方向。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Physics/Collision/NarrowPhase/GJK.hpp>

namespace fbzz::physics
{
    namespace
    {
        bool ReduceLine(Simplex& simplex, math::Vector3& searchDirection)
        {
            const math::Vector3 olderPoint = simplex.verts[0].point;
            const math::Vector3 newestPoint = simplex.verts[1].point;
            const math::Vector3 line = olderPoint - newestPoint;
            const math::Vector3 towardOrigin = -newestPoint;

            if (math::Vector3::Dot(line, towardOrigin) > 0.0f)
            {
                const math::Vector3 lineToOrigin =
                    math::Vector3::Cross(math::Vector3::Cross(line, towardOrigin), line);
                // 原点が辺と同一直線上なら交差として扱う。
                if (lineToOrigin.LengthSq() < 1e-10f) {
                    return true;
                }
                searchDirection = lineToOrigin;
            }
            else
            {
                simplex.Set(simplex.verts[1]);
                searchDirection = towardOrigin;
            }
            return false;
        }

        bool ReduceTriangle(Simplex& simplex, math::Vector3& searchDirection)
        {
            const math::Vector3 newestPoint = simplex.verts[2].point;
            const math::Vector3 edgeToSecond = simplex.verts[1].point - newestPoint;
            const math::Vector3 edgeToFirst = simplex.verts[0].point - newestPoint;
            const math::Vector3 towardOrigin = -newestPoint;
            const math::Vector3 faceNormal = math::Vector3::Cross(edgeToSecond, edgeToFirst);

            if (math::Vector3::Dot(math::Vector3::Cross(faceNormal, edgeToFirst), towardOrigin) > 0.0f)
            {
                if (math::Vector3::Dot(edgeToFirst, towardOrigin) > 0.0f)
                {
                    simplex.Set(simplex.verts[0], simplex.verts[2]);
                    searchDirection = math::Vector3::Cross(
                        math::Vector3::Cross(edgeToFirst, towardOrigin), edgeToFirst);
                    return false;
                }

                simplex.Set(simplex.verts[1], simplex.verts[2]);
                return ReduceLine(simplex, searchDirection);
            }

            if (math::Vector3::Dot(math::Vector3::Cross(edgeToSecond, faceNormal), towardOrigin) > 0.0f)
            {
                simplex.Set(simplex.verts[1], simplex.verts[2]);
                return ReduceLine(simplex, searchDirection);
            }

            if (math::Vector3::Dot(faceNormal, towardOrigin) > 0.0f) {
                searchDirection = faceNormal;
            }
            else
            {
                simplex.Set(simplex.verts[2], simplex.verts[1], simplex.verts[0]);
                searchDirection = -faceNormal;
            }
            return false;
        }

        bool ReduceTetrahedron(Simplex& simplex, math::Vector3& searchDirection)
        {
            const math::Vector3 newestPoint = simplex.verts[3].point;
            const math::Vector3 edgeToThird = simplex.verts[2].point - newestPoint;
            const math::Vector3 edgeToSecond = simplex.verts[1].point - newestPoint;
            const math::Vector3 edgeToFirst = simplex.verts[0].point - newestPoint;
            const math::Vector3 towardOrigin = -newestPoint;

            const math::Vector3 faceNormal321 = math::Vector3::Cross(edgeToThird, edgeToSecond);
            const math::Vector3 faceNormal310 = math::Vector3::Cross(edgeToSecond, edgeToFirst);
            const math::Vector3 faceNormal302 = math::Vector3::Cross(edgeToFirst, edgeToThird);

            if (math::Vector3::Dot(faceNormal321, towardOrigin) > 0.0f)
            {
                simplex.Set(simplex.verts[1], simplex.verts[2], simplex.verts[3]);
                return ReduceTriangle(simplex, searchDirection);
            }
            if (math::Vector3::Dot(faceNormal310, towardOrigin) > 0.0f)
            {
                simplex.Set(simplex.verts[0], simplex.verts[1], simplex.verts[3]);
                return ReduceTriangle(simplex, searchDirection);
            }
            if (math::Vector3::Dot(faceNormal302, towardOrigin) > 0.0f)
            {
                simplex.Set(simplex.verts[0], simplex.verts[2], simplex.verts[3]);
                return ReduceTriangle(simplex, searchDirection);
            }
            return true;
        }
    }

    bool DoSimplex(Simplex& simplex, math::Vector3& searchDirection)
    {
        switch (simplex.size)
        {
        case 2: return ReduceLine(simplex, searchDirection);
        case 3: return ReduceTriangle(simplex, searchDirection);
        case 4: return ReduceTetrahedron(simplex, searchDirection);
        default: return false;
        }
    }
}

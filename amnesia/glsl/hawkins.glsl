#ifndef HAWKINS_GLSL
#define HAWKINS_GLSL

// Hawkins / Burns visibility-buffer barycentric reconstruction.
// 1:1 port from
//   AmnesiaTheDarkDescent/HPL2/resource/visibility_utility.h.fsl
// (which credits the DAIS paper, Appendix A:
//   https://cg.ivd.kit.edu/publications/2015/dais/DAIS.pdf)
//
// Given the 3 homogeneous clip-space vertex positions of a triangle and the
// pixel's NDC position (xy in [-1, 1]), produces:
//   m_lambda — perspective-correct barycentric coordinates at that pixel,
//   m_ddx, m_ddy — screen-space partial derivatives of the barycentric, for
//                  feeding into textureGrad for correct mip selection.
//
// Pair with the Interpolate* helpers below to interpolate any per-vertex
// attribute (UV, normal, color) with matching analytical derivatives.

struct BarycentricDeriv
{
    vec3 m_lambda;
    vec3 m_ddx;
    vec3 m_ddy;
};

struct GradientInterpolationResults
{
    vec2 interp;
    vec2 dx;
    vec2 dy;
};

BarycentricDeriv CalcFullBary(vec4 pt0, vec4 pt1, vec4 pt2,
                              vec2 pixelNdc, vec2 two_over_windowsize)
{
    BarycentricDeriv ret;
    vec3 invW = 1.0 / vec3(pt0.w, pt1.w, pt2.w);

    vec2 ndc0 = pt0.xy * invW.x;
    vec2 ndc1 = pt1.xy * invW.y;
    vec2 ndc2 = pt2.xy * invW.z;

    // Inverse triangle area (2D, post-perspective-divide).
    float invDet = 1.0 / determinant(mat2(ndc2 - ndc1, ndc0 - ndc1));

    ret.m_ddx = vec3(ndc1.y - ndc2.y, ndc2.y - ndc0.y, ndc0.y - ndc1.y)
              * invDet * invW;
    ret.m_ddy = vec3(ndc2.x - ndc1.x, ndc0.x - ndc2.x, ndc1.x - ndc0.x)
              * invDet * invW;

    float ddxSum = dot(ret.m_ddx, vec3(1.0));
    float ddySum = dot(ret.m_ddy, vec3(1.0));

    vec2 deltaVec = pixelNdc - ndc0;

    float interpInvW = invW.x + deltaVec.x * ddxSum + deltaVec.y * ddySum;
    float interpW    = 1.0 / interpInvW;

    ret.m_lambda.x = interpW * (invW.x + deltaVec.x * ret.m_ddx.x + deltaVec.y * ret.m_ddy.x);
    ret.m_lambda.y = interpW * (0.0    + deltaVec.x * ret.m_ddx.y + deltaVec.y * ret.m_ddy.y);
    ret.m_lambda.z = interpW * (0.0    + deltaVec.x * ret.m_ddx.z + deltaVec.y * ret.m_ddy.z);

    ret.m_ddx *= two_over_windowsize.x;
    ret.m_ddy *= two_over_windowsize.y;
    ddxSum    *= two_over_windowsize.x;
    ddySum    *= two_over_windowsize.y;

    // The .fsl flips m_ddy for HLSL-style top-down framebuffer space. GLSL's
    // gl_FragCoord matches the same orientation (origin top-left after the
    // Vulkan viewport flip in this codebase), so keep the flip.
    ret.m_ddy *= -1.0;
    ddySum    *= -1.0;

    // Projected-gradient correction so the derivatives match what the
    // hardware rasterizer would compute. Same algebra as the .fsl source.
    float interpW_ddx = 1.0 / (interpInvW + ddxSum);
    float interpW_ddy = 1.0 / (interpInvW + ddySum);

    ret.m_ddx = interpW_ddx * (ret.m_lambda * interpInvW + ret.m_ddx) - ret.m_lambda;
    ret.m_ddy = interpW_ddy * (ret.m_lambda * interpInvW + ret.m_ddy) - ret.m_lambda;

    return ret;
}

float InterpolateWithDeriv_float3(BarycentricDeriv deriv, vec3 v)
{
    return dot(v, deriv.m_lambda);
}

// Interpolate three per-vertex vec3 attributes (rows of the 3x3 matrix).
// `mat3` columns in GLSL — pack attributes by ROW at the call site
// (mat3(row0, row1, row2)) to match the .fsl's f3x3-rows semantics.
vec3 InterpolateWithDeriv_float3x3_rows(BarycentricDeriv deriv,
                                        vec3 a0, vec3 a1, vec3 a2)
{
    // We want per-component dot(attr[ch], lambda) where attr[ch] = (a0[ch], a1[ch], a2[ch]).
    vec3 col0 = vec3(a0.x, a1.x, a2.x);
    vec3 col1 = vec3(a0.y, a1.y, a2.y);
    vec3 col2 = vec3(a0.z, a1.z, a2.z);
    return vec3(dot(col0, deriv.m_lambda),
                dot(col1, deriv.m_lambda),
                dot(col2, deriv.m_lambda));
}

// Interpolate a 2D attribute (e.g. UV) plus its ddx/ddy for textureGrad.
// `a0`, `a1`, `a2` are the per-vertex UVs.
GradientInterpolationResults Interpolate2DWithDeriv(BarycentricDeriv deriv,
                                                    vec2 a0, vec2 a1, vec2 a2)
{
    // Row-of-x / row-of-y across the 3 vertices.
    vec3 attrX = vec3(a0.x, a1.x, a2.x);
    vec3 attrY = vec3(a0.y, a1.y, a2.y);

    GradientInterpolationResults result;
    result.interp.x = InterpolateWithDeriv_float3(deriv, attrX);
    result.interp.y = InterpolateWithDeriv_float3(deriv, attrY);

    result.dx.x = dot(attrX, deriv.m_ddx);
    result.dx.y = dot(attrY, deriv.m_ddx);
    result.dy.x = dot(attrX, deriv.m_ddy);
    result.dy.y = dot(attrY, deriv.m_ddy);
    return result;
}

float depthLinearization(float depth, float near, float far)
{
    return (2.0 * near) / (far + near - depth * (far - near));
}

#endif // HAWKINS_GLSL

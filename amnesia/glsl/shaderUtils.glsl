float hash13(vec3 p3)
{
    p3 = fract(p3 * .1031);
    p3 += dot(p3, p3.zyx + 31.32);
    return fract((p3.x + p3.y) * p3.z);
}
 
vec3 hash3u1(uint n)
{
    n = (n << 13U) ^ n;
    n = n * (n * n * 15731U + 789221U) + 1376312589U;
    uvec3 k = n * uvec3(n, n * 16807U, n * 48271U);
    return vec3(k & uvec3(0x7fffffffU)) / float(0x7fffffff);
}

vec3 WorldPosFromDepth(in vec2 uv, in float depth, in mat4 invProj, in mat4 invView)
{
    float z = depth;
    uv.y = 1.0 - uv.y;
    vec4 clipSpacePosition = vec4(uv * 2.0 - 1.0, z, 1.0);
    vec4 viewSpacePosition = invProj * clipSpacePosition;
	viewSpacePosition /= viewSpacePosition.w;
    vec4 worldSpacePosition = invView * viewSpacePosition;
	worldSpacePosition /= worldSpacePosition.w;
    return worldSpacePosition.xyz;
}


// Previously this block defined GetState() and selectRandomLight() against
// the GLTF scene model (sceneNodes / geoInfo / lights). The bindless renderer
// doesn't carry that representation, and neither helper had any callers in
// the current shader tree. Removed; see git history if a similar bindless-
// scene helper is needed.
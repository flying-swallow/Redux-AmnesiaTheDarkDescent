#version 460

#extension GL_GOOGLE_include_directive : require

// Procedural fullscreen triangle. Three vertex IDs produce NDC positions
//   id=0 -> (-1, -1)   id=1 -> (-1, 3)   id=2 -> (3, -1)
// covering the entire viewport plus a margin that gets clipped by Vulkan's
// guard band. No vertex/index buffer needed; host issues vkCmdDraw(3, 1, 0, 0).
//
// `screenPosNdc` is the per-fragment NDC position, used by the fragment
// shader's Hawkins barycentric reconstruction (hawkins.glsl::CalcFullBary).

layout(location = 0) out vec2 screenPosNdc;

void main()
{
    vec2 pos;
    pos.x = (gl_VertexIndex == 2) ?  3.0 : -1.0;
    pos.y = (gl_VertexIndex == 1) ?  3.0 : -1.0;
    gl_Position  = vec4(pos, 0.0, 1.0);
    screenPosNdc = pos;
}

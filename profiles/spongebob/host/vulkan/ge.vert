#version 450
// Clip-space Y is flipped at the end so D3D-style +Y-up matches Vulkan's framebuffer.
layout(location = 0) in vec4 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUv;
layout(location = 3) in float inQ;
layout(location = 4) in float inFog;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUv;
layout(location = 2) out float vQ;
layout(location = 3) out float vFog;

layout(std140, set = 0, binding = 0) uniform DrawUniforms {
    vec4 row0;
    vec4 row1;
    vec4 row2;
    vec4 row3;
    vec4 viewZ;
    vec4 uvScaleOffset;
    vec4 fogParameters;
    uvec4 control;
    vec4 colorMul;
    vec4 colorAdd;
    uvec4 pixel0;
    uvec4 pixel1;
} u;

void main() {
    vec4 position;
    vec2 uv;
    float fog;
    if (u.control.x == 1u) {
        vec4 p = inPosition;
        float clipW = dot(u.row3, p);
        if (abs(clipW) < 1.0e-12) clipW = 1.0;
        float clipZ = dot(u.row2, p);
        if (u.control.y == 0u) clipZ = clamp(clipZ, 0.0, clipW);
        position = vec4(dot(u.row0, p), dot(u.row1, p), clipZ, clipW);
        uv = inUv * u.uvScaleOffset.xy + u.uvScaleOffset.zw;
        float viewZ = dot(u.viewZ, p);
        fog = clamp((viewZ + u.fogParameters.x) * u.fogParameters.y, 0.0, 1.0);
    } else if (u.control.x == 2u) {
        float clipW = inPosition.w;
        if (abs(clipW) < 1.0e-12) clipW = 1.0;
        position = vec4(
            (inPosition.x * u.uvScaleOffset.x - 1.0) * clipW,
            (1.0 - inPosition.y * u.uvScaleOffset.y) * clipW,
            clamp(inPosition.z * u.uvScaleOffset.z, 0.0, 1.0) * clipW,
            clipW);
        uv = inUv;
        fog = inFog;
    } else {
        position = inPosition;
        uv = inUv;
        fog = inFog;
    }
    vec4 color = inColor;
    if (u.control.z != 0u) {
        vec4 lit = clamp(color * u.colorMul + u.colorAdd, 0.0, 1.0);
        color = floor(lit * 255.0) * (1.0 / 255.0);
    }
    position.y = -position.y;
    gl_Position = position;
    vColor = color;
    vUv = uv;
    vQ = inQ;
    vFog = fog;
}

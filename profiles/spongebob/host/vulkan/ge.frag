#version 450
layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUv;
layout(location = 2) in float vQ;
layout(location = 3) in float vFog;
layout(location = 0) out vec4 outColor;

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

layout(set = 0, binding = 1) uniform sampler2D sourceTexture;

bool alphaPass(uint fn, uint lhs, uint rhs) {
    switch (fn & 7u) {
    case 0u: return false;
    case 1u: return true;
    case 2u: return lhs == rhs;
    case 3u: return lhs != rhs;
    case 4u: return lhs < rhs;
    case 5u: return lhs <= rhs;
    case 6u: return lhs > rhs;
    case 7u: return lhs >= rhs;
    }
    return true;
}

vec4 applyTextureFunction(vec4 vertex, vec4 texel, uvec4 control, uvec4 envBytes) {
    uint fn = control.x & 7u;
    bool useAlpha = control.y != 0u;
    bool doubleColor = control.z != 0u;
    vec4 result = vertex;
    vec3 env = vec3(envBytes.xyz) / 255.0;
    if (fn == 0u) {
        result.rgb = vertex.rgb * texel.rgb;
        result.a = useAlpha ? vertex.a * texel.a : vertex.a;
    } else if (fn == 1u) {
        float a = useAlpha ? texel.a : 1.0;
        result.rgb = mix(vertex.rgb, texel.rgb, a);
        result.a = vertex.a;
    } else if (fn == 2u) {
        result.rgb = mix(vertex.rgb, env, texel.rgb);
        result.a = useAlpha ? vertex.a * texel.a : vertex.a;
    } else if (fn == 3u) {
        result = texel;
        if (!useAlpha) result.a = vertex.a;
    } else if (fn == 4u) {
        result.rgb = clamp(vertex.rgb + texel.rgb, 0.0, 1.0);
        result.a = useAlpha ? vertex.a * texel.a : vertex.a;
    }
    if (doubleColor) result.rgb = clamp(result.rgb * 2.0, 0.0, 1.0);
    return result;
}

float quantize(float value, float levels) {
    return floor(clamp(value, 0.0, 1.0) * levels + 0.5) / levels;
}

vec4 quantizeFramebuffer(vec4 color, uint format) {
    color = clamp(color, 0.0, 1.0);
    uint kind = format & 3u;
    if (kind == 0u) {
        color.r = quantize(color.r, 31.0);
        color.g = quantize(color.g, 63.0);
        color.b = quantize(color.b, 31.0);
        color.a = 1.0;
    } else if (kind == 1u) {
        color.r = quantize(color.r, 31.0);
        color.g = quantize(color.g, 31.0);
        color.b = quantize(color.b, 31.0);
        color.a = color.a >= 0.5 ? 1.0 : 0.0;
    } else if (kind == 2u) {
        color.r = quantize(color.r, 15.0);
        color.g = quantize(color.g, 15.0);
        color.b = quantize(color.b, 15.0);
        color.a = quantize(color.a, 15.0);
    }
    return color;
}

void main() {
    uvec4 textureControl = uvec4(u.pixel0.y & 0xFFu, (u.pixel0.y >> 8u) & 0xFFu,
                                 (u.pixel0.y >> 16u) & 0xFFu, (u.pixel0.y >> 24u) & 0xFFu);
    uvec4 textureEnv = uvec4(u.pixel0.z & 0xFFu, (u.pixel0.z >> 8u) & 0xFFu,
                             (u.pixel0.z >> 16u) & 0xFFu, 0u);
    uvec4 fogControl = uvec4(u.pixel0.w & 0xFFu, (u.pixel0.w >> 8u) & 0xFFu,
                             (u.pixel0.w >> 16u) & 0xFFu, (u.pixel0.w >> 24u) & 0xFFu);
    uvec4 alphaControl = uvec4(u.pixel0.x & 0xFFu, (u.pixel0.x >> 8u) & 0xFFu,
                               (u.pixel0.x >> 16u) & 0xFFu, (u.pixel0.x >> 24u) & 0xFFu);
    vec4 color = clamp(vColor, 0.0, 1.0);
    if (textureControl.w != 0u) {
        float q = abs(vQ) < 1.0e-20 ? 1.0 : vQ;
        vec4 texel = texture(sourceTexture, vUv / q);
        color = applyTextureFunction(color, texel, textureControl, textureEnv);
    }
    if (fogControl.w != 0u) {
        vec3 fog = vec3(fogControl.xyz) / 255.0;
        color.rgb = mix(fog, color.rgb, clamp(vFog, 0.0, 1.0));
    }
    if (alphaControl.x != 0u) {
        uint a = uint(floor(clamp(color.a, 0.0, 1.0) * 255.0 + 0.5));
        uint mask = alphaControl.w;
        if (!alphaPass(alphaControl.y, a & mask, alphaControl.z & mask)) discard;
    }
    outColor = quantizeFramebuffer(color, u.pixel1.x);
}

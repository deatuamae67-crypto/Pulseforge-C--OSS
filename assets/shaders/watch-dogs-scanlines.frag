#pragma header

uniform float time;
uniform float intensity;
uniform float scanlineDensity;
uniform vec3 tint;

void main() {
    vec2 uv = openfl_TextureCoordv;
    float effect = clamp(intensity, 0.0, 1.0);
    float density = max(scanlineDensity, 1.0);
    float band = step(0.985, fract(uv.y * 23.0 + time * 0.35));
    float displacement = sin(uv.y * 91.0 + time * 11.0) * band
        * 0.006 * effect;
    vec4 source = flixel_texture2D(
        bitmap,
        vec2(clamp(uv.x + displacement, 0.0, 1.0), uv.y)
    );
    float line = 0.94 + 0.06 * sin(uv.y * density * 6.2831853);
    vec3 coolTint = mix(vec3(1.0), max(tint, vec3(0.0)), 0.18 * effect);
    vec3 styled = source.rgb * line * coolTint;
    source.rgb = mix(source.rgb, styled, effect);
    gl_FragColor = source;
}

#pragma header

uniform float amount;

void main() {
    vec2 uv = openfl_TextureCoordv;
    float offset = clamp(amount, 0.0, 1.0) * 0.012;
    vec4 center = flixel_texture2D(bitmap, uv);
    float red = flixel_texture2D(
        bitmap,
        vec2(clamp(uv.x + offset, 0.0, 1.0), uv.y)
    ).r;
    float blue = flixel_texture2D(
        bitmap,
        vec2(clamp(uv.x - offset, 0.0, 1.0), uv.y)
    ).b;
    gl_FragColor = vec4(red, center.g, blue, center.a);
}

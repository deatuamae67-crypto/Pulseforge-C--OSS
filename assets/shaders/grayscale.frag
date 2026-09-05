#pragma header

uniform float intensity;

void main() {
    vec4 source = flixel_texture2D(bitmap, openfl_TextureCoordv);
    float luminance = dot(source.rgb, vec3(0.2126, 0.7152, 0.0722));
    float blendAmount = clamp(intensity, 0.0, 1.0);
    source.rgb = mix(source.rgb, vec3(luminance), blendAmount);
    gl_FragColor = source;
}

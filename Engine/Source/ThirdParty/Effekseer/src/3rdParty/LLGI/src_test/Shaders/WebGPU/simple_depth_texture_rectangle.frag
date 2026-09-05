diagnostic(off, derivative_uniformity);

@group(1) @binding(0) var txt : texture_depth_2d;

@fragment
fn main(@builtin(position) position : vec4<f32>, @location(0u) uv : vec2<f32>, @location(1u) color : vec4<f32>) -> @location(0u) vec4<f32> {
  let size = textureDimensions(txt);
  let coord = vec2<i32>(clamp(vec2<u32>(uv * vec2<f32>(size)), vec2<u32>(0u), size - vec2<u32>(1u)));
  let depth = textureLoad(txt, coord, 0);
  return vec4<f32>(depth, 0.0, 0.0, 1.0);
}

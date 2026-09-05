diagnostic(off, derivative_uniformity);

struct CB {
  offset : vec4<f32>,
}

@group(0) @binding(1) var<uniform> v : CB;

var<private> v_1 : vec4<f32>;

struct PS_INPUT {
  Position : vec4<f32>,
  UV : vec2<f32>,
  Color : vec4<f32>,
}

fn main_inner(v_2 : vec4<f32>, v_3 : vec2<f32>, v_4 : vec4<f32>) {
  var input : PS_INPUT;
  var param : PS_INPUT;
  input.Position = v_2;
  input.UV = v_3;
  input.Color = v_4;
  param = input;
  v_1 = v_5(&(param));
}

fn v_5(input : ptr<function, PS_INPUT>) -> vec4<f32> {
  var c : vec4<f32>;
  c = ((*(input)).Color + v.offset);
  c.w = 1.0f;
  return c;
}

@fragment
fn main(@builtin(position) v_6 : vec4<f32>, @location(0u) v_7 : vec2<f32>, @location(1u) v_8 : vec4<f32>) -> @location(0u) vec4<f32> {
  main_inner(v_6, v_7, v_8);
  return v_1;
}

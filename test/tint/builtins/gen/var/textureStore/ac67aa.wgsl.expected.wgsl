@group(1) @binding(0) var arg_0 : texture_storage_3d<rg32uint, write>;

fn textureStore_ac67aa() {
  var arg_1 = vec3<i32>();
  var arg_2 = vec4<u32>();
  textureStore(arg_0, arg_1, arg_2);
}

@vertex
fn vertex_main() -> @builtin(position) vec4<f32> {
  textureStore_ac67aa();
  return vec4<f32>();
}

@fragment
fn fragment_main() {
  textureStore_ac67aa();
}

@compute @workgroup_size(1)
fn compute_main() {
  textureStore_ac67aa();
}
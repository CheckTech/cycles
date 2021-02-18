/*
 * Copyright 2011-2013 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

CCL_NAMESPACE_BEGIN

/* Texture Coordinate Node */

/* wcs_box_coord gives a Rhino-style WCS box texture coordinate mapping. */
ccl_device_inline void wcs_box_coord(KernelGlobals *kg, ShaderData *sd, float3 *data)
{
  float3 N = sd->N;
  if (sd->object != OBJECT_NONE && kernel_tex_fetch(__objects, sd->object).use_ocs_frame>0)  {
    Transform tfm = kernel_tex_fetch(__objects, sd->object).ocs_frame;
    *data = transform_point(&tfm, *data);
  }

  int side0 = 0;

  float dx = (*data).x;
  float dy = (*data).y;

  // set side0 = side closest to the point
  int side1 = (fabsf(dx) >= fabsf(dy)) ? 0 : 1;
  float rr = side1 ? dy : dx;
  if (fabsf((*data).z) > fabsf(rr))
    side1 = 2;

  float t1 = side1 ? dy : dx;
  if (t1 < 0.0f)
    side0 = 2 * side1 + 1;
  else
    side0 = 2 * side1 + 2;

  side1 = (fabsf(N.x) >= fabsf(N.y)) ? 0 : 1;
  rr = side1 ? N.y : N.x;
  if (fabsf(N.z) > fabsf(rr)) {
    side1 = 2;
  }

  switch (side1) {
    case 0: {
      t1 = N.x;
      break;
    }
    case 1: {
      t1 = N.y;
      break;
    }
    default: {
      t1 = N.z;
      break;
    }
  }
  if (0.0f != t1) {
    if (t1 < 0.0f)
      side0 = 2 * side1 + 1;
    else if (t1 > 0.0f)
      side0 = 2 * side1 + 2;
  }

  // side flag
  //  1 =  left side (x=-1)
  //  2 =  right side (x=+1)
  //  3 =  back side (y=-1)
  //  4 =  front side (y=+1)
  //  5 =  bottom side (z=-1)
  //  6 =  top side (z=+1)
  float3 v = make_float3(0.0f, 0.0f, 0.0f);
  switch (side0) {
    case 1:
      v.x = -(*data).y;
      v.y = (*data).z;
      v.z = (*data).x;
      break;
    case 2:
      v.x = (*data).y;
      v.y = (*data).z;
      v.z = (*data).x;
      break;
    case 3:
      v.x = (*data).x;
      v.y = (*data).z;
      v.z = (*data).y;
      break;
    case 4:
      v.x = -(*data).x;
      v.y = (*data).z;
      v.z = (*data).y;
      break;
    case 5:
      v.x = -(*data).x;
      v.y = (*data).y;
      v.z = (*data).z;
      break;
    case 6:
    default:
      v.x = (*data).x;
      v.y = (*data).y;
      v.z = (*data).z;
      break;
  }

  *data = v;
}

ccl_device_inline float3 get_reflected_incoming_ray(KernelGlobals *kg, ShaderData *sd)
{
  float3 n = sd->N;
  float3 i = sd->I;

  float3 refl = 2 * n * dot(i, n) - i;

  refl = normalize(refl);

  return refl;
}

ccl_device void svm_node_tex_coord(
    KernelGlobals *kg, ShaderData *sd, int path_flag, float *stack, uint4 node, int *offset)
{
  float3 data;
  uint type = node.y;
  uint out_offset = node.z;

  switch (type) {
    case NODE_TEXCO_OBJECT: {
      data = sd->P;
      if (sd->object != OBJECT_NONE && kernel_tex_fetch(__objects, sd->object).use_ocs_frame>0)  {
        Transform tfm = kernel_tex_fetch(__objects, sd->object).ocs_frame;
        data = transform_point(&tfm, data);
      }
      if (node.w == 0) {
        if (sd->object != OBJECT_NONE) {
          object_inverse_position_transform(kg, sd, &data);
        }
      }
      else {
        Transform tfm;
        tfm.x = read_node_float(kg, offset);
        tfm.y = read_node_float(kg, offset);
        tfm.z = read_node_float(kg, offset);
        data = transform_point(&tfm, data);
        //data = transform_direction(&tfm, data);
      }
      break;
    }
    case NODE_TEXCO_WCS_BOX: {
      data = sd->P;
      if (node.w == 0) {
        if (sd->object != OBJECT_NONE) {
          object_inverse_position_transform(kg, sd, &data);
        }
      }
      else {
        Transform tfm;
        tfm.x = read_node_float(kg, offset);
        tfm.y = read_node_float(kg, offset);
        tfm.z = read_node_float(kg, offset);
        data = transform_direction(&tfm, data);
      }
      wcs_box_coord(kg, sd, &data);
      break;
    }
    case NODE_TEXCO_NORMAL: {
      data = sd->N;
      object_inverse_normal_transform(kg, sd, &data);
      break;
    }
    case NODE_TEXCO_CAMERA: {
      Transform tfm = kernel_data.cam.worldtocamera;

      if (sd->object != OBJECT_NONE)
        data = transform_direction(&tfm, sd->P); //data = transform_point(&tfm, sd->P);
      else
        data = transform_direction(&tfm, sd->P + camera_position(kg)); //data = transform_point(&tfm, sd->P + camera_position(kg));
      break;
    }
    case NODE_TEXCO_WINDOW: {
      if ((path_flag & PATH_RAY_CAMERA) && sd->object == OBJECT_NONE &&
          kernel_data.cam.type == CAMERA_ORTHOGRAPHIC)
        data = camera_world_to_ndc(kg, sd, sd->ray_P);
      else
        data = camera_world_to_ndc(kg, sd, sd->P);
      data.z = 0.0f;
      break;
    }
    case NODE_TEXCO_REFLECTION: {
      if (sd->object != OBJECT_NONE)
        data = 2.0f * dot(sd->N, sd->I) * sd->N - sd->I;
      else
        data = sd->I;
      break;
    }
    case NODE_TEXCO_DUPLI_GENERATED: {
      data = object_dupli_generated(kg, sd->object);
      break;
    }
    case NODE_TEXCO_DUPLI_UV: {
      data = object_dupli_uv(kg, sd->object);
      break;
    }
    case NODE_TEXCO_VOLUME_GENERATED: {
      data = sd->P;

#ifdef __VOLUME__
      if (sd->object != OBJECT_NONE)
        data = volume_normalized_position(kg, sd, data);
#endif
      break;
    }
    case NODE_TEXCO_ENV_SPHERICAL: {
      data = get_reflected_incoming_ray(kg, sd);
      data = make_float3(data.y, -data.z, -data.x);
      data = env_spherical(data);
      break;
    }
    case NODE_TEXCO_ENV_EMAP: {
      data = get_reflected_incoming_ray(kg, sd);
      data = env_emap_act(data);
      break;
    }
    case NODE_TEXCO_ENV_BOX: {
      data = get_reflected_incoming_ray(kg, sd);
      data = env_box(data);
      break;
    }
    case NODE_TEXCO_ENV_LIGHTPROBE: {
      data = get_reflected_incoming_ray(kg, sd);
      data = env_light_probe(data);
      break;
    }
    case NODE_TEXCO_ENV_CUBEMAP: {
      data = get_reflected_incoming_ray(kg, sd);
      data = env_cubemap(data);
      break;
    }
    case NODE_TEXCO_ENV_CUBEMAP_VERTICAL_CROSS: {
      data = get_reflected_incoming_ray(kg, sd);
      data = env_cubemap_vertical_cross(data);
      break;
    }
    case NODE_TEXCO_ENV_CUBEMAP_HORIZONTAL_CROSS: {
      data = get_reflected_incoming_ray(kg, sd);
      data = env_cubemap_horizontal_cross(data);
      break;
    }
    case NODE_TEXCO_ENV_HEMI: {
      data = get_reflected_incoming_ray(kg, sd);
      data = make_float3(data.y, -data.z, -data.x);
      data = env_hemispherical(data);
      break;
    }
  }

  stack_store_float3(stack, out_offset, data);
}

ccl_device void svm_node_tex_coord_bump_dx(
    KernelGlobals *kg, ShaderData *sd, int path_flag, float *stack, uint4 node, int *offset)
{
#ifdef __RAY_DIFFERENTIALS__
  float3 data;
  uint type = node.y;
  uint out_offset = node.z;

  switch (type) {
    case NODE_TEXCO_OBJECT: {
      data = sd->P + sd->dP.dx;
      if (sd->object != OBJECT_NONE && kernel_tex_fetch(__objects, sd->object).use_ocs_frame>0)  {
        Transform tfm = kernel_tex_fetch(__objects, sd->object).ocs_frame;
        data = transform_point(&tfm, data);
      }
      if (node.w == 0) {
        if (sd->object != OBJECT_NONE) {
          object_inverse_position_transform(kg, sd, &data);
        }
      }
      else {
        Transform tfm;
        tfm.x = read_node_float(kg, offset);
        tfm.y = read_node_float(kg, offset);
        tfm.z = read_node_float(kg, offset);
        data = transform_point(&tfm, data);
        //data = transform_direction(&tfm, data);
      }
      break;
    }
    case NODE_TEXCO_WCS_BOX: {
      data = sd->P + sd->dP.dx;
      if (node.w == 0) {
        if (sd->object != OBJECT_NONE) {
          object_inverse_position_transform(kg, sd, &data);
        }
      }
      else {
        Transform tfm;
        tfm.x = read_node_float(kg, offset);
        tfm.y = read_node_float(kg, offset);
        tfm.z = read_node_float(kg, offset);
        data = transform_direction(&tfm, data);
      }
      wcs_box_coord(kg, sd, &data);
      break;
    }
    case NODE_TEXCO_NORMAL: {
      data = sd->N;
      object_inverse_normal_transform(kg, sd, &data);
      break;
    }
    case NODE_TEXCO_CAMERA: {
      Transform tfm = kernel_data.cam.worldtocamera;

      if (sd->object != OBJECT_NONE)
        data = transform_direction(&tfm, sd->P + sd->dP.dx); //data = transform_point(&tfm, sd->P + sd->dP.dx);
      else
        data = transform_direction(&tfm, sd->P + sd->dP.dx + camera_position(kg)); //data = transform_point(&tfm, sd->P + sd->dP.dx + camera_position(kg));
      break;
    }
    case NODE_TEXCO_WINDOW: {
      if ((path_flag & PATH_RAY_CAMERA) && sd->object == OBJECT_NONE &&
          kernel_data.cam.type == CAMERA_ORTHOGRAPHIC)
        data = camera_world_to_ndc(kg, sd, sd->ray_P + sd->ray_dP.dx);
      else
        data = camera_world_to_ndc(kg, sd, sd->P + sd->dP.dx);
      data.z = 0.0f;
      break;
    }
    case NODE_TEXCO_REFLECTION: {
      if (sd->object != OBJECT_NONE)
        data = 2.0f * dot(sd->N, sd->I) * sd->N - sd->I;
      else
        data = sd->I;
      break;
    }
    case NODE_TEXCO_DUPLI_GENERATED: {
      data = object_dupli_generated(kg, sd->object);
      break;
    }
    case NODE_TEXCO_DUPLI_UV: {
      data = object_dupli_uv(kg, sd->object);
      break;
    }
    case NODE_TEXCO_VOLUME_GENERATED: {
      data = sd->P + sd->dP.dx;

#  ifdef __VOLUME__
      if (sd->object != OBJECT_NONE)
        data = volume_normalized_position(kg, sd, data);
#  endif
      break;
    }
  }

  stack_store_float3(stack, out_offset, data);
#else
  svm_node_tex_coord(kg, sd, path_flag, stack, node, offset);
#endif
}

ccl_device void svm_node_tex_coord_bump_dy(
    KernelGlobals *kg, ShaderData *sd, int path_flag, float *stack, uint4 node, int *offset)
{
#ifdef __RAY_DIFFERENTIALS__
  float3 data;
  uint type = node.y;
  uint out_offset = node.z;

  switch (type) {
    case NODE_TEXCO_OBJECT: {
      data = sd->P + sd->dP.dy;
      if (sd->object != OBJECT_NONE && kernel_tex_fetch(__objects, sd->object).use_ocs_frame>0)  {
        Transform tfm = kernel_tex_fetch(__objects, sd->object).ocs_frame;
        data = transform_point(&tfm, data);
      }
      if (node.w == 0) {
        if (sd->object != OBJECT_NONE) {
          object_inverse_position_transform(kg, sd, &data);
        }
      }
      else {
        Transform tfm;
        tfm.x = read_node_float(kg, offset);
        tfm.y = read_node_float(kg, offset);
        tfm.z = read_node_float(kg, offset);
        data = transform_point(&tfm, data);
        //data = transform_direction(&tfm, data);
      }
      break;
    }
    case NODE_TEXCO_WCS_BOX: {
      data = sd->P + sd->dP.dy;
      if (node.w == 0) {
        if (sd->object != OBJECT_NONE) {
          object_inverse_position_transform(kg, sd, &data);
        }
      }
      else {
        Transform tfm;
        tfm.x = read_node_float(kg, offset);
        tfm.y = read_node_float(kg, offset);
        tfm.z = read_node_float(kg, offset);
        data = transform_direction(&tfm, data);
      }
      wcs_box_coord(kg, sd, &data);
      break;
    }
    case NODE_TEXCO_NORMAL: {
      data = sd->N;
      object_inverse_normal_transform(kg, sd, &data);
      break;
    }
    case NODE_TEXCO_CAMERA: {
      Transform tfm = kernel_data.cam.worldtocamera;

      if (sd->object != OBJECT_NONE)
        data = transform_direction(&tfm, sd->P + sd->dP.dy); //data = transform_point(&tfm, sd->P + sd->dP.dy);
      else
        data = transform_direction(&tfm, sd->P + sd->dP.dy + camera_position(kg)); //data = transform_point(&tfm, sd->P + sd->dP.dy + camera_position(kg));
      break;
    }
    case NODE_TEXCO_WINDOW: {
      if ((path_flag & PATH_RAY_CAMERA) && sd->object == OBJECT_NONE &&
          kernel_data.cam.type == CAMERA_ORTHOGRAPHIC)
        data = camera_world_to_ndc(kg, sd, sd->ray_P + sd->ray_dP.dy);
      else
        data = camera_world_to_ndc(kg, sd, sd->P + sd->dP.dy);
      data.z = 0.0f;
      break;
    }
    case NODE_TEXCO_REFLECTION: {
      if (sd->object != OBJECT_NONE)
        data = 2.0f * dot(sd->N, sd->I) * sd->N - sd->I;
      else
        data = sd->I;
      break;
    }
    case NODE_TEXCO_DUPLI_GENERATED: {
      data = object_dupli_generated(kg, sd->object);
      break;
    }
    case NODE_TEXCO_DUPLI_UV: {
      data = object_dupli_uv(kg, sd->object);
      break;
    }
    case NODE_TEXCO_VOLUME_GENERATED: {
      data = sd->P + sd->dP.dy;

#  ifdef __VOLUME__
      if (sd->object != OBJECT_NONE)
        data = volume_normalized_position(kg, sd, data);
#  endif
      break;
    }
    case NODE_TEXCO_ENV_SPHERICAL: {
      data = get_reflected_incoming_ray(kg, sd);
      data = make_float3(data.y, -data.z, -data.x);
      data = env_spherical(data);
      break;
    }
    case NODE_TEXCO_ENV_EMAP: {
      data = get_reflected_incoming_ray(kg, sd);
      data = make_float3(-data.z, data.x, -data.y);
      Transform tfm = kernel_data.cam.worldtocamera;
      data = transform_direction(&tfm, data);
      data = env_world_emap(data);
      break;
    }
    case NODE_TEXCO_ENV_LIGHTPROBE: {
      data = get_reflected_incoming_ray(kg, sd);
      data = env_light_probe( data );
      break;
    }
    case NODE_TEXCO_ENV_CUBEMAP: {
      data = get_reflected_incoming_ray(kg, sd);
      data = env_cubemap( data );
      break;
    }
    case NODE_TEXCO_ENV_CUBEMAP_VERTICAL_CROSS: {
      data = get_reflected_incoming_ray(kg, sd);
      data = env_cubemap_vertical_cross( data );
      break;
    }
    case NODE_TEXCO_ENV_CUBEMAP_HORIZONTAL_CROSS: {
      data = get_reflected_incoming_ray(kg, sd);
      data = env_cubemap_horizontal_cross( data );
      break;
    }
    case NODE_TEXCO_ENV_HEMI: {
      data = get_reflected_incoming_ray(kg, sd);
      data = make_float3(data.y, -data.z, -data.x);
      data = env_hemispherical( data );
      break;
    }
  }

  stack_store_float3(stack, out_offset, data);
#else
  svm_node_tex_coord(kg, sd, path_flag, stack, node, offset);
#endif
}

ccl_device void svm_node_normal_map(KernelGlobals *kg, ShaderData *sd, float *stack, uint4 node)
{
  uint color_offset, strength_offset, normal_offset, space;
  svm_unpack_node_uchar4(node.y, &color_offset, &strength_offset, &normal_offset, &space);

  float3 color = stack_load_float3(stack, color_offset);
  color = 2.0f * make_float3(color.x - 0.5f, color.y - 0.5f, color.z - 0.5f);

  bool is_backfacing = (sd->flag & SD_BACKFACING) != 0;
  float3 N;

  if (space == NODE_NORMAL_MAP_TANGENT) {
    /* tangent space */
    if (sd->object == OBJECT_NONE) {
      stack_store_float3(stack, normal_offset, make_float3(0.0f, 0.0f, 0.0f));
      return;
    }

    /* first try to get tangent attribute */
    const AttributeDescriptor attr = find_attribute(kg, sd, node.z);
    const AttributeDescriptor attr_sign = find_attribute(kg, sd, node.w);
    const AttributeDescriptor attr_normal = find_attribute(kg, sd, ATTR_STD_VERTEX_NORMAL);

    if (attr.offset == ATTR_STD_NOT_FOUND || attr_sign.offset == ATTR_STD_NOT_FOUND ||
        attr_normal.offset == ATTR_STD_NOT_FOUND) {
      stack_store_float3(stack, normal_offset, make_float3(0.0f, 0.0f, 0.0f));
      return;
    }

    /* get _unnormalized_ interpolated normal and tangent */
    float3 tangent = primitive_surface_attribute_float3(kg, sd, attr, NULL, NULL);
    float sign = primitive_surface_attribute_float(kg, sd, attr_sign, NULL, NULL);
    float3 normal;

    if (sd->shader & SHADER_SMOOTH_NORMAL) {
      normal = primitive_surface_attribute_float3(kg, sd, attr_normal, NULL, NULL);
    }
    else {
      normal = sd->Ng;

      /* the normal is already inverted, which is too soon for the math here */
      if (is_backfacing) {
        normal = -normal;
      }

      object_inverse_normal_transform(kg, sd, &normal);
    }

    /* apply normal map */
    float3 B = sign * cross(normal, tangent);
    N = safe_normalize(color.x * tangent + color.y * B + color.z * normal);

    /* transform to world space */
    object_normal_transform(kg, sd, &N);
  }
  else {
    /* strange blender convention */
    if (space == NODE_NORMAL_MAP_BLENDER_OBJECT || space == NODE_NORMAL_MAP_BLENDER_WORLD) {
      color.y = -color.y;
      color.z = -color.z;
    }

    /* object, world space */
    N = color;

    if (space == NODE_NORMAL_MAP_OBJECT || space == NODE_NORMAL_MAP_BLENDER_OBJECT)
      object_normal_transform(kg, sd, &N);
    else
      N = safe_normalize(N);
  }

  /* invert normal for backfacing polygons */
  if (is_backfacing) {
    N = -N;
  }

  float strength = stack_load_float(stack, strength_offset);

  if (strength != 1.0f) {
    strength = max(strength, 0.0f);
    N = safe_normalize(sd->N + (N - sd->N) * strength);
  }

  N = ensure_valid_reflection(sd->Ng, sd->I, N);

  if (is_zero(N)) {
    N = sd->N;
  }

  stack_store_float3(stack, normal_offset, N);
}

ccl_device void svm_node_tangent(KernelGlobals *kg, ShaderData *sd, float *stack, uint4 node)
{
  uint tangent_offset, direction_type, axis;
  svm_unpack_node_uchar3(node.y, &tangent_offset, &direction_type, &axis);

  float3 tangent;
  float3 attribute_value;
  const AttributeDescriptor desc = find_attribute(kg, sd, node.z);
  if (desc.offset != ATTR_STD_NOT_FOUND) {
    if (desc.type == NODE_ATTR_FLOAT2) {
      float2 value = primitive_surface_attribute_float2(kg, sd, desc, NULL, NULL);
      attribute_value.x = value.x;
      attribute_value.y = value.y;
      attribute_value.z = 0.0f;
    }
    else {
      attribute_value = primitive_surface_attribute_float3(kg, sd, desc, NULL, NULL);
    }
  }

  if (direction_type == NODE_TANGENT_UVMAP) {
    /* UV map */
    if (desc.offset == ATTR_STD_NOT_FOUND)
      tangent = make_float3(0.0f, 0.0f, 0.0f);
    else
      tangent = attribute_value;
  }
  else {
    /* radial */
    float3 generated;

    if (desc.offset == ATTR_STD_NOT_FOUND)
      generated = sd->P;
    else
      generated = attribute_value;

    if (axis == NODE_TANGENT_AXIS_X)
      tangent = make_float3(0.0f, -(generated.z - 0.5f), (generated.y - 0.5f));
    else if (axis == NODE_TANGENT_AXIS_Y)
      tangent = make_float3(-(generated.z - 0.5f), 0.0f, (generated.x - 0.5f));
    else
      tangent = make_float3(-(generated.y - 0.5f), (generated.x - 0.5f), 0.0f);
  }

  object_normal_transform(kg, sd, &tangent);
  tangent = cross(sd->N, normalize(cross(tangent, sd->N)));
  stack_store_float3(stack, tangent_offset, tangent);
}

CCL_NAMESPACE_END

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

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <eh.h>
#endif
#include <exception>

#include "device/device.h"
#include "device/device_intern.h"

#include "util/util_foreach.h"
#include "util/util_half.h"
#include "util/util_logging.h"
#include "util/util_math.h"
#include "util/util_opengl.h"
#include "util/util_time.h"
#include "util/util_system.h"
#include "util/util_types.h"
#include "util/util_vector.h"
#include "util/util_string.h"

CCL_NAMESPACE_BEGIN

bool Device::need_types_update = true;
bool Device::need_devices_update = true;
thread_mutex Device::device_mutex;
vector<DeviceInfo> Device::opencl_devices;
vector<DeviceInfo> Device::cuda_devices;
vector<DeviceInfo> Device::optix_devices;
vector<DeviceInfo> Device::cpu_devices;
vector<DeviceInfo> Device::network_devices;
uint Device::devices_initialized_mask = 0;

/* Device Requested Features */

std::ostream &operator<<(std::ostream &os, const DeviceRequestedFeatures &requested_features)
{
  os << "Experimental features: " << (requested_features.experimental ? "On" : "Off") << std::endl;
  os << "Max nodes group: " << requested_features.max_nodes_group << std::endl;
  /* TODO(sergey): Decode bitflag into list of names. */
  os << "Nodes features: " << requested_features.nodes_features << std::endl;
  os << "Use Hair: " << string_from_bool(requested_features.use_hair) << std::endl;
  os << "Use Object Motion: " << string_from_bool(requested_features.use_object_motion)
     << std::endl;
  os << "Use Camera Motion: " << string_from_bool(requested_features.use_camera_motion)
     << std::endl;
  os << "Use Baking: " << string_from_bool(requested_features.use_baking) << std::endl;
  os << "Use Subsurface: " << string_from_bool(requested_features.use_subsurface) << std::endl;
  os << "Use Volume: " << string_from_bool(requested_features.use_volume) << std::endl;
  os << "Use Branched Integrator: " << string_from_bool(requested_features.use_integrator_branched)
     << std::endl;
  os << "Use Patch Evaluation: " << string_from_bool(requested_features.use_patch_evaluation)
     << std::endl;
  os << "Use Transparent Shadows: " << string_from_bool(requested_features.use_transparent)
     << std::endl;
  os << "Use Principled BSDF: " << string_from_bool(requested_features.use_principled)
     << std::endl;
  os << "Use Denoising: " << string_from_bool(requested_features.use_denoising) << std::endl;
  os << "Use Displacement: " << string_from_bool(requested_features.use_true_displacement)
     << std::endl;
  os << "Use Background Light: " << string_from_bool(requested_features.use_background_light)
     << std::endl;
  return os;
}

/* Device */

Device::~Device()
{
  if (!background) {
    if (vertex_buffer != 0) {
      glDeleteBuffers(1, &vertex_buffer);
    }
    if (fallback_shader_program != 0) {
      glDeleteProgram(fallback_shader_program);
    }
  }
}

/* TODO move shaders to standalone .glsl file. */
const char *FALLBACK_VERTEX_SHADER =
    "#version 330\n"
    "uniform vec2 fullscreen;\n"
    "in vec2 texCoord;\n"
    "in vec2 pos;\n"
    "out vec2 texCoord_interp;\n"
    "\n"
    "vec2 normalize_coordinates()\n"
    "{\n"
    "   return (vec2(2.0) * (pos / fullscreen)) - vec2(1.0);\n"
    "}\n"
    "\n"
    "void main()\n"
    "{\n"
    "   gl_Position = vec4(normalize_coordinates(), 0.0, 1.0);\n"
    "   texCoord_interp = texCoord;\n"
    "}\n\0";

const char *FALLBACK_FRAGMENT_SHADER =
    "#version 330\n"
    "uniform sampler2D image_texture;\n"
    "in vec2 texCoord_interp;\n"
    "out vec4 fragColor;\n"
    "\n"
    "void main()\n"
    "{\n"
    "   fragColor = texture(image_texture, texCoord_interp);\n"
    "}\n\0";

static void shader_print_errors(const char *task, const char *log, const char *code)
{
  LOG(ERROR) << "Shader: " << task << " error:";
  LOG(ERROR) << "===== shader string ====";

  stringstream stream(code);
  string partial;

  int line = 1;
  while (getline(stream, partial, '\n')) {
    if (line < 10) {
      LOG(ERROR) << " " << line << " " << partial;
    }
    else {
      LOG(ERROR) << line << " " << partial;
    }
    line++;
  }
  LOG(ERROR) << log;
}

static int bind_fallback_shader(void)
{
  GLint status;
  GLchar log[5000];
  GLsizei length = 0;
  GLuint program = 0;

  struct Shader {
    const char *source;
    GLenum type;
  } shaders[2] = {{FALLBACK_VERTEX_SHADER, GL_VERTEX_SHADER},
                  {FALLBACK_FRAGMENT_SHADER, GL_FRAGMENT_SHADER}};

  program = glCreateProgram();

  for (int i = 0; i < 2; i++) {
    GLuint shader = glCreateShader(shaders[i].type);

    string source_str = shaders[i].source;
    const char *c_str = source_str.c_str();

    glShaderSource(shader, 1, &c_str, NULL);
    glCompileShader(shader);

    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);

    if (!status) {
      glGetShaderInfoLog(shader, sizeof(log), &length, log);
      shader_print_errors("compile", log, c_str);
      return 0;
    }

    glAttachShader(program, shader);
  }

  /* Link output. */
  glBindFragDataLocation(program, 0, "fragColor");

  /* Link and error check. */
  glLinkProgram(program);

  glGetProgramiv(program, GL_LINK_STATUS, &status);
  if (!status) {
    glGetShaderInfoLog(program, sizeof(log), &length, log);
    shader_print_errors("linking", log, FALLBACK_VERTEX_SHADER);
    shader_print_errors("linking", log, FALLBACK_FRAGMENT_SHADER);
    return 0;
  }

  return program;
}

bool Device::bind_fallback_display_space_shader(const float width, const float height)
{
  if (fallback_status == FALLBACK_SHADER_STATUS_ERROR) {
    return false;
  }

  if (fallback_status == FALLBACK_SHADER_STATUS_NONE) {
    fallback_shader_program = bind_fallback_shader();
    fallback_status = FALLBACK_SHADER_STATUS_ERROR;

    if (fallback_shader_program == 0) {
      return false;
    }

    glUseProgram(fallback_shader_program);
    image_texture_location = glGetUniformLocation(fallback_shader_program, "image_texture");
    if (image_texture_location < 0) {
      LOG(ERROR) << "Shader doesn't containt the 'image_texture' uniform.";
      return false;
    }

    fullscreen_location = glGetUniformLocation(fallback_shader_program, "fullscreen");
    if (fullscreen_location < 0) {
      LOG(ERROR) << "Shader doesn't containt the 'fullscreen' uniform.";
      return false;
    }

    fallback_status = FALLBACK_SHADER_STATUS_SUCCESS;
  }

  /* Run this every time. */
  glUseProgram(fallback_shader_program);
  glUniform1i(image_texture_location, 0);
  glUniform2f(fullscreen_location, width, height);
  return true;
}

void get_glviewport_data(GLint width, GLint height, GLint& vp_offset_x, GLint& vp_offset_y, GLint& vp_width, GLint& vp_height)
{
  GLint vp_dims[4] = { 0 };
  glGetIntegerv(GL_VIEWPORT, vp_dims);

  vp_offset_x = vp_dims[0];
  vp_offset_y = vp_dims[1];
  vp_width    = vp_dims[2];
  vp_height   = vp_dims[3];

  // If the width of the GL_VIEWPORT is smaller than the width of the
  // render, then we are dealing with a Rhino Detail which has been
  // moved partially outside the window.
  if (vp_width < width)
  {
    // If the x offset is 0, it has been clamped to 0 due to being partially outside
    // the window to the left.
    if (vp_offset_x == 0)
    {
      // We want to express the offset as negative coordinates instead of clamping to 0.
      vp_offset_x = vp_width - width;
    }
    // In all cases we want the width to be the render width.
    vp_width = width;
  }

  // See comments above.
  if (vp_height < height)
  {
    if (vp_offset_y == 0)
    {
      vp_offset_y = vp_height - height;
    }
    vp_height = height;
  }
}

void Device::prepare_pixels(device_memory &rgba,
                         int y,
                         int w,
                         int h,
                         int dx,
                         int dy,
                         int width,
                         int height,
                         int full_width,
                         int full_height,
                         bool transparent,
                         const DeviceDrawParams &draw_params)
{
  assert(rgba.type == MEM_PIXELS);
  mem_copy_from(rgba, y, w, h, rgba.memory_elements_size(1));
}

void Device::draw_pixels(device_memory &rgba,
                         int y,
                         int w,
                         int h,
                         int dx,
                         int dy,
                         int width,
                         int height,
                         int full_width,
                         int full_height,
                         bool transparent,
                         const DeviceDrawParams &draw_params)
{
  //const bool use_fallback_shader = (draw_params.bind_display_space_shader_cb == NULL);

  assert(rgba.type == MEM_PIXELS);
  mem_copy_from(rgba, y, w, h, rgba.memory_elements_size(1));

  if (transparent) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  }


  GLuint texid;
  glGenTextures(1, &texid);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, texid);

  if (rgba.data_type == TYPE_HALF) {
    GLhalf *data_pointer = (GLhalf *)rgba.host_pointer;
    data_pointer += 4 * y * w;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F_ARB, w, h, 0, GL_RGBA, GL_HALF_FLOAT, data_pointer);
  }
  else {
    uint8_t *data_pointer = (uint8_t *)rgba.host_pointer;
    data_pointer += 4 * y * w;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data_pointer);
  }

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

#if OLDSTUFF
  GLint shader_program;
  if (use_fallback_shader) {
    if (!bind_fallback_display_space_shader(dw, dh)) {
      return;
    }
    shader_program = fallback_shader_program;
  }
  else {
    draw_params.bind_display_space_shader_cb();
    glGetIntegerv(GL_CURRENT_PROGRAM, &shader_program);
  }

  if (!vertex_buffer) {
    glGenBuffers(1, &vertex_buffer);
  }

  glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
  /* invalidate old contents - avoids stalling if buffer is still waiting in queue to be rendered
   */
  glBufferData(GL_ARRAY_BUFFER, 16 * sizeof(float), NULL, GL_STREAM_DRAW);

  float *vpointer = (float *)glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY);

  if (vpointer) {
    /* texture coordinate - vertex pair */
    vpointer[0] = 0.0f;
    vpointer[1] = 0.0f;
    vpointer[2] = dx;
    vpointer[3] = dy;

    vpointer[4] = 1.0f;
    vpointer[5] = 0.0f;
    vpointer[6] = (float)width + dx;
    vpointer[7] = dy;

    vpointer[8] = 1.0f;
    vpointer[9] = 1.0f;
    vpointer[10] = (float)width + dx;
    vpointer[11] = (float)height + dy;

    vpointer[12] = 0.0f;
    vpointer[13] = 1.0f;
    vpointer[14] = dx;
    vpointer[15] = (float)height + dy;

    if (vertex_buffer) {
      glUnmapBuffer(GL_ARRAY_BUFFER);
    }
  }

  GLuint vertex_array_object;
  GLuint position_attribute, texcoord_attribute;

  glGenVertexArrays(1, &vertex_array_object);
  glBindVertexArray(vertex_array_object);

  texcoord_attribute = glGetAttribLocation(shader_program, "texCoord");
  position_attribute = glGetAttribLocation(shader_program, "pos");

  glEnableVertexAttribArray(texcoord_attribute);
  glEnableVertexAttribArray(position_attribute);

  glVertexAttribPointer(
      texcoord_attribute, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (const GLvoid *)0);
  glVertexAttribPointer(position_attribute,
                        2,
                        GL_FLOAT,
                        GL_FALSE,
                        4 * sizeof(float),
                        (const GLvoid *)(sizeof(float) * 2));

  glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

  if (vertex_buffer) {
    glBindBuffer(GL_ARRAY_BUFFER, 0);
  }

  if (use_fallback_shader) {
    glUseProgram(0);
  }
  else {
    draw_params.unbind_display_space_shader_cb();
  }

  glDeleteVertexArrays(1, &vertex_array_object);
  glBindTexture(GL_TEXTURE_2D, 0);
  glDeleteTextures(1, &texid);
#endif
    // We need to know the GL_VIEWPORT rect information to be able to
    // calculate the correct viewport uv-coordinates when using a Rhino Detail.
    GLint vp_offset_x, vp_offset_y, vp_width, vp_height;
    get_glviewport_data(full_width, full_height, vp_offset_x, vp_offset_y, vp_width, vp_height);

    /* TODO [NATHANLOOK] use bind/unbind shader cbs. */
    GLint tex = glGetUniformLocation(draw_params.program, "tex");
    GLint subsize = glGetUniformLocation(draw_params.program, "subsize");
    GLint alpha = glGetUniformLocation(draw_params.program, "alpha");
    GLint vp_rect = glGetUniformLocation(draw_params.program, "vp_rect");

    glUniform1i(tex, 0);
    glUniform4f(subsize, (float)dx, (float)dy, (float)width, (float)height);
    glUniform1f(alpha, draw_params.alpha);
    glUniform4f(vp_rect, (float)vp_offset_x, (float)vp_offset_y, (float)vp_width, (float)vp_height);

    GLuint temp_vao = 0;
    glGenVertexArrays(1, &temp_vao);
    glBindVertexArray(temp_vao);
    GLuint temp_vbo = 0;
    glGenBuffers(1, &temp_vbo);

    static const float vertices[] = { -1,-1, 1,-1, 1,1, -1,1 };
    glBindBuffer(GL_ARRAY_BUFFER, temp_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 8, vertices, GL_STREAM_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, false, 0, nullptr);

    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDeleteBuffers(1, &temp_vbo);
    glBindVertexArray(0);
    glDeleteVertexArrays(1, &temp_vao);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDeleteTextures(1, &texid);

  if (transparent) {
    glDisable(GL_BLEND);
  }
}

Device *Device::create(DeviceInfo &info, Stats &stats, Profiler &profiler, bool background)
{
  Device *device;

  switch (info.type) {
    case DEVICE_CPU:
      device = device_cpu_create(info, stats, profiler, background);
      break;
#ifdef WITH_CUDA
    case DEVICE_CUDA:
      if (device_cuda_init())
        device = device_cuda_create(info, stats, profiler, background);
      else
        device = NULL;
      break;
#endif
#ifdef WITH_OPTIX
    case DEVICE_OPTIX:
      if (device_optix_init())
        device = device_optix_create(info, stats, profiler, background);
      else
        device = NULL;
      break;
#endif
#ifdef WITH_MULTI
    case DEVICE_MULTI:
      device = device_multi_create(info, stats, profiler, background);
      break;
#endif
#ifdef WITH_NETWORK
    case DEVICE_NETWORK:
      device = device_network_create(info, stats, profiler, "127.0.0.1");
      break;
#endif
#ifdef WITH_OPENCL
    case DEVICE_OPENCL:
      if (device_opencl_init())
        device = device_opencl_create(info, stats, profiler, background);
      else
        device = NULL;
      break;
#endif
    default:
      return NULL;
  }

  return device;
}

DeviceType Device::type_from_string(const char *name)
{
  if (strcmp(name, "CPU") == 0)
    return DEVICE_CPU;
  else if (strcmp(name, "CUDA") == 0)
    return DEVICE_CUDA;
  else if (strcmp(name, "OPTIX") == 0)
    return DEVICE_OPTIX;
  else if (strcmp(name, "OPENCL") == 0)
    return DEVICE_OPENCL;
  else if (strcmp(name, "NETWORK") == 0)
    return DEVICE_NETWORK;
  else if (strcmp(name, "MULTI") == 0)
    return DEVICE_MULTI;

  return DEVICE_NONE;
}

string Device::string_from_type(DeviceType type)
{
  if (type == DEVICE_CPU)
    return "CPU";
  else if (type == DEVICE_CUDA)
    return "CUDA";
  else if (type == DEVICE_OPTIX)
    return "OPTIX";
  else if (type == DEVICE_OPENCL)
    return "OPENCL";
  else if (type == DEVICE_NETWORK)
    return "NETWORK";
  else if (type == DEVICE_MULTI)
    return "MULTI";

  return "";
}

vector<DeviceType> Device::available_types()
{
  vector<DeviceType> types;
  types.push_back(DEVICE_CPU);
#ifdef WITH_CUDA
  types.push_back(DEVICE_CUDA);
#endif
#ifdef WITH_OPTIX
  types.push_back(DEVICE_OPTIX);
#endif
#ifdef WITH_OPENCL
  types.push_back(DEVICE_OPENCL);
#endif
#ifdef WITH_NETWORK
  types.push_back(DEVICE_NETWORK);
#endif
  return types;
}

class CyclesDriverCrashException : std::exception
{
public:
  CyclesDriverCrashException() : m_nVDE(-1) {}
  CyclesDriverCrashException(unsigned int n) : m_nVDE(n) {}

  unsigned int VDENumber() const { return m_nVDE; }

private:
  unsigned int m_nVDE;
};

#ifdef _WIN32

static
void crash_translator_function(unsigned int eCode, EXCEPTION_POINTERS*)
{
  throw CyclesDriverCrashException(eCode);
}

class CrashTranslatorHelper
{
private:
    const _se_translator_function old_SE_translator;
public:
    CrashTranslatorHelper( _se_translator_function new_SE_translator ) noexcept
        : old_SE_translator{ _set_se_translator( new_SE_translator ) } {}
    ~CrashTranslatorHelper() noexcept { _set_se_translator( old_SE_translator ); }
};
#endif

vector<DeviceInfo> Device::available_devices(uint mask)
{
  /* Lazy initialize devices. On some platforms OpenCL or CUDA drivers can
   * be broken and cause crashes when only trying to get device info, so
   * we don't want to do any initialization until the user chooses to. */
  thread_scoped_lock lock(device_mutex);
  vector<DeviceInfo> devices;

#ifdef _WIN32
  CrashTranslatorHelper se_translator(crash_translator_function);
#endif

  if (mask & DEVICE_MASK_CPU) {
    if (!(devices_initialized_mask & DEVICE_MASK_CPU)) {
      device_cpu_info(cpu_devices);
      devices_initialized_mask |= DEVICE_MASK_CPU;
    }
    foreach (DeviceInfo &info, cpu_devices) {
      devices.push_back(info);
    }
  }

#ifdef WITH_OPENCL
  if (mask & DEVICE_MASK_OPENCL) {
    if (!(devices_initialized_mask & DEVICE_MASK_OPENCL)) {
      try {
        if (device_opencl_init()) {
          device_opencl_info(opencl_devices);
        }
        devices_initialized_mask |= DEVICE_MASK_OPENCL;
        foreach (DeviceInfo &info, opencl_devices) {
          devices.push_back(info);
        }
      }
      catch (CyclesDriverCrashException) {
      }
    }
}
#endif

#ifdef WITH_CUDA
  if (mask & DEVICE_MASK_CUDA) {
    try {
      if (!(devices_initialized_mask & DEVICE_MASK_CUDA)) {
        if (device_cuda_init()) {
          device_cuda_info(cuda_devices);
        }
        devices_initialized_mask |= DEVICE_MASK_CUDA;
      }
      foreach (DeviceInfo &info, cuda_devices) {
        devices.push_back(info);
      }
    }
    catch (CyclesDriverCrashException) {
    }
  }
#endif

#ifdef WITH_OPTIX
  if (mask & DEVICE_MASK_OPTIX) {
    try {
      if (!(devices_initialized_mask & DEVICE_MASK_OPTIX)) {
        if (device_optix_init()) {
          device_optix_info(optix_devices);
        }
        devices_initialized_mask |= DEVICE_MASK_OPTIX;
      }
      foreach (DeviceInfo &info, optix_devices) {
        devices.push_back(info);
      }
    }
    catch (CyclesDriverCrashException) {
    }
  }
#endif

#ifdef WITH_NETWORK
  if (mask & DEVICE_MASK_NETWORK) {
    try {
      if (!(devices_initialized_mask & DEVICE_MASK_NETWORK)) {
        device_network_info(network_devices);
        devices_initialized_mask |= DEVICE_MASK_NETWORK;
      }
      foreach (DeviceInfo &info, network_devices) {
        devices.push_back(info);
      }
    }
    catch (CyclesDriverCrashException) {
    }
  }
#endif

  return devices;
}

string Device::device_capabilities(uint mask)
{
  thread_scoped_lock lock(device_mutex);
  string capabilities = "";

  if (mask & DEVICE_MASK_CPU) {
    capabilities += "\nCPU device capabilities: ";
    capabilities += device_cpu_capabilities() + "\n";
  }

#ifdef WITH_OPENCL
  if (mask & DEVICE_MASK_OPENCL) {
    if (device_opencl_init()) {
      capabilities += "\nOpenCL device capabilities:\n";
      capabilities += device_opencl_capabilities();
    }
  }
#endif

#ifdef WITH_CUDA
  if (mask & DEVICE_MASK_CUDA) {
    if (device_cuda_init()) {
      capabilities += "\nCUDA device capabilities:\n";
      capabilities += device_cuda_capabilities();
    }
  }
#endif

  return capabilities;
}

DeviceInfo Device::get_multi_device(const vector<DeviceInfo> &subdevices,
                                    int threads,
                                    bool background)
{
  assert(subdevices.size() > 0);

  if (subdevices.size() == 1) {
    /* No multi device needed. */
    return subdevices.front();
  }

  DeviceInfo info;
  info.type = DEVICE_MULTI;
  info.id = "MULTI";
  info.description = "Multi Device";
  info.num = 0;

  info.has_half_images = true;
  info.has_volume_decoupled = true;
  info.has_osl = true;
  info.has_profiling = true;

  foreach (const DeviceInfo &device, subdevices) {
    /* Ensure CPU device does not slow down GPU. */
    if (device.type == DEVICE_CPU && subdevices.size() > 1) {
      if (background) {
        int orig_cpu_threads = (threads) ? threads : system_cpu_thread_count();
        int cpu_threads = max(orig_cpu_threads - (subdevices.size() - 1), 0);

        VLOG(1) << "CPU render threads reduced from " << orig_cpu_threads << " to " << cpu_threads
                << ", to dedicate to GPU.";

        if (cpu_threads >= 1) {
          DeviceInfo cpu_device = device;
          cpu_device.cpu_threads = cpu_threads;
          info.multi_devices.push_back(cpu_device);
        }
        else {
          continue;
        }
      }
      else {
        VLOG(1) << "CPU render threads disabled for interactive render.";
        continue;
      }
    }
    else {
      info.multi_devices.push_back(device);
    }

    /* Accumulate device info. */
    info.has_half_images &= device.has_half_images;
    info.has_volume_decoupled &= device.has_volume_decoupled;
    info.has_osl &= device.has_osl;
    info.has_profiling &= device.has_profiling;
  }

  return info;
}

void Device::tag_update()
{
  free_memory();
}

void Device::free_memory()
{
  devices_initialized_mask = 0;
  cuda_devices.free_memory();
  optix_devices.free_memory();
  opencl_devices.free_memory();
  cpu_devices.free_memory();
  network_devices.free_memory();
}

CCL_NAMESPACE_END

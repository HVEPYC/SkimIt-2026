// Headless GLES stand-in for the desktop runner: object names are generated, compiles and links
// succeed, queries return a typical 2013 GLES 2.0 profile, and everything else is a counted no-op.
// The Android build (M4) forwards these to the real GLES instead.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "hle/modules.h"
#include "hle/registry.h"

namespace skim::hle {

namespace {
using u32 = std::uint32_t;
std::atomic<u32> next_name{1};
std::atomic<std::uint64_t> calls{0};

bool trace() {
    static const bool on = std::getenv("SKIM_TRACE_GL") != nullptr;
    return on;
}

void gl_noop(Ctx& c) { ++calls; c.ret(0); }
void gl_gen(u32 n, gaddr out) { ++calls; for (u32 i = 0; i < n; ++i) mem().write<u32>(out + 4 * i, next_name++); }
u32 gl_create() { ++calls; return next_name++; }
u32 gl_create1(u32) { return gl_create(); }
u32 gl_get_error() { return 0; }
u32 gl_check_fb(u32) { return 0x8CD5; }  // GL_FRAMEBUFFER_COMPLETE
// A generic GLES 2.0 device of the game's era.
gaddr gl_get_string(u32 name) {
    static const gaddr vendor = mem().static_str("Generic"), renderer = mem().static_str("GLES 2.0 headless stand-in"),
                       version = mem().static_str("OpenGL ES 2.0"), glsl = mem().static_str("OpenGL ES GLSL ES 1.00"),
                       ext = mem().static_str("GL_OES_rgb8_rgba8 GL_OES_depth24 GL_OES_packed_depth_stencil GL_OES_standard_derivatives "
                                              "GL_OES_compressed_ETC1_RGB8_texture GL_IMG_texture_compression_pvrtc GL_OES_texture_npot "
                                              "GL_OES_mapbuffer GL_OES_element_index_uint GL_EXT_texture_format_BGRA8888 GL_OES_vertex_half_float");
    switch (name) {
    case 0x1F00: return vendor;
    case 0x1F01: return renderer;
    case 0x1F02: return version;
    case 0x1F03: return ext;
    case 0x8B8C: return glsl;
    default: return 0;
    }
}
void gl_get_integerv(u32 pname, gaddr out) {
    ++calls;
    u32 v = 0;
    switch (pname) {
    case 0x0D33: v = 2048; break;   // GL_MAX_TEXTURE_SIZE
    case 0x851C: v = 2048; break;   // GL_MAX_CUBE_MAP_TEXTURE_SIZE
    case 0x8869: v = 8; break;      // GL_MAX_VERTEX_ATTRIBS
    case 0x8872: v = 8; break;      // GL_MAX_TEXTURE_IMAGE_UNITS
    case 0x84E2: v = 8; break;      // GL_MAX_TEXTURE_UNITS (ES1)
    case 0x8B4C: v = 8; break;      // GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS
    case 0x8DFB: v = 128; break;    // GL_MAX_VERTEX_UNIFORM_VECTORS
    case 0x8DFD: v = 64; break;     // GL_MAX_FRAGMENT_UNIFORM_VECTORS
    case 0x8DFC: v = 8; break;      // GL_MAX_VARYING_VECTORS
    case 0x84E8: v = 2048; break;   // GL_MAX_RENDERBUFFER_SIZE
    case 0x0D3A: mem().write<u32>(out + 4, 4096); v = 4096; break;  // GL_MAX_VIEWPORT_DIMS
    case 0x0D52: case 0x0D54: v = 5; break; case 0x0D53: v = 6; break;  // red/blue/green bits (RGB565)
    case 0x0D56: v = 24; break;     // GL_DEPTH_BITS
    case 0x0D57: v = 8; break;      // GL_STENCIL_BITS
    case 0x8CA6: case 0x8CA7: v = 0; break;  // framebuffer / renderbuffer bindings
    default: if (trace()) std::fprintf(stderr, "[gl] glGetIntegerv(0x%x) -> 0\n", pname);
    }
    mem().write<u32>(out, v);
}
void gl_get_status(u32, u32 pname, gaddr out) {  // glGetShaderiv / glGetProgramiv
    ++calls;
    u32 v = 0;
    if (pname == 0x8B81 || pname == 0x8B82 || pname == 0x8B83) v = 1;  // COMPILE / LINK / VALIDATE status
    mem().write<u32>(out, v);
}
void gl_get_log(u32, u32 max, gaddr len, gaddr buf) {
    if (len) mem().write<u32>(len, 0);
    if (buf && max) mem().write<std::uint8_t>(buf, 0);
}
}  // namespace

std::uint64_t gl_call_count() { return calls; }

void register_gles() {
    def_prefix("gl", gl_noop);
    for (const char* n : {"glGenTextures", "glGenBuffers", "glGenFramebuffers", "glGenRenderbuffers", "glGenFramebuffersOES",
                          "glGenRenderbuffersOES", "glGenVertexArraysOES"})
        def(n, wrap<gl_gen>);
    def("glCreateProgram", wrap<gl_create>); def("glCreateShader", wrap<gl_create1>);
    def("glGetError", wrap<gl_get_error>); def("glGetString", wrap<gl_get_string>);
    def("glCheckFramebufferStatus", wrap<gl_check_fb>); def("glCheckFramebufferStatusOES", wrap<gl_check_fb>);
    def("glGetIntegerv", wrap<gl_get_integerv>);
    def("glGetShaderiv", wrap<gl_get_status>); def("glGetProgramiv", wrap<gl_get_status>);
    def("glGetShaderInfoLog", wrap<gl_get_log>); def("glGetProgramInfoLog", wrap<gl_get_log>);
    def("glGetAttribLocation", wrap<gl_create1>);
}

}  // namespace skim::hle

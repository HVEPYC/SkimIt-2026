// GLES for the Android build: libunity's gl* imports forwarded to the device's real GLES 2.0.
// The context itself is the original Java player's (GLSurfaceView creates it on the GL thread,
// which is where Unity calls GL from). Guest pointers become host pointers by adding the arena
// base; the generated wrappers (gles_forward.inc, scripts/gen_gles.py) do that for plain pointer
// arguments, and the functions below handle the cases that need more.
#include <GLES2/gl2.h>
#define GL_GLEXT_PROTOTYPES
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>

#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "hle/modules.h"
#include "hle/registry.h"

namespace skim::hle {

namespace {
using u32 = std::uint32_t;

void* host_ptr(gaddr a) { return a ? mem().ptr(a) : nullptr; }

void gl_es1_noop(Ctx&) {}

// Pointer-or-offset arguments: with a buffer bound, the "pointer" is an offset into it.
// Bindings are tracked here (all GL calls come from the one GL thread).
GLuint array_buffer = 0, element_buffer = 0;

void gl_BindBuffer(Ctx& c) {
    const GLenum target = c.u32();
    const GLuint buf = c.u32();
    if (target == GL_ARRAY_BUFFER) array_buffer = buf;
    else if (target == GL_ELEMENT_ARRAY_BUFFER) element_buffer = buf;
    glBindBuffer(target, buf);
}
void drop_shadow(GLuint buf);
void gl_DeleteBuffers(Ctx& c) {
    const GLsizei n = c.u32();
    const auto* bufs = static_cast<const GLuint*>(host_ptr(c.u32()));
    for (GLsizei i = 0; i < n; ++i) {
        drop_shadow(bufs[i]);
        if (bufs[i] == array_buffer) array_buffer = 0;
        if (bufs[i] == element_buffer) element_buffer = 0;
    }
    glDeleteBuffers(n, bufs);
}
const void* ptr_or_offset(gaddr a, GLuint bound) {
    return bound ? reinterpret_cast<const void*>(static_cast<std::uintptr_t>(a)) : host_ptr(a);
}
void gl_VertexAttribPointer(Ctx& c) {
    const GLuint index = c.u32();
    const GLint size = c.u32();
    const GLenum type = c.u32();
    const GLboolean norm = c.u32();
    const GLsizei stride = c.u32();
    glVertexAttribPointer(index, size, type, norm, stride, ptr_or_offset(c.u32(), array_buffer));
}
void gl_DrawElements(Ctx& c) {
    const GLenum mode = c.u32();
    const GLsizei count = c.u32();
    const GLenum type = c.u32();
    glDrawElements(mode, count, type, ptr_or_offset(c.u32(), element_buffer));
}
void gl_GetVertexAttribPointerv(Ctx& c) {  // only meaningful for offsets; report 0 for client arrays
    const GLuint index = c.u32();
    const GLenum pname = c.u32();
    const gaddr out = c.u32();
    void* p = nullptr;
    glGetVertexAttribPointerv(index, pname, &p);
    mem().write<u32>(out, static_cast<u32>(reinterpret_cast<std::uintptr_t>(p)));
}
void gl_ShaderSource(Ctx& c) {  // an array of guest string pointers
    const GLuint shader = c.u32();
    const GLsizei count = c.u32();
    const gaddr strings = c.u32(), lengths = c.u32();
    std::vector<const GLchar*> s(count);
    for (GLsizei i = 0; i < count; ++i) s[i] = static_cast<const GLchar*>(host_ptr(mem().read<u32>(strings + 4 * i)));
    glShaderSource(shader, count, s.data(), static_cast<const GLint*>(host_ptr(lengths)));
}
void gl_GetString(Ctx& c) {  // copied into guest memory once per name
    static std::mutex lock;
    static std::map<GLenum, gaddr> copies;
    const GLenum name = c.u32();
    std::lock_guard g{lock};
    auto& copy = copies[name];
    if (!copy) {
        const auto* s = reinterpret_cast<const char*>(glGetString(name));
        if (!s) { c.ret(0); return; }
        copy = mem().static_str(s);
        std::fprintf(stderr, "[gl] glGetString(0x%x) = %s\n", name, s);
    }
    c.ret(copy);
}
// glMapBufferOES: the driver's mapping isn't guest-addressable, so each buffer that isn't
// GL_STATIC_DRAW keeps a guest shadow mirroring its contents (glBufferData / glBufferSubData).
// Map returns the shadow (with the buffer's current contents, as a real mapping has), and
// Unmap uploads it. Unity's dynamic VBO maps once and appends batches, so stale or uninitialised
// bytes must never be uploaded over data the GPU still uses.
struct Shadow { gaddr data = 0; GLsizeiptr size = 0; };
std::map<GLuint, Shadow> shadows;
GLuint bound(GLenum target) { return target == GL_ARRAY_BUFFER ? array_buffer : target == GL_ELEMENT_ARRAY_BUFFER ? element_buffer : 0; }
void drop_shadow(GLuint buf) {
    auto it = shadows.find(buf);
    if (it == shadows.end()) return;
    heap::free(it->second.data);
    shadows.erase(it);
}
void gl_BufferData(Ctx& c) {
    const GLenum target = c.u32();
    const GLsizeiptr size = static_cast<std::int32_t>(c.u32());
    const gaddr data = c.u32();
    const GLenum usage = c.u32();
    glBufferData(target, size, host_ptr(data), usage);
    const GLuint buf = bound(target);
    drop_shadow(buf);
    if (!buf || usage == GL_STATIC_DRAW) return;
    Shadow& s = shadows[buf];
    s.size = size;
    s.data = heap::calloc(size ? size : 1, 1);
    if (data) std::memcpy(mem().ptr(s.data), mem().ptr(data), size);
}
void gl_BufferSubData(Ctx& c) {
    const GLenum target = c.u32();
    const GLintptr off = static_cast<std::int32_t>(c.u32());
    const GLsizeiptr size = static_cast<std::int32_t>(c.u32());
    const gaddr data = c.u32();
    glBufferSubData(target, off, size, host_ptr(data));
    auto it = shadows.find(bound(target));
    if (it != shadows.end() && off + size <= it->second.size) std::memcpy(mem().ptr(it->second.data + off), mem().ptr(data), size);
}
void gl_MapBufferOES(Ctx& c) {
    const GLenum target = c.u32();
    c.u32();  // access (GL_WRITE_ONLY_OES)
    const GLuint buf = bound(target);
    if (!buf) { c.ret(0); return; }
    auto it = shadows.find(buf);
    if (it == shadows.end()) {  // a static buffer: start from zeros (contents can't be read back in ES 2.0)
        GLint size = 0;
        glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
        it = shadows.emplace(buf, Shadow{heap::calloc(size ? size : 1, 1), size}).first;
    }
    c.ret(it->second.data);
}
void gl_UnmapBufferOES(Ctx& c) {
    const GLenum target = c.u32();
    auto it = shadows.find(bound(target));
    if (it == shadows.end()) { c.ret(GL_FALSE); return; }
    glBufferSubData(target, 0, it->second.size, host_ptr(it->second.data));
    c.ret(GL_TRUE);
}

#include "hle/gles_forward.inc"
}  // namespace

void register_gles() {
    register_forwarded();
    def("glBindBuffer", gl_BindBuffer); def("glDeleteBuffers", gl_DeleteBuffers);
    def("glVertexAttribPointer", gl_VertexAttribPointer); def("glDrawElements", gl_DrawElements);
    def("glGetVertexAttribPointerv", gl_GetVertexAttribPointerv); def("glShaderSource", gl_ShaderSource);
    def("glGetString", gl_GetString);
    def("glBufferData", gl_BufferData); def("glBufferSubData", gl_BufferSubData);
    def("glMapBufferOES", gl_MapBufferOES); def("glUnmapBufferOES", gl_UnmapBufferOES);
}

}  // namespace skim::hle

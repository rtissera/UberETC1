// gl_decode.c — Headless EGL + GLES2 ETC1 decoder.
// Reads our raw stream format (8-byte hdr: int32 w, int32 h, then bw*bh blocks of 8 bytes),
// uploads as a GL_OES_compressed_ETC1_RGB8_texture, renders a textured fullscreen quad
// to an off-screen FBO, glReadPixels, and writes a PNG. This proves bitstream
// validity for hardware decoders and gives a true GPU-decoded reference image.
//
// Build:
//   gcc -O2 gl_decode.c -lEGL -lGLESv2 -o gl_decode
// Usage:
//   gl_decode in.bin out.png

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#ifndef GL_ETC1_RGB8_OES
#define GL_ETC1_RGB8_OES 0x8D64
#endif

static const char *vsrc =
"attribute vec2 a_pos;"
"attribute vec2 a_uv;"
"varying vec2 v_uv;"
"void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }";

static const char *fsrc =
"precision mediump float;"
"varying vec2 v_uv;"
"uniform sampler2D u_tex;"
"void main() { gl_FragColor = texture2D(u_tex, v_uv); }";

static GLuint mkshader(GLenum t, const char *s) {
    GLuint sh = glCreateShader(t);
    glShaderSource(sh, 1, &s, NULL);
    glCompileShader(sh);
    GLint ok; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]; glGetShaderInfoLog(sh, 1024, NULL, log);
        fprintf(stderr, "shader compile fail: %s\n", log); exit(1);
    }
    return sh;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s in.bin out.png\n", argv[0]); return 1; }

    // --- Load raw ETC1 stream ---
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    int32_t hdr[2];
    if (fread(hdr, sizeof(hdr), 1, f) != 1) { fprintf(stderr, "header read fail\n"); return 1; }
    int W = hdr[0], H = hdr[1];
    int bw = W/4, bh = H/4;
    size_t blocks_size = (size_t)bw*bh*8;
    uint8_t *blocks = malloc(blocks_size);
    if (fread(blocks, 1, blocks_size, f) != blocks_size) { fprintf(stderr, "blocks read fail\n"); return 1; }
    fclose(f);
    fprintf(stderr, "loaded %dx%d ETC1 (%zu bytes)\n", W, H, blocks_size);

    // --- EGL surfaceless setup ---
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major, minor;
    if (!eglInitialize(dpy, &major, &minor)) { fprintf(stderr, "eglInit fail\n"); return 1; }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_BLUE_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_RED_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
    EGLConfig cfg; EGLint nc;
    eglChooseConfig(dpy, cfg_attribs, &cfg, 1, &nc);
    EGLint pb_attribs[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb_attribs);
    EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
    eglMakeCurrent(dpy, surf, surf, ctx);

    // --- Verify ETC1 extension present ---
    const char *exts = (const char*)glGetString(GL_EXTENSIONS);
    if (!strstr(exts, "GL_OES_compressed_ETC1_RGB8_texture") &&
        !strstr(exts, "GL_EXT_compressed_ETC1_RGB8_sub_texture")) {
        fprintf(stderr, "ERROR: GL_OES_compressed_ETC1_RGB8_texture NOT supported\n");
        return 2;
    }
    fprintf(stderr, "GL_VENDOR=%s\n", glGetString(GL_VENDOR));
    fprintf(stderr, "GL_RENDERER=%s\n", glGetString(GL_RENDERER));

    // --- Compressed texture upload ---
    GLuint tex; glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_ETC1_RGB8_OES, W, H, 0, blocks_size, blocks);
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) { fprintf(stderr, "glCompressedTexImage2D err 0x%x\n", err); return 3; }

    // --- FBO target ---
    GLuint rgb_tex; glGenTextures(1, &rgb_tex);
    glBindTexture(GL_TEXTURE_2D, rgb_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GLuint fbo; glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rgb_tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "FBO incomplete\n"); return 4;
    }

    // --- Shader + quad ---
    GLuint vs = mkshader(GL_VERTEX_SHADER, vsrc);
    GLuint fs = mkshader(GL_FRAGMENT_SHADER, fsrc);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs); glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "a_pos");
    glBindAttribLocation(prog, 1, "a_uv");
    glLinkProgram(prog);
    GLint ok; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { char log[1024]; glGetProgramInfoLog(prog, 1024, NULL, log); fprintf(stderr, "link: %s\n", log); return 5; }
    glUseProgram(prog);
    GLint u_tex = glGetUniformLocation(prog, "u_tex");
    glUniform1i(u_tex, 0);

    float verts[] = {
        -1, -1,   0, 1,
         1, -1,   1, 1,
        -1,  1,   0, 0,
         1,  1,   1, 0,
    };
    GLuint vbo; glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float)));

    glViewport(0, 0, W, H);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // --- Readback ---
    uint8_t *rgba = malloc((size_t)W*H*4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    err = glGetError();
    if (err != GL_NO_ERROR) { fprintf(stderr, "readPixels err 0x%x\n", err); return 6; }

    // --- Flip vertically (FBO origin is bottom-left, PNG is top-left) ---
    uint8_t *flipped = malloc((size_t)W*H*4);
    for (int y = 0; y < H; ++y) {
        memcpy(&flipped[y*W*4], &rgba[(H-1-y)*W*4], W*4);
    }

    // --- Convert RGBA->RGB and save PNG ---
    uint8_t *rgb = malloc((size_t)W*H*3);
    for (int i = 0; i < W*H; ++i) {
        rgb[i*3+0] = flipped[i*4+0];
        rgb[i*3+1] = flipped[i*4+1];
        rgb[i*3+2] = flipped[i*4+2];
    }
    if (!stbi_write_png(argv[2], W, H, 3, rgb, W*3)) {
        fprintf(stderr, "png write fail\n"); return 7;
    }
    fprintf(stderr, "wrote %s\n", argv[2]);
    free(blocks); free(rgba); free(flipped); free(rgb);

    eglDestroyContext(dpy, ctx);
    eglDestroySurface(dpy, surf);
    eglTerminate(dpy);
    return 0;
}

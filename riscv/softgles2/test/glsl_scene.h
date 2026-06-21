/* GLES2 scene driven by REAL GLSL ES shaders compiled by the interpreter, with
 * bilinear (GL_LINEAR) texture filtering. Same geometry as gl_scene.h but the
 * pipeline is the user's shaders, not the built-in one. */
#ifndef GLSL_SCENE_H
#define GLSL_SCENE_H

#include "sgl_gl.h"

static const char *QUAD_VS =
    "#version 100\n"
    "attribute vec4 position;\n attribute vec4 color;\n attribute vec2 texcoord;\n"
    "uniform mat4 mvp;\n varying vec4 v_color;\n varying vec2 v_uv;\n"
    "void main(){ gl_Position = mvp * position; v_color = color; v_uv = texcoord; }\n";

static const char *QUAD_FS =
    "precision mediump float;\n"
    "uniform sampler2D tex0;\n varying vec4 v_color;\n varying vec2 v_uv;\n"
    "void main(){ gl_FragColor = v_color * texture2D(tex0, v_uv); }\n";

static const char *TRI_VS =
    "attribute vec4 position;\n attribute vec4 color;\n varying vec4 v_color;\n"
    "void main(){ gl_Position = position; v_color = color; }\n";

static const char *TRI_FS =
    "precision mediump float;\n varying vec4 v_color;\n"
    "void main(){ gl_FragColor = vec4(v_color.rgb, 0.5); }\n";

static GLuint compile_prog(const char *vs, const char *fs)
{
    GLuint v = glCreateShader(GL_VERTEX_SHADER);   glShaderSource(v, 1, &vs, 0); glCompileShader(v);
    GLuint f = glCreateShader(GL_FRAGMENT_SHADER); glShaderSource(f, 1, &fs, 0); glCompileShader(f);
    GLuint p = glCreateProgram(); glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
    return p;
}

static GLuint make_checker_linear(int s)
{
    static unsigned char px[64 * 64 * 4];
    for (int y = 0; y < s; ++y)
        for (int x = 0; x < s; ++x) {
            int c = (((x >> 2) ^ (y >> 2)) & 1);
            unsigned char *p = &px[(y * s + x) * 4];
            if (c) { p[0] = 240; p[1] = 240; p[2] = 240; } else { p[0] = 60; p[1] = 60; p[2] = 90; }
            p[3] = 255;
        }
    GLuint t;
    glGenTextures(1, &t); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, s, s, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);   /* bilinear */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

static void glsl_scene(void)
{
    glClearColor(0.06f, 0.12f, 0.25f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);

    /* textured, vertex-coloured quad via real GLSL + bilinear */
    GLuint quad = compile_prog(QUAD_VS, QUAD_FS);
    glUseProgram(quad);
    make_checker_linear(32);
    float I[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    glUniformMatrix4fv(glGetUniformLocation(quad, "mvp"), 1, GL_FALSE, I);
    glUniform1i(glGetUniformLocation(quad, "tex0"), 0);

    int lp = glGetAttribLocation(quad, "position");
    int lc = glGetAttribLocation(quad, "color");
    int lt = glGetAttribLocation(quad, "texcoord");
    float quadv[] = {
        -0.85f, -0.7f, 0.5f,  1.0f, 0.4f, 0.4f, 1,  0, 1,
         0.85f, -0.7f, 0.5f,  0.4f, 1.0f, 0.4f, 1,  1, 1,
         0.85f,  0.7f, 0.5f,  0.4f, 0.4f, 1.0f, 1,  1, 0,
        -0.85f, -0.7f, 0.5f,  1.0f, 0.4f, 0.4f, 1,  0, 1,
         0.85f,  0.7f, 0.5f,  0.4f, 0.4f, 1.0f, 1,  1, 0,
        -0.85f,  0.7f, 0.5f,  1.0f, 1.0f, 0.4f, 1,  0, 0,
    };
    GLuint vbo; glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadv), quadv, GL_STATIC_DRAW);
    glVertexAttribPointer(lp, 3, GL_FLOAT, GL_FALSE, 9 * 4, (void *)0);
    glVertexAttribPointer(lc, 4, GL_FLOAT, GL_FALSE, 9 * 4, (void *)(3 * 4));
    glVertexAttribPointer(lt, 2, GL_FLOAT, GL_FALSE, 9 * 4, (void *)(7 * 4));
    glEnableVertexAttribArray(lp); glEnableVertexAttribArray(lc); glEnableVertexAttribArray(lt);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    /* alpha-blended triangle via a second program (procedural 0.5 alpha) */
    GLuint tri = compile_prog(TRI_VS, TRI_FS);
    glUseProgram(tri);
    glDisableVertexAttribArray(lt);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDisable(GL_DEPTH_TEST);
    int lp2 = glGetAttribLocation(tri, "position");
    int lc2 = glGetAttribLocation(tri, "color");
    float triv[] = {
         0.0f,  0.6f, 0.2f,  1, 1, 1, 1,  0, 0,
        -0.5f, -0.3f, 0.2f,  1, 1, 0, 1,  0, 0,
         0.5f, -0.3f, 0.2f,  0, 1, 1, 1,  0, 0,
    };
    GLuint vbo2; glGenBuffers(1, &vbo2); glBindBuffer(GL_ARRAY_BUFFER, vbo2);
    glBufferData(GL_ARRAY_BUFFER, sizeof(triv), triv, GL_STATIC_DRAW);
    glVertexAttribPointer(lp2, 3, GL_FLOAT, GL_FALSE, 9 * 4, (void *)0);
    glVertexAttribPointer(lc2, 4, GL_FLOAT, GL_FALSE, 9 * 4, (void *)(3 * 4));
    glEnableVertexAttribArray(lp2); glEnableVertexAttribArray(lc2);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

#endif /* GLSL_SCENE_H */

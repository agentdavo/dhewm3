/* Shared GLES2 test scene: a textured, vertex-coloured quad with an
 * alpha-blended triangle on top — driven purely through the gl* API the way
 * gl4es would drive it (genBuffers/bufferData/vertexAttribPointer/genTextures/
 * texImage2D/useProgram/drawArrays). */
#ifndef GL_SCENE_H
#define GL_SCENE_H

#include "sgl_gl.h"

static GLuint make_checker(int s)
{
    static unsigned char px[64 * 64 * 4];
    for (int y = 0; y < s; ++y)
        for (int x = 0; x < s; ++x) {
            int c = (((x >> 2) ^ (y >> 2)) & 1);
            unsigned char *p = &px[(y * s + x) * 4];
            if (c) { p[0] = 240; p[1] = 240; p[2] = 240; }
            else   { p[0] = 60;  p[1] = 60;  p[2] = 90;  }
            p[3] = 255;
        }
    GLuint t;
    glGenTextures(1, &t);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, s, s, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);  /* bilinear (RVV) */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

static void gl_scene(void)
{
    glClearColor(0.06f, 0.12f, 0.25f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);

    GLuint prog = glCreateProgram();
    glUseProgram(prog);
    make_checker(32);
    glUniform1i(glGetUniformLocation(prog, "tex0"), 0);

    /* interleaved: x,y,z, r,g,b,a, u,v  (9 floats/vertex) */
    float quad[] = {
        -0.85f, -0.7f, 0.5f,  1.0f, 0.4f, 0.4f, 1,  0, 1,
         0.85f, -0.7f, 0.5f,  0.4f, 1.0f, 0.4f, 1,  1, 1,
         0.85f,  0.7f, 0.5f,  0.4f, 0.4f, 1.0f, 1,  1, 0,
        -0.85f, -0.7f, 0.5f,  1.0f, 0.4f, 0.4f, 1,  0, 1,
         0.85f,  0.7f, 0.5f,  0.4f, 0.4f, 1.0f, 1,  1, 0,
        -0.85f,  0.7f, 0.5f,  1.0f, 1.0f, 0.4f, 1,  0, 0,
    };
    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glVertexAttribPointer(SGL_ATTRIB_POSITION, 3, GL_FLOAT, GL_FALSE, 9 * 4, (void *)0);
    glVertexAttribPointer(SGL_ATTRIB_COLOR,    4, GL_FLOAT, GL_FALSE, 9 * 4, (void *)(3 * 4));
    glVertexAttribPointer(SGL_ATTRIB_TEXCOORD, 2, GL_FLOAT, GL_FALSE, 9 * 4, (void *)(7 * 4));
    glEnableVertexAttribArray(SGL_ATTRIB_POSITION);
    glEnableVertexAttribArray(SGL_ATTRIB_COLOR);
    glEnableVertexAttribArray(SGL_ATTRIB_TEXCOORD);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    /* alpha-blended triangle in front (no texture, no depth write) */
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisableVertexAttribArray(SGL_ATTRIB_TEXCOORD);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    float tri[] = {
         0.0f,  0.6f, 0.2f,  1, 1, 1, 0.5f,  0, 0,
        -0.5f, -0.3f, 0.2f,  1, 1, 0, 0.5f,  0, 0,
         0.5f, -0.3f, 0.2f,  0, 1, 1, 0.5f,  0, 0,
    };
    GLuint vbo2;
    glGenBuffers(1, &vbo2);
    glBindBuffer(GL_ARRAY_BUFFER, vbo2);
    glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
    glVertexAttribPointer(SGL_ATTRIB_POSITION, 3, GL_FLOAT, GL_FALSE, 9 * 4, (void *)0);
    glVertexAttribPointer(SGL_ATTRIB_COLOR,    4, GL_FLOAT, GL_FALSE, 9 * 4, (void *)(3 * 4));
    glEnableVertexAttribArray(SGL_ATTRIB_POSITION);
    glEnableVertexAttribArray(SGL_ATTRIB_COLOR);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

#endif /* GL_SCENE_H */

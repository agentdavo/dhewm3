#ifndef GLMARK2_GL_STATE_DOOMOS_H_
#define GLMARK2_GL_STATE_DOOMOS_H_

#include "gl-state.h"

extern "C" {
#include "../../softgles2/include/sgl.h"
}

class GLStateDoomOS : public GLState
{
public:
    GLStateDoomOS();
    ~GLStateDoomOS() override;

    bool init_display(void *native_display, GLVisualConfig& config_pref) override;
    bool init_surface(void *native_window) override;
    bool init_gl_extensions() override;
    bool valid() override;
    bool reset() override;
    void swap() override;
    bool gotNativeConfig(intptr_t& vid, std::vector<uint64_t>& mods) override;
    void getVisualConfig(GLVisualConfig& vc) override;
    bool supports_sync() override;
    std::unique_ptr<GLStateSync> sync() override;

private:
    sgl_framebuffer *fb_;
    int width_;
    int height_;
    bool gl_loaded_;
};

#endif

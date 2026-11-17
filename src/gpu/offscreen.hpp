// 离屏渲染目标：RGBA8 颜色纹理 + DEPTH24_STENCIL8，可选多重采样。
//
// 视口与导出共用同一套渲染 pass，只是把目标换成这里的 FBO。samples > 1 时
// 先渲染进多重采样 renderbuffer，read_rgba() 前 blit 到单采样 FBO 再回读。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "gpu/gl_api.hpp"

namespace pmx {

class OffscreenTarget {
public:
    OffscreenTarget() = default;
    ~OffscreenTarget();

    OffscreenTarget(const OffscreenTarget&) = delete;
    OffscreenTarget& operator=(const OffscreenTarget&) = delete;

    // 重建附件；w/h <= 0 或无 GL 入口时返回 false 并写 error。
    bool resize(int w, int h, int samples, std::string* error);
    void bind();
    void unbind();

    // glReadPixels 回读色纹理并垂直翻转成自上而下，out 为 w*h*4。
    bool read_rgba(int w, int h, std::vector<std::uint8_t>& out, std::string* error);

    GLuint texture() const { return color_tex_; }
    int width() const { return width_; }
    int height() const { return height_; }
    int samples() const { return samples_; }

private:
    void release();
    bool create_attachments(int w, int h, int samples, std::string* error);
    bool create_single_sample(int w, int h, std::string* error);
    bool create_multisample(int w, int h, int samples, std::string* error);
    bool check_complete(GLenum target, const char* label, std::string* error) const;

    GLuint fbo_ = 0;         // 绘制目标；MSAA 时是多重采样 FBO
    GLuint resolve_fbo_ = 0; // MSAA 单采样解析目标；1x 时保持 0
    GLuint color_tex_ = 0;
    GLuint color_rb_ = 0;
    GLuint depth_rb_ = 0;
    int width_ = 0;
    int height_ = 0;
    int samples_ = 1;
};

}  // namespace pmx

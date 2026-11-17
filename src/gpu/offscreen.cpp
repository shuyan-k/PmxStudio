#include "gpu/offscreen.hpp"

#include <algorithm>
#include <cstdio>

namespace pmx {
namespace {

std::string* error_sink(std::string* error, std::string& scratch) {
    return error ? error : &scratch;
}

std::string status_text(GLenum status) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%04X", static_cast<unsigned>(status));
    return buf;
}

}  // namespace

OffscreenTarget::~OffscreenTarget() { release(); }

void OffscreenTarget::release() {
    if (resolve_fbo_) glDeleteFramebuffers(1, &resolve_fbo_);
    if (fbo_) glDeleteFramebuffers(1, &fbo_);
    if (color_tex_) glDeleteTextures(1, &color_tex_);
    if (color_rb_) glDeleteRenderbuffers(1, &color_rb_);
    if (depth_rb_) glDeleteRenderbuffers(1, &depth_rb_);
    resolve_fbo_ = 0;
    fbo_ = 0;
    color_tex_ = 0;
    color_rb_ = 0;
    depth_rb_ = 0;
    width_ = 0;
    height_ = 0;
    samples_ = 1;
}

bool OffscreenTarget::check_complete(GLenum target, const char* label,
                                     std::string* error) const {
    const GLenum status = glCheckFramebufferStatus(target);
    if (status == glc::GL_FRAMEBUFFER_COMPLETE) return true;
    if (error) {
        *error = std::string(label) + " FBO 不完整（状态 " + status_text(status) + "）";
    }
    return false;
}

bool OffscreenTarget::create_single_sample(int w, int h, std::string* error) {
    glGenFramebuffers(1, &fbo_);
    if (fbo_ == 0) {
        if (error) *error = "创建离屏 FBO 失败";
        return false;
    }
    glBindFramebuffer(glc::GL_FRAMEBUFFER, fbo_);

    glGenTextures(1, &color_tex_);
    if (color_tex_ == 0) {
        if (error) *error = "创建离屏颜色纹理失败";
        return false;
    }
    glBindTexture(glc::GL_TEXTURE_2D, color_tex_);
    glTexImage2D(glc::GL_TEXTURE_2D, 0, glc::GL_RGBA8, w, h, 0, glc::GL_RGBA,
                 glc::GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(glc::GL_TEXTURE_2D, glc::GL_TEXTURE_MIN_FILTER, glc::GL_LINEAR);
    glTexParameteri(glc::GL_TEXTURE_2D, glc::GL_TEXTURE_MAG_FILTER, glc::GL_LINEAR);
    glTexParameteri(glc::GL_TEXTURE_2D, glc::GL_TEXTURE_WRAP_S, glc::GL_CLAMP_TO_EDGE);
    glTexParameteri(glc::GL_TEXTURE_2D, glc::GL_TEXTURE_WRAP_T, glc::GL_CLAMP_TO_EDGE);
    glBindTexture(glc::GL_TEXTURE_2D, 0);
    glFramebufferTexture2D(glc::GL_FRAMEBUFFER, glc::GL_COLOR_ATTACHMENT0,
                           glc::GL_TEXTURE_2D, color_tex_, 0);

    glGenRenderbuffers(1, &depth_rb_);
    if (depth_rb_ == 0) {
        if (error) *error = "创建离屏深度缓冲失败";
        return false;
    }
    glBindRenderbuffer(glc::GL_RENDERBUFFER, depth_rb_);
    glRenderbufferStorage(glc::GL_RENDERBUFFER, glc::GL_DEPTH24_STENCIL8, w, h);
    glBindRenderbuffer(glc::GL_RENDERBUFFER, 0);
    glFramebufferRenderbuffer(glc::GL_FRAMEBUFFER, glc::GL_DEPTH_STENCIL_ATTACHMENT,
                              glc::GL_RENDERBUFFER, depth_rb_);

    return check_complete(glc::GL_FRAMEBUFFER, "离屏", error);
}

bool OffscreenTarget::create_multisample(int w, int h, int samples, std::string* error) {
    // 解析目标：单采样颜色纹理，仅作为 blit 的 draw buffer。
    glGenFramebuffers(1, &resolve_fbo_);
    if (resolve_fbo_ == 0) {
        if (error) *error = "创建解析 FBO 失败";
        return false;
    }
    glBindFramebuffer(glc::GL_FRAMEBUFFER, resolve_fbo_);
    glGenTextures(1, &color_tex_);
    if (color_tex_ == 0) {
        if (error) *error = "创建解析颜色纹理失败";
        return false;
    }
    glBindTexture(glc::GL_TEXTURE_2D, color_tex_);
    glTexImage2D(glc::GL_TEXTURE_2D, 0, glc::GL_RGBA8, w, h, 0, glc::GL_RGBA,
                 glc::GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(glc::GL_TEXTURE_2D, glc::GL_TEXTURE_MIN_FILTER, glc::GL_LINEAR);
    glTexParameteri(glc::GL_TEXTURE_2D, glc::GL_TEXTURE_MAG_FILTER, glc::GL_LINEAR);
    glTexParameteri(glc::GL_TEXTURE_2D, glc::GL_TEXTURE_WRAP_S, glc::GL_CLAMP_TO_EDGE);
    glTexParameteri(glc::GL_TEXTURE_2D, glc::GL_TEXTURE_WRAP_T, glc::GL_CLAMP_TO_EDGE);
    glBindTexture(glc::GL_TEXTURE_2D, 0);
    glFramebufferTexture2D(glc::GL_FRAMEBUFFER, glc::GL_COLOR_ATTACHMENT0,
                           glc::GL_TEXTURE_2D, color_tex_, 0);
    if (!check_complete(glc::GL_FRAMEBUFFER, "解析", error)) return false;

    // 绘制目标：多重采样颜色 / 深度模板 renderbuffer。
    glGenFramebuffers(1, &fbo_);
    if (fbo_ == 0) {
        if (error) *error = "创建多重采样 FBO 失败";
        return false;
    }
    glBindFramebuffer(glc::GL_FRAMEBUFFER, fbo_);

    glGenRenderbuffers(1, &color_rb_);
    if (color_rb_ == 0) {
        if (error) *error = "创建多重采样颜色缓冲失败";
        return false;
    }
    glBindRenderbuffer(glc::GL_RENDERBUFFER, color_rb_);
    glRenderbufferStorageMultisample(glc::GL_RENDERBUFFER, samples, glc::GL_RGBA8, w, h);
    glBindRenderbuffer(glc::GL_RENDERBUFFER, 0);
    glFramebufferRenderbuffer(glc::GL_FRAMEBUFFER, glc::GL_COLOR_ATTACHMENT0,
                              glc::GL_RENDERBUFFER, color_rb_);

    glGenRenderbuffers(1, &depth_rb_);
    if (depth_rb_ == 0) {
        if (error) *error = "创建多重采样深度缓冲失败";
        return false;
    }
    glBindRenderbuffer(glc::GL_RENDERBUFFER, depth_rb_);
    glRenderbufferStorageMultisample(glc::GL_RENDERBUFFER, samples,
                                     glc::GL_DEPTH24_STENCIL8, w, h);
    glBindRenderbuffer(glc::GL_RENDERBUFFER, 0);
    glFramebufferRenderbuffer(glc::GL_FRAMEBUFFER, glc::GL_DEPTH_STENCIL_ATTACHMENT,
                              glc::GL_RENDERBUFFER, depth_rb_);

    return check_complete(glc::GL_FRAMEBUFFER, "多重采样", error);
}

bool OffscreenTarget::resize(int w, int h, int samples, std::string* error) {
    std::string scratch;
    std::string* err = error_sink(error, scratch);
    err->clear();

    if (w <= 0 || h <= 0) {
        *err = "离屏尺寸必须为正";
        return false;
    }
    if (samples < 1) samples = 1;
    if (samples > 16) samples = 16;

    if (fbo_ != 0 && w == width_ && h == height_ && samples == samples_) return true;

    release();

    bool ok = samples > 1 ? create_multisample(w, h, samples, err)
                          : create_single_sample(w, h, err);
    if (ok) {
        width_ = w;
        height_ = h;
        samples_ = samples;
    } else {
        release();
    }
    glBindFramebuffer(glc::GL_FRAMEBUFFER, 0);
    return ok;
}

void OffscreenTarget::bind() {
    if (fbo_ != 0) glBindFramebuffer(glc::GL_FRAMEBUFFER, fbo_);
}

void OffscreenTarget::unbind() { glBindFramebuffer(glc::GL_FRAMEBUFFER, 0); }

bool OffscreenTarget::read_rgba(int w, int h, std::vector<std::uint8_t>& out,
                                std::string* error) {
    std::string scratch;
    std::string* err = error_sink(error, scratch);
    err->clear();

    if (color_tex_ == 0 || fbo_ == 0) {
        *err = "离屏目标尚未创建";
        return false;
    }
    if (w <= 0 || h <= 0) {
        *err = "回读尺寸必须为正";
        return false;
    }
    if (w != width_ || h != height_) {
        *err = "回读尺寸 " + std::to_string(w) + "x" + std::to_string(h) +
               " 与离屏目标 " + std::to_string(width_) + "x" + std::to_string(height_) +
               " 不一致";
        return false;
    }

    if (resolve_fbo_ != 0) {
        // 多重采样内容先解析到单采样纹理。
        glBindFramebuffer(glc::GL_READ_FRAMEBUFFER, fbo_);
        glBindFramebuffer(glc::GL_DRAW_FRAMEBUFFER, resolve_fbo_);
        glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, glc::GL_COLOR_BUFFER_BIT,
                          glc::GL_NEAREST);
        glBindFramebuffer(glc::GL_FRAMEBUFFER, resolve_fbo_);
    } else {
        glBindFramebuffer(glc::GL_FRAMEBUFFER, fbo_);
    }

    glReadBuffer(glc::GL_COLOR_ATTACHMENT0);
    out.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, 0);
    glReadPixels(0, 0, w, h, glc::GL_RGBA, glc::GL_UNSIGNED_BYTE, out.data());
    glBindFramebuffer(glc::GL_FRAMEBUFFER, 0);

    // OpenGL 原点在左下，写 PNG 需要自上而下，逐行交换。
    const std::size_t stride = static_cast<std::size_t>(w) * 4;
    std::vector<std::uint8_t> row(stride);
    for (int y = 0; y < h / 2; ++y) {
        std::uint8_t* top = out.data() + static_cast<std::size_t>(y) * stride;
        std::uint8_t* bottom =
            out.data() + static_cast<std::size_t>(h - 1 - y) * stride;
        std::copy(top, top + stride, row.begin());
        std::copy(bottom, bottom + stride, top);
        std::copy(row.begin(), row.end(), bottom);
    }
    return true;
}

}  // namespace pmx

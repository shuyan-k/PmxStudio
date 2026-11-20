#include "app/api.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "app/export.hpp"
#include "app/theme.hpp"
#include "app/timeline.hpp"
#include "app/viewer.hpp"
#include "core/fs.hpp"
#include "core/math.hpp"
#include "core/script.hpp"
#include "core/vmd.hpp"
#include "pmxstudio/plugin_api.h"
#ifdef PMX_MODULE_RENDER_OUT
#include "gpu/offscreen.hpp"
#endif

#ifndef PMXSTUDIO_VERSION
#define PMXSTUDIO_VERSION "0.1.0"
#endif

namespace {

using pmx::JsonValue;

// ---------------------------------------------------------------- 参数读取
struct ApiError {
    std::string message;
    int code = -32000;
};

bool has(const JsonValue& params, const char* key) { return params.find(key) != nullptr; }

const JsonValue& require_node(const JsonValue& params, const char* key) {
    const JsonValue* node = params.find(key);
    if (!node) throw ApiError{std::string("缺少参数 \"") + key + "\""};
    return *node;
}

std::string require_string(const JsonValue& params, const char* key) {
    const JsonValue& node = require_node(params, key);
    if (!node.is_string())
        throw ApiError{std::string("参数 \"") + key + "\" 必须是字符串"};
    return node.string;
}

bool optional_bool(const JsonValue& params, const char* key, bool fallback) {
    const JsonValue* node = params.find(key);
    if (!node) return fallback;
    if (!node->is_bool()) throw ApiError{std::string("参数 \"") + key + "\" 必须是布尔值"};
    return node->boolean;
}

double optional_number(const JsonValue& params, const char* key, double fallback) {
    const JsonValue* node = params.find(key);
    if (!node) return fallback;
    if (!node->is_number() || !std::isfinite(node->number))
        throw ApiError{std::string("参数 \"") + key + "\" 必须是有限数字"};
    return node->number;
}

int optional_int(const JsonValue& params, const char* key, int fallback) {
    const JsonValue* node = params.find(key);
    if (!node) return fallback;
    if (!node->is_number() || !std::isfinite(node->number) ||
        std::floor(node->number) != node->number)
        throw ApiError{std::string("参数 \"") + key + "\" 必须是整数"};
    return static_cast<int>(node->number);
}

float optional_float(const JsonValue& params, const char* key, float fallback) {
    return static_cast<float>(optional_number(params, key, fallback));
}

pmx::Vec3 read_vec3(const JsonValue& node, const char* what) {
    if (!node.is_array() || node.array.size() != 3)
        throw ApiError{std::string(what) + " 必须是 [x,y,z] 数组"};
    pmx::Vec3 out;
    for (std::size_t i = 0; i < 3; ++i) {
        if (!node.array[i].is_number())
            throw ApiError{std::string(what) + " 的三个分量必须都是数字"};
        out[static_cast<int>(i)] = static_cast<float>(node.array[i].number);
    }
    return out;
}

pmx::Vec4 read_vec4(const JsonValue& node, const char* what) {
    if (!node.is_array() || node.array.size() != 4)
        throw ApiError{std::string(what) + " 必须是 [x,y,z,w] 数组"};
    pmx::Vec4 out;
    for (std::size_t i = 0; i < 4; ++i) {
        if (!node.array[i].is_number())
            throw ApiError{std::string(what) + " 的四个分量必须都是数字"};
        out[static_cast<int>(i)] = static_cast<float>(node.array[i].number);
    }
    return out;
}

JsonValue vec3_json(const pmx::Vec3& v) {
    JsonValue out = pmx::json_array();
    pmx::json_push(out, pmx::json_number(v.x));
    pmx::json_push(out, pmx::json_number(v.y));
    pmx::json_push(out, pmx::json_number(v.z));
    return out;
}

JsonValue quat_json(const pmx::Quat& q) {
    JsonValue out = pmx::json_array();
    pmx::json_push(out, pmx::json_number(q.x));
    pmx::json_push(out, pmx::json_number(q.y));
    pmx::json_push(out, pmx::json_number(q.z));
    pmx::json_push(out, pmx::json_number(q.w));
    return out;
}

JsonValue vec4_json(const pmx::Vec4& v) {
    JsonValue out = pmx::json_array();
    pmx::json_push(out, pmx::json_number(v.x));
    pmx::json_push(out, pmx::json_number(v.y));
    pmx::json_push(out, pmx::json_number(v.z));
    pmx::json_push(out, pmx::json_number(v.w));
    return out;
}

void set_string(JsonValue& object, const char* key, const std::string& value) {
    pmx::json_set(object, key, pmx::json_string(value));
}

// ---------------------------------------------------------------- 名称解析
int require_index(const JsonValue& params, int count, const char* what) {
    const JsonValue& node = require_node(params, "index");
    if (!node.is_number() || !std::isfinite(node.number) ||
        std::floor(node.number) != node.number)
        throw ApiError{std::string(what) + "的 index 必须是整数"};
    const int index = static_cast<int>(node.number);
    if (index < 0 || index >= count)
        throw ApiError{std::string(what) + " index 越界：" + std::to_string(index) + "（共 " +
                      std::to_string(count) + " 项）"};
    return index;
}

int find_named(const std::vector<std::string>& names,
               const std::vector<std::string>& names_en, const std::string& wanted) {
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i] == wanted) return static_cast<int>(i);
        if (i < names_en.size() && names_en[i] == wanted) return static_cast<int>(i);
    }
    return -1;
}

int resolve_bone(const pmx::Model& model, const JsonValue& params) {
    if (has(params, "index")) return require_index(params, static_cast<int>(model.bones.size()), "骨骼");
    const JsonValue* name = params.find("bone");
    if (!name) name = params.find("name");
    if (name && name->is_string()) {
        for (std::size_t i = 0; i < model.bones.size(); ++i) {
            if (model.bones[i].name == name->string || model.bones[i].name_en == name->string)
                return static_cast<int>(i);
        }
        throw ApiError{"找不到骨骼 \"" + name->string + "\""};
    }
    throw ApiError{"需要 index 或 bone（骨骼名）"};
}

int resolve_morph(const pmx::Model& model, const JsonValue& params) {
    if (has(params, "index"))
        return require_index(params, static_cast<int>(model.morphs.size()), "形态");
    const JsonValue* name = params.find("morph");
    if (!name) name = params.find("name");
    if (name && name->is_string()) {
        for (std::size_t i = 0; i < model.morphs.size(); ++i) {
            if (model.morphs[i].name == name->string || model.morphs[i].name_en == name->string)
                return static_cast<int>(i);
        }
        throw ApiError{"找不到形态 \"" + name->string + "\""};
    }
    throw ApiError{"需要 index 或 morph（形态名）"};
}

int resolve_material(const pmx::Model& model, const JsonValue& params) {
    if (has(params, "index"))
        return require_index(params, static_cast<int>(model.materials.size()), "材质");
    const JsonValue* name = params.find("material");
    if (!name) name = params.find("name");
    if (name && name->is_string()) {
        for (std::size_t i = 0; i < model.materials.size(); ++i) {
            if (model.materials[i].name == name->string ||
                model.materials[i].name_en == name->string)
                return static_cast<int>(i);
        }
        throw ApiError{"找不到材质 \"" + name->string + "\""};
    }
    throw ApiError{"需要 index 或 material（材质名）"};
}

// ---------------------------------------------------------------- 状态快照
pmx::JsonValue model_info(const Viewer& viewer) {
    JsonValue out = pmx::json_object();
    pmx::json_set(out, "loaded", pmx::json_bool(viewer.pose != nullptr));
    set_string(out, "path", viewer.model_path);
    set_string(out, "name", viewer.model.name);
    set_string(out, "name_en", viewer.model.name_en);
    set_string(out, "comment", viewer.model.comment);
    pmx::json_set(out, "version", pmx::json_number(viewer.model.version));
    pmx::json_set(out, "encoding", pmx::json_number(viewer.model.encoding));
    pmx::json_set(out, "add_uv_count", pmx::json_number(viewer.model.add_uv_count));
    pmx::json_set(out, "vertices", pmx::json_number(static_cast<double>(viewer.model.n_vertices())));
    pmx::json_set(out, "indices",
                  pmx::json_number(static_cast<double>(viewer.model.indices.size())));
    pmx::json_set(out, "faces", pmx::json_number(static_cast<double>(viewer.model.n_faces())));
    pmx::json_set(out, "textures",
                  pmx::json_number(static_cast<double>(viewer.model.textures.size())));
    pmx::json_set(out, "materials",
                  pmx::json_number(static_cast<double>(viewer.model.materials.size())));
    pmx::json_set(out, "bones", pmx::json_number(static_cast<double>(viewer.model.bones.size())));
    pmx::json_set(out, "morphs", pmx::json_number(static_cast<double>(viewer.model.morphs.size())));
    pmx::json_set(out, "frames",
                  pmx::json_number(static_cast<double>(viewer.model.frames.size())));
    pmx::json_set(out, "rigid_bodies",
                  pmx::json_number(static_cast<double>(viewer.model.rigid.size())));
    pmx::json_set(out, "joints", pmx::json_number(static_cast<double>(viewer.model.joints.size())));
    pmx::json_set(out, "soft_bodies",
                  pmx::json_number(static_cast<double>(viewer.model.soft_bodies.size())));
    pmx::json_set(out, "dirty", pmx::json_bool(viewer.dirty));

    if (!viewer.model.vertices.pos.empty()) {
        pmx::Vec3 lo = viewer.model.vertices.pos.front();
        pmx::Vec3 hi = lo;
        for (const pmx::Vec3& p : viewer.model.vertices.pos) {
            lo.x = std::min(lo.x, p.x);
            lo.y = std::min(lo.y, p.y);
            lo.z = std::min(lo.z, p.z);
            hi.x = std::max(hi.x, p.x);
            hi.y = std::max(hi.y, p.y);
            hi.z = std::max(hi.z, p.z);
        }
        JsonValue bbox = pmx::json_object();
        pmx::json_set(bbox, "min", vec3_json(lo));
        pmx::json_set(bbox, "max", vec3_json(hi));
        pmx::json_set(out, "bbox", std::move(bbox));
    }
    return out;
}

JsonValue camera_json(const Viewer& viewer) {
    const pmx::OrbitCamera& camera = viewer.camera;
    JsonValue out = pmx::json_object();
    pmx::json_set(out, "target", vec3_json(camera.target));
    pmx::json_set(out, "eye", vec3_json(camera.eye()));
    pmx::json_set(out, "distance", pmx::json_number(camera.distance));
    pmx::json_set(out, "yaw", pmx::json_number(camera.yaw));
    pmx::json_set(out, "pitch", pmx::json_number(camera.pitch));
    pmx::json_set(out, "fov", pmx::json_number(camera.fov));
    pmx::json_set(out, "ortho", pmx::json_bool(camera.ortho));
    pmx::json_set(out, "ortho_scale", pmx::json_number(camera.ortho_scale));
    pmx::json_set(out, "near", pmx::json_number(camera.near_plane));
    pmx::json_set(out, "far", pmx::json_number(camera.far_plane));
    pmx::json_set(out, "camera_view", pmx::json_bool(viewer.camera_view));
    return out;
}

JsonValue animation_state(const Viewer& viewer) {
    JsonValue out = pmx::json_object();
    const bool clip = viewer.timeline.clip != nullptr;
    const bool loaded = viewer.motion != nullptr || clip;
    pmx::json_set(out, "loaded", pmx::json_bool(loaded));
    set_string(out, "path", viewer.vmd_path);
    pmx::json_set(out, "frame", pmx::json_number(viewer.motion_frame));
    int max_frame = 0;
    int bone_tracks = 0;
    int morph_tracks = 0;
    if (clip) {
        max_frame = viewer.timeline.model_max_frame();
        bone_tracks = static_cast<int>(viewer.timeline.clip->bones.size());
        morph_tracks = static_cast<int>(viewer.timeline.clip->morphs.size());
    } else if (viewer.motion) {
        max_frame = viewer.motion->max_frame;
        bone_tracks = static_cast<int>(viewer.motion->bone_tracks.size());
        morph_tracks = static_cast<int>(viewer.motion->morph_tracks.size());
    }
    pmx::json_set(out, "max_frame", pmx::json_number(max_frame));
    pmx::json_set(out, "playing", pmx::json_bool(viewer.motion_playing));
    pmx::json_set(out, "speed", pmx::json_number(viewer.motion_speed));
    pmx::json_set(out, "loop", pmx::json_bool(viewer.motion_loop));
    pmx::json_set(out, "bone_tracks", pmx::json_number(bone_tracks));
    pmx::json_set(out, "morph_tracks", pmx::json_number(morph_tracks));
    const int bone_matched = clip ? viewer.timeline.binding.bone_matched
                                  : viewer.motion_binding.bone_matched;
    const int morph_matched = clip ? viewer.timeline.binding.morph_matched
                                   : viewer.motion_binding.morph_matched;
    pmx::json_set(out, "bone_matched", pmx::json_number(bone_matched));
    pmx::json_set(out, "morph_matched", pmx::json_number(morph_matched));
    const std::vector<std::string>& missing =
        clip ? viewer.timeline.binding.missing_bones : viewer.motion_binding.missing_bones;
    JsonValue missing_json = pmx::json_array();
    for (std::size_t i = 0; i < missing.size() && i < 32; ++i)
        pmx::json_push(missing_json, pmx::json_string(missing[i]));
    pmx::json_set(out, "missing_bones", std::move(missing_json));
    pmx::json_set(out, "timeline", pmx::json_bool(clip));
    pmx::json_set(out, "timeline_enabled", pmx::json_bool(viewer.timeline.enabled));
    JsonValue camera = pmx::json_object();
    pmx::json_set(camera, "loaded", pmx::json_bool(viewer.camera_motion.loaded()));
    pmx::json_set(camera, "keys",
                  pmx::json_number(static_cast<double>(viewer.camera_motion.track.keys.size())));
    pmx::json_set(camera, "max_frame", pmx::json_number(viewer.camera_motion.max_frame()));
    set_string(camera, "source", viewer.camera_motion.source);
    pmx::json_set(out, "camera", std::move(camera));
    return out;
}

const char* selection_kind_name(SelKind kind) {
    switch (kind) {
        case SelKind::Bone: return "bone";
        case SelKind::Material: return "material";
        case SelKind::Morph: return "morph";
        case SelKind::Rigid: return "rigid";
        case SelKind::Joint: return "joint";
        case SelKind::Soft: return "soft";
        case SelKind::Frame: return "frame";
        case SelKind::Vertex: return "vertex";
        case SelKind::Face: return "face";
        case SelKind::None: break;
    }
    return "none";
}

std::string selection_name(const Viewer& viewer) {
    const int index = viewer.sel.index;
    if (index < 0) return std::string();
    switch (viewer.sel.kind) {
        case SelKind::Bone:
            if (index < static_cast<int>(viewer.model.bones.size()))
                return viewer.model.bones[static_cast<std::size_t>(index)].name;
            break;
        case SelKind::Material:
            if (index < static_cast<int>(viewer.model.materials.size()))
                return viewer.model.materials[static_cast<std::size_t>(index)].name;
            break;
        case SelKind::Morph:
            if (index < static_cast<int>(viewer.model.morphs.size()))
                return viewer.model.morphs[static_cast<std::size_t>(index)].name;
            break;
        case SelKind::Rigid:
            if (index < static_cast<int>(viewer.model.rigid.size()))
                return viewer.model.rigid[static_cast<std::size_t>(index)].name;
            break;
        case SelKind::Joint:
            if (index < static_cast<int>(viewer.model.joints.size()))
                return viewer.model.joints[static_cast<std::size_t>(index)].name;
            break;
        case SelKind::Soft:
            if (index < static_cast<int>(viewer.model.soft_bodies.size()))
                return viewer.model.soft_bodies[static_cast<std::size_t>(index)].name;
            break;
        case SelKind::Frame:
            if (index < static_cast<int>(viewer.model.frames.size()))
                return viewer.model.frames[static_cast<std::size_t>(index)].name;
            break;
        default: break;
    }
    return std::string();
}

JsonValue selection_json(const Viewer& viewer) {
    JsonValue out = pmx::json_object();
    pmx::json_set(out, "kind", pmx::json_string(selection_kind_name(viewer.sel.kind)));
    pmx::json_set(out, "index", pmx::json_number(viewer.sel.index));
    set_string(out, "name", selection_name(viewer));
    return out;
}

JsonValue settings_json(const Viewer& viewer) {
    JsonValue out = pmx::json_object();
    pmx::json_set(out, "show_mesh", pmx::json_bool(viewer.show_mesh));
    pmx::json_set(out, "show_bones", pmx::json_bool(viewer.show_bones));
    pmx::json_set(out, "show_rigid", pmx::json_bool(viewer.show_rigid));
    pmx::json_set(out, "show_joints", pmx::json_bool(viewer.show_joints));
    pmx::json_set(out, "use_gpu", pmx::json_bool(viewer.use_gpu));
    pmx::json_set(out, "wireframe", pmx::json_bool(viewer.wireframe));
    pmx::json_set(out, "grid", pmx::json_bool(viewer.grid));
    pmx::json_set(out, "outline", pmx::json_bool(viewer.outline));
    pmx::json_set(out, "preview_alpha", pmx::json_bool(viewer.preview_alpha));
    pmx::json_set(out, "bone_alpha", pmx::json_number(viewer.bone_alpha));
    pmx::json_set(out, "rigid_alpha", pmx::json_number(viewer.rigid_alpha));
    pmx::json_set(out, "joint_alpha", pmx::json_number(viewer.joint_alpha));
    pmx::json_set(out, "toon", pmx::json_number(viewer.toon));
    pmx::json_set(out, "rim", pmx::json_number(viewer.rim));
    pmx::json_set(out, "toon_edge", pmx::json_number(viewer.toon_edge));
    pmx::json_set(out, "background", vec3_json({viewer.bg[0], viewer.bg[1], viewer.bg[2]}));
    pmx::json_set(out, "bg_brightness", pmx::json_number(viewer.bg_brightness));
    pmx::json_set(out, "model_brightness", pmx::json_number(viewer.model_brightness));
    pmx::json_set(out, "bg_follow_theme", pmx::json_bool(!viewer.bg_custom));
    pmx::json_set(out, "grid_follow_theme", pmx::json_bool(!viewer.grid_custom));
    pmx::json_set(out, "grid_minor",
                  vec3_json({viewer.grid_minor[0], viewer.grid_minor[1], viewer.grid_minor[2]}));
    pmx::json_set(out, "grid_major",
                  vec3_json({viewer.grid_major[0], viewer.grid_major[1], viewer.grid_major[2]}));
    pmx::json_set(out, "grid_axis_x", vec3_json({viewer.grid_axis_x[0], viewer.grid_axis_x[1],
                                                 viewer.grid_axis_x[2]}));
    pmx::json_set(out, "grid_axis_z", vec3_json({viewer.grid_axis_z[0], viewer.grid_axis_z[1],
                                                 viewer.grid_axis_z[2]}));
    pmx::json_set(out, "light_dir",
                  vec3_json({viewer.light[0], viewer.light[1], viewer.light[2]}));
    pmx::json_set(out, "light_color",
                  vec3_json({viewer.light_color[0], viewer.light_color[1], viewer.light_color[2]}));
    pmx::json_set(out, "light_intensity", pmx::json_number(viewer.light_intensity));
    pmx::json_set(out, "ambient_color",
                  vec3_json({viewer.ambient[0], viewer.ambient[1], viewer.ambient[2]}));
    pmx::json_set(out, "show_light", pmx::json_bool(viewer.show_light));
    pmx::json_set(out, "shadows", pmx::json_bool(viewer.shadows));
    pmx::json_set(out, "shadow_strength", pmx::json_number(viewer.shadow_strength));
    pmx::json_set(out, "shadow_softness", pmx::json_number(viewer.shadow_softness));
    pmx::json_set(out, "shadow_ground", pmx::json_bool(viewer.shadow_ground));
    pmx::json_set(out, "flesh", pmx::json_bool(viewer.flesh));
    pmx::json_set(out, "flesh_range", pmx::json_number(viewer.flesh_range));
    pmx::json_set(out, "flesh_dent", pmx::json_number(viewer.flesh_dent));
    pmx::json_set(out, "flesh_bulge", pmx::json_number(viewer.flesh_bulge));
    pmx::json_set(out, "flesh_width", pmx::json_number(viewer.flesh_width));
    pmx::json_set(out, "flesh_firmness", pmx::json_number(viewer.flesh_firmness));
    pmx::json_set(out, "flesh_region_bone", pmx::json_number(viewer.flesh_region_bone));
    pmx::json_set(out, "flesh_region_firmness", pmx::json_number(viewer.flesh_region_firmness));
    pmx::json_set(out, "camera_gizmo", pmx::json_bool(viewer.camera_gizmo));
    pmx::json_set(out, "ground", pmx::json_bool(viewer.ground));
    pmx::json_set(out, "physics_enabled", pmx::json_bool(viewer.physics_enabled));
    pmx::json_set(out, "gravity", pmx::json_number(viewer.gravity));
    pmx::json_set(out, "iterations", pmx::json_number(viewer.iterations));
    return out;
}

void apply_settings(Viewer& viewer, const JsonValue& params) {
    auto set_bool = [&](const char* key, bool& target) {
        if (has(params, key)) target = optional_bool(params, key, target);
    };
    set_bool("show_mesh", viewer.show_mesh);
    set_bool("show_bones", viewer.show_bones);
    set_bool("show_rigid", viewer.show_rigid);
    set_bool("show_joints", viewer.show_joints);
    set_bool("use_gpu", viewer.use_gpu);
    set_bool("wireframe", viewer.wireframe);
    set_bool("grid", viewer.grid);
    set_bool("outline", viewer.outline);
    set_bool("preview_alpha", viewer.preview_alpha);
    set_bool("show_light", viewer.show_light);
    set_bool("camera_gizmo", viewer.camera_gizmo);

    if (has(params, "bone_alpha")) viewer.bone_alpha = optional_float(params, "bone_alpha", viewer.bone_alpha);
    if (has(params, "rigid_alpha")) viewer.rigid_alpha = optional_float(params, "rigid_alpha", viewer.rigid_alpha);
    if (has(params, "joint_alpha")) viewer.joint_alpha = optional_float(params, "joint_alpha", viewer.joint_alpha);
    if (has(params, "toon")) viewer.toon = optional_float(params, "toon", viewer.toon);
    if (has(params, "rim")) viewer.rim = optional_float(params, "rim", viewer.rim);
    if (has(params, "toon_edge")) viewer.toon_edge = optional_float(params, "toon_edge", viewer.toon_edge);
    if (has(params, "light_intensity"))
        viewer.light_intensity = optional_float(params, "light_intensity", viewer.light_intensity);
    if (has(params, "bg_brightness"))
        viewer.bg_brightness =
            std::clamp(optional_float(params, "bg_brightness", viewer.bg_brightness), 0.2f, 3.0f);
    if (has(params, "model_brightness"))
        viewer.model_brightness = std::clamp(
            optional_float(params, "model_brightness", viewer.model_brightness), 0.2f, 2.2f);

    auto set_vec3 = [&](const char* key, float* target) {
        if (const JsonValue* node = params.find(key)) {
            const pmx::Vec3 value = read_vec3(*node, key);
            target[0] = value.x;
            target[1] = value.y;
            target[2] = value.z;
        }
    };
    if (const JsonValue* node = params.find("background")) {
        const pmx::Vec3 value = read_vec3(*node, "background");
        viewer.bg[0] = value.x;
        viewer.bg[1] = value.y;
        viewer.bg[2] = value.z;
        viewer.bg_custom = true;
    }
    if (const JsonValue* node = params.find("grid_minor")) {
        const pmx::Vec3 value = read_vec3(*node, "grid_minor");
        viewer.grid_minor[0] = value.x;
        viewer.grid_minor[1] = value.y;
        viewer.grid_minor[2] = value.z;
        viewer.grid_custom = true;
    }
    if (const JsonValue* node = params.find("grid_major")) {
        const pmx::Vec3 value = read_vec3(*node, "grid_major");
        viewer.grid_major[0] = value.x;
        viewer.grid_major[1] = value.y;
        viewer.grid_major[2] = value.z;
        viewer.grid_custom = true;
    }
    if (const JsonValue* node = params.find("grid_axis_x")) {
        const pmx::Vec3 value = read_vec3(*node, "grid_axis_x");
        viewer.grid_axis_x[0] = value.x;
        viewer.grid_axis_x[1] = value.y;
        viewer.grid_axis_x[2] = value.z;
        viewer.grid_custom = true;
    }
    if (const JsonValue* node = params.find("grid_axis_z")) {
        const pmx::Vec3 value = read_vec3(*node, "grid_axis_z");
        viewer.grid_axis_z[0] = value.x;
        viewer.grid_axis_z[1] = value.y;
        viewer.grid_axis_z[2] = value.z;
        viewer.grid_custom = true;
    }
    set_vec3("light_dir", viewer.light);
    set_vec3("light_color", viewer.light_color);
    set_vec3("ambient_color", viewer.ambient);
    if (has(params, "shadows")) viewer.shadows = optional_bool(params, "shadows", viewer.shadows);
    if (has(params, "shadow_strength"))
        viewer.shadow_strength =
            std::clamp(optional_float(params, "shadow_strength", viewer.shadow_strength), 0.0f,
                       1.0f);
    if (has(params, "shadow_softness"))
        viewer.shadow_softness =
            std::clamp(optional_float(params, "shadow_softness", viewer.shadow_softness), 0.5f,
                       3.0f);
    if (has(params, "shadow_ground"))
        viewer.shadow_ground = optional_bool(params, "shadow_ground", viewer.shadow_ground);
    set_bool("flesh", viewer.flesh);
    if (has(params, "flesh_range"))
        viewer.flesh_range =
            std::clamp(optional_float(params, "flesh_range", viewer.flesh_range), 0.001f, 0.08f);
    if (has(params, "flesh_dent"))
        viewer.flesh_dent =
            std::clamp(optional_float(params, "flesh_dent", viewer.flesh_dent), 0.0f, 1.0f);
    if (has(params, "flesh_bulge"))
        viewer.flesh_bulge =
            std::clamp(optional_float(params, "flesh_bulge", viewer.flesh_bulge), 0.0f, 2.0f);
    if (has(params, "flesh_width"))
        viewer.flesh_width =
            std::clamp(optional_float(params, "flesh_width", viewer.flesh_width), 0.5f, 12.0f);
    if (has(params, "flesh_firmness"))
        viewer.flesh_firmness =
            std::clamp(optional_float(params, "flesh_firmness", viewer.flesh_firmness), 0.0f, 1.0f);
    if (has(params, "flesh_region_bone"))
        viewer.flesh_region_bone =
            static_cast<int>(std::lround(optional_float(params, "flesh_region_bone",
                                                        static_cast<float>(viewer.flesh_region_bone))));
    if (has(params, "flesh_region_firmness"))
        viewer.flesh_region_firmness = std::clamp(
            optional_float(params, "flesh_region_firmness", viewer.flesh_region_firmness), 0.0f,
            1.0f);

    if (has(params, "bg_follow_theme")) {
        viewer.bg_custom = !optional_bool(params, "bg_follow_theme", !viewer.bg_custom);
        if (!viewer.bg_custom) pmxtheme::apply_scene_theme(viewer);
    }
    if (has(params, "grid_follow_theme")) {
        viewer.grid_custom = !optional_bool(params, "grid_follow_theme", !viewer.grid_custom);
        if (!viewer.grid_custom) pmxtheme::apply_scene_theme(viewer);
    }

    bool physics_changed = false;
    if (has(params, "ground")) {
        viewer.ground = optional_bool(params, "ground", viewer.ground);
        physics_changed = true;
    }
    if (has(params, "gravity")) {
        viewer.gravity = optional_float(params, "gravity", viewer.gravity);
        physics_changed = true;
    }
    if (has(params, "iterations")) {
        viewer.iterations = std::clamp(optional_int(params, "iterations", viewer.iterations), 1, 256);
        physics_changed = true;
    }
    if (has(params, "physics_enabled")) {
        viewer.physics_enabled = optional_bool(params, "physics_enabled", viewer.physics_enabled);
        physics_changed = true;
    }
    if (physics_changed) viewer.reset_physics();
}

pmx::RenderSettings render_settings(const Viewer& viewer, bool overlays) {
    pmx::RenderSettings settings;
    settings.show_mesh = viewer.show_mesh;
    settings.show_bones = overlays && viewer.show_bones;
    settings.show_rigid = overlays && viewer.show_rigid;
    settings.show_joints = overlays && viewer.show_joints;
    settings.focus_selection = overlays && viewer.focus_overlay;
    settings.use_gpu_skinning = viewer.use_gpu;
    settings.wireframe = viewer.wireframe;
    settings.grid = overlays && viewer.grid;
    settings.background = {viewer.bg[0], viewer.bg[1], viewer.bg[2]};
    settings.bg_brightness = viewer.bg_brightness;
    settings.model_brightness = viewer.model_brightness;
    settings.grid_minor = {viewer.grid_minor[0], viewer.grid_minor[1], viewer.grid_minor[2]};
    settings.grid_major = {viewer.grid_major[0], viewer.grid_major[1], viewer.grid_major[2]};
    settings.grid_axis_x = {viewer.grid_axis_x[0], viewer.grid_axis_x[1], viewer.grid_axis_x[2]};
    settings.grid_axis_z = {viewer.grid_axis_z[0], viewer.grid_axis_z[1], viewer.grid_axis_z[2]};
    settings.outline = viewer.outline;
    settings.preview_alpha = viewer.preview_alpha;
    settings.bone_alpha = viewer.bone_alpha;
    settings.rigid_alpha = viewer.rigid_alpha;
    settings.joint_alpha = viewer.joint_alpha;
    settings.toon = viewer.toon;
    settings.rim = viewer.rim;
    settings.toon_edge = viewer.toon_edge;
    settings.background = {viewer.bg[0], viewer.bg[1], viewer.bg[2]};
    settings.light_dir = {viewer.light[0], viewer.light[1], viewer.light[2]};
    settings.light_color = {viewer.light_color[0], viewer.light_color[1], viewer.light_color[2]};
    settings.light_intensity = viewer.light_intensity;
    settings.ambient_color = {viewer.ambient[0], viewer.ambient[1], viewer.ambient[2]};
    settings.show_light = overlays && viewer.show_light;
    settings.shadows = viewer.shadows;
    settings.shadow_strength = viewer.shadow_strength;
    settings.shadow_softness = viewer.shadow_softness;
    settings.shadow_ground = viewer.shadow_ground;
    settings.flesh = viewer.flesh;
    settings.flesh_range = viewer.flesh_range;
    settings.flesh_dent = viewer.flesh_dent;
    settings.flesh_bulge = viewer.flesh_bulge;
    settings.flesh_width = viewer.flesh_width;
    settings.flesh_firmness = viewer.flesh_firmness;
    settings.flesh_region_bone = viewer.flesh_region_bone;
    settings.flesh_region_firmness = viewer.flesh_region_firmness;
    settings.show_camera = false;
    return settings;
}

// ---------------------------------------------------------------- 命令实现
JsonValue cmd_model_load(Viewer& viewer, const JsonValue& params) {
    std::string path = require_string(params, "path");
    if (!pmx::path_exists(path) && !viewer.model_path.empty()) {
        const std::filesystem::path candidate =
            pmx::utf8_path(viewer.model_path).parent_path() / pmx::utf8_path(path);
        if (pmx::path_exists(pmx::utf8_string(candidate)))
            path = pmx::utf8_string(candidate);
    }
    if (!pmx::path_exists(path)) throw ApiError{"文件不存在：" + path};

    std::string error;
    if (!viewer.load_model(path, &error)) throw ApiError{"加载失败：" + error};
    return model_info(viewer);
}

JsonValue cmd_model_save(Viewer& viewer, const JsonValue& params) {
    if (!viewer.pose) throw ApiError{"尚未加载模型"};
    if (viewer.preview_mode) throw ApiError{"预览模式下模型只读，请先切换到编辑模式"};
    const std::string path = require_string(params, "path");
    std::error_code ec;
    const std::filesystem::path target = pmx::utf8_path(path);
    if (target.has_parent_path()) std::filesystem::create_directories(target.parent_path(), ec);
    std::string error;
    if (!viewer.save_model(path, &error)) throw ApiError{"保存失败：" + error};
    JsonValue out = pmx::json_object();
    set_string(out, "path", path);
    pmx::json_set(out, "bytes", pmx::json_number(static_cast<double>(std::filesystem::file_size(
                                                 target, ec))));
    return out;
}

JsonValue cmd_model_script(Viewer& viewer, const JsonValue& params) {
#ifdef PMX_MODULE_SCRIPT
    if (!viewer.pose) throw ApiError{"尚未加载模型"};
    if (viewer.preview_mode) throw ApiError{"预览模式下模型只读，请先切换到编辑模式"};

    std::string text;
    std::string base_dir;
    if (const JsonValue* commands = params.find("commands")) {
        if (!commands->is_array()) throw ApiError{"参数 \"commands\" 必须是数组"};
        text = pmx::json_dump(*commands);
    } else if (const JsonValue* script = params.find("script")) {
        if (!script->is_string()) throw ApiError{"参数 \"script\" 必须是字符串"};
        text = script->string;
    } else if (const JsonValue* file = params.find("path")) {
        if (!file->is_string()) throw ApiError{"参数 \"path\" 必须是字符串"};
        std::ifstream in = pmx::open_ifstream(file->string);
        if (!in) throw ApiError{"无法打开脚本文件：" + file->string};
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        base_dir = pmx::utf8_string(pmx::utf8_path(file->string).parent_path());
    } else {
        throw ApiError{"需要 commands（命令数组）、script（脚本文本）或 path（脚本文件）"};
    }

    pmx::ScriptResult result;
    viewer.edit("Agent 脚本",
                [&](pmx::Model& model) { result = pmx::run_script_json(model, text, base_dir); },
                "model");
    JsonValue out = pmx::json_object();
    pmx::json_set(out, "ok", pmx::json_bool(result.ok));
    pmx::json_set(out, "commands", pmx::json_number(result.commands));
    set_string(out, "message", result.message);
    if (!result.ok) throw ApiError{result.message};
    return out;
#else
    (void)viewer;
    (void)params;
    throw ApiError{"当前构建未启用脚本模块（PMX_MODULE_SCRIPT）"};
#endif
}

JsonValue cmd_scene_state(const Viewer& viewer) {
    JsonValue out = pmx::json_object();
    pmx::json_set(out, "model_loaded", pmx::json_bool(viewer.pose != nullptr));
    set_string(out, "model_path", viewer.model_path);
    pmx::json_set(out, "dirty", pmx::json_bool(viewer.dirty));
    pmx::json_set(out, "preview_mode", pmx::json_bool(viewer.preview_mode));
    pmx::json_set(out, "camera_view", pmx::json_bool(viewer.camera_view));
    pmx::json_set(out, "physics_enabled", pmx::json_bool(viewer.physics_enabled));
    pmx::json_set(out, "frames", pmx::json_number(viewer.frames));
    pmx::json_set(out, "fps", pmx::json_number(viewer.fps));
    pmx::json_set(out, "can_undo", pmx::json_bool(viewer.can_undo()));
    pmx::json_set(out, "can_redo", pmx::json_bool(viewer.can_redo()));
    set_string(out, "undo_label", viewer.undo_label());
    set_string(out, "redo_label", viewer.redo_label());
    pmx::json_set(out, "selection", selection_json(viewer));
    pmx::json_set(out, "motion", animation_state(viewer));
    set_string(out, "status", viewer.status);
    set_string(out, "error", viewer.error);
    return out;
}

JsonValue cmd_scene_camera(Viewer& viewer, const JsonValue& params) {
    if (has(params, "view")) {
        const bool want = optional_bool(params, "view", viewer.camera_view);
        if (want != viewer.camera_view) viewer.toggle_camera_view();
    }
    pmx::OrbitCamera& camera = viewer.camera;
    if (const JsonValue* node = params.find("target")) camera.target = read_vec3(*node, "target");
    camera.distance = optional_float(params, "distance", camera.distance);
    camera.yaw = optional_float(params, "yaw", camera.yaw);
    camera.pitch = optional_float(params, "pitch", camera.pitch);
    camera.fov = optional_float(params, "fov", camera.fov);
    camera.ortho = optional_bool(params, "ortho", camera.ortho);
    camera.ortho_scale = optional_float(params, "ortho_scale", camera.ortho_scale);
    camera.near_plane = optional_float(params, "near", camera.near_plane);
    camera.far_plane = optional_float(params, "far", camera.far_plane);
    if (has(params, "distance")) camera.distance = std::max(0.01f, camera.distance);
    return camera_json(viewer);
}

JsonValue cmd_scene_render(Viewer& viewer, const JsonValue& params) {
#ifdef PMX_MODULE_RENDER_OUT
    if (!viewer.pose) throw ApiError{"尚未加载模型"};
    const std::string path = require_string(params, "path");
    const int width = std::clamp(optional_int(params, "width", viewer.output.width), 16, 8192);
    const int height = std::clamp(optional_int(params, "height", viewer.output.height), 16, 8192);
    const int samples = std::clamp(optional_int(params, "samples", 1), 1, 4);
    const bool overlays = optional_bool(params, "overlays", false);

    if (has(params, "frame")) {
        viewer.motion_frame = std::max(0.0f, optional_float(params, "frame", viewer.motion_frame));
        if (viewer.motion || viewer.timeline.clip) {
            viewer.apply_motion_frame();
            if (viewer.camera_view) viewer.apply_camera_motion();
            viewer.motion_dirty = false;
        }
    }
    if (viewer.morph_dirty) viewer.apply_morphs();
    else viewer.pose->apply(&viewer.morph_weights);
    if (viewer.preview_mode) viewer.pose->solve_ik();
    viewer.pose->world();

    pmx::OffscreenTarget target;
    std::string error;
    if (!target.resize(width, height, samples, &error))
        throw ApiError{"创建离屏渲染目标失败：" + error};
    const float aspect = static_cast<float>(width) / static_cast<float>(std::max(1, height));
    target.bind();
    viewer.renderer.render(viewer.model, *viewer.pose, nullptr, viewer.camera.view(),
                           viewer.camera.projection(aspect), width, height,
                           render_settings(viewer, overlays), pmx::Selection{});
    std::vector<std::uint8_t> rgba;
    const bool read_ok = target.read_rgba(width, height, rgba, &error);
    target.unbind();
    if (!read_ok) throw ApiError{"回读像素失败：" + error};

    std::error_code ec;
    const std::filesystem::path file = pmx::utf8_path(path);
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    if (!pmx::write_png(path, width, height, rgba, &error))
        throw ApiError{"写出 PNG 失败：" + error};

    JsonValue out = pmx::json_object();
    set_string(out, "path", path);
    pmx::json_set(out, "width", pmx::json_number(width));
    pmx::json_set(out, "height", pmx::json_number(height));
    pmx::json_set(out, "frame", pmx::json_number(viewer.motion_frame));
    return out;
#else
    (void)viewer;
    (void)params;
    throw ApiError{"当前构建未启用离屏渲染（PMX_MODULE_RENDER_OUT）"};
#endif
}

SelKind parse_sel_kind(const std::string& text) {
    if (text == "bone") return SelKind::Bone;
    if (text == "material") return SelKind::Material;
    if (text == "morph") return SelKind::Morph;
    if (text == "rigid") return SelKind::Rigid;
    if (text == "joint") return SelKind::Joint;
    if (text == "soft") return SelKind::Soft;
    if (text == "frame") return SelKind::Frame;
    if (text == "none") return SelKind::None;
    throw ApiError{"未知选择类型 \"" + text + "\"（bone/material/morph/rigid/joint/soft/frame/none）"};
}

JsonValue cmd_scene_select(Viewer& viewer, const JsonValue& params) {
    const std::string text = require_string(params, "kind");
    const SelKind kind = parse_sel_kind(text);
    int index = -1;
    if (kind != SelKind::None) {
        if (has(params, "index")) {
            const int count = [&]() {
                switch (kind) {
                    case SelKind::Bone: return static_cast<int>(viewer.model.bones.size());
                    case SelKind::Material: return static_cast<int>(viewer.model.materials.size());
                    case SelKind::Morph: return static_cast<int>(viewer.model.morphs.size());
                    case SelKind::Rigid: return static_cast<int>(viewer.model.rigid.size());
                    case SelKind::Joint: return static_cast<int>(viewer.model.joints.size());
                    case SelKind::Soft: return static_cast<int>(viewer.model.soft_bodies.size());
                    case SelKind::Frame: return static_cast<int>(viewer.model.frames.size());
                    default: return 0;
                }
            }();
            index = require_index(params, count, "对象");
        } else {
            const JsonValue* name = params.find("name");
            if (!name || !name->is_string()) throw ApiError{"需要 index 或 name"};
            std::vector<std::string> names;
            std::vector<std::string> names_en;
            switch (kind) {
                case SelKind::Bone:
                    for (const auto& b : viewer.model.bones) { names.push_back(b.name); names_en.push_back(b.name_en); }
                    break;
                case SelKind::Material:
                    for (const auto& m : viewer.model.materials) { names.push_back(m.name); names_en.push_back(m.name_en); }
                    break;
                case SelKind::Morph:
                    for (const auto& m : viewer.model.morphs) { names.push_back(m.name); names_en.push_back(m.name_en); }
                    break;
                case SelKind::Rigid:
                    for (const auto& r : viewer.model.rigid) { names.push_back(r.name); names_en.push_back(r.name_en); }
                    break;
                case SelKind::Joint:
                    for (const auto& j : viewer.model.joints) { names.push_back(j.name); names_en.push_back(j.name_en); }
                    break;
                case SelKind::Soft:
                    for (const auto& s : viewer.model.soft_bodies) { names.push_back(s.name); names_en.push_back(s.name_en); }
                    break;
                case SelKind::Frame:
                    for (const auto& f : viewer.model.frames) { names.push_back(f.name); names_en.push_back(f.name_en); }
                    break;
                default: break;
            }
            index = find_named(names, names_en, name->string);
            if (index < 0) throw ApiError{"找不到对象 \"" + name->string + "\""};
        }
    }
    viewer.select(kind, index);
    return selection_json(viewer);
}

JsonValue cmd_bone_list(const Viewer& viewer, const JsonValue& params) {
    std::string filter;
    if (const JsonValue* node = params.find("filter")) {
        if (!node->is_string()) throw ApiError{"参数 \"filter\" 必须是字符串"};
        filter = node->string;
    }
    JsonValue out = pmx::json_array();
    for (std::size_t i = 0; i < viewer.model.bones.size(); ++i) {
        const pmx::Bone& bone = viewer.model.bones[i];
        if (!filter.empty() && bone.name.find(filter) == std::string::npos &&
            bone.name_en.find(filter) == std::string::npos)
            continue;
        JsonValue item = pmx::json_object();
        pmx::json_set(item, "index", pmx::json_number(static_cast<double>(i)));
        set_string(item, "name", bone.name);
        set_string(item, "name_en", bone.name_en);
        pmx::json_set(item, "parent", pmx::json_number(bone.parent));
        pmx::json_set(item, "pos", vec3_json(bone.pos));
        pmx::json_set(item, "flags", pmx::json_number(bone.flags));
        pmx::json_push(out, std::move(item));
    }
    return out;
}

JsonValue cmd_bone_pose(Viewer& viewer, const JsonValue& params) {
    if (!viewer.pose) throw ApiError{"尚未加载模型"};
    const int index = resolve_bone(viewer.model, params);
    pmx::Pose& pose = *viewer.pose;

    if (optional_bool(params, "reset", false)) {
        pose.ov_rot.erase(index);
        pose.ov_trans.erase(index);
    } else {
        if (const JsonValue* rotation = params.find("rotation")) {
            const pmx::Vec4 q = read_vec4(*rotation, "rotation");
            if (std::fabs(q.x) + std::fabs(q.y) + std::fabs(q.z) + std::fabs(q.w) < 1e-8f)
                throw ApiError{"rotation 不能是零四元数"};
            pose.ov_rot[index] = pmx::normalize(pmx::Quat(q.x, q.y, q.z, q.w));
        } else if (const JsonValue* euler = params.find("euler")) {
            const pmx::Vec3 angles = read_vec3(*euler, "euler");
            constexpr float kDegToRad = pmx::kPi / 180.0f;
            pose.ov_rot[index] = pmx::euler_to_quat(angles.x * kDegToRad, angles.y * kDegToRad,
                                                    angles.z * kDegToRad);
        }
        if (const JsonValue* translation = params.find("translation"))
            pose.ov_trans[index] = read_vec3(*translation, "translation");
    }
    if (has(params, "ik_enabled"))
        pose.set_ik_enabled(index, optional_bool(params, "ik_enabled", true));

    if (viewer.morph_dirty) viewer.apply_morphs();
    else pose.apply(&viewer.morph_weights);
    if (viewer.preview_mode) pose.solve_ik();
    pose.world();

    const pmx::Bone& bone = viewer.model.bones[static_cast<std::size_t>(index)];
    JsonValue out = pmx::json_object();
    pmx::json_set(out, "index", pmx::json_number(index));
    set_string(out, "name", bone.name);
    pmx::json_set(out, "rotation", quat_json(pose.rot[static_cast<std::size_t>(index)]));
    pmx::json_set(out, "translation", vec3_json(pose.trans[static_cast<std::size_t>(index)]));
    return out;
}

JsonValue cmd_morph_list(const Viewer& viewer) {
    JsonValue out = pmx::json_array();
    for (std::size_t i = 0; i < viewer.model.morphs.size(); ++i) {
        const pmx::Morph& morph = viewer.model.morphs[i];
        JsonValue item = pmx::json_object();
        pmx::json_set(item, "index", pmx::json_number(static_cast<double>(i)));
        set_string(item, "name", morph.name);
        set_string(item, "name_en", morph.name_en);
        pmx::json_set(item, "panel", pmx::json_number(morph.panel));
        pmx::json_set(item, "type", pmx::json_number(morph.type));
        pmx::json_set(item, "offsets", pmx::json_number(static_cast<double>(morph.offsets.size())));
        const float weight = i < viewer.morph_values.size() ? viewer.morph_values[i] : 0.0f;
        pmx::json_set(item, "weight", pmx::json_number(weight));
        pmx::json_push(out, std::move(item));
    }
    return out;
}

JsonValue cmd_morph_set(Viewer& viewer, const JsonValue& params) {
    if (!viewer.pose) throw ApiError{"尚未加载模型"};
    const int index = resolve_morph(viewer.model, params);
    const float weight = optional_float(params, "weight", 0.0f);
    if (!std::isfinite(weight)) throw ApiError{"weight 必须是有限数字"};
    viewer.morph_values[static_cast<std::size_t>(index)] = weight;
    viewer.morph_weights.clear();
    for (std::size_t i = 0; i < viewer.morph_values.size(); ++i)
        if (viewer.morph_values[i] > 1e-4f)
            viewer.morph_weights.emplace_back(static_cast<int>(i), viewer.morph_values[i]);
    viewer.morph_dirty = true;
    viewer.apply_morphs();
    if (viewer.preview_mode) viewer.pose->solve_ik();
    viewer.pose->world();

    const pmx::Morph& morph = viewer.model.morphs[static_cast<std::size_t>(index)];
    JsonValue out = pmx::json_object();
    pmx::json_set(out, "index", pmx::json_number(index));
    set_string(out, "name", morph.name);
    pmx::json_set(out, "weight", pmx::json_number(weight));
    return out;
}

JsonValue cmd_material_list(const Viewer& viewer) {
    JsonValue out = pmx::json_array();
    for (std::size_t i = 0; i < viewer.model.materials.size(); ++i) {
        const pmx::Material& material = viewer.model.materials[i];
        JsonValue item = pmx::json_object();
        pmx::json_set(item, "index", pmx::json_number(static_cast<double>(i)));
        set_string(item, "name", material.name);
        set_string(item, "name_en", material.name_en);
        pmx::json_set(item, "diffuse", vec4_json(material.diffuse));
        pmx::json_set(item, "texture", pmx::json_number(material.texture));
        pmx::json_set(item, "sphere", pmx::json_number(material.sphere));
        pmx::json_set(item, "sphere_mode", pmx::json_number(material.sphere_mode));
        pmx::json_set(item, "toon", pmx::json_number(material.toon));
        pmx::json_set(item, "flags", pmx::json_number(material.draw_flags));
        pmx::json_set(item, "double_sided",
                      pmx::json_bool((material.draw_flags & pmx::MF_DOUBLE_SIDED) != 0));
        pmx::json_set(item, "edge", pmx::json_bool((material.draw_flags & pmx::MF_EDGE) != 0));
        pmx::json_set(item, "edge_size", pmx::json_number(material.edge_size));
        pmx::json_set(item, "faces", pmx::json_number(material.face_count));
        pmx::json_push(out, std::move(item));
    }
    return out;
}

JsonValue cmd_material_set(Viewer& viewer, const JsonValue& params) {
    if (!viewer.pose) throw ApiError{"尚未加载模型"};
    if (viewer.preview_mode) throw ApiError{"预览模式下模型只读，请先切换到编辑模式"};
    const int index = resolve_material(viewer.model, params);

    viewer.edit(
        "Agent 材质",
        [&](pmx::Model& model) {
            pmx::Material& material = model.materials[static_cast<std::size_t>(index)];
            if (has(params, "alpha"))
                material.diffuse.w = std::clamp(optional_float(params, "alpha", material.diffuse.w),
                                                0.0f, 1.0f);
            if (const JsonValue* diffuse = params.find("diffuse")) {
                const pmx::Vec4 value = read_vec4(*diffuse, "diffuse");
                material.diffuse = value;
            }
            if (has(params, "double_sided")) {
                const bool value = optional_bool(params, "double_sided", true);
                if (value) material.draw_flags |= pmx::MF_DOUBLE_SIDED;
                else material.draw_flags &= static_cast<std::uint8_t>(~pmx::MF_DOUBLE_SIDED);
            }
            if (has(params, "edge")) {
                const bool value = optional_bool(params, "edge", true);
                if (value) material.draw_flags |= pmx::MF_EDGE;
                else material.draw_flags &= static_cast<std::uint8_t>(~pmx::MF_EDGE);
            }
            if (has(params, "edge_size"))
                material.edge_size = optional_float(params, "edge_size", material.edge_size);
            if (has(params, "texture"))
                material.texture = optional_int(params, "texture", material.texture);
        },
        "material");

    const pmx::Material& material = viewer.model.materials[static_cast<std::size_t>(index)];
    JsonValue out = pmx::json_object();
    pmx::json_set(out, "index", pmx::json_number(index));
    set_string(out, "name", material.name);
    pmx::json_set(out, "diffuse", vec4_json(material.diffuse));
    pmx::json_set(out, "flags", pmx::json_number(material.draw_flags));
    return out;
}

JsonValue cmd_animation_load(Viewer& viewer, const JsonValue& params) {
    const std::string path = require_string(params, "path");
    if (!pmx::path_exists(path)) throw ApiError{"文件不存在：" + path};
    if (optional_bool(params, "preview", true) && !viewer.preview_mode)
        viewer.set_preview_mode(true);

    std::string error;
    const bool camera_only = optional_bool(params, "camera_only", false);
    const bool ok = camera_only ? viewer.load_camera_file(path, &error)
                                : viewer.load_vmd_file(path, &error);
    if (!ok) throw ApiError{error};
    if (viewer.pose) {
        if (viewer.morph_dirty) viewer.apply_morphs();
        else viewer.pose->apply(&viewer.morph_weights);
        viewer.pose->solve_ik();
        viewer.pose->world();
    }
    return animation_state(viewer);
}

JsonValue cmd_animation_play(Viewer& viewer, const JsonValue& params) {
    if (!viewer.pose) throw ApiError{"尚未加载模型"};
    if (!viewer.motion && !viewer.timeline.clip && !viewer.camera_motion.loaded())
        throw ApiError{"尚未导入动作（先调用 animation.load）"};
    if (!viewer.preview_mode) viewer.set_preview_mode(true);

    if (has(params, "playing"))
        viewer.motion_playing = optional_bool(params, "playing", viewer.motion_playing);
    if (has(params, "frame"))
        viewer.motion_frame = std::max(0.0f, optional_float(params, "frame", viewer.motion_frame));
    if (has(params, "speed"))
        viewer.motion_speed = optional_float(params, "speed", viewer.motion_speed);
    if (has(params, "loop"))
        viewer.motion_loop = optional_bool(params, "loop", viewer.motion_loop);

    viewer.motion_dirty = true;
    if (viewer.motion || viewer.timeline.clip || viewer.camera_motion.loaded()) {
        viewer.apply_motion_frame();
        if (viewer.camera_view) viewer.apply_camera_motion();
        viewer.motion_dirty = false;
    }
    if (viewer.morph_dirty) viewer.apply_morphs();
    else viewer.pose->apply(&viewer.morph_weights);
    viewer.pose->solve_ik();
    viewer.pose->world();
    return animation_state(viewer);
}

JsonValue dispatch(Viewer& viewer, const std::string& method, const JsonValue& params) {
    if (method == "system.ping") {
        JsonValue out = pmx::json_object();
        pmx::json_set(out, "pong", pmx::json_bool(true));
        const double seconds = std::chrono::duration<double>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();
        pmx::json_set(out, "time", pmx::json_number(seconds));
        return out;
    }
    if (method == "system.info") {
        JsonValue out = pmx::json_object();
        set_string(out, "name", "PmxStudio");
        set_string(out, "version", PMXSTUDIO_VERSION);
        set_string(out, "api", "PmxStudio Agent API");
        pmx::json_set(out, "api_version", pmx::json_number(1));
        set_string(out, "protocol", "JSON-RPC 2.0 over TCP（换行分隔）");
        set_string(out, "plugin_abi", "PmxHostV1");
        pmx::json_set(out, "plugin_abi_version", pmx::json_number(PMX_PLUGIN_ABI_VERSION));
#ifdef _WIN32
        set_string(out, "platform", "windows");
#elif defined(__APPLE__)
        set_string(out, "platform", "macos");
#else
        set_string(out, "platform", "linux");
#endif
        pmx::json_set(out, "model_loaded", pmx::json_bool(viewer.pose != nullptr));
        set_string(out, "model_path", viewer.model_path);
        return out;
    }

    if (method == "model.load") return cmd_model_load(viewer, params);
    if (method == "model.info") return model_info(viewer);
    if (method == "model.save") return cmd_model_save(viewer, params);
    if (method == "model.script") return cmd_model_script(viewer, params);
    if (method == "model.bbox") {
        JsonValue info = model_info(viewer);
        const JsonValue* bbox = info.find("bbox");
        return bbox ? *bbox : pmx::json_object();
    }

    if (method == "scene.state") return cmd_scene_state(viewer);
    if (method == "scene.mode") {
        if (!viewer.pose) throw ApiError{"尚未加载模型"};
        viewer.set_preview_mode(optional_bool(params, "preview", viewer.preview_mode));
        return cmd_scene_state(viewer);
    }
    if (method == "scene.camera") return cmd_scene_camera(viewer, params);
    if (method == "scene.camera_fit") {
        viewer.fit_camera();
        return camera_json(viewer);
    }
    if (method == "scene.settings") {
        apply_settings(viewer, params);
        return settings_json(viewer);
    }
    if (method == "scene.select") return cmd_scene_select(viewer, params);
    if (method == "scene.render") return cmd_scene_render(viewer, params);
    if (method == "scene.undo") {
        viewer.undo();
        return cmd_scene_state(viewer);
    }
    if (method == "scene.redo") {
        viewer.redo();
        return cmd_scene_state(viewer);
    }

    if (method == "animation.load") return cmd_animation_load(viewer, params);
    if (method == "animation.play") return cmd_animation_play(viewer, params);
    if (method == "animation.state") return animation_state(viewer);

    if (method == "bone.list") return cmd_bone_list(viewer, params);
    if (method == "bone.pose") return cmd_bone_pose(viewer, params);

    if (method == "morph.list") return cmd_morph_list(viewer);
    if (method == "morph.set") return cmd_morph_set(viewer, params);

    if (method == "material.list") return cmd_material_list(viewer);
    if (method == "material.set") return cmd_material_set(viewer, params);

    throw ApiError{"未知方法 \"" + method + "\"", -32601};
}

// ---------------------------------------------------------------- 传输层
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;
#endif

constexpr std::size_t kMaxRequestBytes = 4u * 1024u * 1024u;
constexpr std::size_t kMaxPendingBytes = 8u * 1024u * 1024u;
constexpr int kMaxConnections = 16;
constexpr int kMaxRequestsPerFrame = 64;

bool would_block() {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN;
#endif
}

bool set_nonblocking(Socket socket) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

void close_socket(Socket socket) {
    if (socket == kInvalidSocket) return;
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

struct Connection {
    Socket fd = kInvalidSocket;
    std::string in;
    std::string out;
    bool peer_closed = false;
    bool dead = false;
};

JsonValue error_object(int code, const std::string& message) {
    JsonValue error = pmx::json_object();
    pmx::json_set(error, "code", pmx::json_number(code));
    set_string(error, "message", message);
    return error;
}

}  // namespace

// ---------------------------------------------------------------- api_execute
bool api_execute(Viewer& viewer, const std::string& method, const pmx::JsonValue& params,
                 pmx::JsonValue& result, std::string& error, int* error_code) {
    try {
        result = dispatch(viewer, method, params);
        return true;
    } catch (const ApiError& api_error) {
        error = api_error.message;
        if (error_code) *error_code = api_error.code;
        return false;
    } catch (const std::exception& exception) {
        error = exception.what();
        if (error_code) *error_code = -32603;
        return false;
    }
}

// ---------------------------------------------------------------- ApiServer
struct ApiServer::Impl {
    Socket listen_fd = kInvalidSocket;
    int port = 0;
    std::vector<Connection> connections;
    int requests = 0;
    std::string last_error;
    PluginListFn plugin_list;
    PluginInvokeFn plugin_invoke;
#ifdef _WIN32
    bool winsock_ready = false;
#endif

    ~Impl() { close_all(); }

    void close_all() {
        for (Connection& connection : connections) close_socket(connection.fd);
        connections.clear();
        close_socket(listen_fd);
        listen_fd = kInvalidSocket;
        port = 0;
    }

    std::string handle_request(Viewer& viewer, const std::string& line) {
        JsonValue request;
        try {
            request = pmx::parse_json(line);
        } catch (const pmx::ParseError& parse_error) {
            return make_error(pmx::json_null(), -32700,
                              "JSON 解析失败（第 " + std::to_string(parse_error.pos) +
                                  " 字节）：" + parse_error.message);
        }
        if (!request.is_object())
            return make_error(pmx::json_null(), -32600, "请求必须是 JSON 对象");

        const JsonValue* id = request.find("id");
        const JsonValue* method = request.find("method");
        if (!method || !method->is_string())
            return make_error(id ? *id : pmx::json_null(), -32600, "缺少 method 字符串");

        const JsonValue* params_ptr = request.find("params");
        JsonValue params =
            params_ptr && params_ptr->is_object() ? *params_ptr : pmx::json_object();

        const std::string& name = method->string;
        JsonValue result;
        std::string error;
        int code = -32000;
        bool ok = false;

        if (name == "plugin.list") {
            if (plugin_list) {
                result = plugin_list();
            } else {
                result = pmx::json_object();
                pmx::json_set(result, "plugins", pmx::json_array());
                pmx::json_set(result, "commands", pmx::json_array());
            }
            ok = true;
        } else if (name.rfind("plugin.", 0) == 0) {
            if (!plugin_invoke) {
                error = "未启用插件命令分发";
                code = -32601;
            } else {
                const std::string params_json = pmx::json_dump(params);
                std::string result_json;
                if (plugin_invoke(name, params_json, result_json, error)) {
                    try {
                        result = pmx::parse_json(result_json);
                        ok = true;
                    } catch (const pmx::ParseError& parse_error) {
                        error = "插件返回非法 JSON：" + parse_error.message;
                        code = -32603;
                    }
                }
            }
        } else {
            ok = api_execute(viewer, name, params, result, error, &code);
            if (ok && name == "system.info" && plugin_list) {
                pmx::json_set(result, "plugins", plugin_list());
            }
        }

        // 没有 id 的是通知：执行但不应答。
        if (!id) return std::string();
        if (ok) {
            JsonValue response = pmx::json_object();
            set_string(response, "jsonrpc", "2.0");
            pmx::json_set(response, "id", *id);
            pmx::json_set(response, "result", std::move(result));
            return pmx::json_dump(response);
        }
        return make_error(*id, code, error);
    }

    static std::string make_error(const JsonValue& id, int code, const std::string& message) {
        JsonValue response = pmx::json_object();
        set_string(response, "jsonrpc", "2.0");
        pmx::json_set(response, "id", id);
        pmx::json_set(response, "error", error_object(code, message));
        return pmx::json_dump(response);
    }

    void accept_pending() {
        while (static_cast<int>(connections.size()) < kMaxConnections) {
            sockaddr_in peer{};
            socklen_t length = sizeof(peer);
            const Socket fd = accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &length);
            if (fd == kInvalidSocket) return;
            if (!set_nonblocking(fd)) {
                close_socket(fd);
                continue;
            }
            Connection connection;
            connection.fd = fd;
            connections.push_back(std::move(connection));
        }
    }

    static bool read_available(Connection& connection) {
        char buffer[16384];
        while (true) {
            const int n = static_cast<int>(recv(connection.fd, buffer, sizeof(buffer), 0));
            if (n > 0) {
                connection.in.append(buffer, static_cast<std::size_t>(n));
                if (connection.in.size() > kMaxRequestBytes) {
                    connection.in.clear();
                    connection.dead = true;
                }
            } else if (n == 0) {
                connection.peer_closed = true;
                return true;
            } else if (would_block()) {
                return true;
            } else {
                connection.dead = true;
                return false;
            }
        }
    }

    static bool flush(Connection& connection) {
        while (!connection.out.empty()) {
            const std::size_t chunk = std::min<std::size_t>(connection.out.size(), 65536);
            const int n = static_cast<int>(send(connection.fd, connection.out.data(),
                                                static_cast<int>(chunk), 0));
            if (n > 0) {
                connection.out.erase(0, static_cast<std::size_t>(n));
            } else if (n < 0 && would_block()) {
                return true;
            } else {
                connection.dead = true;
                return false;
            }
        }
        return true;
    }
};

ApiServer::ApiServer() : impl_(std::make_unique<Impl>()) {}

ApiServer::~ApiServer() { stop(); }

bool ApiServer::start(int port, std::string* error) {
    if (impl_->listen_fd != kInvalidSocket) {
        if (error) *error = "API 已在运行";
        return false;
    }
#ifdef _WIN32
    if (!impl_->winsock_ready) {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            if (error) *error = "WSAStartup 失败";
            return false;
        }
        impl_->winsock_ready = true;
    }
#endif
    const Socket fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kInvalidSocket) {
        if (error) *error = "创建 socket 失败";
        return false;
    }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one),
               sizeof(one));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<unsigned short>(std::clamp(port, 0, 65535)));
    if (bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        close_socket(fd);
        if (error) *error = "无法绑定 127.0.0.1:" + std::to_string(port) + "（端口被占用？）";
        return false;
    }
    if (listen(fd, 8) != 0) {
        close_socket(fd);
        if (error) *error = "listen 失败";
        return false;
    }
    if (!set_nonblocking(fd)) {
        close_socket(fd);
        if (error) *error = "无法把监听 socket 设为非阻塞";
        return false;
    }

    sockaddr_in bound{};
    socklen_t length = sizeof(bound);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &length) == 0)
        impl_->port = ntohs(bound.sin_port);
    else
        impl_->port = port;

    impl_->listen_fd = fd;
    impl_->last_error.clear();
    return true;
}

void ApiServer::stop() {
    if (!impl_) return;
    impl_->close_all();
#ifdef _WIN32
    if (impl_->winsock_ready) {
        WSACleanup();
        impl_->winsock_ready = false;
    }
#endif
}

bool ApiServer::running() const { return impl_ && impl_->listen_fd != kInvalidSocket; }

int ApiServer::port() const { return impl_ ? impl_->port : 0; }

void ApiServer::set_plugin_hooks(PluginListFn list, PluginInvokeFn invoke) {
    if (!impl_) return;
    impl_->plugin_list = std::move(list);
    impl_->plugin_invoke = std::move(invoke);
}

void ApiServer::poll(Viewer& viewer) {
    if (!running()) return;
    impl_->accept_pending();

    int processed = 0;
    for (Connection& connection : impl_->connections) {
        if (connection.dead) continue;
        Impl::read_available(connection);
        while (!connection.dead && processed < kMaxRequestsPerFrame) {
            const std::size_t newline = connection.in.find('\n');
            if (newline == std::string::npos) break;
            std::string line = connection.in.substr(0, newline);
            connection.in.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            const std::string response = impl_->handle_request(viewer, line);
            ++processed;
            ++impl_->requests;
            if (!response.empty()) {
                connection.out += response;
                connection.out.push_back('\n');
            }
            if (connection.out.size() > kMaxPendingBytes) {
                connection.dead = true;
                break;
            }
        }
        Impl::flush(connection);
    }

    const auto remove_dead = [](const Connection& connection) {
        return connection.dead ||
               (connection.peer_closed && connection.in.empty() && connection.out.empty());
    };
    for (Connection& connection : impl_->connections) {
        if (remove_dead(connection)) close_socket(connection.fd);
    }
    impl_->connections.erase(
        std::remove_if(impl_->connections.begin(), impl_->connections.end(), remove_dead),
        impl_->connections.end());
}

int ApiServer::requests() const { return impl_ ? impl_->requests : 0; }

std::string ApiServer::last_error() const { return impl_ ? impl_->last_error : std::string(); }

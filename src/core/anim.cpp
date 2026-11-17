#include "core/anim.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>

namespace pmx {
namespace {

template <class Key>
void sort_and_dedupe(std::vector<Key>& keys) {
    std::stable_sort(keys.begin(), keys.end(),
                     [](const Key& a, const Key& b) { return a.frame < b.frame; });
    std::size_t write = 0;
    for (std::size_t read = 0; read < keys.size(); ++read) {
        if (write > 0 && keys[write - 1].frame == keys[read].frame) {
            keys[write - 1] = keys[read];
        } else {
            keys[write++] = keys[read];
        }
    }
    keys.resize(write);
}

template <class Key>
int set_key_in(std::vector<Key>& keys, const Key& key) {
    const auto it = std::lower_bound(keys.begin(), keys.end(), key.frame,
                                     [](const Key& k, int f) { return k.frame < f; });
    const std::size_t index = static_cast<std::size_t>(it - keys.begin());
    if (it != keys.end() && it->frame == key.frame) {
        keys[index] = key;
    } else {
        keys.insert(it, key);
    }
    return static_cast<int>(index);
}

template <class Key>
int max_key_frame(const std::vector<Key>& keys) {
    int out = 0;
    for (const Key& k : keys) out = std::max(out, k.frame);
    return out;
}

// 采样：取 <= frame 的最后一个关键帧下标；空轨返回 false。
template <class Key>
bool locate_key(const std::vector<Key>& keys, double frame, std::size_t& lo, std::size_t& hi,
                float& t) {
    if (keys.empty()) return false;
    if (frame <= keys.front().frame) {
        lo = hi = 0;
        t = 0.0f;
        return true;
    }
    if (frame >= keys.back().frame) {
        lo = hi = keys.size() - 1;
        t = 0.0f;
        return true;
    }
    const std::size_t upper = static_cast<std::size_t>(
        std::upper_bound(keys.begin(), keys.end(), frame,
                         [](double f, const Key& k) { return f < k.frame; }) -
        keys.begin());
    lo = upper - 1;
    hi = upper;
    const float span = static_cast<float>(keys[hi].frame - keys[lo].frame);
    t = span > 0.0f ? static_cast<float>(frame - keys[lo].frame) / span : 0.0f;
    return true;
}

}  // namespace

bool AnimClip::empty() const {
    return bones.empty() && morphs.empty() && ik.empty();
}

int AnimClip::max_frame() const {
    int out = 0;
    for (const AnimBoneTrack& track : bones) out = std::max(out, max_key_frame(track.keys));
    for (const AnimMorphTrack& track : morphs) out = std::max(out, max_key_frame(track.keys));
    out = std::max(out, max_key_frame(ik));
    return out;
}

void AnimClip::sort_keys() {
    for (AnimBoneTrack& track : bones) sort_and_dedupe(track.keys);
    for (AnimMorphTrack& track : morphs) sort_and_dedupe(track.keys);
    sort_and_dedupe(ik);
}

// ------------------------------------------------------------------ 相机动作

int CameraTrack::max_frame() const {
    return max_key_frame(keys);
}

void CameraTrack::sort_keys() {
    sort_and_dedupe(keys);
}

int CameraTrack::set_key(const AnimCameraKey& key) {
    return set_key_in(keys, key);
}

int AnimClip::find_bone_track(int bone, const std::string& name) const {
    for (std::size_t i = 0; i < bones.size(); ++i) {
        if (bone >= 0) {
            if (bones[i].bone == bone) return static_cast<int>(i);
        } else if (bones[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int AnimClip::find_morph_track(int morph, const std::string& name) const {
    for (std::size_t i = 0; i < morphs.size(); ++i) {
        if (morph >= 0) {
            if (morphs[i].morph == morph) return static_cast<int>(i);
        } else if (morphs[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

AnimBoneTrack& AnimClip::ensure_bone_track(int bone, const std::string& name) {
    const int index = find_bone_track(bone, name);
    if (index >= 0) return bones[static_cast<std::size_t>(index)];
    AnimBoneTrack track;
    track.name = name;
    track.bone = bone;
    bones.push_back(std::move(track));
    return bones.back();
}

AnimMorphTrack& AnimClip::ensure_morph_track(int morph, const std::string& name) {
    const int index = find_morph_track(morph, name);
    if (index >= 0) return morphs[static_cast<std::size_t>(index)];
    AnimMorphTrack track;
    track.name = name;
    track.morph = morph;
    morphs.push_back(std::move(track));
    return morphs.back();
}

int AnimClip::set_bone_key(int track_index, const AnimBoneKey& key) {
    if (track_index < 0 || track_index >= static_cast<int>(bones.size())) return -1;
    return set_key_in(bones[static_cast<std::size_t>(track_index)].keys, key);
}

int AnimClip::set_morph_key(int track_index, const AnimMorphKey& key) {
    if (track_index < 0 || track_index >= static_cast<int>(morphs.size())) return -1;
    return set_key_in(morphs[static_cast<std::size_t>(track_index)].keys, key);
}

AnimClip from_vmd(const VmdMotion& motion, const VmdBinding& binding) {
    AnimClip clip;
    clip.name = motion.model_name;

    clip.bones.reserve(motion.bone_tracks.size());
    for (std::size_t i = 0; i < motion.bone_tracks.size(); ++i) {
        const VmdBoneTrack& src = motion.bone_tracks[i];
        AnimBoneTrack track;
        track.name = src.name;
        track.bone = i < binding.bone_target.size() ? binding.bone_target[i] : -1;
        track.keys.reserve(src.keys.size());
        for (const VmdBoneKey& key : src.keys) {
            AnimBoneKey dst;
            dst.frame = key.frame;
            dst.pos = key.pos;
            dst.rot = key.rot;
            dst.curve = key.curve;
            track.keys.push_back(dst);
        }
        clip.bones.push_back(std::move(track));
    }

    clip.morphs.reserve(motion.morph_tracks.size());
    for (std::size_t i = 0; i < motion.morph_tracks.size(); ++i) {
        const VmdMorphTrack& src = motion.morph_tracks[i];
        AnimMorphTrack track;
        track.name = src.name;
        track.morph = i < binding.morph_target.size() ? binding.morph_target[i] : -1;
        track.keys.reserve(src.keys.size());
        for (const VmdMorphKey& key : src.keys)
            track.keys.push_back(AnimMorphKey{key.frame, key.weight});
        clip.morphs.push_back(std::move(track));
    }

    clip.ik.reserve(motion.ik_frames.size());
    for (std::size_t i = 0; i < motion.ik_frames.size(); ++i) {
        const VmdIkFrame& src = motion.ik_frames[i];
        AnimIkKey frame;
        frame.frame = src.frame;
        frame.visible = src.visible;
        const bool has_binding = i < binding.ik_states.size();
        frame.states.reserve(src.states.size());
        for (std::size_t k = 0; k < src.states.size(); ++k) {
            AnimIkState state;
            state.name = src.states[k].bone;
            state.bone = has_binding && k < binding.ik_states[i].size()
                             ? binding.ik_states[i][k].first
                             : -1;
            state.enabled = src.states[k].enabled;
            frame.states.push_back(std::move(state));
        }
        clip.ik.push_back(std::move(frame));
    }

    clip.sort_keys();
    return clip;
}

VmdMotion to_vmd(const AnimClip& clip, const Model* model, const std::string& model_name) {
    VmdMotion motion;
    motion.version = "0002";
    motion.model_name = model_name.empty() ? clip.name : model_name;

    motion.bone_tracks.reserve(clip.bones.size());
    for (const AnimBoneTrack& src : clip.bones) {
        VmdBoneTrack track;
        if (src.bone >= 0 && model != nullptr &&
            src.bone < static_cast<int>(model->bones.size()))
            track.name = model->bones[static_cast<std::size_t>(src.bone)].name;
        else
            track.name = src.name;
        track.keys.reserve(src.keys.size());
        for (const AnimBoneKey& key : src.keys) {
            VmdBoneKey dst;
            dst.frame = key.frame;
            dst.pos = key.pos;
            dst.rot = key.rot;
            dst.curve = key.curve;
            track.keys.push_back(dst);
        }
        std::stable_sort(track.keys.begin(), track.keys.end(),
                         [](const VmdBoneKey& a, const VmdBoneKey& b) {
                             return a.frame < b.frame;
                         });
        motion.bone_tracks.push_back(std::move(track));
    }

    motion.morph_tracks.reserve(clip.morphs.size());
    for (const AnimMorphTrack& src : clip.morphs) {
        VmdMorphTrack track;
        if (src.morph >= 0 && model != nullptr &&
            src.morph < static_cast<int>(model->morphs.size()))
            track.name = model->morphs[static_cast<std::size_t>(src.morph)].name;
        else
            track.name = src.name;
        track.keys.reserve(src.keys.size());
        for (const AnimMorphKey& key : src.keys)
            track.keys.push_back(VmdMorphKey{key.frame, key.weight});
        std::stable_sort(track.keys.begin(), track.keys.end(),
                         [](const VmdMorphKey& a, const VmdMorphKey& b) {
                             return a.frame < b.frame;
                         });
        motion.morph_tracks.push_back(std::move(track));
    }

    motion.ik_frames.reserve(clip.ik.size());
    for (const AnimIkKey& src : clip.ik) {
        VmdIkFrame frame;
        frame.frame = src.frame;
        frame.visible = src.visible;
        frame.states.reserve(src.states.size());
        for (const AnimIkState& state : src.states) {
            VmdIkState dst;
            if (state.bone >= 0 && model != nullptr &&
                state.bone < static_cast<int>(model->bones.size()))
                dst.bone = model->bones[static_cast<std::size_t>(state.bone)].name;
            else
                dst.bone = state.name;
            dst.enabled = state.enabled;
            frame.states.push_back(std::move(dst));
        }
        motion.ik_frames.push_back(std::move(frame));
    }
    std::stable_sort(motion.ik_frames.begin(), motion.ik_frames.end(),
                     [](const VmdIkFrame& a, const VmdIkFrame& b) {
                         return a.frame < b.frame;
                     });

    int max_frame = 0;
    for (const VmdBoneTrack& track : motion.bone_tracks)
        max_frame = std::max(max_frame, max_key_frame(track.keys));
    for (const VmdMorphTrack& track : motion.morph_tracks)
        max_frame = std::max(max_frame, max_key_frame(track.keys));
    max_frame = std::max(max_frame, max_key_frame(motion.ik_frames));
    motion.max_frame = max_frame;
    return motion;
}

CameraTrack camera_track_from_vmd(const VmdMotion& motion) {
    CameraTrack track;
    track.keys.reserve(motion.camera_track.size());
    for (const VmdCameraKey& src : motion.camera_track) {
        AnimCameraKey dst;
        dst.frame = src.frame;
        dst.distance = src.distance;
        dst.target = src.target;
        dst.rot = src.rot;
        dst.curve = src.curve;
        dst.fov = src.fov;
        dst.perspective = src.perspective;
        track.keys.push_back(dst);
    }
    track.sort_keys();
    return track;
}

VmdMotion to_vmd_camera(const CameraTrack& track) {
    VmdMotion motion;
    motion.version = "0002";
    // 相机 VMD 的头名称字段按 MMD / MMD 系工具的惯例写「カメラ・照明」，
    // 不沿用某个模型动作的 model_name。
    motion.model_name = "カメラ・照明";
    motion.camera_track.reserve(track.keys.size());
    for (const AnimCameraKey& src : track.keys) {
        VmdCameraKey dst;
        dst.frame = src.frame;
        dst.distance = src.distance;
        dst.target = src.target;
        dst.rot = src.rot;
        dst.curve = src.curve;
        dst.fov = src.fov;
        dst.perspective = src.perspective;
        motion.camera_track.push_back(dst);
    }
    std::stable_sort(motion.camera_track.begin(), motion.camera_track.end(),
                     [](const VmdCameraKey& a, const VmdCameraKey& b) {
                         return a.frame < b.frame;
                     });
    motion.camera_keys = static_cast<int>(motion.camera_track.size());
    motion.max_frame = max_key_frame(motion.camera_track);
    return motion;
}

void evaluate_clip(const AnimClip& clip, double frame, Pose& pose, MorphWeights* morph_weights) {
    pose.reset();
    // 表示枠 / IK：取 <= 当前帧的最后一个表示枠帧，状态保持到下一次变化。
    if (!clip.ik.empty()) {
        const int current = static_cast<int>(frame);
        std::size_t idx = 0;
        bool found = false;
        for (std::size_t i = 0; i < clip.ik.size(); ++i) {
            if (clip.ik[i].frame <= current) {
                idx = i;
                found = true;
            } else {
                break;
            }
        }
        if (found) {
            for (const AnimIkState& state : clip.ik[idx].states)
                if (state.bone >= 0) pose.set_ik_enabled(state.bone, state.enabled);
        }
    }

    const int n = pose.n;
    for (const AnimBoneTrack& track : clip.bones) {
        if (track.bone < 0 || track.bone >= n) continue;
        Vec3 pos;
        Quat rot;
        sample_bone_track(track, frame, pos, rot);
        pose.base_rot[static_cast<std::size_t>(track.bone)] = rot;
        pose.base_trans[static_cast<std::size_t>(track.bone)] = pos;
    }

    std::map<int, float> resolved;
    for (const AnimMorphTrack& track : clip.morphs) {
        if (track.morph < 0) continue;
        const float weight = sample_morph_track(track, frame);
        if (std::fabs(weight) > 1e-6f) resolved[track.morph] = weight;
    }
    MorphWeights weights;
    weights.reserve(resolved.size());
    for (const auto& [index, weight] : resolved) weights.emplace_back(index, weight);

    pose.invalidate_world();
    pose.apply(&weights);
    if (morph_weights) *morph_weights = std::move(weights);
}

bool sample_bone_track(const AnimBoneTrack& track, double frame, Vec3& pos, Quat& rot) {
    std::size_t lo = 0, hi = 0;
    float t = 0.0f;
    if (!locate_key(track.keys, frame, lo, hi, t)) {
        pos = Vec3{};
        rot = identity_quat();
        return false;
    }
    const AnimBoneKey& a = track.keys[lo];
    const AnimBoneKey& b = track.keys[hi];
    if (lo == hi) {
        pos = a.pos;
        rot = a.rot;
        return true;
    }
    const float ex = a.curve[0].ease(t);
    const float ey = a.curve[1].ease(t);
    const float ez = a.curve[2].ease(t);
    pos.x = a.pos.x + (b.pos.x - a.pos.x) * ex;
    pos.y = a.pos.y + (b.pos.y - a.pos.y) * ey;
    pos.z = a.pos.z + (b.pos.z - a.pos.z) * ez;
    rot = slerp_quat(a.rot, b.rot, a.curve[3].ease(t));
    return true;
}

float sample_morph_track(const AnimMorphTrack& track, double frame) {
    std::size_t lo = 0, hi = 0;
    float t = 0.0f;
    if (!locate_key(track.keys, frame, lo, hi, t)) return 0.0f;
    const AnimMorphKey& a = track.keys[lo];
    const AnimMorphKey& b = track.keys[hi];
    if (lo == hi) return a.weight;
    return a.weight + (b.weight - a.weight) * t;
}

bool sample_camera_track(const std::vector<AnimCameraKey>& keys, double frame,
                         AnimCameraKey& out) {
    std::size_t lo = 0, hi = 0;
    float t = 0.0f;
    if (!locate_key(keys, frame, lo, hi, t)) return false;
    const AnimCameraKey& a = keys[lo];
    const AnimCameraKey& b = keys[hi];
    out = a;
    if (lo == hi) {
        out.frame = a.frame;
        return true;
    }
    const float ex = a.curve[0].ease(t);
    const float ey = a.curve[1].ease(t);
    const float ez = a.curve[2].ease(t);
    out.target.x = a.target.x + (b.target.x - a.target.x) * ex;
    out.target.y = a.target.y + (b.target.y - a.target.y) * ey;
    out.target.z = a.target.z + (b.target.z - a.target.z) * ez;
    const float er = a.curve[3].ease(t);
    out.rot.x = a.rot.x + (b.rot.x - a.rot.x) * er;
    out.rot.y = a.rot.y + (b.rot.y - a.rot.y) * er;
    out.rot.z = a.rot.z + (b.rot.z - a.rot.z) * er;
    out.distance = a.distance + (b.distance - a.distance) * t;
    out.fov = static_cast<int>(std::lround(a.fov + (b.fov - a.fov) * t));
    out.frame = static_cast<int>(std::lround(frame));
    if (out.frame < a.frame) out.frame = a.frame;
    if (out.frame > b.frame) out.frame = b.frame;
    return true;
}

AnimBoneKey make_bone_key(const Pose& pose, int bone, int frame) {
    AnimBoneKey key;
    key.frame = frame;
    if (bone >= 0 && bone < pose.n) {
        const std::size_t index = static_cast<std::size_t>(bone);
        key.pos = pose.base_trans[index];
        key.rot = pose.base_rot[index];
    }
    return key;
}

AnimMorphKey make_morph_key(const MorphWeights& weights, int morph, int frame) {
    AnimMorphKey key;
    key.frame = frame;
    for (const auto& [index, weight] : weights) {
        if (index == morph) {
            key.weight = weight;
            break;
        }
    }
    return key;
}

Quat camera_rot_quat(const Vec3& euler) {
    return rigid_euler_quat(euler.x, euler.y, euler.z);
}

Vec3 camera_rot_euler(const Quat& q, const Vec3& previous) {
    const Mat3 m = quat_to_mat3(q);
    float rx = std::asin(clampf(-m.at(1, 2), -1.0f, 1.0f));
    float ry = std::atan2(m.at(0, 2), m.at(2, 2));
    float rz = std::atan2(m.at(1, 0), m.at(1, 1));

    // 俯仰接近 ±90° 时存在等价分支，按上一帧角度选连续的。
    float ax, ay, az;
    euler_alt(rx, ry, rz, ax, ay, az);
    if (euler_distance(ax, ay, az, previous.x, previous.y, previous.z) <
        euler_distance(rx, ry, rz, previous.x, previous.y, previous.z)) {
        rx = ax;
        ry = ay;
        rz = az;
    }
    return Vec3{rx, ry, rz};
}

}  // namespace pmx

// 检查器面板入口（每个面板一个 .cpp，避免 main.cpp 继续膨胀）。
#pragma once

struct Viewer;

void panel_info(Viewer& v);
void panel_bone(Viewer& v);
void panel_frame(Viewer& v);
void panel_vertex(Viewer& v);
void panel_material(Viewer& v);
void panel_face(Viewer& v);
void panel_morph(Viewer& v);
void panel_motion(Viewer& v);
void panel_physics(Viewer& v);
void panel_soft(Viewer& v);
void panel_settings(Viewer& v);
// 灯光：MMD 风格的一盏方向光 + 环境色，含视口光源指示开关与预设。
void panel_light(Viewer& v);

// 预览模式（模型只读）专用的控制面板。
void panel_preview_search(Viewer& v);
void panel_preview_physics(Viewer& v);
void panel_preview_morph(Viewer& v);
// 预览模式的时间线：轨道树、关键帧列表与曲线编辑（实现在 panels/timeline.cpp）。
void panel_timeline(Viewer& v);
// 相机：独立于模型动作的相机动作导入 / 保存 / 关键帧与视角控制。
void panel_camera(Viewer& v);
// 输出：离屏渲染、序列帧、FFmpeg 编码与音频对轴（panels/output.cpp）。
void panel_output(Viewer& v);
// 烘焙：实时物理固化到骨骼轨（panels/bake.cpp）。
void panel_bake(Viewer& v);

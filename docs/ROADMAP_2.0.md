# PmxStudio 2.0 路线图（计划书）

> 目标：在保持「原生 C++、单文件 exe、断网可构建」的前提下，把 PmxStudio 从
> **PMX 编辑器 + VMD 播放器** 扩展为 **围绕 PMX/VMD 生态的闭环工作站**：
> 模型微调 → 动作创作 → 物理定型 → 成片输出，全部在同一个轻量环境内完成。
>
> 本文件只描述计划，不含实现。分支：`PmxStudio2.0`。

---

## 1. 现状盘点

### 1.1 已有能力（v1.1.1）

| 领域 | 现状 | 代码落点 |
| --- | --- | --- |
| PMX 读写 | 2.0 / 2.1 全字段 round-trip，编码与索引宽度保留 | `src/core/pmx_io.cpp` |
| 姿态 | FK、付与继承、CCD IK、逐轴限位、拖动骨骼、VPD 导入导出 | `src/core/pose.cpp`、`src/core/vpd.cpp` |
| 动作 | VMD 解析 / 绑定 / 采样（逐轴贝塞尔）、循环、变速、拖帧 | `src/core/vmd.cpp` |
| 动作导出 | **仅当前单帧** | `save_vmd(..., int frame)`，`src/core/vmd.hpp:105` |
| 相机轨 | 只统计帧数，**数据被跳过** | `src/core/vmd.cpp:336-342` |
| 形态 | 顶点 / UV / 材质 / 骨骼 / 组 / 翻转 / 刚体偏移 | `src/core/morph.cpp` |
| 物理 | Bullet 刚体 + 6DOF 关节 + 柔体，实时演算 | `src/physics/physics.cpp` |
| 渲染 | Compute 蒙皮、材质 / 球面 / Toon、模板描边、叠加层 | `src/gpu/renderer.cpp` |
| 输出 | **只有视口**，无序列帧、无编码 | — |
| 音频 | 无 | — |
| 脚本 | 无 | — |
| 模式 | 编辑模式改数据；预览模式播放 VMD / 摆姿 / 物理 | `Viewer::preview_mode` |

### 1.2 关键约束（决定架构取舍）

1. **单文件分发**：Windows 产物是一个 exe，不依赖 DLL / Python 运行时。
2. **断网构建**：第三方源码随仓库提供，不引入需要在线拉取的重依赖。
3. **编辑模式只读数据**：VMD / 姿态覆盖 / 物理回写只在预览模式求值
   （`src/app/main.cpp:1134-1137`、`1411-1421`）。2.0 的时间线必须遵守同样的隔离。
4. **物理推进与渲染帧率绑定**：主循环按 `1.0f/60.0f` 调 `Physics::step`
   （`src/app/main.cpp:1417`），慢机器上物理时间会滞后。烘焙要求「动画时间驱动」，
   这是一处必须修的正确性问题。
5. **相机轨缺失**：VMD 相机帧 61 字节被整体跳过，无法运镜。

---

## 2. 目标形态与非目标

### 2.1 目标形态（2.0 完成时）

- 底部时间线：骨骼 / 形态 / 相机 / IK 四条轨道，关键帧增删改、框选、拖动、
  贝塞尔曲线编辑，手动摆姿可直接打帧。
- 相机动作：独立于模型动作的一份机位数据；VMD 相机帧解析、单独导入 / 保存 /
  清除，共用播放头播放，按相机轨取景导出。
- 物理烘焙：把实时演算结果逐帧固化为骨骼轨，可复现、可分发。
- 输出：离屏渲染 → PNG 序列帧 → 调用系统 ffmpeg 编码 mp4；音频作为对轴基准。
- 脚本：无 GL 的 `pmxstudio-cli` 批处理 + JSON 命令，覆盖重命名 / 批量改参数 / 烘焙 / 导出。
- 模块化：编译期开关 + 运行时工作区预设，核心编辑器保持轻快。

### 2.2 非目标（明确不做）

- 不做节点式材质编辑器、不做 Blender 替代、不做多模型同场景合成。
- 不做实时 PBR + IBL 的完整离线渲染器；Toon / PBR 走「少量核心参数 + 预设」。
- 不改变 PMX 数据的 round-trip 语义；翻译词表仍只用于显示。
- 不在 2.0 引入动态插件（`dlopen`）ABI；留到 2.x 评估。

---

## 3. 架构决策（ADR）

| 编号 | 决策 | 理由 |
| --- | --- | --- |
| ADR-1 | 新增内存动画模型 `AnimClip`，**VMD 降级为 I/O 格式** | 时间线需要「帧 + 值 + 4 轴贝塞尔」的通用结构；直接改 `VmdMotion` 会把 UI 绑死在 VMD 的字段上，后续无法加相机 / 音频 / 程序化轨道 |
| ADR-2 | 物理烘焙用**确定性重放**，不做 Bullet 世界序列化 | Bullet 无稳定序列化；固定步长 + 单线程 + 从起始帧重放即可复现，且实现量小一个数量级 |
| ADR-3 | 视频**不在进程内链接 FFmpeg**，调用系统 `ffmpeg` | 避免 LGPL/GPL 链接合规问题与几十 MB 体积增长，保住单文件 exe |
| ADR-4 | 音频用 `miniaudio` 单头文件（随仓库提供） | 播放 + 解码一次解决，MIT-0 / public domain，断网可构建 |
| ADR-5 | 脚本先做 **CLI + JSON 批处理**，CPython 嵌入作为可选模块 | 嵌入 Python 会引入运行时路径、版本、体积三重问题；批处理已覆盖 90% 自动化需求 |
| ADR-6 | 视口与导出**共用同一套渲染 pass**，只是目标 FBO 与分辨率不同 | 保证「所见即所得」，避免维护两套着色器 |

---

## 4. 里程碑

依赖顺序：**M0 → M1 → M2 → M3 → M4 → M5**。M3 的离屏渲染可与 M2 并行。

### M0 基础重构（行为零变化）

**目标**：为后续功能腾出结构，不改变任何现有交互。

**新增**

```
src/core/anim.hpp / anim.cpp      # 通用关键帧模型（M1 落地，M0 只建骨架）
src/app/playback.hpp / playback.cpp  # 从 Viewer 拆出的播放状态
src/app/profiler.hpp / profiler.cpp  # 轻量区间计时（物理 / IK / 蒙皮 / UI / IO）
src/app/modules.hpp               # 模块开关与工作区预设
```

**改造**

- `Viewer` 中的 `motion / motion_frame / motion_speed / motion_playing / motion_loop /
  motion_dirty / motion_muted_*` 迁入 `PlaybackState`，`Viewer` 持有一个实例。
  字段名与语义保持不变，减少一次性 diff。
- `Viewer::update_motion`、`apply_motion_frame` 改为调用 `PlaybackState`。
- `main.cpp` 主循环包上 `PMX_PROFILE_SCOPE(...)`，设置页新增「性能」分区：
  物理 / IK / 蒙皮 / UI / 单帧总耗时的滑动平均。
- `CMakeLists.txt` 增加模块选项（默认全开）：
  `PMX_MODULE_TIMELINE / PMX_MODULE_BAKE / PMX_MODULE_RENDER_OUT /
  PMX_MODULE_AUDIO / PMX_MODULE_SCRIPT`。

**验收**

- 237 项核心回归全绿；
- 编辑 / 预览模式行为与 v1.1.1 逐项一致（手工核对清单见第 8 节）；
- 关闭全部新模块后，二进制体积与现在相差 < 1%。

---

### M1 时间线编辑器（最高优先级）

**目标**：预览模式从「播放器」升级为「可编辑动画的创作环境」。

#### M1.1 数据层

```
src/core/anim.hpp / anim.cpp
```

```cpp
namespace pmx {

struct AnimKey {                 // 骨骼关键帧
    int frame = 0;
    Vec3 pos{};
    Quat rot{};
    std::array<VmdCurve, 4> curve{};
};

struct MorphKey { int frame; float weight; };

struct CameraKey {               // VMD 相机帧
    int frame = 0;
    float distance = 0.0f;
    Vec3 pos{};
    Vec3 rot{};                  // 欧拉角（弧度），与 MMD 一致
    std::array<VmdCurve, 4> curve{};   // 24 字节插值中实际使用的 4 段
    int fov = 30;
    bool perspective = true;
};

struct IkKey { int frame; std::vector<std::pair<int, bool>> states; };

struct BoneTrack  { int bone = -1;  std::vector<AnimKey>  keys; };
struct MorphTrack { int morph = -1; std::vector<MorphKey> keys; };
struct CameraTrack{ std::vector<CameraKey> keys; };

class AnimClip {
public:
    std::string name;
    std::vector<BoneTrack> bones;
    std::vector<MorphTrack> morphs;
    CameraTrack camera;
    std::vector<IkKey> ik;
    int max_frame() const;
    void sort_all();                 // 按帧号排序 + 去重（同帧后者覆盖）
};

AnimClip from_vmd(const VmdMotion&, const VmdBinding&);
VmdMotion to_vmd(const AnimClip&, const Model&, const std::string& model_name);
}  // namespace pmx
```

- `from_vmd` / `to_vmd` 必须保证 **VMD → AnimClip → VMD 的关键帧数值不变**
  （曲线四轴、帧号、形态权重、IK 帧）。
- 采样函数从 `evaluate_vmd` 迁移为 `evaluate_clip(const AnimClip&, ...)`，
  `evaluate_vmd` 保留为薄封装，避免一次改太多调用点。

#### M1.2 相机动作（独立）

- `src/core/vmd.cpp` 补相机帧解析：每帧 61 字节
  （u32 帧号 / f32 距离 / vec3 位置 / vec3 旋转 / 24B 插值 / u32 FOV / u8 透视）。
- 相机动作与模型动作分开存放：`pmx::CameraTrack` 独立于 `AnimClip`，应用层为
  `Viewer::camera_motion`；同一个 VMD 文件里的相机帧在导入时自动拆出来。
- 相机视角（`0` / 小键盘 `0`）按相机轨采样 `OrbitCamera`；未进入相机视角时
  相机轨不劫持自由视角。播放头变化时才接管，暂停时手动调整机位可用 `K` 打帧。
- 相机动作有自己的导入 / 保存 / 清除入口（「相机」页），动作 VMD 导出不含相机帧。

#### M1.3 UI

```
src/app/timeline.hpp / timeline.cpp
src/app/panels/timeline.cpp
```

- 布局：视口下方独立面板，高度可拖拽（默认 220px，记入设置文件），
  左侧轨道树（240px）+ 右侧 dope sheet。
- 轨道树：四类根节点（骨骼 / 形态 / 相机 / IK），支持按名称过滤（复用
  `pmxui::match_filter`，同时匹配日文原名、英文名与中文译名）；
  默认只列出「有关键帧」的轨道，避免几百行空轨道。
- Dope sheet（`ImDrawList` 自绘 + 一个覆盖全区的 `InvisibleButton`）：
  - 30 fps 标尺、播放头、当前帧高亮；
  - 关键帧画菱形，选中高亮；同帧多关键帧合并显示；
  - 滚轮缩放（以鼠标位置为锚点）、中键平移、`Ctrl+滚轮` 横向缩放；
  - 框选、拖动多选、`Del` 删除、`Ctrl+C / Ctrl+V`、`Ctrl+Z` 走既有撤销栈；
  - 播放头拖动与「动作」页的帧滑杆双向同步。
- 关键帧详情（时间线右侧或检查器新增「关键帧」标签）：
  帧号、位移 / 旋转 / 形态权重、四轴曲线预设（线性 / 缓入 / 缓出 / 缓入缓出 / 自定义）
  + 1×1 单位方框内的贝塞尔控制点拖拽。
- 打帧方式：
  - `K`：把当前姿态写入当前帧（骨骼 + 形态取并集，跳过与静止姿态相同的项）；
  - 「自动关键」开关：拖动骨骼 / 形态滑杆时若该骨骼当前帧已有关键帧则更新，否则
    按开关决定自动插入或仅提示。
- 手动摆姿与动作播放的并存规则沿用现有实现（`motion_muted_bones` /
  `manual_ik_mutes`）；时间线编辑只改 clip，不改模型数据。

#### M1.4 撤销

- `Viewer::UndoEntry` 增加 `std::shared_ptr<pmx::AnimClip> clip`（为空表示不涉及）。
- clip 快照按「保存前拷贝」处理；与模型快照共用内存预算裁剪。
- 撤销条目名称：`插入关键帧`、`删除关键帧`、`移动关键帧`、`修改曲线`。

**验收**

1. 导入 VMD → 时间线上删除第 30 帧 → 重新导出 → 用 `pmx::load_vmd` 断言该帧消失，
   其余帧数值逐字段不变；
2. 导入带相机帧的 VMD → 模型动作与相机动作被拆分 → 播放时视角随相机轨运动 →
   单独导出相机 VMD，帧数与数值一致，动作 VMD 不含相机帧；
3. 手动摆姿 + `K` 打帧 → 关闭动作播放 → 拖到该帧，姿态与打帧时一致（IK 求解后误差 < 1e-4）；
4. 关键帧框选 / 拖动 / 删除后撤销重做 N 次，数据回到初始状态；
5. 2000 帧 × 200 轨道的时间线，拖动播放头保持 60 FPS（采样只重算变化帧）。

**风险**

- dope sheet 自绘的命中测试与 DPI 缩放易出错 → 统一使用 `pmxui::scale()`，
  并为命中测试写单元测试（纯函数，可脱离 GL 编译）。
- 相机欧拉角插值存在万向节与短路径问题 → 用四元数插值后转回欧拉，再写回 VMD 时保持
  与 MMD 一致的角度范围。

---

### M2 物理烘焙与交互优化

**目标**：实时物理 → 确定性动画数据；拖动时物理不再「抢」操作。

#### M2.1 帧率无关的物理推进

- `Physics` 增加 `advance(double dt)`：内部累加固定 `1/120` 步进，与渲染帧率解耦。
- 主循环改为按**动画时间**推进：`dt = (motion_frame - last_frame) / 30.0`（播放中）。
- 拖帧 / 回绕沿用现有 `settle_physics()`（只清速度，不重摆刚体）。

#### M2.2 烘焙

```
src/core/bake.hpp / bake.cpp
src/app/panels/bake.cpp
```

- 流程：
  1. `Physics::reset(pose, weights, /*warmup=*/1)`；
  2. 对 `frame = begin..end`：
     - 采样 clip → `pose.apply()` → `solve_ik()`；
     - `physics.advance(1/30)`（固定 4 个子步，不依赖真实时间）；
     - 读取动力学骨骼世界变换，换算回局部（相对父骨骼），写入 `AnimClip` 骨骼轨；
  3. 输出到「新轨道」或「覆盖同名轨道」，可选择是否保留手工关键帧。
- 只写参与动力学的骨骼（`Physics::dynamic_bones()`）及其必要的父级；
  柔体顶点无法写入 VMD，UI 明确提示「柔体效果仅在引擎内可见」。
- 进度条 + 取消；烘焙在 UI 线程分块执行（每帧最多 N 帧），避免卡死。

#### M2.3 交互优化（借鉴 MikuMikuPhysics）

- **拖动保护**：`Viewer::drag_mode != DRAG_NONE` 时，物理子步降为 1，并在拖动结束后
  恢复；`Physics` 增加 `set_substeps(int)`。
- **作用域冻结**：拖动骨骼 `b` 时，只推进与 `b` 有付与 / IK 关联的链，
  其余链 `set_active(false)`（跳过 `stepSimulation` 中的运动学同步与回写）。
  关联关系在 `Physics::build` 时按刚体 → 骨骼 → 付与父子传递闭包预计算。

**验收**

1. 同一模型 + 同一动作，连续两次烘焙输出 VMD 逐字节一致（写测试）；
2. 烘焙结果在关闭物理后播放，与实时物理播放的骨骼位置逐帧误差 < 1e-3；
3. 拖动长袖 / 裙摆骨骼时无「回弹打架」，拖动帧率提升可量化（性能面板对比）。

**风险**

- 浮点确定性跨平台不保证 → 文档与 UI 标注「同一构建、同一平台可复现」，测试只在 CI
  同一镜像内比较。
- 多线程 Broadphase 可能引入顺序差异 → 烘焙时强制单线程（`setNumThreads(1)`）。

---

### M3 渲染与输出

**目标**：视口能出片。

#### M3.1 离屏渲染与 pass 链

```
src/gpu/pipeline.hpp / pipeline.cpp
```

- `SceneRenderer` 增加 `render_to(unsigned fbo, int w, int h, const RenderSettings&)`；
  现有 `render()` 内部改为调用它，默认目标为 0 号 FBO。
- 支持 1x / 2x / 4x 超采样（`glBlitFramebuffer` 降采样）。
- 后处理链（单 pass 起步）：曝光 / 对比度 / 饱和度 / 暗角 / 简易辉光（阈值 + 两趟模糊）。
  参数进入 `RenderSettings`，与视口共用。
- Toon 参数扩展：描边颜色、描边宽度、阴影阈值、球面贴图权重、边缘光颜色。
  以「卡通预设」下拉提供 3 组：`MMD 默认` / `柔和` / `硬边`。

#### M3.2 序列帧与视频

```
src/app/export.hpp / export.cpp
third_party/stb/stb_image_write.h   # 需补入（public domain，单头文件）
```

- 序列帧：按帧号渲染 → `glReadPixels` → 垂直翻转 → `stbi_write_png`，
  命名 `frame_0001.png`，可选起止帧 / 步长 / 分辨率。
- 视频：探测 `ffmpeg`（PATH + Windows 常见安装路径），调用
  `ffmpeg -y -framerate 30 -i frame_%04d.png -c:v libx264 -crf 18 -pix_fmt yuv420p out.mp4`；
  子进程使用 `CreateProcessW` / `posix_spawn`，日志回显到 UI。
- 找不到 ffmpeg：只输出序列帧，并给出下载链接与手动命令（可一键复制）。

#### M3.3 音频

```
third_party/miniaudio/miniaudio.h   # 单头文件
src/core/audio.hpp / audio.cpp
```

- 打开 WAV / MP3（miniaudio 解码），作为时间线的音频轨：
  波形缩略图、播放 / 暂停与动作播放同步、音频偏移校准（±500ms，步进 10ms）。
- 音频只作为对轴基准，不写入 VMD；导出视频时作为 ffmpeg 的 `-i` 第二输入。

**验收**

1. 导出 1920×1080、120 帧序列帧，单帧与视口截图逐像素一致（关闭超采样时）；
2. 调用 ffmpeg 生成 mp4，`ffprobe` 显示 30 fps、帧数正确、音频轨与动作同步（误差 < 1 帧）；
3. 2x 超采样导出无锯齿，耗时与内存占用记录在性能面板。

**风险**

- `glReadPixels` 在大分辨率下同步阻塞 → 使用 PBO 双缓冲异步回读。
- ffmpeg 编码参数与色彩空间（BT.709）不一致导致偏色 → 固定 `-vf scale=out_color_matrix=bt709`
  并在文档说明。

---

### M4 脚本与模块化

**目标**：可自动化、可按需裁剪。

#### M4.1 CLI 批处理

```
src/cli/main.cpp          # 新目标 pmxstudio-cli，只链接 pmx_core，无 GL
src/core/script.hpp / script.cpp
```

- 命令集（JSON 数组，逐条执行，失败即停并返回非零）：
  - `open_pmx` / `save_pmx`
  - `rename_bone` / `rename_material` / `rename_morph`
  - `set_physics`（质量 / 摩擦 / 弹性 / 模式）
  - `bake`（复用 M2）
  - `export_frames` / `export_video`（需要 GL，转交主程序）
- `pmxstudio.exe --batch script.json --model x.pmx` 与 `pmxstudio-cli` 共用同一实现。
- 脚本层不暴露 ImGui / GL 类型，保持可测试。

#### M4.2 模块化

- CMake 开关见 M0；关闭 `PMX_MODULE_*` 时对应源文件不参与编译，UI 标签不出现。
- 运行时「工作区」预设：
  - `编辑`：信息 / 骨骼 / 表示枠 / 顶点 / 材质 / 面 / 形态 / 物理 / 柔体
  - `动画`：时间线 + 检索 + 动作 + 形态 + 物理
  - `输出`：渲染设置 + 导出 + 性能
- 预设只控制标签可见性，不改变数据；记入设置文件。

**验收**

1. 示例脚本批量重命名 100 根骨骼，输出 PMX 与原始文件的差异仅名称字段；
2. 关闭 `PMX_MODULE_AUDIO` 后构建成功，二进制不含 miniaudio 符号；
3. `pmxstudio-cli --batch` 在无显示器的 CI 上跑通（不需要 GL）。

---

### M5 打磨与 2.0 发布

- 快捷键总表更新（时间线：空格播放 / K 打帧 / Del 删除 / Ctrl+C/V / 滚轮缩放）。
- README 增加「动画创作」「渲染输出」「脚本」三节，附截图与最短工作流。
- `project(PmxStudio VERSION 2.0.0)`，发版说明按既有格式撰写。
- 回归：237 项核心 + 新增 anim / bake / wav / timeline 命中测试，目标 ≥ 320 项。
- 发布 `v2.0.0`，附带示例工程（模型 + VMD + 批处理脚本）。

---

## 5. 借鉴对照表

| 来源 | 借鉴的解法 | PmxStudio 的落地方式 | 不照搬的部分 |
| --- | --- | --- | --- |
| Blender NLA | 动画是「可复用片段」而非一条线性曲线 | 时间线以 `AnimClip` 为单位，支持多 clip 载入与简单叠加（2.1 只做单 clip + 静音轨道，叠加留到 2.x） | 完整 NLA 的 strip 混合、动作约束、驱动器 |
| SekaiBlender | 确定性 Action Bake | M2 的固定步长重放烘焙 | 完整 Blender 依赖图 |
| SekaiBlender | VMD 骨骼 / 形态 / 相机原生支持 | M1 的 `AnimClip` 四类轨道 | — |
| SekaiBlender | FSR / 低分辨率预览 | M3 的 1x / 2x / 4x 超采样与「快速预览」质量档（先做分辨率缩放，FSR 留到 2.x） | FSR 具体算法 |
| MikuMikuToon | 一键卡渲 | M3 的「卡通预设」下拉 + 少量核心参数 | 节点编辑器 |
| MikuMikuPhysics | Fast Drag Protection | M2.3 拖动时降子步 | — |
| MikuMikuPhysics | Interaction Scope | M2.3 作用域冻结 | — |
| MikuMikuPhysics | 性能统计面板 | M0 的 `profiler` + 设置页「性能」 | — |
| MMD Auto Tools | 烘焙 + 批量渲染 + FFmpeg | M2 + M3 的导出向导 | 批处理节点图 |
| mmd_tools / VMDPhysicsBake | 物理烘焙到骨骼轨 | M2.2 | — |

---

## 6. 模块与文件规划

```
src/core/
  anim.hpp/.cpp        # 通用关键帧模型 + VMD 互转 + 采样
  bake.hpp/.cpp        # 确定性物理烘焙
  audio.hpp/.cpp       # miniaudio 封装（播放 / 解码 / 波形）
  script.hpp/.cpp      # JSON 批处理命令
src/app/
  playback.hpp/.cpp    # 播放状态（从 Viewer 拆出）
  timeline.hpp/.cpp    # 时间线状态、编辑操作、命中测试
  export.hpp/.cpp      # 序列帧 / ffmpeg 调用
  profiler.hpp/.cpp    # 区间计时
  modules.hpp          # 模块开关 / 工作区预设
src/app/panels/
  timeline.cpp         # dope sheet + 曲线编辑
  bake.cpp             # 烘焙面板
  output.cpp           # 渲染设置 + 导出向导
src/gpu/
  pipeline.hpp/.cpp    # FBO + pass 链 + 后处理
src/cli/
  main.cpp             # pmxstudio-cli（无 GL）
third_party/
  stb/stb_image_write.h
  miniaudio/miniaudio.h
tests/
  test_anim.cpp        # VMD 互转 / 采样 / 曲线
  test_bake.cpp        # 烘焙确定性
  test_timeline.cpp    # 命中测试等纯函数
  test_audio.cpp       # WAV 解析
```

CMake 新增目标：`pmx_cli`（`pmx_core` + `script`），可选 `PMX_BUILD_CLI`（默认 ON）。

---

## 7. 版本节奏

| 版本 | 内容 | 判定标准 |
| --- | --- | --- |
| v1.2 | M0 + M1（时间线 + 相机轨） | 能改关键帧并导出 VMD |
| v1.3 | M1 曲线编辑 + M2（烘焙） | 烘焙确定性测试通过 |
| v1.4 | M3（离屏渲染 + 序列帧 + ffmpeg + 音频） | 能一键出 mp4 |
| v2.0 | M4 + M5（CLI / 模块化 / 文档） | 示例工程 + 全量回归 |

每个版本沿用现有流程：分支开发 → 语法检查 + 核心回归 → 合并 main → 打标签发版。

---

## 8. 测试与验收策略

### 8.1 自动化

- **核心回归**（现有 237 项）：每次提交必须全绿。
- **新增纯函数测试**（无需 GL）：
  - VMD ↔ AnimClip 往返（含全角名、缺失轨道、空帧、重复帧）；
  - 贝塞尔采样与四轴曲线；
  - 时间线命中测试（帧 → 像素 → 帧）；
  - WAV 头解析（PCM 8/16/24/32、float、单 / 双声道）；
  - 烘焙确定性（同输入两次输出的 VMD 字节一致）。
- **语法检查**：本沙箱缺 `libxrandr` 头，无法配置完整 app；沿用
  `-fsyntax-only` + GL 桩头检查新增 `src/app/*`，`src/core/*` 直接编译。

### 8.2 手工验收（Windows 产物）

- 编辑模式：所有面板与 v1.1.1 一致，时间线不出现；
- 预览模式：时间线可折叠、播放头同步、关键帧编辑不写坏模型；
- 物理：开启 / 重置 / 烘焙三条路径无「袖子飞走」；
- 输出：1080p 序列帧与 mp4 播放正常，音频对齐；
- 关闭全部新模块构建：体积与功能回归基线。

---

## 9. 风险登记

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| 时间线 UI 复杂度失控 | 工期膨胀、回归难 | 先 dope sheet，曲线编辑只做单曲线；命中测试纯函数化 |
| 物理跨平台不确定 | 烘焙结果不可复现 | 固定单线程 / 固定步长；限定「同构建同平台」 |
| 大模型 + 长时间线卡顿 | 交互体验退化 | 只重建变化帧；轨道树按需展开；性能面板定位瓶颈 |
| FFmpeg 不可用 | 无法出视频 | 降级为序列帧 + 复制命令 |
| 相机帧解析细节错误 | 运镜错位 | 用 MMD 导出的样本做往返测试；保留原始 24B 插值数据 |
| 单文件体积增长 | 分发优势削弱 | 模块开关；miniaudio 只在启用音频时编译；stb 单头文件 |
| 撤销栈内存膨胀 | 长时间编辑 OOM | clip 快照纳入内存预算，超出时裁剪最旧条目 |

---

## 10. 需要确认的默认选择

以下按推荐值写入计划，如需调整可在开工前提出：

1. **时间线位置**：视口下方独立面板（不是检查器标签），可拖拽高度。
2. **相机动作独立**：与模型动作分开导入 / 保存；只有进入相机视角才接管视角，
   避免破坏现有自由观察习惯。
3. **烘焙输出**：默认写入「新轨道」，不覆盖手工关键帧。
4. **视频编码**：默认调用系统 ffmpeg，不内置编码器。
5. **脚本**：默认 JSON 批处理 + `pmxstudio-cli`，不嵌入 CPython。
6. **模块开关**：默认全部编译进 exe（保持单文件分发），只做 UI 可见性裁剪；
   真正的裁剪构建由 CMake 选项控制。

---

## 11. 下一步

1. 确认本计划与第 10 节的默认选择；
2. 从 M0 开始：拆 `PlaybackState`、加性能计时、加模块开关，行为零变化；
3. M0 合入后用 v1.2 预发布验证基线，再进入 M1 的时间线实现。

# NVIDIA VFG 固定目标帧率补帧（2026-11-25）

给 NVIDIA VFG 补帧加一个"固定目标帧率"模式：输出按 `1/target_fps` 的固定节拍走，源帧间隔变长时在区间里多生成几帧吸收波动；原来的倍率模式保持不变，仍是默认。

- **范围**：只改 VFG 路径（唯一支持显式时间步的后端），倍率模式、其它后端、导出链路的行为都不变。
- **默认**：`fgFixedTarget.enabled=false`，关着的时候所有新分支都不激活。
- **分支**：本地工作区，未提交。

## 为什么要它

现在的倍率模式在每个源帧区间里固定插 `M-1` 帧，位置是 `t=i/M`。源帧间隔一变，输出节奏跟着变：

- 采集卡收 PS5 的 60 Hz 画面，先去重还原真实的 30 fps；
- 游戏掉到 25 fps，或者帧间隔在 33/40 ms 之间抖动时，2X 的输出就在 60→50 fps 之间来回摆；
- 这种"输出帧率在变"比"帧数少"更容易被看成卡顿。

固定目标帧率把输出和源解耦：源每对帧之间有 33 ms 就插 1~2 帧，40 ms 就插 2 帧，50 ms 就插 3~4 帧（80 fps 目标），输出节拍不动。

## VFG 的两种模式

NVIDIA VFX SDK 的 VFG 有两种互斥的插值方式：

| | 倍率模式 | 显式时间步模式 |
|---|---|---|
| 参数 | `FrameMultiplier=M`（2~8）+ 每次运行前 `FrameIndex=i`（1~M-1） | `FrameMultiplier=0` + 每次运行前 `Timestep=t`（F32，(0,1) 开区间） |
| 生成位置 | `t=i/M` | `t=Timestep` |
| API | `NvVFX_SetU32` | `NvVFX_SetF32`（`NVVFXVIDEOFRAMEGENERATION_TIMESTEP`） |

约束（沿用现有代码的约定）：上一帧 `NVVFX_INPUT_IMAGE_0` 是 t=0，当前帧 `NVVFX_INPUT_IMAGE_1` 是 t=1；设置 `FrameMultiplier`（包括设成 0）会重置 `FrameIndex` 和 `Timestep`，所以每次 Run 前都要重设 `Timestep`；所有 Run 的输出写在同一批输出缓冲里，下一次 Run 前必须取走；batch 只能是 1，只能插值不能外推；`SHOT_CHANGE=1` 时直接复制当前帧，该标志会持续，需要手动清零。

## 核心调度

`include/veyra/engine/FixedTargetSchedule.h`（纯头文件、无依赖，被设置结构、图和单测共用）。

每个被接受的源帧对 `(A,B)` 调一次 `FixedTargetScheduler::plan(a,b,contentFps)`：

1. `period = round(1e7 / target_fps)`（100 ns 单位）。
2. 首次用 `phase = a + period` 锚定网格；之后 `phase` 一直落在网格上。
3. 循环取 `phase < b` 的每个网格点，`t = (phase - a) / denominator`，作为这一帧的 `Timestep`。
4. 超过 `maxRuns`（默认 4）的 tick 只记账（`droppedByCap`）并继续推进 `phase`：输出时钟不停，空洞由真实帧或已生成的帧覆盖，GPU 慢不会累积延迟。
5. 循环结束时 `phase` 正好落在 **B 之后最近的网格点**，这就是真实帧的提交时间 `realPts100ns`；下一对从这个点再加一个 period 开始。

第 5 条是关键。网格会跨区间自由运行，真实帧不可能都落在网格上，所以：

- 如果保留 B 原来的 PTS，每个区间会周期性地插进一个短间隔或长间隔 —— 恰好是眼睛最敏感的、也是最新的那一帧；
- 把 B 吸附到 B 之后最近的网格点，代价是不到一个输出周期的额外延迟（80 fps 下 ≤ 12.5 ms，实测抖动源最坏 11.7 ms），换来的是**每一个提交间隔都恰好是一个周期**。

### 边界容错

| 情况 | 处理 |
|---|---|
| 源间隔 > `maxIntervalMs`（默认 100 ms） | 不插帧，保持上一帧，`phase=b` 重新同步 |
| 实测内容帧率 ≥ `targetFps * 0.98`（真 60 fps 源、菜单） | 自动旁路，不插帧，重新同步 |
| 源间隔 ≤ 0 或时间戳倒退 | 旁路并失效网格，下一对重新锚定 |
| 每区间超过 `maxRuns` | 丢弃多余 tick，不累积延迟 |
| 场景切换 | 沿用原有 `SHOT_CHANGE` 处理：VFG 那一对直接复制当前帧，无需额外代码 |
| 平滑开启 | 先把间隔钳到 `[clampMinMs, clampMaxMs]`，再做 EMA 得到 `t` 的分母；网格本身不受影响 |

平滑默认关闭（0）。关着的时候 `denominator = 真实 span`，`t` 就是真实时间位置，运动速度精确。

## 看图/看代码的入口

- **调度数学**：`include/veyra/engine/FixedTargetSchedule.h`
- **每对计划**：`src/engine/EngineController.cpp` 的 `fixedEligible` 块（在倍率/自适应/输出上限之后、准入之前），把 `previewFgMultiplier` 设成 `count+1`
- **生成帧**：`src/pipeline/EnhanceGraph.cpp::process()` 里 `fixedTarget` 分支：`fgCalls=fixedTicks`、`tickTimestep(sub)`、`tickPts(sub)`、真实帧用 `realPts100ns`
- **VFG 调用**：`src/pipeline/VfgBackend.cpp` 的 `cudaJob`：`timestep>0` 时把 `FrameMultiplier` 切成 0 并 `SetF32("Timestep")`，否则走 `FrameIndex`
- **容量**：`fixedTargetCapacity()` / `EnhanceGraph::vfgSlotCount()`，图和 VFG 的输出池都按它分配（`max(用户倍率, maxRuns+1)`）
- **单测**：`tests/unit/FixedTargetScheduleTests.cpp`（`veyra_fixed_target_schedule_tests`，不需要显卡）
- **离线仿真**：`tools/fixed_target_sim.py`，逐行移植的同一套算法 + 倍率模式对比 + 全矩阵扫描

## 设置与 UI

- **设置**：`EnhancementSettings::fgFixedTarget`（`FixedTargetConfig`）。
- **结构 vs 逐对**：`enabled` 与 `maxRuns` 决定生成槽位数量，改它会重建图；`targetFps`、`smoothing`、`clamp*`、`maxIntervalMs`、`nearStart` 只影响每对计划，改它们不重建（`fixedTargetSameStructure()` / `sameVideoConfiguration()`）。
- **持久化**：跟"自适应倍率""帧节奏修复"一样是外壳偏好（`fgFixedTarget`、`fgFixedTargetFps`、`fgFixedTargetSmoothing`、`fgFixedTargetClampMinMs`、`fgFixedTargetClampMaxMs`），不进预设。
- **UI**：专业页与节点页的补帧分组里，"固定目标帧率"开关（只在 VFG 且倍率 >1 时可见）、目标帧率滑块（**0~160 任意整数**，点右侧数字可直接输入，按 ↑↓ 走 1）、帧间隔平滑滑块、帧间隔上下限滑块（只在平滑 > 0 时显示）。
- **目标帧率取值 0~160**（2026-11-25 追加）：面板不再给固定档位。填 `0` 表示"完全不生成"——`fixedTargetValid()` 接受 0，`plan()` 在最前面就把它变成 `bypass=true, reason=targetOff`，不会走到 `1/0` 的除法。`normalizedFixedTarget()` 把输入钳到 `[0,160]`。
- **`maxRuns` 跟着目标帧率走**：`fixedTargetRunsFor(targetFps) = clamp(ceil(targetFps/25), 2, 6)`，按"最慢的 25 fps 内容（40 ms）"算需要几个 tick。80 → 4（历史默认不变）、100 → 4、120 → 5、144 → 6、160 → 6。UI 提交与偏好恢复两条路都调用它，所以手输 144 不会悄悄被旧的 4 截断成丢帧。改目标帧率跨档（4→5/6）会重建一次图，这是槽位数量变化决定的。

## 统计

`EngineController` 每秒最多打一条 `fixed-target` 日志：

```
revision=<n> targetFps=<f> contentFps=<f> sourceMs=<f> smoothedMs=<f> ticks=<n>
realGridDelayMs=<f> bypass=<0|1> reason=<ticks|none|longInterval|sourceAtTarget|targetOff|interval|firstPair>
intervals=<n> ticks=<n> droppedByCap=<n> bypassLong=<n> bypassAtTarget=<n> bypassOff=<n> resyncs=<n>
```

`realGridDelayMs` 是真实帧被吸附到网格上多出来的延迟，正常应该 < 1000/targetFps。

## 如何验证

### 1. 不用显卡的部分（现在就能跑）

```bash
python tools/fixed_target_sim.py
```

当前结果：全部 PASS。关键几行：

```
PASS  steady 30 FPS: 480 frames, interval 12.500-12.500 ms (spread 0.00%)
PASS  30 <-> 25 FPS wobble: 512 frames, interval 12.500-12.500 ms
PASS  30 FPS with +-3.4 ms transport jitter: 480 frames
PASS  the real frame lands on the first grid point at or after B
PASS  snap cost stays under one period (worst 11.672 ms)
2X 对比: interval 16.667-20.000 ms (spread 18.2%), effective 56.3 FPS
Sweep: worst interval deviation: 0.00 us (94 combinations excluded as capped or bypassed)
```

有 C++ 工具链时编 `veyra_fixed_target_schedule_tests`，断言的场景与仿真一致。

### 2. 真机对比（需要 30 fps 且会掉到 25 fps 的素材）

1. 采集一段 60 Hz 输入、游戏锁 30 fps 但会掉到 25 fps 的素材（或人为让游戏掉帧），去重打开。
2. **倍率模式基线**：VFG 2X，录 10 秒。用 `ffprobe -show_frames` 或 `mpdecimate` 看输出帧间隔，应该看到 16.7 ms 与 20 ms 交替（约 56 fps，波动 18%）。
3. **固定目标帧率**：VFG + 固定目标帧率，目标 80。同样的画面再录 10 秒。输出帧间隔应该基本恒定在 12.5 ms，只有"每对源帧生成几帧"在 1~3 之间变。
4. 看日志 `fixed-target` 行：`contentFps` 在 25~30 之间，`ticks` 在 1~3 之间，`realGridDelayMs` < 12.5，`bypass=0`。
5. **停顿/菜单**：暂停游戏或进菜单（源变成 60 fps），日志应出现 `reason=sourceAtTarget` 或 `longInterval`，画面不出现追帧、不累积延迟。

### 3. 回归（默认行为不变）

- 不开固定目标帧率，跑一遍现有 VFG 测试与真机预览，行为应完全一致：`fixedPlan=nullptr` 时 `fixedTarget=false`、`fgCalls=fgMultiplier-1`、`generate()` 的 `timestep=0/primary=-1`。
- 改目标帧率/平滑/钳位时不应触发"这套参数需要重建管线"；改 `maxRuns` 或开关模式才重建。

## 尚未确定、需要确认

1. ~~**目标帧率的取值**：现在 UI 给 60/72/80/100/120/144，调度器接受 20~480。要不要跟显示器刷新率绑定？~~ 已改为 0~160 任意整数（见上文），是否再跟刷新率绑定仍未定。
2. ~~**`maxRuns` 是否暴露给用户**：现在固定 4（上限 6）。20 fps 源 + 120 fps 目标需要 6 帧，被截断成 4 会留下空洞。~~ 已改为随目标帧率自动派生（`fixedTargetRunsFor`）；160 fps 目标配 20 fps 源（50 ms）仍会超上限，这是 GPU 侧的上限，暂不放开。
3. **输出上限的交互**：`fixed_target` 计划在"输出上限（自定义 fps）"之后计算，会用 `count+1` 覆盖上限推导出来的倍率。上限仍由提交端做残差丢弃，但更合理的做法可能是把 `target_fps` 直接钳到上限。
4. **和"自适应倍率"共存**：自适应倍率会先算一个倍率，随后被固定目标帧率的 `count+1` 覆盖，等于失效。要不要在固定目标帧率打开时禁用/隐藏自适应倍率？
5. **和"VFG 自动调控"共存**：2X 快档捷径（读的是"每对一次"的耗时，固定目标下每对帧数在变）已在该模式下关闭；`vfgOverload` 的全组采样改用计划自己的 tick 数。这两处是有意的取舍，需要真机确认快档在这个模式下还有没有用。
6. **真实帧吸附带来的延迟**：最坏不到一个输出周期（80 fps：12.5 ms）。如果觉得这点延迟不能接受，另一条路是保留 B 的 PTS 并接受每个区间一次长短间隔 —— 但那就回到了这个模式要解决的问题。
7. **单模型档是否也强制主模型**：现在混合档（`vfgQuality` 3~5）按 `|t-0.5|` 选主模型 tick；单模型档保留原来的自动选择，让 `fastReady()` 还能救场。
8. **导出链路**：`VideoExportJob` 走 `describeStages`，会读到 `fgFixedTarget` 但因为 `process()` 不传 `fixedPlan` 而保持倍率行为。要不要给导出也支持固定目标帧率（导出是离线逐帧，可以精确按时间戳算）？
9. **HUD 合成**：固定目标下 `useNewerHud` 由 `t > 0.5` 决定（倍率模式是 `sub*2 > M`）。t 很接近 0.5 时前后真实帧的 HUD 都"差不多对"，没有更细的处理。

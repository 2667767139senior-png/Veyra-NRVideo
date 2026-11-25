# NVIDIA VFG 固定目标帧率补帧（2026-11-25，AFG 版）

给 NVIDIA VFG 补帧加一个"固定目标帧率"模式：**每个真实帧间隔按自己的长度决定生成几帧**，间隔长了多生成、短了少生成，长期平均输出帧率落在目标上；原来的倍率模式保持不变，仍是默认。

- **范围**：只改 VFG 路径（唯一能按位置插帧的后端），倍率模式、其它后端、导出的行为都不变。
- **默认**：`fgFixedTarget.enabled=false`，关着的时候所有新分支都不激活。
- **与倍率互斥**：UI 上是"补帧模式"二选一（倍率 / 固定目标帧率），选固定目标时倍率控件消失 —— 帧数由间隔算，不由用户选。

## 为什么是 AFG 而不是"先补上再锁帧"

第一版做的是另一种思路：让输出跑在 `1/target_fps` 的自由网格上，把真实帧吸附到网格点，这样每个提交间隔都恰好是一个周期。真机验证被否掉了，原因有两条：

1. **它把画面重新定时了**：真实帧被推到网格点，等于凭空加了一个周期的延迟（80 fps 下最多 12.5 ms），用户侧的直接观感就是"补上去然后锁帧"。
2. **它依赖 VFG 的显式时间步模式**（`FrameMultiplier=0` + `Timestep`），这是整条链路里唯一没有经过长期验证的分支。

AFG（adaptive frame generation）的思路简单得多：**不动真实帧，只改每组生成几帧**。

```
want  = spanMs * targetFps / 1000 + carry     // 这一段应该贡献几帧（含真实帧 B）
count = clamp(round(want) - 1, 0, maxRuns)     // 生成 count 帧，B 是最后一帧
carry = want - (count + 1)                     // 余数交给下一对
```

- 40 ms 的间隔在 80 fps 下要 3.2 帧 → 生成 2 帧（VFG 倍率 3），0.2 帧的余数带到下一对；
- 下一对 3.4 → 2 帧，再下一对 3.6 → 3 帧，于是倍率走 3,3,4,3,3…，**平均值正好是目标**；
- 33.33 ms 的间隔在 80 fps 下要 2.67 帧 → 倍率走 3,2,3,2…；
- 这就是"吸收间隔抖动"：30→25 fps 掉帧时，40 ms 的那一对自动多要一帧，输出帧率不跟着掉。

生成帧的位置是 `t = i/(count+1)`，也就是 **VFG 自己的 `FrameMultiplier=count+1` + `FrameIndex=i`**：和倍率模式走同一条路径，没有新增任何 SDK 依赖。`Timestep` 那条分支仍在 `VfgBackend` 里保留，但管线不再传非零值。

## 核心调度

`include/veyra/engine/FixedTargetSchedule.h`（纯头文件、无依赖，被设置结构、图和单测共用）。

每个被接受的源帧对 `(A,B)` 调一次 `FixedTargetScheduler::plan(a,b,contentFps)`，结果只有一个数：`count`（以及 `outputMultiplier = count+1`）。

| 情况 | 处理 |
|---|---|
| 源间隔 > `maxIntervalMs`（默认 100 ms） | 不插帧，`count=0`，余数清零（停顿不欠账） |
| 实测内容帧率 ≥ `targetFps * 0.98`（真 60 fps 源、菜单） | 自动旁路，`count=0` |
| 目标帧率填 0 | `count=0`，`reason=targetOff`（不会走到除法） |
| 源间隔 ≤ 0 / 时间戳倒退 | `count=0`，`reason=interval` |
| `count` 超过 `maxRuns` | 截断到 `maxRuns`，多出来的帧计入 `droppedByCap`，余数清零 —— **丢帧，不欠延迟** |
| 平滑开启 | 间隔先钳到 `[clampMinMs, clampMaxMs]` 再做 EMA，得到用于算帧数的间隔 |
| 场景切换 | 沿用原有 `SHOT_CHANGE` 处理，VFG 那一对直接复制当前帧 |

余数 `carry` 被限制在 ±1 帧以内：一次异常间隔最多让下一组多/少一帧，不会连续补偿。

## 看图/看代码的入口

- **调度数学**：`include/veyra/engine/FixedTargetSchedule.h`
- **每对计划**：`src/engine/EngineController.cpp` 的 `fixedEligible` 块，把 `previewFgMultiplier` 设成 `count+1`
- **生成帧**：`src/pipeline/EnhanceGraph.cpp::process()` —— 固定目标只是把本对的 `fgMultiplier` 换成 `count+1`，后面的槽位、`t=i/M`、HUD 选择、时间戳全部走原有倍率逻辑；`count=0` 时 `fgCalls=0`，这一对不生成
- **VFG 调用**：`VfgBackend::generate(..., timestep=0, primary=-1)`，即纯 `FrameMultiplier`/`FrameIndex`
- **容量**：`fixedTargetCapacity()` / `EnhanceGraph::vfgSlotCount()`，图和 VFG 输出池按 `max(用户倍率, maxRuns+1)` 分配
- **单测**：`tests/unit/FixedTargetScheduleTests.cpp`（`veyra_fixed_target_schedule_tests`，不需要显卡）
- **离线仿真**：`tools/fixed_target_sim.py`，逐行移植的同一套算法 + 倍率模式对比 + 全矩阵扫描

## 设置与 UI

- **设置**：`EnhancementSettings::fgFixedTarget`（`FixedTargetConfig`）。
- **结构 vs 逐对**：`enabled` 与 `maxRuns` 决定生成槽位数量，改它会重建图；`targetFps`、`smoothing`、`clamp*`、`maxIntervalMs` 只影响每对计划，改它们不重建（`fixedTargetSameStructure()` / `sameVideoConfiguration()`）。
- **持久化**：跟"自适应倍率""帧节奏修复"一样是外壳偏好（`fgFixedTarget`、`fgFixedTargetFps`、`fgFixedTargetSmoothing`、`fgFixedTargetClampMinMs`、`fgFixedTargetClampMaxMs`），不进预设。
- **UI（2026-11-25 二改）**：专业页与节点页的补帧分组里新增 **补帧模式**（`fgMode`：倍率 / 固定目标帧率），放在"补帧方式"下面。
  - 选 **倍率** → 显示原来的倍率控件与自适应倍率；
  - 选 **固定目标帧率** → 倍率控件与自适应倍率隐藏，改为显示目标帧率（**0~160 任意整数**，点右侧数字可直接输入）/ 帧间隔平滑 / 帧间隔上下限；
  - 模式行**始终可见**，非 VFG 后端时置灰并写明"固定目标帧率需要补帧方式选 NVIDIA VFG"（以前是整行隐藏，用户根本找不到）；
  - 不再要求"倍率 > 1"才能出现 —— 补帧关着时倍率本来就是 1，那个门槛把这行藏在了最需要它的时候。
- **目标帧率取值 0~160**：填 `0` 表示"完全不生成"（`reason=targetOff`），`normalizedFixedTarget()` 把输入钳到 `[0,160]`。
- **`maxRuns` 跟着目标帧率走**：`fixedTargetRunsFor(targetFps) = clamp(ceil(targetFps/25), 2, 6)`，按"最慢的 25 fps 内容（40 ms）"算需要几帧。80 → 4（历史默认不变）、100 → 4、120 → 5、144 → 6、160 → 6。改目标帧率跨档（4→5/6）会重建一次图，这是槽位数量变化决定的。

## 统计

`EngineController` 每秒最多打一条 `fixed-target` 日志：

```
revision=<n> targetFps=<f> contentFps=<f> sourceMs=<f> smoothedMs=<f> frames=<n>
multiplier=<n> carry=<f> bypass=<0|1> reason=<adaptive|none|longInterval|sourceAtTarget|targetOff|interval|firstPair>
intervals=<n> generated=<n> dropped=<n> bypassLong=<n> bypassAtTarget=<n> bypassOff=<n>
```

- `frames`：这一对生成几帧；`multiplier`：`frames+1`，即 VFG 这一组的宽度。
- `carry`：交给下一对的余数（帧），正常在 ±1 之间。
- `dropped`：被 `maxRuns` 截掉的帧数。**它持续增长说明源帧率比 25 还低**（或目标帧率过高），不是延迟问题。

## 如何验证

### 1. 不用显卡的部分

```bash
python tools/fixed_target_sim.py
```

当前结果：全部 PASS。关键几行：

```
PASS  30->80: 80.00 FPS out (target 80)
PASS  25/30 wobble: 80.00 FPS out (target 80)
PASS  25/30 wobble: closer to target than a constant 2X (54.55 FPS)
PASS  40 ms x5: generated [2, 2, 3, 2, 2] -> 16 output frames (80 FPS)
PASS  20->160: the cap leaves no debt behind
PASS  every case inside the cap lands within 2% of target (worst 25->72 steady (72.08), 0.12%)
note: 9 case(s) need more than maxRuns+1 frames per interval (VFG tops out at 8 per pair)
```

那 9 个例外全部是 **20 fps 源 + 120 fps 以上目标**（50 ms 间隔要 8 帧以上，VFG 一组最多 8 帧）。25~30 fps 内容（PS5 去重后的实际范围）在所有目标帧率下都在 2% 以内。

### 2. 真机对比（需要 30 fps 且会掉到 25 fps 的素材）

1. 采集 60 Hz 输入、游戏锁 30 fps 但会掉到 25 fps 的素材，打开去重。
2. **倍率模式基线**：VFG 2X，录 10 秒。`ffprobe -show_frames` 看输出帧间隔，应该是 16.7 ms 与 20 ms 交替（约 56 fps，波动 18%）。
3. **固定目标帧率**：补帧模式选它，目标填 80。同样画面再录 10 秒。这时帧间隔跟着源间隔走，但**每对源帧的输出帧数在 2~4 之间变**，总帧数应该明显多于 2X（80 fps vs 56 fps）。
4. 看日志 `fixed-target` 行：`contentFps` 在 25~30，`frames` 在 2~3 之间跳，`carry` 在 ±1 内，`dropped=0`，`bypass=0`。
5. **停顿/菜单**：暂停或进菜单（源变 60 fps），日志应出现 `reason=sourceAtTarget` 或 `longInterval`，画面不追帧、不累积延迟。

### 3. 回归（默认行为不变）

- 模式选"倍率"（或不碰它）时 `fixedPlan=nullptr` → `fgCalls=fgMultiplier-1`、`outputMultiplier=fgMultiplier`，与原来完全一致。
- 改目标帧率/平滑/钳位不应触发"这套参数需要重建管线"；改 `maxRuns` 或开关模式才重建。

## 尚未确定、需要确认

1. **输出上限的交互**：计划在"输出上限（自定义 fps）"之后计算，会用 `count+1` 覆盖上限推导出来的倍率；上限仍由提交端做残差丢弃。更合理的做法可能是把 `target_fps` 直接钳到上限。
2. **和"VFG 自动调控"共存**：2X 快档捷径（读的是"每对一次"的耗时，固定目标下每对帧数在变）已在该模式下关闭；`vfgOverload` 的全组采样改用计划自己的帧数。需真机确认快档在这个模式下还有没有用。
3. **VFG 每组宽度的重建代价**：倍率在 3/4 之间逐对变化，`FrameMultiplier` 是每对写一次。SDK 说明里它是选择器（写它会重置 FrameIndex/Timestep），不是重新配置，真机上需要确认逐对变化不带来额外耗时。
4. **`maxRuns` 是否要放到 7**：放到 7 才能让 20 fps 源跑满 144/160，代价是输出池正好用满 14 个槽（VFG 上限 8 帧一组）。当前按 25 fps 内容设计，暂不放开。
5. **导出链路**：`VideoExportJob` 走 `describeStages`，会读到 `fgFixedTarget` 但因为 `process()` 不传 `fixedPlan` 而保持倍率行为。要不要给导出也支持（离线逐帧可以精确按时间戳算）？
6. **HUD 合成**：固定目标下 `useNewerHud` 用 `sub*2 > M`，与倍率模式统一；`t` 很接近 0.5 时前后真实帧的 HUD 都"差不多对"，没有更细的处理。

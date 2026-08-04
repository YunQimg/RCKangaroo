# RCKangaroo 单机断点续算设计文档

- 版本: v1.0 (设计稿)
- 目标: 单机运行（无网络）时支持求解中断后从上次进度继续，不重做已完成的工作
- 关联: 与 [distributed_design.md](distributed_design.md) 配套；server 落盘直接复用本文档的存档格式

---

## 1. 目标与范围

- 求解过程中（含任意时刻杀进程 / Ctrl+C / 断电）都能从最近一次保存的进度恢复，已收集的 DP 不丢失。
- 恢复后正确性不变：继续走线的新 DP 与已存 DP 仍能正常碰撞求解。
- 不加参数时保持现有行为完全不变（向后兼容）。

### 非目标（v1 不做）

- 不保存 kangaroo 当前走线位置 / GPU 内部状态（理由见 §2.2）
- 不做跨进程/网络同步（那是分布式方案的事）
- 不做加密

---

## 2. 现状与可行性分析

### 2.1 可复用的现有能力

| 能力 | 位置 | 说明 |
|---|---|---|
| `TFastBase::SaveToFile/LoadFromFile` | [utils.cpp:230-303](utils.cpp#L230-L303) | 全量 DB 二进制落盘/加载，字节级跨平台，已有文件头 `Header[256]` |
| tames 存档机制 | [RCKangaroo.cpp:372-386](RCKangaroo.cpp#L372-L386) | 已实现"启动时按 range 校验加载文件"，本设计将其泛化为通用 checkpoint |
| `FindOrAddDataBlock` 幂等 | [utils.cpp:213-227](utils.cpp#L213-L227) | 重复插入同一 DP 返回已有记录、无副作用 → 日志回放天然安全 |
| 线程抽象 `CriticalSection` | [utils.h:52-62](utils.h#L52-L62) | 现有跨平台锁，日志缓冲并发写入直接用 |

### 2.2 什么必须存、什么不必存

**必须存（可恢复正确性的全部依据）：**
- 全量 DP 数据库（tame + wild 都要）——碰撞检测只依赖 DP 与距离 d，与走线历史无关
- 任务元数据：range / dp / start / pubkey —— 校验存档属于当前任务
- 累计 ops（`PntTotalOps`）—— 用于 K 统计与 `-max` 预算

**不必存（v1 明确放弃）：**
- kangaroo 当前走线位置、GPU 内部状态、`DPTable`（GPU 侧局部缓存）

**论证"只存 DP 即可恢复"：** 每台/每次运行 kangaroo 都以 tickcount 随机起点起步（[RCKangaroo.cpp:425](RCKangaroo.cpp#L425)），恢复后新走线独立于旧走线；新 DP 与库中任意旧 DP（tame/wild 皆可）碰撞即可求解，判定逻辑 [CheckNewPoints](RCKangaroo.cpp#L212-L289) 完全不依赖"两条走线属于同一次运行"。这与分布式多机互相独立走线是同一数学结构。丢失的仅是"最后一条在途走线"（长度 ≈ `KangCnt × STEP_CNT` 次运算，相对总量可忽略）。

### 2.3 关键量级（与分布式文档一致）

- 单卡 DP=16 时 DP 生成速率 ≈ 22 万条/s（~8 MB/s）
- 84 位 DP=16 全量 DB ≈ 8000 万条 ≈ **2~3 GB**（一次全量落盘需数秒~数十秒）
- 主循环 DP 缓冲 `pPntList` 上限 `MAX_CNT_LIST × 48B = 24 MB`（单卡约 2 秒即满）

→ **全量落盘期间若不暂停计算会溢出丢点**，因此采用"增量日志 + 周期全量"方案（§3）。

---

## 3. 总体设计

采用 **全量 checkpoint + 增量 journal（WAL）+ 原子替换**：

```
求解主循环（每 10ms）
  ├─ CheckNewPoints() → 新 DP 入 DB → 同时追加到内存 journal 缓冲
  ├─ 每 save_sec        → journal 缓冲 flush 到 .log 文件（追加写，几百 KB，零阻塞）
  ├─ 每 checkpoint_sec 或 journal 超阈值
  │      → 全量 DB 写 .tmp → rename 成 .dat（原子）→ 截断 .log
  └─ Ctrl+C / 异常退出  → 停止 GPU → flush journal → 全量 checkpoint → 退出
```

- 正常运行时 GPU **永不暂停**：增量日志只需每秒写几百 KB，全量落盘时新 DP 继续进 journal 缓冲，不丢点。
- 恢复 = 加载 `.dat`（全量）→ 回放 `.log`（增量）→ 继续求解。

### 3.1 为什么不直接周期全量落盘（备选方案 B）

备选：落盘前停 GPU → 排空队列 → 全量写盘 → 重启 GPU。实现最简单，但 GB 级落盘期间 GPU 空转、且重启 GPU 放弃在途走线。仅作为兜底实现（v1 不采用）。

---

## 4. 文件格式设计

### 4.1 全量存档 `.dat`（复用现有 TFastBase 格式 + 扩展 Header）

现有 `SaveToFile` 结构：`Header[256]` + 256³ 个桶，每桶 `u16 cnt` + cnt × 32B 记录（记录不含 x 前 3 字节桶键）。**DB 部分字节不变，只扩展 Header 语义**，保证与旧 tames 文件双向兼容。

`Header[256]` 元数据布局（v1）：

```
offset  size  字段                    说明
0       1     range                   已有约定（tames 文件在用）
1       1     dp                      新增
2       1     mode                    0=solve, 1=gen
3       1     format_version=1        新存档置 1；旧文件为 0 → 按 legacy 处理
4..47   44    start_hex[44]           start 偏移十六进制（43 字符+\0）
48..159 112   pubkey_xy_hex[112]      pubkey 的 x+y 十六进制（各 64 字符）
160..167 8    ops_done (u64)          已累计运算量（PntTotalOps）
168..175 8    saved_time (u64)        最近一次存档时间戳（管理用）
176..183 8    task_start_time (u64)   任务首次启动时间戳（跨重启连续计时）
184..255 72   保留，填 0
```

兼容规则：
- 加载时 `format_version==0` → 旧 tames 文件，仅校验 `Header[0]==range`（现有逻辑不变）。
- `format_version==1` → 完整校验 range/dp/start/pubkey，任一不匹配则**报错退出**（绝不静默清库）。
- `mode` 不符（如 solve 存档被当 gen 用）→ 报错退出。

### 4.2 增量日志 `.log`

- 格式：**定长 35 字节/条**，即 DBRec（x[12] + d[22] + type[1]，[RCKangaroo.cpp:60-66](RCKangaroo.cpp#L60-L66)），与网络协议线格式一致。
- 记录时机：`CheckNewPoints` 中 `FindOrAddDataBlock` 返回 NULL（确为新入库）时追加。
- **尾部截断容错**：`文件大小 % 35 != 0` → 截断尾部 0..34 字节再回放（最后一条写一半）。
- 回放：逐条 `FindOrAddDataBlock`（幂等，重复无害）。
- 命名：用户指定 `-save mytask.dat` → 日志 `mytask.log`、临时 `mytask.tmp`。

### 4.3 原子性与清理

- 全量写 `.tmp` → 成功后 `rename` 为 `.dat`（同文件系统内原子，Windows/POSIX 均成立）→ 截断 `.log`。
- 求解成功（`gSolved`）→ 删除 `.dat/.log`（任务完成，避免下次误加载已完成库）。

---

## 5. 命令行设计

```
# 启用断点续算（.dat 不存在则新建；存在则校验并恢复）
RCKangaroo.exe -save mytask.dat -dp 16 -range 84 -start ... -pubkey ...

# 可选控制
-save_sec <n>         日志 flush 间隔，默认 60 秒
-checkpoint_sec <n>   全量 checkpoint 间隔，默认 1800 秒
```

- 指定 `-save` 时，`-tames` 变为可选（checkpoint 已含 tames DP）。
- 不指定 `-save` → 行为与现在完全一致。

### 5.1 求解进度显示方案

**现状**：现有 [ShowStats](RCKangaroo.cpp#L291-L324) 每 10s 输出 `Speed / Err / DPs: 已收集K/预期K / Time: 已耗时/预期总耗时`，已有基础进度信息，但缺失：进度百分比、剩余时间（当前显示的是总预期耗时而非 ETA）、实时 K；且断点续算场景下已耗时从进程启动计（重启清零）、预期值不扣除已完成工作量（失真）。

**目标输出**（每 10s 一行，替换/增强现有 ShowStats 输出）：

```
MAIN: [#############-----] 73.4% | Speed: 14500 MKeys/s | K: 1.18 | DPs: 56600K/77200K | Elapsed: 02:15:33 | ETA: 00:49:12
```

**指标定义**：
- `Progress%`：`min(100, 100 × PntTotalOps / exp_ops)`（ops 口径；用 `double` 计算避免 u64 溢出，如 170 位 range 时 `exp_ops` 超过 2^64）
- `进度条`：`[###-----]` 由百分比换算，可选开关（默认关闭纯文本，避免刷屏干扰）
- `K(实时)`：`PntTotalOps / 2^(Range/2)`（SOTA 理论值 1.15，含 DP 与 GPU 开销后偏高）
- `DPs: 已收集/预期`：保留现有字段（[RCKangaroo.cpp:323](RCKangaroo.cpp#L323)），预期 = `exp_ops/dp_val`；恢复后 `db.GetBlockCnt()` 含已存 DP，比例天然正确
- `Elapsed`：`now - task_start_time`，跨重启连续（存档 Header 需含 `task_start_time`，见 §4.1）
- `ETA`：`(exp_ops - PntTotalOps) / speed`（已扣除已完成工作量；`PntTotalOps >= exp_ops` 或 `speed == 0` 时显示 `--`）

**GEN 模式（tames 生成）**：预算口径 `Progress = PntTotalOps / MaxTotalOps`（`-max` 即本次生成预算），其余字段同。

**恢复场景启动信息**：`Resumed: X DPs (Y% progress), ops_done=Z, elapsed=D`，`Y` 用 §4.1 存档中已收集 DP 数 / 预期 DP 数估算（复用 [RCKangaroo.cpp:339-358](RCKangaroo.cpp#L339-L358) 的公式）。

**数据来源与改动**：
- 新增全局 `gOpsDone`、`gTaskStartTime`：恢复时从存档 Header 读入；旧存档（无 `task_start_time`）取当前时间，即"从本次运行起算"（`ops_done` 仍正确恢复）
- `ShowStats` 增加上述字段输出；保留 `tm_start` 参数供 GEN/bench 模式使用
- 全部运算走 `double`，仅最后格式化时取整

**边界情况**：
- 恢复后前几秒 `speed` 未测得（`GetStatsSpeed` 返回 0）→ ETA 显示 `--`，进度百分比不受影响（只依赖 `PntTotalOps`）
- `-max` 触发停止前进度显示到 100% 或按 `-max` 口径显示
- 求解成功即刻退出，不再多刷一行

### 5.2 tames 预生成与断点续算的组合

**结论：方案完全保留"预生成 tames → 再求解"的工作流**。tames 预生成继续使用现有 `-tames` 机制（不变），`-save` 只承载求解阶段的断点。

**标准三步用法**：

```
# 1) 预生成 tames（现有机制，与 -save 无关）
RCKangaroo.exe -dp 16 -range 84 -tames tames84.dat -max 10

# 2) 首次求解（.dat 不存在 → 走现有 -tames 加载路径初始化 DB）
RCKangaroo.exe -save task.dat -tames tames84.dat -dp 16 -range 84 -start ... -pubkey ...

# 3) 断点恢复（.dat 已含 tames + wild，-tames 可省略）
RCKangaroo.exe -save task.dat -dp 16 -range 84 -start ... -pubkey ...
```

**组合规则**：
- 首次运行：`.dat` 不存在 → 走现有 `-tames` 加载逻辑（§6.2.1 第二条），tames 装入 DB 并随首次 checkpoint 落盘。
- 恢复运行：`.dat` 存在且校验通过 → 以 `.dat` 为准（内含 tames）；若同时仍指定 `-tames` 则**忽略**，避免不一致的 tames 覆盖库内数据。
- 同一 range 换目标 key、复用同一批 tames：`.dat` 与 pubkey 绑定（§4.1 校验会拒绝复用），需换新的 `-save` 文件名，并继续指定 `-tames tames84.dat`。
- GEN 存档（mode=1）与 solve 存档（mode=0）互不混用（§4.1 mode 校验拒绝）；tames 预生成始终使用 `-tames` 机制，`-save` 支持 GEN 模式断点留作 v2 增强。

**正确性说明**：tames 与目标 key 无关（只依赖 range 与固定跳表种子 [RCKangaroo.cpp:388](RCKangaroo.cpp#L388)），可跨多次求解复用；`.dat` 中的 wild DP 与目标 key 相关，故存档与 pubkey 绑定。预生成 tames 的运算量（GEN 运行）不计入 solve 的 `PntTotalOps`，因此通过 `-tames` 预加载大量 tame DP 时，`DPs: 已收集/预期` 口径比 ops 口径的 `Progress%` 更贴近真实进度（§5.1 的 ops 进度从 0 起算）。

---

## 6. 关键改动点

### 6.1 `utils.h/.cpp` — TFastBase 扩展

- `SaveToFile`：增加参数或新函数 `SaveToFileEx(fn, meta)`，写 Header 元数据（§4.1）。
- `LoadFromFile`：返回 `Header` 已含元数据，新增校验函数 `ValidateMeta(range, dp, mode, start, pubkey)`。
- 新增 `AppendJournalFile(fn, buf, cnt)` / `ReplayJournalFile(fn)`（定长记录 + 尾部截断）。

### 6.2 `RCKangaroo.cpp` — 求解流程

1. **恢复加载**：把 [RCKangaroo.cpp:372-386](RCKangaroo.cpp#L372-L386) 的 tames 加载块泛化为：
   - `-save` 且 `.dat` 存在 → 校验元数据 → `db.LoadFromFile(.dat)` → 回放 `.log` → `PntTotalOps = ops_done`（此时若同时指定 `-tames` 则忽略，规则见 §5.2）；
   - 否则走现有 `-tames` 逻辑（不变）。
2. **日志挂钩**：`CheckNewPoints` 内新入库处追加到内存 journal 缓冲（互斥锁保护，容量上限如 64 MB，满则强制 flush）。
3. **主循环定时钩子**（[RCKangaroo.cpp:464-480](RCKangaroo.cpp#L464-L480)）：
   - 到 `save_sec` → flush journal 缓冲到 `.log`；
   - 到 `checkpoint_sec` 或 journal 累计超阈值（如 1 GB）→ 全量 `.tmp`→rename→截断 `.log`。
4. **优雅退出**：新增 `gExitRequested` 标志；Windows 用 `SetConsoleCtrlHandler`（回调只置标志），Linux 用 `signal(SIGINT)`；主循环检测到后走现有"停止 GPU → join 线程"路径（[RCKangaroo.cpp:482-494](RCKangaroo.cpp#L482-L494)），随后 flush + 全量 checkpoint 再返回。
5. **`-max` 语义**：上限判断改为 `(PntTotalOps - ops_done) > MaxTotalOps`（恢复后预算从零重新计，历史 ops 只用于统计与 K 显示），`ops_done` 在恢复时保存、在结束时更新到存档。
6. **进度显示**：按 §5.1 改造 [ShowStats](RCKangaroo.cpp#L291-L324)——
   - 新增全局 `gOpsDone`、`gTaskStartTime`（恢复时从存档 Header 读入，旧存档缺省取当前时间）；
   - 输出进度百分比、实时 K、连续 elapsed、剩余 ETA；`tm_start` 参数保留给 GEN/bench 模式。
7. **成功清理**：`gSolved` 分支删除 `.dat/.log`。

### 6.3 `defs.h` — 常量

- journal 缓冲大小、flush/checkpoint 默认间隔、日志记录长度（35）等宏。

### 6.4 构建

- 无新源文件时无需改工程；若新增独立 `Checkpoint.h/.cpp` 则加入 vcxproj 与 CMakeLists。

---

## 7. 崩溃一致性分析

| 崩溃时刻 | 恢复结果 |
|---|---|
| 两次日志 flush 之间 | 丢最近 ≤ `save_sec` 秒的新 DP（等价于少收集几条），不损坏，正确性不受影响 |
| 日志写一半 | 尾部截断丢弃，回放其余完整记录 |
| 全量 `.tmp` 写一半 | `.tmp` 无效被丢弃；恢复 = 旧 `.dat` + 完整 `.log` |
| rename 前 | 同上一行 |
| rename 后 | 新 `.dat` + 已截断的 `.log` |
| Ctrl+C / 正常退出 | 优雅路径：flush + 全量 checkpoint 完成后才退出，不丢 |

正确性关键：**任何时刻被杀，恢复后都等价于"少收集了若干 DP 继续走线"**——这正是断点续算所允许的语义。

---

## 8. 性能影响

| 项 | 开销 |
|---|---|
| 日志 flush（每 60s） | 单卡 ~数百 KB~几 MB 写盘，IO 可忽略 |
| 全量 checkpoint（每 30min） | 1~3 GB 写盘（建议 SSD）；期间 GPU 不暂停，新 DP 进 journal 缓冲 |
| 恢复加载 | 加载 `.dat`（GB 级，分钟级）+ 回放 `.log`（≤ 1 GB 阈值，分钟级）；一次性开销 |
| 内存 | 新增 journal 缓冲 ≤ 64 MB |

---

## 9. 与分布式方案的衔接

- 单机存档 `.dat` 的格式（Header 元数据 + DB dump）与分布式 server 落盘**采用同一套代码与格式**（见 distributed_design.md §6.3），分布式 server 直接复用。
- 分布式 worker 不需要本能力（它的权威数据在 server）。
- 单机 checkpoint 可视为分布式 server 的"单节点特例"，两者互不冲突。

---

## 10. 风险与边界情况

1. **恢复后 K 变差**：恢复后新走线随机重起步，期望上不劣于"从未中断"（DP 全部保留），但最后一次 checkpoint 前的在途走线作废（≈一个 flush 间隔的工作量），影响可忽略。
2. **存档文件被手动改动/损坏**：加载失败 → 报错退出或按 `-tames` 语义降级，绝不静默清库重来。
3. **任务参数被误改**：元数据校验失败 → 报错退出，防止"用 A 任务的库续 B 任务"。
4. **journal 与 .dat 不一致**：仅可能由磁盘故障导致；v1 不做 checksum（SSD 时代概率低），v2 可加。
5. **长时间运行磁盘写放大**：journal 阈值 + checkpoint 间隔可配置；对 SSD 寿命无实质影响。
6. **`-max` 预算语义变化**：恢复后重新计预算（§6.2.5），文档中明示，避免用户误解。

---

## 11. 实施步骤（里程碑）

| 阶段 | 内容 | 验收 |
|---|---|---|
| M1 | Header 元数据扩展 + LoadFromFile 兼容旧 tames + ValidateMeta | 用现有 tames 文件加载无回归；构造不同元数据存档能正确拒绝 |
| M2 | journal 追加/回放/尾部截断容错 | 构造 10 万条合成记录，模拟半截文件，回放结果与逐条插入一致 |
| M3 | 主循环钩子 + Ctrl+C 优雅退出 + `-max` 语义 + 进度显示（§5.1） | 32-bit 随机点求解中 Ctrl+C → 重启 `-save` 续算解出；结果与单次连续求解一致；重启后 Progress/Elapsed/ETA 与中断前连续衔接 |
| M4 | checkpoint 周期落盘 + 恢复进度显示 + 调优 | 84 位以下长时间求解：中途杀进程重启续算，K 与未中断相比偏差 < 3% |

---

## 12. 新增/修改文件清单

**修改**
- `utils.h/.cpp` — Header 元数据读写、校验函数、journal 追加/回放接口
- `RCKangaroo.cpp` — 恢复加载块、CheckNewPoints 日志挂钩、主循环定时钩子、信号处理、`-max` 语义、`-save` 参数解析、ShowStats 进度显示改造（§5.1）
- `defs.h` — 相关常量宏

**新增（可选，视实现习惯）**
- `Checkpoint.h/.cpp` — 封装 journal 缓冲、flush/checkpoint 调度，避免 RCKangaroo.cpp 膨胀

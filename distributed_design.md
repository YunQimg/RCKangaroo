# RCKangaroo 分布式求解设计文档

- 版本: v1.0 (设计稿)
- 目标: 支持多台机器（可跨 Windows/Linux）同时求解同一个 ECDLP，定期共享 DP 数据

---

## 1. 目标与范围

- 在一台"服务器"上维护全量 DP 数据库，所有"工作机"（GPU 节点）把各自产生的 DP 增量上传，由服务器统一做碰撞检测，命中后广播结果。
- 支持断点续传：服务器定期把 DP 数据库落盘，重启后可恢复。
- 保持现有单机模式行为不变（向后兼容，不加参数时行为与现在完全一致）。

### 非目标（v1 不做）

- TLS 加密传输（v1 面向可信内网；跨公网可先套 VPN，v2 再加 TLS）
- benchmark 模式的分布式（随机目标无法共享；分布式必须使用 `-pubkey`）
- 节点间多跳/去中心化全复制同步（见 §11 备选方案）

---

## 2. 现状分析（关键代码事实）

### 2.1 DP 数据流（单机）

```
GPU kernel 填 Kparams.DPs_out（48 字节/条）
  -> RCGpuKang::Execute() 拷贝到 host 并调用 AddPointsToList()   [GpuKang.cpp:616-671]
  -> AddPointsToList(): kang_ind 转 TAME/WILD，写入全局 pPntList  [RCKangaroo.cpp:153-175]
  -> 主循环每 10ms 调 CheckNewPoints()                           [RCKangaroo.cpp:212-289, 464-480]
       - 从 pPntList 取新 DP，构造 DBRec（35 字节），插入 db(TFastBase)
       - FindOrAddDataBlock 返回已有记录 => 命中 => 碰撞检测 => 解出则 gSolved=true
```

### 2.2 数据结构

| 结构 | 大小 | 字段 | 位置 |
|---|---|---|---|
| GPU 缓冲记录 | 48 B | x[16] + d[24] + kang_ind[4]（+pad） | [defs.h:36](defs.h#L36) `GPU_DP_SIZE` |
| `DBRec`（DB 与网络共用） | 35 B | x[12] + d[22] + type[1]，pack(1) | [RCKangaroo.cpp:60-66](RCKangaroo.cpp#L60-L66) |
| DB 实际存储 | 32 B | 前 3 字节 x 作为桶键不落盘 | [utils.cpp:174-198](utils.cpp#L174-L198) |

- `d` 固定 22 字节（支持 170 位 range；负数用高字节 0xFF 作符号扩展标记，见 [RCKangaroo.cpp:262-264](RCKangaroo.cpp#L262-L264)）。**v1 网络传输保持 22 字节不动**，裁剪压缩列为 v2 优化。

### 2.3 碰撞检测逻辑

[CheckNewPoints](RCKangaroo.cpp#L212-L289) 的判定规则（新 DP vs DB 已有 DP）：
- 同为 TAME → 忽略
- 同为 WILD 且 d 相同 → 忽略
- 其余情况 → 尝试 `Collision_SOTA`（tame-wild 组合），验证成功才置 gSolved

**该逻辑可整体抽成函数，服务器端原样复用，是本设计的基础。**

### 2.4 关键量级（84 位 range、DP=16、4090）

- 哈希率 14.5 GH/s → DP 生成速率 ≈ 14.5e9 / 2^16 ≈ **22 万 DP/s ≈ 8 MB/s（每卡）**
- 全量 DB ≈ 8000 万条 ≈ **2~3 GB**（服务器内存要求）
- 单次求解总耗时从小时到天 → 网络同步延迟控制在秒级对 K 的影响可忽略（见 §8）

### 2.5 有利条件

- 各节点 RNG 用 `GetTickCount64()` 播种（[RCKangaroo.cpp:425](RCKangaroo.cpp#L425)），走线天然独立，无需协调随机源。
- 跳表用固定种子 0 生成（[RCKangaroo.cpp:388](RCKangaroo.cpp#L388)），所有节点跳表一致，tames 兼容性有保证。
- `TFastBase::SaveToFile/LoadFromFile` 已是跨平台字节格式，可复用做落盘。
- 重复插入 DP 幂等（`FindOrAddDataBlock` 对重复记录返回已有指针，碰撞检测对重复不会误报），简化了断线重连语义。

---

## 3. 总体架构

```
                        ┌─────────────────────────────┐
   worker 1 (GPU机) ───▶│                             │
   worker 2 (GPU机) ───▶│   SERVER（全量 DB + 碰撞检测）│
   worker 3 (GPU机) ───▶│   + 定期落盘 SaveToFile      │
       ...              └─────────────────────────────┘
                              │ SOLVED 广播（私钥）
                              ▼
                     所有 worker 停止并输出
```

- **Server**：无 GPU 也可（纯收数入库 + 检测）；可同时以 worker 身份参与求解（带 `-server` + `-pubkey` 时自己也是计算节点）。
- **Worker**：只上传 DP + 收 SOLVED 广播；**不持有 DB、不加载 tames、不做碰撞检测**，内存占用与单机模式相比显著降低。

---

## 4. 命令行设计

```
# 服务器模式（可同时是 worker）
RCKangaroo.exe -server 12345 [-pubkey ... -range ... -dp ... -start ...] [-tames tames76.dat]

# worker 模式（必须与服务器同一求解任务）
RCKangaroo.exe -connect 192.168.1.10:12345 -pubkey ... -range 84 -dp 16 -start ...
```

- worker 模式禁止 `-tames`/`-max`(生成模式)；`-max` 作为本机 ops 上限仍可用。
- worker 模式下 `-gpu` 用法不变（指定本机 GPU）。
- 新增参数：
  - `-server <port>`：以服务器模式运行
  - `-connect <host:port>`：以 worker 模式运行
  - `-sync_ms <n>`：DP 批量发送间隔，默认 200（仅 worker）
  - `-save_sec <n>`：DB 落盘间隔，默认 300（仅 server）

---

## 5. 网络协议设计

### 5.1 通用帧（二进制、小端、统一 x86/ARM64 字节序）

```
offset  size  field
0       4     magic = 0x4B435222 ("RCK2")
4       4     msg_type (u32)
8       4     payload_len (u32)
12      8     seq (u64)   // 消息级序号，用于监控与断点
20      n     payload
```

### 5.2 消息类型

| 消息 | 方向 | payload | 说明 |
|---|---|---|---|
| `HELLO` | worker→server | task_id(16B 哈希) + node_id(u64) + last_acked_seq(u64) | task_id = SHA256(range‖dp‖start‖pubkey) 前 16 字节，任务不一致则 server 拒绝 |
| `HELLO_ACK` | server→worker | 0 | 握手成功 |
| `DP_BATCH` | worker→server | u32 count + count × 35B 记录 | 记录格式见 5.3 |
| `BATCH_ACK` | server→worker | seq(u64) | 确认收到（用于统计与断点） |
| `SOLVED` | server→worker | EcInt pk(32B) + result 校验字段 | 广播给所有 worker |
| `PING` / `PONG` | 双向 | 0 | 心跳，间隔 5s |

### 5.3 DP 记录（网络线格式，35 字节，即 DBRec）

```
offset  size  field
0       12    x[12]      // 点 X 坐标前 12 字节（与 DBRec 一致）
12      22    d[22]      // 距离，含符号扩展约定
34      1     type       // 0=TAME, 1=WILD（与现有 TAME/WILD 宏一致）
```

- server 收到后直接构造 `DBRec` 调用现有入库/检测函数，无格式转换。
- 重复接收同一条 DP：`FindOrAddDataBlock` 返回已有记录 → 幂等，无害。

### 5.4 可靠性与重连语义

- 单条 TCP 连接，worker 内独立发送线程 + 接收线程。
- 断线：worker 指数退避重连（1s→2s→4s…上限 30s），重连后重新 HELLO。
- **重连后不重发历史数据**：server DB 是权威且已持久化；TCP 断线瞬间丢失的少量数据由幂等性兜底（该 DP 可能永远丢失，只相当于少收集一条，不影响正确性）。
- seq 仅用于监控（server 记录各 worker 累计入库量）与调试，不承担去重职责。

---

## 6. 服务器端设计

### 6.1 线程模型（节点数少，每连接一线程）

- 主线程：accept 循环 + 统计显示（入库速率、DB 总量、节点列表）+ 定时落盘。
- 每连接一个处理线程：收 DP_BATCH → 批量入库 + 碰撞检测。
- 锁策略：`TFastBase` 无锁（现有代码单线程使用）。v1 采用**按 x 首字节分 256 把锁**包裹 `FindOrAddDataBlock`，每批每把锁持锁时间短，256 路并发下冲突极小；若实测不足再升级分片锁（按 bucket）。

### 6.2 碰撞检测（复用现有逻辑）

- 将 [CheckNewPoints](RCKangaroo.cpp#L212-L289) 的"单条 DP 入 DB + 判定 + Collision_SOTA"部分抽为：
  `bool ProcessDP(DBRec* rec)` → 返回是否已解出（解出时填充 gPrivKey）。
- server 处理线程逐条调用，命中后置全局 solved 标志，向所有连接发送 SOLVED（含私钥），退出监听。
- server 自身若带 GPU（worker 兼 server），其本地 DP 走同一 ProcessDP，无需特殊处理。

### 6.3 落盘（断点续传）

- 沿用 `TFastBase::SaveToFile` 字节格式；文件头 `Header[256]` 中约定：
  - Header[0] = range（现有）
  - Header[1..4] = 任务标识
  - Header[5..12] = 落盘时间戳（供管理）
- 落盘期间暂停入库（用锁），DB 达 GB 级时一次性写盘耗时数秒，可接受；期间 worker 的 TCP 缓冲自然吸收（客户端也有本地发送队列）。
- 启动加载：server 启动时若指定 `-tames` 或发现 `dp_save.dat`，先 LoadFromFile 再开始 accept。

---

## 7. 客户端（worker）设计

### 7.1 改动点

- `AddPointsToList()`：worker 模式下不写 `pPntList`，改写入**发送队列**（互斥队列，容量上限 ~100 万条防堆积）。
- 主循环（[RCKangaroo.cpp:464-480](RCKangaroo.cpp#L464-L480)）：`CheckNewPoints()` 替换为 `WorkerFlush()`（把发送队列按序打包为 DP_BATCH 交给发送线程）。
- `gSolved` 由接收线程在收到 SOLVED 时置位（并校验私钥：对 gPntToSolve 做一次 MultiplyG 比对，防伪造）。
- `PntTotalOps` 仍在本机累计，`-max` 上限逻辑不变。

### 7.2 网络线程

- 发送线程：每 `sync_ms`（默认 200ms）或队列满 10000 条，取队列构造 DP_BATCH 发送；未连接时丢弃并丢弃队列（或保留至重连后发送，v1 选择保留最近 100 万条）。
- 接收线程：阻塞读帧；HELLO_ACK/PONG/SOLVED 分别处理。
- 两线程与 GPU 主循环互不阻塞，GPU 吞吐不受网络抖动影响。

### 7.3 worker 无需本地 DB 的论证

- GPU 走线不引用 DB（kernel 只查 jump 表与 DP 表）。
- 碰撞检测全部在 server：两个 DP 无论来自同一 worker 还是不同 worker，都会在 server 的 ProcessDP 中相遇。
- 检测延迟 = RTT + server 处理 ≈ 毫秒级，相对秒级的批量间隔可忽略（见 §8）。
- 因此 worker 不加载 tames、不建 db，内存占用只取决于发送队列。

---

## 8. 同步频率 / 带宽 / 资源分析

### 8.1 对 K 的影响（冗余率）

同步延迟 L 秒内产生的 DP 尚未进入公共 DB，等价于少量"无效"工作量：

```
冗余率 ≈ (总 DP 生成速率 × L) / 总 DP 数 = L / 总求解时长 T
```

例：T=24h，L=1s → 冗余 0.001%。**秒级批量间隔完全可接受**；默认 200ms 已极其保守。

### 8.2 带宽

- 每卡上行 ≈ 8 MB/s（DP=16）。10 卡 ≈ 80 MB/s → 需要 ≥ 千兆上行。
- 帧开销：每批 header 20B + 批量（默认最多 1 万条 = 350KB），开销 < 0.1%。
- 服务器入站带宽 = Σ 各 worker 上行；服务器几乎无下行（仅 SOLVED/ACK），所以用"上行大、下行小"的家庭宽带拓扑也基本可行。

### 8.3 服务器 CPU / 内存

- 入库速率 = 总 DP 速率（10 卡 ≈ 220 万条/s）。`TFastBase` 为二分 + memmove，配合 256 把分桶锁，现代 CPU 可支撑；需关注 memmove 开销，v1 先实测。
- 内存：84 位 DP=16 全量 ≈ 2~3 GB；另加 mempool 增长预留。server 建议 ≥ 32 GB。

### 8.4 可选优化（v2）

- 按 range 裁剪 d（84 位只需 11 字节）：35B → 24B，省 30%。
- 批量 zlib 压缩（x 不可压，d 高位与 padding 可压），整体再省 ~10%。
- 上述均需 server/worker 双端协议配合，v1 不做。

---

## 9. 断点续传与崩溃恢复

| 场景 | 行为 |
|---|---|
| server 崩溃/重启 | 启动时加载 `dp_save.dat`（或 -tames），恢复全量 DB；worker 重连后继续 |
| worker 崩溃/重启 | server DB 不受影响；worker 重启后重新走线（GPU 状态本来不持久），已入库 DP 不丢 |
| 网络长时间断开 | worker 保留发送队列（最多 100 万条）重连后续传；超过容量的丢弃，等价于少收集 |
| 求解中断后想续算 | 直接用同一 `dp_save.dat` 重新启动 server，worker 全部重连即可 |

---

## 10. 跨平台实现要点

- **Socket 抽象**：新增 `NetSock` 类，Windows 用 Winsock2（WSAStartup/WSACleanup），Linux 用 BSD socket；封装 `connect/accept/send_all/recv_all/close`。项目已有 `#ifdef _WIN32` 分支先例（[RCKangaroo.cpp:136-152](RCKangaroo.cpp#L136-L152)）。
- **线程**：沿用 `_beginthreadex` / `pthread_create` 分支模式。
- **字节序**：全链路小端，x86/ARM64 均为小端，不做转换；文档中固定协议为小端即可。
- **文件格式**：现有 SaveToFile/LoadFromFile 字节级跨平台，直接复用。
- **编译**：新增文件加入 [RCKangaroo.vcxproj](RCKangaroo.vcxproj) 与 CMakeLists.txt；Linux 链接 `-lpthread`。

---

## 11. 备选方案（对比结论）

| 方案 | 结论 |
|---|---|
| **A. 中心 TCP 服务器（本设计）** | ✅ 采用：改动最小、带宽最优、检测逻辑完全复用 |
| B. 全量复制 P2P | 每节点持全量 DB（2~3GB×N）且带宽 O(N²)，仅适合 ≤10 节点且无中心场景；作为 v2 可选 |
| C. 文件 + Syncthing/网盘同步 | 零网络代码，但延迟秒~分钟级、合并逻辑需自写，且无法实时广播解；适合"离线攒 DP"的弱网场景 |
| D. Redis/MQTT 总线 | 解决公网 NAT/重连，但引入中间件；若未来需跨公网多节点可在此协议上叠加 broker 适配层 |

---

## 12. 风险与边界情况

1. **server 入库成为瓶颈**：220 万条/s 需验证；预案 = 分片锁细化 / 批量插入 / server 端多 worker 线程。
2. **worker 发送队列堆积**：server 短暂不可用时队列膨胀；容量上限 + 丢弃策略（等价丢 DP）已设计。
3. **任务参数不一致**：HELLO 携带 task_id，server 拒绝不匹配连接，防止多任务混跑污染 DB。
4. **SOLVED 伪造/损坏**：worker 收到后本地用 MultiplyG 验证私钥，失败则报错并忽略（现有 Collision_SOTA 已有同类验证先例）。
5. **同一 DP 被多 worker 重复产生**：属正常现象（kangaroo 方法固有），server 幂等处理，只增加少量无用插入。
6. **-max ops 限制**：仅本机生效；服务器侧无全局预算（v1 明确不做全局调度）。

---

## 13. 实施步骤（里程碑）

| 阶段 | 内容 | 验收 |
|---|---|---|
| M1 | `NetSock` + 协议帧 + server 骨架（accept/HELLO/DP_BATCH/统计） | 回环：server 能接收并入库 100 万条合成 DP |
| M2 | worker 模式接通（发送队列 + SOLVED 接收 + 主循环改造）；单机双进程跑 32-bit 随机点 | 双进程回环能解出，结果与单机一致 |
| M3 | 断点续传：落盘/加载 + 断线重连 | 杀 server 重启后仍能续解；断网 30s 恢复 |
| M4 | 双机/多机实测（不同 GPU、不同 OS），锁与批量调优 | 84 位以下多机 K 与单机理论值偏差 < 5% |

---

## 14. 新增/修改文件清单

**新增**
- `NetSock.h/.cpp` — 跨平台 socket 封装
- `DPProtocol.h/.cpp` — 帧、消息构造/解析、DP 序列化
- `DPServer.h/.cpp` — server：连接管理、ProcessDP 调用、落盘
- `DPWorker.h/.cpp` — worker：发送/接收线程、重连、队列

**修改**
- `RCKangaroo.cpp` — 参数解析（-server/-connect 等）；`SolvePoint()` 主循环分支；`CheckNewPoints()` 拆出 `ProcessDP()`；`AddPointsToList()` worker 分支
- `defs.h` — 协议常量、消息类型、队列容量宏
- `utils.h/.cpp` — （可选）TFastBase 加锁封装
- `RCKangaroo.vcxproj` / `CMakeLists.txt` — 加入新文件

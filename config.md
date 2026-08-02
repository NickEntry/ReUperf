# ReUperf 配置说明

## 文件位置

默认路径：`/data/adb/ReUperf/ReUperf.json`

## 配置结构

```json
{
  "meta": {
    "name": "配置名称",
    "author": "作者"
  },
  "modules": {
    "sched": {
      "enable": true,
      "case_insensitive": false,
      "refresh_interval_ms": 2000,
      "highspeed_sched_ms": 300,
      "top_scan_budget_us": 4000,
      "full_scan_budget_us": 12000,
      "scan_batch_size": 32,
      "scan_batch_yield_us": 0,
      "timing": {
        "event_throttle_ms": 50,
        "min_schedule_interval_ms": 200,
        "schedule_cleanup_interval_ms": 5000,
        "cgroup_check_interval_ms": 1000,
        "cpuset_retry_count": 3,
        "cpuset_retry_interval_ms": 10,
        "cpuset_group_check_ttl_ms": 1000,
        "process_cache_ttl_ms": 100,
        "file_cache_ttl_ms": 100,
        "cgroup_cache_ttl_ms": 100,
        "monitor_initial_restart_delay_s": 1,
        "monitor_restart_retry_delay_s": 5,
        "config_retry_initial_delay_s": 1,
        "config_retry_max_delay_s": 5
      },
      "log": {
        "level": "info",
        "output": "/data/adb/ReUperf/ReUperf.log"
      },
      "cpumask": {...},
      "affinity": {...},
      "prio": {...},
      "rules": [...]
    }
  }
}
```

## 全局参数

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `enable` | bool | true | 启用调度模块 |
| `case_insensitive` | bool | false | 正则匹配忽略大小写，开启后 `surfaceflinger` 可匹配 `SurfaceFlinger` |
| `refresh_interval_ms` | int | 2000 | 全量校准间隔（毫秒）。从本次全量扫描完成后开始计时。模板建议设为 2000。 |
| `highspeed_sched_ms` | int | 300 | 目标扫描间隔（毫秒），仅扫描 top-app、`pinned` 和满足条件的 `topfore` PID。默认模板采用 300ms；确有低延迟需求时可从 100-300ms 逐步调整。 |
| `top_scan_budget_us` | int | 4000 | 高频扫描的线程读取/投递时间预算，范围 500-20000 微秒。 |
| `full_scan_budget_us` | int | 12000 | 全量校准后分派线程的时间预算，范围 1000-50000 微秒。 |
| `scan_batch_size` | int | 32 | 每读取多少个线程后检查批量让出策略，范围 1-256。 |
| `scan_batch_yield_us` | int | 0 | 每批次主动让出 CPU 的时间，0 为关闭，范围 0-1000 微秒。 |
| `timing` | object | 见下文 | 运行时延迟、复查、缓存与重试参数。 |
| `log.level` | string | "info" | 日志级别：err/warn/info/debug/trace |
| `log.output` | string | 见下文 | 日志输出文件路径 |

### timing（运行时延迟与周期）

所有字段均可省略；省略时保持当前稳定版行为。运行中修改配置会重建调度对象并应用新值。

| 参数 | 默认值 | 有效范围 | 降低后的效果与代价 |
|------|--------|----------|--------------------|
| `event_throttle_ms` | 50ms | 0-5000 | 更快处理进程创建/退出事件，但事件突发时回调与锁竞争增加；0 表示不额外合并等待。 |
| `min_schedule_interval_ms` | 200ms | 0-60000 | 更快重新处理同一线程的同状态任务，但增加状态核验和系统调用；TOP、pinned、topfore 同样受此参数抑制，状态变化和首次投递不受影响。0 表示不抑制。 |
| `schedule_cleanup_interval_ms` | 5000ms | 100-600000 | 更快清理工作线程中的调度时间记录，但清理遍历更频繁；它不是扫描周期。 |
| `cgroup_check_interval_ms` | 1000ms | 0-60000 | 更快发现外部 cgroup 漂移，但增加 `/proc/<pid>/task/<tid>/cgroup` 读取；0 表示每次候选任务都检查。 |
| `cpuset_retry_count` | 3 | 1-20 | 降低可缩短失败路径，但瞬时失败恢复能力降低。此值是总尝试次数。 |
| `cpuset_retry_interval_ms` | 10ms | 0-1000 | 降低可缩短 cpuset 写入重试延迟，但瞬时内核状态可能尚未恢复；0 表示连续重试。 |
| `cpuset_group_check_ttl_ms` | 1000ms | 0-60000 | 更快重新确认 cpuset 组可用性，但增加控制文件读取；0 表示每次确认。 |
| `process_cache_ttl_ms` | 100ms | 0-60000 | 更快重新匹配进程规则，但增加正则匹配；0 表示关闭该结果缓存。 |
| `file_cache_ttl_ms` | 100ms | 0-60000 | 更快反映普通文件变化，但增加文件读取；动态 `/proc`、cpuset、cpuctl 路径原本就不会使用此缓存。0 表示关闭。 |
| `cgroup_cache_ttl_ms` | 100ms | 0-60000 | 更快反映进程 cgroup 状态变化，但增加 cgroup 文件读取；0 表示关闭。 |
| `monitor_initial_restart_delay_s` | 1s | 0-60 | 监控首次启动失败后更快重试；0 表示主循环下一轮即可尝试。 |
| `monitor_restart_retry_delay_s` | 5s | 1-300 | 监控异常停止后更快恢复增量事件，但持续故障时日志与重试负载增加。 |
| `config_retry_initial_delay_s` | 1s | 1-60 | 无效配置修复后更快重试解析，但持续写入坏配置时解析更频繁。 |
| `config_retry_max_delay_s` | 5s | 1-300 | 限制无效配置指数退避的上限；若小于初始延迟，会自动提升到初始延迟。 |

#### 延迟优先建议起点

下面只是一组渐进式起点，不保证适合所有 ROM。建议一次只调一组参数，并同时观察 ReUperf CPU 占用、上下文切换和目标线程生效时间：

```json
"highspeed_sched_ms": 100,
"refresh_interval_ms": 1000,
"top_scan_budget_us": 6000,
"full_scan_budget_us": 16000,
"timing": {
  "event_throttle_ms": 10,
  "min_schedule_interval_ms": 50,
  "cgroup_check_interval_ms": 250,
  "cpuset_retry_count": 3,
  "cpuset_retry_interval_ms": 2,
  "cpuset_group_check_ttl_ms": 250,
  "process_cache_ttl_ms": 25,
  "file_cache_ttl_ms": 25,
  "cgroup_cache_ttl_ms": 25
}
```

优先降低 `event_throttle_ms` 和 `highspeed_sched_ms`，它们最直接影响新进程事件与新线程发现延迟。只有确认 cgroup 被其他组件频繁改写时，才建议明显降低 `cgroup_check_interval_ms`。缓存 TTL 降为 0 会显著增加读取或正则匹配，不建议作为通用配置。

### 日志级别说明

| 级别 | 数值 | 说明 |
|------|------|------|
| err | 0 | 仅输出错误信息 |
| warn | 1 | 输出错误和警告信息 |
| info | 2 | 输出错误、警告和普通信息 |
| debug | 3 | 输出除 trace 外的所有信息 |
| trace | 4 | 输出所有信息 |

### 日志输出路径

默认日志路径：`/data/adb/ReUperf/ReUperf.log`

**注意**：JSON 文件不支持注释，请在单独的文档中记录配置说明。

### 运行中更新配置

程序会通过 inotify 监控配置所在目录，并以 mtime/hash 作为后备检测方式。检测到修改后会重新解析配置、重建调度对象并进行完整重扫；解析失败时保留旧配置。

建议先写入临时文件，再通过同目录的原子重命名替换 `ReUperf.json`，以避免调度器读到部分写入的 JSON。当前监控主要监听修改事件；若 ROM 或编辑器仅产生重命名事件，mtime/hash 后备检测仍会在后续循环中发现变化。

`comm_regex` 已移除；旧配置中出现该字段时会被忽略并记录警告。请使用进程 `regex` 与线程规则 `k` 分别匹配进程和线程。

## cpumask（CPU 核心组）

定义用于亲和性绑定的 CPU 核心组。

```json
"cpumask": {
  "all": [0,1,2,3,4,5,6,7],
  "c0": [0,1,2,3],
  "c1": [4,5,6],
  "c2": [7]
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| key | string | 组名称（如 "c0"、"c1"、"all"） |
| value | int[] | CPU 核心 ID 列表 |

## affinity（亲和性类别）

定义各进程状态（bg/fg/top）的 CPU 亲和性。

```json
"affinity": {
  "norm": {
    "bg": "",
    "fg": "all",
    "top": "all"
  },
  "ui": {
    "bg": "",
    "fg": "all",
    "top": "c1"
  }
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| bg | string | 后台状态的 cpumask 名称，空字符串表示跳过 |
| fg | string | 前台状态的 cpumask 名称 |
| top | string | 顶层应用状态的 cpumask 名称 |

**注意**：uperf 的 `touch` 状态会自动转换为 `top`。

## prio（优先级类别）

定义各状态的调度优先级。

```json
"prio": {
  "ui": {
    "bg": -3,
    "fg": 120,
    "top": 98
  }
}
```

| 优先级值 | 含义 |
|----------|------|
| 0 | 跳过（不更改） |
| 1-98 | SCHED_FIFO，优先级为该值 |
| 100-139 | SCHED_NORMAL，nice = value - 120 |
| -1 | SCHED_NORMAL |
| -2 | SCHED_BATCH |
| -3 | SCHED_IDLE |

## rules（进程规则）

匹配进程并应用亲和性/优先级规则。

```json
"rules": [
  {
    "name": "Launcher",
    "regex": "/HOME_PACKAGE/",
    "pinned": true,
    "topfore": false,
    "rules": [
      {
        "k": "/MAIN_THREAD/",
        "ac": "crit",
        "pc": "rtusr",
        "uclamp_max": 100,
        "cpu_share": 1024,
        "enable_limit": true
      }
    ]
  }
]
```

### 进程规则字段

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `name` | string | 必填 | 规则名称（用于 cpuctl 路径：`/dev/cpuctl/ReUperf/{name}/A{index}`） |
| `regex` | string | 无 | 进程名或 cmdline 的 ECMAScript 正则表达式；缺失、非字符串或空字符串时记录警告并跳过整条规则 |
| `pinned` | bool | false | 始终视为 TOP 状态 |
| `topfore` | bool | false | 在前台时视为 TOP 状态 |
| `rules` | array | [] | 线程规则列表 |

### 线程规则字段

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `k` | string | "." | 线程名正则表达式（keyword） |
| `ac` | string | "auto" | 亲和性类别名称 |
| `pc` | string | "auto" | 优先级类别名称 |
| `uclamp_max` | int | - | uclamp.max 值（0-100），需 enable_limit |
| `cpu_share` | int | - | cpu.shares 值（0-1024），需 enable_limit |
| `enable_limit` | bool | false | 启用 uclamp/cpu_share 限制 |

### 正则大小写匹配

正则默认遵循 C++ ECMAScript 语法并区分大小写。除全局 `case_insensitive: true` 外，进程 `regex` 和线程 `k` 可在**开头**加 `(?i)`，仅对该条规则启用忽略大小写匹配：

```json
{
  "k": "(?i)network",
  "ac": "0-4",
  "pc": "net"
}
```

`(?i)` 仅支持作为整个正则的开头前缀；实现会移除前缀，并使用 `std::regex::icase` 编译余下表达式。`foo(?i)bar`、`(?i:foo)` 等 PCRE 风格的内联或分组标志不受支持。若同时设置 `case_insensitive: true`，所有规则均忽略大小写，`(?i)` 没有额外效果。

### 正则转义、包含匹配与性能风险

`regex` 与线程规则 `k` 都是 C++ `std::regex` 的 ECMAScript 正则，不是普通字符串。实现会以包含匹配方式使用它们，因此普通非空模式会匹配名称或 cmdline 中包含该模式的位置。

JSON 中反斜杠必须再次转义；例如要匹配包名中的字面量点号 `.`，应写成：

```json
"regex": "com\\.example\\.app"
```

`"."` 保持正则语义，匹配任意单个字符，可用作几乎所有非空进程的兜底规则。

| JSON 值 | 正则含义 | 示例用途 |
|------|------|------|
| `"."` | 任意单个字符 | 匹配几乎所有非空进程 |
| `"\\."` | 字面量点号 `.` | 匹配包含点号的名称 |
| `"com\\.example"` | 字面量 `com.example` | 包名包含匹配 |
| `"^com\\.example\\.app$"` | 完整精确匹配 | 仅匹配完整包名 |

单条正则当前限制为 500 字符，但长度限制不等于匹配耗时限制。`std::regex` 对某些含嵌套量词、重复分支或高歧义结构的模式可能产生灾难性回溯；单次匹配当前没有超时中断机制。应避免 `(a+)+`、`(.*)*` 等模式，并优先使用明确锚点和低歧义表达式。此性能风险目前作为已知限制，由配置作者负责规避。

## 特殊宏

| 宏 | 替换为 |
|----|--------|
| `/HOME_PACKAGE/` | 启动器包名（通过 dumpsys 获取） |
| `/MAIN_THREAD/` | 进程名（主线程名） |

## 状态判定

| 状态 | 条件 |
|------|------|
| TOP | 进程的 `cpu` controller 路径为 `/top-app`，或 `pinned=true`，或 (`topfore=true` 且 FG) |
| FG | 进程的 `cpu` controller 路径为 `/foreground` |
| BG | 进程的 `cpu` controller 路径为 `/background` 或 `/system-background` |

## 示例：限制特定线程

```json
{
  "name": "Game",
  "regex": "^com\\.game\\.",
  "rules": [
    {
      "k": "UnityMain",
      "ac": "c2",
      "pc": "rtusr",
      "uclamp_max": 100,
      "cpu_share": 512,
      "enable_limit": true
    }
  ]
}
```

此配置将 Unity 主线程限制为 uclamp 100 和 512 cpu shares。

## 调度周期

程序采用“目标扫描 + 低频全量校准”模型。目标扫描、同状态重复调度和 cgroup 漂移复查分别由配置参数独立限频：

```text
每 `highspeed_sched_ms`（默认模板为 300ms）：
  仅扫描上一次校准得到的 top-app PID、pinned PID、以及处于 FG 的 topfore PID。
  线程读取与任务投递最多占用 top_scan_budget_us；预算耗尽后记录 PID/TID 游标，下一轮续扫。
  包括 TOP 在内的同状态重复任务受 `timing.min_schedule_interval_ms` 限制；首次投递和状态变化仍立即处理。
  包括 TOP、pinned、topfore 在内的 cgroup 漂移读取受 `timing.cgroup_check_interval_ms` 限制。

每 refresh_interval_ms（从上一轮全量扫描结束后计时）：
  遍历 /proc，刷新 top-app / FG / pinned / topfore PID 集合，清理死亡 PID；
  再以 full_scan_budget_us 分派 TOP、规则提升 TOP 和 FG 线程。

进程创建/退出事件：
  仅处理受影响的单一 PID，不触发全局 /proc 扫描。
```

线程目录不会先 `stat()`：`opendir("/proc/<pid>/task")` 直接充当存在性检查，读取 `comm` 失败的已退出线程会跳过。额外的预检查会增加一次 syscall，且不能消除线程同时退出导致的 TOCTOU 竞态。

| 参数 | 推荐起点 | 调整建议 |
|------|---------|---------|
| `highspeed_sched_ms` | 300ms | 降低可提高新线程响应速度，但增加目标 PID 扫描频率。 |
| `refresh_interval_ms` | 2000ms | 提高可降低全系统 `/proc` 校准频率，但 PID/cgroup 集合更新更慢。 |
| `top_scan_budget_us` | 4000us | 主线程尖峰偏高时下调；目标游戏线程很多且发现延迟明显时上调。 |
| `full_scan_budget_us` | 12000us | 全量校准后的线程投递预算；不限制 PID 枚举本身。 |
| `scan_batch_yield_us` | 0us | 仅在预算仍无法平滑峰值时尝试 50-100us；不建议 1000us。 |

## 兼容性

- 兼容 uperf v3 JSON 格式
- 仅使用 `modules.sched` 部分
- 忽略 `idle` 和 `boost` 状态
- `touch` 在内部重命名为 `top`
- 市售 Android 设备即使底层采用 unified cgroup v2，通常仍会暴露 `/dev/cpuset`、`/dev/cpuctl` 等 Android 兼容路径；底层 cgroup 版本本身不是功能可用性的判断依据。
- ReUperf cpuset 组统一创建于 `/dev/cpuset/top-app/ReUperf_<mask>`；进程状态仍以独立 `cpu` controller 的 `top-app`、`foreground`、`background`、`system-background` 路径判断，不能从 cpuset 路径反推。
- cpuset/cpuctl 限制以目标组实际暴露且可写的 `tasks`、`cpu.uclamp.max`、`cpu.shares` 等控制文件为准。程序以 `open(O_WRONLY|O_CLOEXEC)` 加单次/完整 `write()` 向 cgroup 控制文件提交 TID，不使用普通文件截断，也不需要 `O_APPEND`；缺失或不可写的控制文件会被记录。
- SELinux、内核策略或厂商实现可能拒绝 cgroup 迁移或参数设置；CPU affinity 和优先级设置同样需要内核接口与足够权限。

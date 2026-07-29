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
      "refresh_interval_ms": 1000,
      "highspeed_sched_ms": 100,
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
| `refresh_interval_ms` | int | 1000 | 低频扫描间隔（毫秒），扫描 fg/top 进程并更新缓存 |
| `highspeed_sched_ms` | int | 100 | 高频调度间隔（毫秒），调度 pinned/topfore 进程 |
| `log.level` | string | "info" | 日志级别：err/warn/info/debug/trace |
| `log.output` | string | 见下文 | 日志输出文件路径 |

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
| `regex` | string | "." | 进程名正则表达式 |
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

## 特殊宏

| 宏 | 替换为 |
|----|--------|
| `/HOME_PACKAGE/` | 启动器包名（通过 dumpsys 获取） |
| `/MAIN_THREAD/` | 进程名（主线程名） |

## 状态判定

| 状态 | 条件 |
|------|------|
| TOP | 进程在 `/dev/cpuset/top-app` 或 `pinned=true` 或 (`topfore=true` 且 FG) |
| FG | 进程在 `/dev/cpuset/foreground` |
| BG | 其他进程 |

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

程序采用双层调度架构：

```
┌─────────────────────────────────────────────────────────────┐
│  高频循环 (highspeed_sched_ms)                               │
│  └─ 调度缓存中的 pinned/topfore 进程                         │
│  └─ 扫描当前 FG/TOP 进程并分派线程任务                        │
│     响应速度快，用于实时调度关键进程                           │
└─────────────────────────────────────────────────────────────┘
                              ↓
              每 (refresh_interval_ms / highspeed_sched_ms) 次后
                              ↓
┌─────────────────────────────────────────────────────────────┐
│  低频扫描 (refresh_interval_ms)                             │
│  └─ 扫描 /proc 与进程 cgroup 状态                            │
│  └─ 更新 pinned/topfore 缓存列表                             │
│  └─ 清理死亡 PID                                            │
│     保证进程列表与实际状态同步                               │
└─────────────────────────────────────────────────────────────┘
```

| 参数 | 默认值 | 调整建议 |
|------|--------|---------|
| `highspeed_sched_ms` | 100ms | 降低可提高响应速度，但增加 CPU 占用 |
| `refresh_interval_ms` | 1000ms | 提高可降低系统负载，但进程发现延迟增加 |

## 兼容性

- 兼容 uperf v3 JSON 格式
- 仅使用 `modules.sched` 部分
- 忽略 `idle` 和 `boost` 状态
- `touch` 在内部重命名为 `top`
- 市售 Android 设备即使底层采用 unified cgroup v2，通常仍会暴露 `/dev/cpuset`、`/dev/cpuctl` 等 Android 兼容路径；底层 cgroup 版本本身不是功能可用性的判断依据。
- cpuset/cpuctl 限制以目标组实际暴露且可写的 `tasks`、`cpu.uclamp.max`、`cpu.shares` 等控制文件为准。线程会直接写入目标组的 `tasks`，以实现线程级迁移；缺失或不可写的控制文件会被记录并跳过。
- SELinux、内核策略或厂商实现可能拒绝 cgroup 迁移或参数设置；CPU affinity 和优先级设置同样需要内核接口与足够权限。

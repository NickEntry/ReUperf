# ReUperf Thread Scheduler Memory

## 项目概览

ReUperf 是基于 uperf JSON 配置格式的 Android 线程调度器。它通过 `sched_setaffinity`、`sched_setscheduler`、cpuset 和 cpuctl cgroup，为匹配的线程应用 CPU 亲和性、调度优先级和可选资源限制。

## 关键路径

| 项目 | 路径 |
|---|---|
| 配置文件 | `/data/adb/ReUperf/ReUperf.json` |
| 日志文件 | `/data/adb/ReUperf/ReUperf.log` |
| 守护进程 | `/data/adb/ReUperf/thread_scheduler` |
| Magisk 模板 | `module_template/` |
| 自动打包脚本 | `scripts/package_module.sh` |

## 构建与模块产物

- `build.sh`：动态链接构建；成功后自动生成 Magisk 模块 zip。
- `build_static.sh`：静态链接构建；成功后自动生成 Magisk 模块 zip。
- 模块输出目录默认为 `out/`，文件名包含 ABI、链接类型和版本时间。
- 动态模块会打包同 ABI 的 `libc++_shared.so`；静态模块不包含该库。
- GitHub Actions 构建 4 个 ABI：`arm64-v8a`、`armeabi-v7a`、`x86_64`、`x86`，每个 ABI 产出 dynamic/static 两种模块。

## 调度模型

### 进程状态

- **BG**：`cpu` controller 路径为 `/background` 或 `/system-background` 的进程。
- **FG**：`cpu` controller 路径为 `/foreground` 的进程。
- **TOP**：`cpu` controller 路径为 `/top-app` 的进程。
- `cpu` controller 是状态权威来源；ReUperf 对 cpuset controller 的线程级迁移不会改变进程状态判定。
- `pinned=true` 会将规则有效状态固定为 TOP；`topfore=true` 会在进程处于 FG 时提升为 TOP。

### 调度循环与事件

1. 启动时扫描规则、初始化 cgroup，并启动 4 个 `ScanWorker`。
2. 高频循环按 `highspeed_sched_ms` 仅扫描缓存中的 top-app、`pinned` 和满足条件的 `topfore` PID；线程读取与投递受 `top_scan_budget_us` 限制。
3. 低频循环按 `refresh_interval_ms` 从上一轮全量扫描完成后计时，刷新 PID 分类、清理死亡 PID，并以 `full_scan_budget_us` 分派线程。
4. `ProcMonitor` 通过 inotify 监控 `/proc` 的 PID 创建和删除；事件经 `EventRouter` 合并后仅触发受影响 PID 的增量 dispatch，不触发完整重扫。
5. `ConfigFileWatcher` 监控配置所在目录的文件修改；检测到有效更新后会重建 matcher、scanner 和 worker。重载与事件调度通过互斥保护，避免旧 worker 被并发访问。

## cgroup 结构与线程迁移

```text
/dev/cpuset/top-app/ReUperf_{cpumask_name}
/dev/cpuctl/ReUperf/{rule_name}/A{thread_rule_index + 1}
```

- cpuset 用于将线程放入与 cpumask 对应的组；同时会调用 `sched_setaffinity`。
- affinity/cpuset、priority 和 cpuctl 的接管状态独立记录；即使整体应用失败，已尝试维度在规则移除时仍会恢复 baseline。
- 当线程规则的 `enable_limit=true` 时，会创建对应的 cpuctl 子组，并按需设置 `cpu.uclamp.max` 与 `cpu.shares`。
- 调度器将 **TID 直接写入最终目标 cgroup 的 `tasks` 文件**，从而保持线程级迁移；不会通过 `cgroup.procs` 迁移整个进程。

## 兼容性

- 使用 C++17 和 nlohmann/json。
- 市售 Android 设备即使底层采用 unified cgroup v2，通常仍会暴露 `/dev/cpuset`、`/dev/cpuctl` 等 Android 兼容路径；底层 cgroup 版本本身不是功能可用性的判断依据。
- cgroup 限制功能以实际暴露且可写的控制文件为准，包括目标组的 `tasks`、`cpu.shares` 和可能的 `cpu.uclamp.max`；亲和性和优先级能力仍取决于内核接口、SELinux 和进程权限。

## 配置

配置兼容 uperf v3 的 `modules.sched` 结构。字段、优先级语义和示例见 [config.md](config.md)。

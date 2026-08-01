# ReUperf

ReUperf 是一个面向 Android 的线程级调度器，使用 uperf 风格的 JSON 规则匹配进程和线程，并通过 cpuset、`sched_setaffinity`、调度策略/优先级以及可选 cpuctl 限制应用调度配置。

## 主要功能

- 按进程名、cmdline 和线程名正则匹配规则。
- 区分 BG、FG、TOP 状态，并支持 `pinned` 和 `topfore`。
- 为单个 TID 设置 cpuset、CPU affinity、scheduler policy、priority 和 nice。
- 可选配置 `cpu.uclamp.max` 与 `cpu.shares`。
- 保存线程原始调度状态，在规则、进程状态、配置或程序生命周期变化时恢复对应 baseline。
- 支持运行中重新加载配置以及 Magisk 模块安装。

## 运行环境条件

当前正式支持范围以同时满足以下条件的 Android 环境为准：

1. 设备具有 root 权限，并允许 ReUperf 访问其他进程的 `/proc` 信息和调度接口。
2. `cpu` controller 能稳定提供可识别的进程状态路径：
   - `top-app` 对应 TOP；
   - `foreground` 对应 FG；
   - `background` 或 `system-background` 对应 BG。
3. `cpu` controller 是进程状态的权威来源，且不会被 ReUperf 的线程级 cpuset 迁移改变。
4. `/dev/cpuset` 存在，并提供可创建、可配置的子组以及可写的 `tasks`、`cpus`、`mems` 控制文件。
5. 使用 cpuctl 限制时，`/dev/cpuctl` 及目标组的 `tasks`、`cpu.shares`、`cpu.uclamp.max` 等所需文件实际存在且可写。
6. 内核和 SELinux 策略允许对目标 TID 执行 cgroup 迁移、`sched_setaffinity`、`sched_setscheduler` 和 `setpriority`。实时调度通常还需要等效于 `CAP_SYS_NICE` 的权限。
7. 配置中的 CPU 编号必须对应设备实际 CPU；模板默认配置面向 8 核、CPU ID 为 0-7 的设备，其他拓扑应先调整 `cpumask`。

底层使用 cgroup v1 还是带 Android 兼容 hierarchy 的其他实现不是唯一判断依据；以上控制器路径、文件语义和权限是否实际可用才是支持条件。不满足这些条件的 ROM 或内核目前不属于已验证运行范围。

## 安装与运行

推荐使用构建产出的 Magisk 模块 zip。模块安装后主要文件位于：

```text
/data/adb/ReUperf/thread_scheduler
/data/adb/ReUperf/ReUperf.json
/data/adb/ReUperf/ReUperf.log
```

手动前台测试：

```sh
su
sh /data/adb/ReUperf/OnceRun.sh
```

后台启动：

```sh
su
sh /data/adb/ReUperf/AlreadyRun.sh
```

## 配置

默认配置位于 `module_template/ReUperf/ReUperf.json`，安装后的有效配置位于 `/data/adb/ReUperf/ReUperf.json`。

完整字段、状态语义、正则规则和调优说明见 [config.md](config.md)。

运行中替换有效配置后，ReUperf 会恢复旧配置已管理线程的 baseline，重建调度对象并进行完整重扫。建议通过同目录临时文件加原子重命名更新 JSON。

## 构建

构建依赖、Termux/NDK 编译、ABI 选择及 Magisk 打包方法见 [BUILD.md](BUILD.md)。常用命令：

```sh
./build.sh
./build_static.sh
```

本机开发构建与测试：

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

## 状态恢复语义

ReUperf 分别跟踪 affinity/cpuset、scheduler priority 和 cpuctl limit：

- 有效 affinity 变为空时，恢复原始 cpuset 与 affinity。
- 应用有效 affinity 时，先迁入目标 cpuset，再设置目标 affinity；因此不同且互不相交的有效 CPU mask 之间可以直接切换。
- priority 从受管理值变为 `0` 时，恢复原始 scheduler policy、priority 和 nice。
- `enable_limit` 从 `true` 变为 `false` 时，恢复原始 cpuctl cgroup。
- 当线程完全不再受任何调度维度管理时，恢复完整 baseline。
- 所有恢复均结合 PID/TID start time 校验，避免将旧状态应用到复用的 ID。

## 运行检查

针对具体线程检查实际状态：

```sh
cat /proc/<pid>/cgroup
cat /proc/<pid>/task/<tid>/cgroup
grep -E 'Cpus_allowed|Cpus_allowed_list' /proc/<pid>/task/<tid>/status
chrt -p <tid>
```

发生异常时可将日志级别临时改为 `trace`，再检查：

```sh
grep -E 'apply_with_result|Set affinity|rolling back|failed to restore' \
  /data/adb/ReUperf/ReUperf.log
```

注意：ReUperf 迁移的是线程 TID，只查看 `/proc/<pid>/cgroup` 不能确认某个工作线程的实际 cpuset。

## 已知限制

- 正则由受信任的本地配置提供，复杂表达式可能产生高匹配开销；请避免灾难性回溯模式。
- cgroup 文件存在不代表厂商内核一定接受写入，最终行为仍取决于内核、权限和 SELinux。
- 不保证在 `cpu` controller 无法稳定提供 TOP/FG/BG 状态的环境中正确工作；cpuset fallback 仅作为兼容路径，不属于当前正式验证范围。
- 普通主机测试可验证解析、状态转换和恢复决策，但 Android cgroup 与调度系统调用仍需在目标设备真机验证。

## License

本项目采用 [MIT License](LICENSE)。第三方组件信息见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

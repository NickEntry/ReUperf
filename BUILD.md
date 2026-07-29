# Thread Scheduler 编译指南

## 文件说明

| 文件 | 用途 |
|------|------|
| `CMakeLists.txt` | 编译配置（支持 `BUILD_STATIC` 选项） |
| `build.sh` / `build.bat` | 正常编译脚本（动态链接） |
| `build_static.sh` / `build_static.bat` | 静态编译脚本（静态链接） |
| `scripts/package_module.sh` | 将已构建的二进制打包为 Magisk 模块 |
| `module_template/` | Magisk 模块模板、启动脚本和默认配置 |

## 前置要求

### Termux（Android 设备直接编译）

1. **CMake** (3.14 或更高版本)
   ```bash
   pkg install cmake
   ```

2. **nlohmann-json**（构建脚本会自动检测并提示安装）
   ```bash
   pkg install nlohmann-json
   ```

3. **lld 链接器**（修复 tagged pointers 问题，构建脚本会自动检测）
   ```bash
   pkg install lld
   ```

### 交叉编译（Linux/Windows + NDK）

1. **Android NDK** (r25c 或更高版本)
   - 下载地址：https://developer.android.com/ndk/downloads

2. **CMake** (3.14 或更高版本)

3. **nlohmann-json**
   - 优先使用系统包：`pkg install nlohmann-json`（Termux）
   - 或由 CMake 从 GitHub 自动下载

## 编译方法

### Termux 编译（推荐）

```bash
# 安装依赖
pkg install cmake nlohmann-json lld

# 正常编译（动态链接）
./build.sh

# 静态编译（需额外安装静态库）
pkg install ndk-sysroot ndk-multilib-native-static libandroid-support-static
./build_static.sh
```

构建脚本会自动：
- 检测 Termux 环境
- 检查并提示安装缺失依赖
- 使用本地编译器（c++/clang）
- 使用 lld 链接器（修复 tagged pointers 问题）
- 在构建成功后调用 `scripts/package_module.sh`，将 Magisk 模块 zip 输出到 `out/`

### 交叉编译（Linux/Windows）

#### 环境变量设置

**Windows**
```cmd
set NDK=C:\android-ndk-r25c
```

**Linux/Mac**
```bash
export NDK=$HOME/android-ndk-r25c
```

#### 正常编译
```bash
./build.sh
```

#### 静态编译
```bash
./build_static.sh
```

### 手动编译

#### 正常编译（动态链接）
```bash
mkdir build && cd build

cmake -S .. -B . \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_STATIC=OFF

cmake --build . -j$(nproc)
```

#### 静态编译
```bash
mkdir build_static && cd build_static

cmake -S .. -B . \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_STATIC=ON

cmake --build . -j$(nproc)
```

#### 交叉编译（NDK）
```bash
mkdir build && cd build

export NDK=/path/to/android-ndk-r25c
export TOOLCHAIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64
export TARGET=aarch64-linux-android
export API=29

cmake -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a \
      -DANDROID_PLATFORM=android-29 \
      -DCMAKE_BUILD_TYPE=Release \
      ..

make -j$(nproc)
```

## 自定义架构

```bash
# ARM64 (默认)
export TARGET_ARCH=arm64-v8a
./build.sh

# ARMv7
export TARGET_ARCH=armeabi-v7a
./build.sh

# x86_64
export TARGET_ARCH=x86_64
./build.sh

# x86
export TARGET_ARCH=x86
./build.sh
```

自定义 API 级别：
```bash
export API_LEVEL=24  # Android 7.0
./build.sh
```

## Magisk 模块打包

`build.sh` 和 `build_static.sh` 在二进制构建成功后会自动打包模块，默认输出到项目根目录的 `out/`：

```text
out/ReUperf-{ABI}-{dynamic|static}-{版本时间}.zip
```

- 支持 ABI：`arm64-v8a`、`armeabi-v7a`、`x86_64`、`x86`。
- **dynamic** 模块会从 Android NDK 打包同 ABI 的 `libc++_shared.so`；模块启动脚本会设置 `LD_LIBRARY_PATH=/data/adb/ReUperf`。
- **static** 模块不包含 `libc++_shared.so`。
- 模块安装和开机服务将二进制放在 `/data/adb/ReUperf/thread_scheduler`。首次安装会复制默认配置到 `/data/adb/ReUperf/ReUperf.json`；之后模块更新会保留用户已修改的配置。

也可以在已有二进制时单独打包：

```bash
bash scripts/package_module.sh \
  --binary build/thread_scheduler \
  --arch arm64-v8a \
  --type dynamic \
  --ndk "$NDK" \
  --output out
```

## 验证编译结果

### 检查文件信息
```bash
# Linux (需要 file 命令)
file build/thread_scheduler

# Termux (可能没有 file 命令)
ls -lh build/thread_scheduler
```

正常编译输出示例：
```
thread_scheduler: ELF 64-bit LSB pie executable, ARM aarch64, version 1 (SYSV), dynamically linked, interpreter /system/bin/linker64, stripped
```

静态编译输出示例：
```
thread_scheduler: ELF 64-bit LSB executable, ARM aarch64, version 1 (GNU/Linux), statically linked, stripped
```

### 检查依赖库
```bash
# Linux
ldd build/thread_scheduler

# Android (使用 readelf)
readelf -d build/thread_scheduler
```

### 检查文件大小
```bash
ls -lh build/thread_scheduler
```

- 正常编译：约 500KB - 1MB
- 静态编译：约 2MB - 5MB

## 部署到 Android

```bash
# 推送到设备
adb push build/thread_scheduler /data/local/tmp/

# 进入设备
adb shell

# 移动到执行目录
su
mv /data/local/tmp/thread_scheduler /data/adb/ReUperf/
chmod 755 /data/adb/ReUperf/thread_scheduler

# 运行
/data/adb/ReUperf/thread_scheduler /data/adb/ReUperf/ReUperf.json
```

## 常见问题

### 1. Tagged Pointers 错误
```
Pointer tag for 0x... was truncated
c++: error: linker command failed
```
**原因**：Android 设备的 tagged pointers 机制与链接器冲突
**解决**：安装 lld 链接器
```bash
pkg install lld
```

### 2. 无法连接 GitHub
```
fatal: unable to access 'https://github.com/nlohmann/json.git'
```
**原因**：网络问题，无法克隆依赖库
**解决**：手动安装 nlohmann-json
```bash
pkg install nlohmann-json
```

### 3. 静态链接失败
```
ld.lld: error: unable to find library -lc
```
**原因**：缺少静态库文件
**解决**：安装静态链接依赖
```bash
pkg install ndk-sysroot ndk-multilib-native-static libandroid-support-static
```

### 4. NDK 找不到
```
ERROR: NDK not found
```
**原因**：未设置 `NDK` 环境变量（仅交叉编译需要）
**解决**：
- 如果在 Termux 上编译，不需要 NDK，脚本会自动检测
- 如果交叉编译，设置 `NDK` 环境变量指向 NDK 安装目录

### 5. CMake 找不到 nlohmann-json
```
-- Could NOT find nlohmann_json
```
**原因**：未安装 nlohmann-json 开发库
**解决**：
```bash
pkg install nlohmann-json
```

### 6. 编译时权限错误
```
Permission denied
```
**解决**：
```bash
chmod +x build.sh build_static.sh
```

### 7. lld 未找到
```
warning: 未找到 lld，尝试使用默认链接器
```
**原因**：未安装 lld 链接器
**解决**：
```bash
pkg install lld
```

## 性能对比

| 编译方式 | 文件大小 | 启动速度 | 兼容性 | 推荐场景 |
|----------|----------|----------|--------|----------|
| 动态链接 | 小 (~500KB) | 快 | 需要系统库 | 大多数设备 |
| 静态链接 | 大 (~3MB) | 稍慢 | 独立运行 | 旧设备/特殊 ROM |

## 许可证

本项目使用 nlohmann/json 库 (MIT License)

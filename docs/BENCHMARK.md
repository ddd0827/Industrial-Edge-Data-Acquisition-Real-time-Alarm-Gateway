# EdgeGuard 验证与性能基准

本文记录本次实际构建、测试和基准结果。结果来自单一开发机，只用于说明实现已经跑通和提供量级参考，**不代表真实工业现场的吞吐、延迟或稳定性**。

## 1. 环境

| 项目 | 值 |
|---|---|
| CPU | AMD Ryzen 7 7840H with Radeon 780M Graphics |
| OS | Windows 11 Home China |
| Compiler | MinGW-w64 GCC 16.2.0 (UCRT, POSIX threads, SEH) |
| Build system | CMake 4.4.3 + Ninja |
| Build type | Release (`-O3 -DNDEBUG`) |
| 日期 | 2026-10-19 |

## 2. 构建与测试

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4

cd build
ctest --output-on-failure
./edgeguard_tests
```

结果：

```text
100% tests passed out of 1
Test #1: edgeguard_tests ... Passed

18 passed, 0 failed, 18 total
```

测试覆盖 SPSC 队列、SampleBus、Modbus 编解码/仿真器集成、配置、JSON、DSL 编译、类型检查、告警状态机、环形时序存储和 WAL 恢复/撕裂尾部截断。

## 3. 端到端验证

命令：

```bash
./build/edgeguard.exe run --config configs/edgeguard.conf --with-simulator --duration-ms 20000
```

验证内容：

- 2 台 Modbus TCP 设备（unit 1/2）在线，采集计划分别为 5 指标/2 次读取、4 指标/2 次读取。
- 20 秒运行共处理 536 个样本，WAL 写入 536，WAL 丢弃 0。
- `boiler_overheat` 与 `boiler_pressure_high` 触发后按滞回条件自动恢复。
- HTTP 首页返回 200 `text/html`。
- `/api/v1/health`、`/overview`、`/series`、`/alarms` 返回符合面板契约的 JSON。
- `/api/v1/stream` 实际收到 `ready`、`sample`、`alarm` 事件。
- 第二次启动时 WAL 恢复日志为 536 samples / 545 records / 15831 bytes，健康接口显示 `recovered_samples=536`。
- 进程在 `--duration-ms` 到期后 0 退出；本次在干净的 CMake build 目录中完成构建，无编译错误、无编译警告。

## 4. 微基准

命令：

```bash
./build/edgeguard.exe bench --iterations 1000000
```

干净重建后连续执行 3 次；下表中的数字为这 3 次结果的中位数。

| 测试 | 3 次结果（M/s） | 中位数 |
|---|---|---:|
| SPSC 队列单生产者/单消费者吞吐 | 137.19 / 132.03 / 131.39 Mmsg/s | **132.03 Mmsg/s** |
| 规则 VM 条件求值 | 28.58 / 28.47 / 28.73 Meval/s | **28.58 Meval/s** |
| 时序存储单线程追加 | 74.10 / 75.96 / 72.86 Msample/s | **74.10 Msample/s** |

运行间波动主要来自 CPU 频率/调度；应以量级和可复现性为目标，而不是追求单次最高值。

### 这些数字代表什么

- **SPSC**：1 个生产者、1 个消费者，`uint64_t` 负载，队列容量 65536；主要反映无锁环形队列的交接开销，不包含 Modbus 网络、解码和规则执行。
- **Rule VM**：单个已编译的短路条件，预填充 available/latest，循环执行 `evaluate_condition`；每次调用会用 `std::vector` 作为求值栈，所以这个数字仍包含少量分配开销，但可反映字节码分发的量级。
- **Series append**：16 条时序、每条容量 200000、单线程写入 1M 个样本；没有 HTTP 查询并发和 WAL 磁盘 I/O，主要反映环形缓冲与锁开销。



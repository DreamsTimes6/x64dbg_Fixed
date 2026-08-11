# x64dbg_Fixed

基于官方 [x64dbg](https://github.com/x64dbg/x64dbg) `development` 分支的修复版，在保留官方全部功能的基础上：

- **修复了一批缺陷**（内存断点、数据库健壮性、JIT 取消、多线程单步等，见[修复清单](#修复清单)）
- **新增 headless JSON-RPC 命令行接口**，让 AI / 脚本可以直接驱动调试器（见 [AI 命令行使用](#ai-命令行使用headless-json-rpc)）
- **补充了官方简体中文翻译缺失的选项文本**

---

## 目录

- [修复清单](#修复清单)
- [AI 命令行使用（headless JSON-RPC）](#ai-命令行使用headless-json-rpc)
- [构建与测试](#构建与测试)
- [界面中文化](#界面中文化)
- [详细文档](#详细文档)

---

## 修复清单

### 1. 内存断点（Memory Breakpoints）

| 文件 | 修复内容 |
|---|---|
| `src/dbg/commands/cmd-breakpoint-control.cpp` | `bpm`/`membp` 使用整个内存区域（region）语义；地址不在内存映射时（如 x64 下 32 位寄存器表达式）回退到所在页；`SetMemoryBPXEx` 武装失败（栈守卫页 / 保留区间）时自动降级为单页断点 |
| `src/dbg/breakpoint.cpp` | `findMemoryBreakpoint` 改为线性扫描精确范围匹配，修复同页多个断点（整页断点 + 精确大小断点）时 `upper_bound` 漏查；加载数据库断点时正确标记 `active` |
| `src/tests/membp/test*.txt` | 补充断点命中断言（`run` 后 `mbasserthit`） |

### 2. 数据库健壮性（Database）

| 文件 | 修复内容 |
|---|---|
| `src/dbg/database.cpp` | 保存改为**原子写**（先写 `.tmp` 再 `MoveFileExW` 替换），进程中途被杀不再留下半截损坏的 `.dd32`；主库损坏时**自动回退加载 `.bak`** 并恢复主库；清空数据时同步删除备份；备份/恢复失败打印错误 |
| `src/dbg/filemap.h` | `BufferedWriter` 暴露 `Flush()` 并检测部分写入，尾部缓冲写盘错误不再被析构函数吞掉 |

### 3. 即时调试器（JIT）取消

| 文件 | 修复内容 |
|---|---|
| `src/dbg/jit.cpp` / `jit.h` | 新增 `dbgclearjit`：支持删除注册表 `AeDebug\Debugger` 值 |
| `src/dbg/commands/cmd-misc.cpp` | `setjit restore` 在没有保存旧 JIT 时（首次设置 / 重复设置）不再报错，改为清除注册表 JIT —— GUI“设为即时调试器”取消不再失败 |
| `src/gui/Src/Gui/SettingsDialog.cpp` | 移除取消勾选时的强制拦截（`NOT FOUND OLD JIT` 警告块） |

### 4. 单线程步进与 F4 线程锁定（Single-Threaded Stepping）

| 文件 | 修复内容 |
|---|---|
| `src/dbg/thread.cpp` / `thread.h` | 新增 `ThreadSuspendAllExceptActive()` |
| `src/dbg/commands/cmd-debug-control.cpp/.h` | 新增设置 `Engine.SingleThreadStepping`：`step`/`stepover`/`stepout` 时自动挂起其他线程（等效 OllyDbg 单线程步进）；**F4（`run <addr>`）同样锁定其他线程**（只当前线程跑到目标地址），`run`（F9）恢复所有线程；`stop` 时清理 |
| `src/dbg/debugger.cpp` | 新增 `gRunToAddress`：**F4 运行到地址优先于已有断点**——命中目标地址时干净暂停，**跳过已有 F2 断点的命令/日志/命中计数**；`gRunToThreadId` 线程级命中（其他线程踩到不暂停） |
| `src/gui/Src/Gui/SettingsDialog.*` | 选项页新增“单步调试时挂起其他线程”勾选框 |

### 4b. 线程断点（进程断点 / 线程断点）

| 文件 | 修复内容 |
|---|---|
| `src/dbg/breakpoint.h/cpp` | `BREAKPOINT.threadId`（0=进程级，非0=线程级）；`BpSetThreadId`；序列化保存/恢复；`bpf_threadid` 放枚举末尾（向后兼容） |
| `src/dbg/debugger.cpp` | 命中线程过滤：非目标线程命中 → 不暂停（TitanEngine 保持断点武装，无死循环） |
| `src/dbg/commands/cmd-breakpoint-control.cpp/.h` | 新命令 `bpt 地址[,线程ID]`（设线程断点，默认当前活动线程）、`bpthread 地址`（进程↔线程切换） |
| `src/dbg/_dbgfunctions.h` | `BP_FIELD.bpf_threadid`（枚举末尾） |
| `src/gui/.../CommonActions.*` | 反汇编右键菜单：线程断点（当前线程）/（选择线程...）/ 切换 进程/线程断点 |
| `src/gui/.../BreakpointsView.*` | 断点列表右键菜单：切换 进程/线程断点、修改线程...；类型列线程断点显示 `T: <十进制线程ID>` |
| `src/gui/.../CPUSideBar.*` | 反汇编侧栏线程断点图标：红点内白色 T（禁用变灰，字形精确居中） |

### 5. 插件菜单命令化（headless 可用）

| 文件 | 修复内容 |
|---|---|
| `src/dbg/plugin_loader.cpp` | 插件 `_plugin_menuaddentry` 注册成功后自动生成 `menu_<title>` 命令，CLI / headless 下可直接调用插件菜单功能（触发 `CBMENUENTRY`） |

### 6. headless JSON-RPC 模式（AI 命令行）

| 文件 | 修复内容 |
|---|---|
| `src/headless/headless.cpp` | 新增 `-rpc` 模式：stdin/stdout JSON Lines 协议（`ping`/`get`/`eval`/`cmd`/`wait`/`exit`），支持日志 / 状态事件，`cmd` 支持同步等待；`GUI_PROCESS_EVENTS` 排空插件线程命令队列 |
| `src/dbg/x64dbg.cpp` | 新增 `_dbg_processpendingcommands` 导出（处理插件线程排队的命令）；`sleep` 命令别名 |
| `src/dbg/simplescript.cpp` | 新增 `_dbg_scriptisrunning` 导出（headless 退出时等待脚本结束） |
| `src/bridge/*` | 导出 `DBGPROCESSPENDINGCOMMANDS` / `DBGSCRIPTISRUNNING` bridge 函数 |
| `src/headless/x64dbg_rpc.py` / `test_rpc.py` / `README-rpc.md` | Python 客户端、功能自测（x64/x32 均 23/23 通过）、RPC 详细文档 |

---

## AI 命令行使用（headless JSON-RPC）

`headless.exe -rpc` 提供**供 AI（或任意脚本）直接调用的无界面调试接口**：
**stdin 每行一条 JSON 请求，stdout 每行一条 JSON 响应/事件（JSON Lines）**。
它保留 x64dbg 的全部命令、表达式、脚本能力，并可用 `-plugin` 加载插件（如 xx_vm 的 `.dp32` / `.dp64`）。

GUI 版（`x64dbg.exe` / `x32dbg.exe`）完全不受影响 —— `-rpc` 只在 headless 下有意义。

### 启动

```
headless.exe -rpc -userdir <绝对路径> -plugin <插件路径> [-plugin ...] [-- 目标程序参数]
```

- `-userdir`：隔离的用户目录（数据库 / 符号 / 配置放这里；需已存在，客户端负责创建）
- `-plugin`：可重复，预加载插件
- 就绪信号：初始化完成后 stdout 输出 `{"hello":"headless-rpc"}`，之后才能收发请求
- 退出：发送 `{"exit":true}` 或关闭 stdin；进程以 0 退出

**配套**：`headless.exe` 必须与同目录的 `x64dbg.dll`（x64）/ `x32dbg.dll`（x32）一起使用。
当前构建产物在 `src/bin/<arch>/`。

### 协议参考（JSON Lines）

| 请求 | 说明 | 响应 |
|---|---|---|
| `{"ping":true}` | 连通性测试 | `{"ok":true,"pong":true}` |
| `{"get":"state"}` | 调试器状态 | `{"ok":true,"state":"initialized\|paused\|running\|stopped","isDebugging":bool}` |
| `{"eval":"<表达式>"}` | 求值表达式（寄存器 / 内存 / 符号 / 数学 / 系统变量） | `{"ok":true,"value":N,"hex":"0x..."}` 或 `{"ok":false,"err":"..."}` |
| `{"cmd":"<命令>"}` | 执行命令，**异步**（入队即返回 ok） | `{"ok":true}` |
| `{"cmd":"<命令>","sync":true}` | 执行命令，**同步**（`DbgCmdExecDirect`，命令完成才返回；适合 step / run） | `{"ok":true}` 或 `{"ok":false,"err":"..."}` |
| `{"wait":"<state>","timeout":ms}` | 阻塞直到调试器进入指定状态（如 run 后等 paused） | `{"ok":true,"state":"..."}` 或 `{"ok":false,"err":"timeout ..."}` |
| `{"exit":true}` | 退出会话 | 进程结束 |

失败统一 `{"ok":false,"err":"..."}`。非法 JSON / 未知请求也会得到 `ok:false`，不会崩溃。

**异步事件**（随时出现，客户端应收集 / 忽略）：
- `{"log":"<文本>"}` — 调试器日志（命令输出、符号加载、断点命中提示等）
- `{"event":"state","state":"..."}` — 调试状态变化（initialized / paused / running / stopped）

### AI 典型工作流

```
{"cmd":"init C:\\targets\\app.exe"}    → ok（异步）
{"wait":"initialized"}                 → 目标加载完成
{"cmd":"bp app:Check"}                 → 设断点
{"cmd":"run"}                          → ok（异步）
{"wait":"paused"}                      → 断点命中
{"eval":"cip"}                         → 0x...（当前指令）
{"eval":"byte:[cip]"}                  → 当前指令字节
{"eval":"rax"}                         → 寄存器值
{"cmd":"step","sync":true}             → 单步（完成后直接读结果）
{"eval":"cip"}
{"cmd":"pause"}                        → 暂停
{"exit":true}
```

**读内存 / 寄存器（eval 表达式语法）**：

| 想读什么 | 表达式 |
|---|---|
| 当前指令指针 | `cip` |
| 通用寄存器 | `rax` `rbx` `eax` ... |
| 某地址 1/2/4/8 字节 | `byte:[0x401000]` `word:[0x401000]` `dword:[...]` `qword:[...]` |
| 指针解引用 | `[0x401000]`（按位宽） |
| 模块符号 | `membp:ReadTarget` `kernel32:VirtualAlloc` |
| 数学 / 位运算 | `1+2*3`、`(cip-0x1000)/4`、`(eax & 0xFF) \| 1` |

**同步 vs 异步 —— 何时用哪个**：

| 命令 | 建议 |
|---|---|
| `step` `stepinto` `stepover` `run` `pause` | **`sync:true`** —— 返回即命令完成，可直接读寄存器 / 内存 |
| `init` `attach` | 异步即可，配合 `{"wait":"initialized"}` |
| `bp` `bpc` `dump` 等即时命令 | 异步即可（结果在 `log` 事件里） |
| 需要精确失败信息的命令 | `sync:true`（未知命令 / 参数错误会返回 `ok:false`） |

> 注意：`step` 前后都是 paused，`wait` 无法区分单步是否完成 —— 单步必须用 `sync:true`。

### Python 客户端

`src/headless/x64dbg_rpc.py` 提供 `X64dbgRpc` 类：

```python
from x64dbg_rpc import X64dbgRpc

with X64dbgRpc(arch="x64", userdir=r"C:\tmp\ud",
               plugins=[r"tests\membp\membp.dp64"]) as rpc:
    rpc.ping()                        # True
    rpc.cmd("init tests/membp.exe")
    rpc.wait("initialized")
    rpc.cmd("bp membp:ReadSequence")
    rpc.cmd("run")
    rpc.wait("paused")
    eip = rpc.eval("cip")             # 读寄存器
    val = rpc.eval("byte:[0x401000]") # 读内存
    rpc.cmd("step", sync=True)        # 同步单步
```

API：`start()/close()/__enter__/__exit__`、`ping()`、`get_state()`、`wait(state, timeout)`、
`eval(expr)`、`cmd(command, sync=False)`。失败抛 `RpcError`。

自测：`python src/headless/x64dbg_rpc.py`（demo）和
`python src/headless/test_rpc.py --arch x64|x32`（23 项功能测试，双架构全过）。

### 直接管道使用（无 Python）

```bash
printf '%s\n' \
  '{"ping":true}' \
  '{"cmd":"init C:\\targets\\app.exe"}' \
  '{"wait":"initialized","timeout":15000}' \
  '{"cmd":"run"}' \
  '{"wait":"paused","timeout":15000}' \
  '{"eval":"cip"}' \
  '{"exit":true}' | headless.exe -rpc -userdir C:\tmp\ud
```

逐行读取 stdout，`json.loads` 每行；忽略 `log`/`event`，等 `ok`/`fail`。

### 集成注意事项（Windows）

1. **编码**：headless 输出为 UTF-8；Windows 控制台 / Python 子进程请用 UTF-8 读取（Python `text=True` 即可）。
2. **管道缓冲**：客户端必须**持续读取 stdout**，否则调试器输出堆积会阻塞 headless（死锁风险）。Python `bufsize=1`（行缓冲）逐行读。
3. **stdin 行缓冲**：每条请求必须**以换行结尾**并 `flush`。
4. **userdir 隔离**：每个会话用独立 `-userdir`，避免数据库 / 配置互相污染。
5. **异步命令**：`cmd` 的 `ok` 只表示“已入队”，不代表命令成功 —— 需要结果可靠时用 `sync:true` 或配合 `wait`/`log` 判断。
6. **超时**：`wait` 自带 timeout，客户端侧也建议设整体读超时，防止调试器卡死导致挂起。
7. **`.dp64` vs `.dp32`**：x64 用 `x64dbg.dll`/`xxx.dp64`，x32 用 `x32dbg.dll`/`xxx.dp32`，别混用。

### 与 `-cf` 脚本模式的关系

| | `-cf script.txt` | `-rpc` |
|---|---|---|
| 交互 | 一次性脚本，跑完退出 | 长会话，一问一答 |
| AI 适用 | 批处理（已知完整流程） | 交互式决策（观察 → 决策 → 下一步） |
| 输出 | 日志流 | 结构化 JSON |
| 插件 | `-plugin` | `-plugin`（相同） |

两者互补：`-cf` 适合“跑一遍就完”的回归；`-rpc` 适合 AI 逐步分析。

### 插件线程命令支持（命令泵）

headless 与 GUI 的一个关键差异已修复：

- **GUI 版**：命令循环线程在等待脚本完成时，`GuiProcessEvents()` 处理 Qt 事件泵。
- **headless 版**（修复后）：`GUI_PROCESS_EVENTS` 会**排空调试器命令队列**（`gMsgQueue`），因此**插件独立线程通过 `DbgCmdExec()` 下发的命令在脚本运行期间也会被执行**（例如分析线程循环下发 `sti`/`read` 等命令）。

相关机制：`_dbg_processpendingcommands()`（dbg 导出，非阻塞消费命令队列）、
`DbgProcessPendingCommands()`（bridge API）、`-testing` 模式下不排空（依赖严格命令顺序）、
退出时 headless 会等待 `-cf` 脚本完成再销毁命令队列。

### 插件线程单步（sti）的正确模式

x64dbg 的 step 命令是**异步语义**（请求单步后立即返回，单步完成是异步的）。插件线程不能这样：

```c
// ❌ 错误：sti 可能还没完成就读取 → 读到旧寄存器快照（cip 卡在入口）
DbgCmdExec("sti");
Sleep(1);                 // 不保证单步完成
DbgGetRegDumpEx(&rd, ...); // 竞态：可能读到上一拍的旧值
```

正确做法是**先等调试器暂停（单步完成 = 暂停），再读寄存器**：

```c
// ✅ 正确
DbgCmdExec("sti");              // 或 DbgCmdExecDirect("sti")
DbgWaitForPause(1000);          // 阻塞直到暂停（单步完成），超时返回 false
DbgGetRegDumpEx(&rd, sizeof(rd));
duint eip = rd.regcontext.cip;  // 此时是新值
```

- `DbgWaitForPause(timeoutMs)`（bridge API，新增）：轮询 `DbgIsRunning()`，暂停时返回 `true`。
- 时序不受 `Sleep` 粒度影响；每步可靠地同步到“单步完成 + 寄存器刷新”。
- 配合本分支的“单线程步进”设置，插件线程单步时其他线程被挂起，不会被其它线程的断点打断。

---

## 构建与测试

```
# 需要 VS2022/2026 + Qt 5.15 + CMake
cmake --build build64 --config Release    # x64：GUI + headless + tests
cmake --build build32 --config Release    # x32：同上

# headless 相关单独构建
cmake --build build64 --config Release --target headless dbg
cmake --build build32 --config Release --target headless dbg

# headless RPC 自测（双架构 23/23 通过）
python src/headless/test_rpc.py --arch x64
python src/headless/test_rpc.py --arch x32
```

构建输出在 `src/bin/<arch>/`。发布时把整套（`*.exe`、`*.dll`、资源目录）复制到顶层
`bin/<arch>\` 即可；只需额外把 `headless.exe` 一起复制进去，GUI/CLI 就齐了。
（辅助 stub `*_bridge.dll` / `*_dbg.dll` 是 cmake 中间产物，不要发布。）

**版本一致性**：GUI 和 CLI 用的 `x64bridge.dll` / `x64dbg.dll` 必须来自**同一次构建**，
升级时整目录替换，避免新旧混用。

---

## 界面中文化

x64dbg 的多语言靠 `translations/x64dbg_<语言>.qm` 加载。官方 `x64dbg_zh_CN.qm`
未覆盖的新增字符串会显示英文。本分支：

- `src/gui/Src/main.cpp` 支持加载 `x64dbg_<locale>_patch.qm` 补丁翻译（覆盖官方缺失的新字符串）。
- `src/gui/translations/x64dbg_zh_CN_patch.ts/.qm` 补译了官方 `zh_CN` 缺失的 11 条选项文本
  （单线程步进、分离进程、十六进制表示法等）。

使用：把官方 `translations/` 目录与 `x64dbg_zh_CN_patch.qm` 一起部署，选项里把语言切到
中文（`zh_CN`）即可。

---

## 详细文档

- `src/headless/README-rpc.md` —— headless RPC 完整协议与集成细节
- 官方文档：<https://x64dbg.com/blog>、<https://github.com/x64dbg/x64dbg/wiki>

---

*本分支与官方 x64dbg 保持同步（`development` 分支），可随时 `git fetch upstream` 合并上游更新。*

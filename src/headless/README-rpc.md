# headless JSON-RPC 模式 —— AI / 脚本 CLI 接口

`headless.exe -rpc` 提供一个供 AI（或任意脚本）直接调用的无界面调试接口：
**stdin 每行一条 JSON 请求，stdout 每行一条 JSON 响应/事件（JSON Lines）**。
它保留 x64dbg 的全部命令、表达式、脚本能力，并可用 `-plugin` 加载插件。

GUI 版（`x64dbg.exe` / `x32dbg.exe`）完全不受影响 —— `-rpc` 只在 headless 下有意义。

---

## 1. 启动

```
headless.exe -rpc -userdir <绝对路径> -plugin <插件路径> [-plugin ...] [-- 目标程序参数]
```

- `-userdir`：隔离的用户目录（数据库/符号/配置放这里；需已存在，客户端负责创建）
- `-plugin`：可重复，预加载插件（与 GUI 版插件同名，如 `xx.dp64` / `xx.dp32`）
- 就绪信号：初始化完成后 stdout 输出 `{"hello":"headless-rpc"}`，之后才能收发请求
- 退出：发送 `{"exit":true}` 或关闭 stdin；进程以 0 退出

**文件配套**：`headless.exe` 必须与同目录的 `x64dbg.dll`（x64）/ `x32dbg.dll`（x32）一起使用
（headless 按目录加载调试内核）。当前构建产物在 `src/bin/<arch>/`。

---

## 2. 协议参考（JSON Lines）

### 请求 → 响应

| 请求 | 说明 | 响应 |
|---|---|---|
| `{"ping":true}` | 连通性测试 | `{"ok":true,"pong":true}` |
| `{"get":"state"}` | 调试器状态 | `{"ok":true,"state":"initialized\|paused\|running\|stopped","isDebugging":bool}` |
| `{"eval":"<表达式>"}` | 求值表达式（寄存器/内存/符号/数学/系统变量） | `{"ok":true,"value":N,"hex":"0x..."}` 或 `{"ok":false,"err":"..."}` |
| `{"cmd":"<命令>"}` | 执行命令，**异步**（入队即返回 ok） | `{"ok":true}` |
| `{"cmd":"<命令>","sync":true}` | 执行命令，**同步**（`DbgCmdExecDirect`，命令完成才返回；适合 step/run） | `{"ok":true}` 或 `{"ok":false,"err":"..."}` |
| `{"wait":"<state>","timeout":ms}` | 阻塞直到调试器进入指定状态（如 run 后等 paused） | `{"ok":true,"state":"..."}` 或 `{"ok":false,"err":"timeout ..."}` |
| `{"exit":true}` | 退出会话 | 进程结束 |

失败统一 `{"ok":false,"err":"..."}`。非法 JSON / 未知请求也会得到 `ok:false`，不会崩溃。

### 异步事件（随时出现，客户端应收集/忽略）

- `{"log":"<文本>"}` — 调试器日志（命令输出、符号加载、断点命中提示等）
- `{"event":"state","state":"..."}` — 调试状态变化（initialized / paused / running / stopped）

> 注意：`log` 事件由调试器内部线程异步投递，顺序可能与命令响应交错；请以
> `ok`/`fail` 响应判断命令完成，`log`/`event` 仅作附带信息。

---

## 3. AI 典型工作流

### 3.1 调试一个程序

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

### 3.2 读内存 / 寄存器（eval 表达式语法）

| 想读什么 | 表达式 |
|---|---|
| 当前指令指针 | `cip` |
| 通用寄存器 | `rax` `rbx` `eax` ... |
| 某地址 1/2/4/8 字节 | `byte:[0x401000]` `word:[0x401000]` `dword:[...]` `qword:[...]` |
| 指针解引用 | `[0x401000]`（按位宽） |
| 模块符号 | `membp:ReadTarget` `kernel32:VirtualAlloc` |
| 数学 | `1+2*3`、`(cip-0x1000)/4` |
| 位运算 | `(eax & 0xFF) | 1` |

### 3.3 常见命令速查（与 GUI 版命令框一致）

`init` `attach` `bp` `bpc`（删）`bph` `run` `pause` `stop` `step` `stepinto` `stepover`
`stepout` `dump` `mem` `dis` `x64dbg` 自带全套命令（`help` 可列出）。

### 3.4 同步 vs 异步 —— 何时用哪个

| 命令 | 建议 |
|---|---|
| `step` `stepinto` `stepover` `run` `pause` | **`sync:true`** —— 返回即命令完成，可直接读寄存器/内存 |
| `init` `attach` | 异步即可，配合 `{"wait":"initialized"}` |
| `bp` `bpc` `dump` 等即时命令 | 异步即可（结果在 `log` 事件里） |
| 需要精确失败信息的命令 | `sync:true`（未知命令/参数错误会返回 `ok:false`） |

### 3.5 wait 状态说明

- run/step 命中暂停 → `{"wait":"paused"}`
- 进程退出 → 状态 `stopped`
- 注意：`step` 前后都是 paused，`wait` 无法区分单步是否完成 —— 所以单步必须用 `sync:true`

---

## 4. Python 客户端

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
`python src/headless/test_rpc.py --arch x64|x32`（23 项功能测试，当前双架构全过）。

---

## 5. 直接管道使用（无 Python）

任意语言启动子进程即可：

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

---

## 6. 集成注意事项（Windows）

1. **编码**：headless 输出为 UTF-8；Windows 控制台/Python 子进程请用 UTF-8 读取
   （Python `text=True` 即可；避免 GBK 控制台混入）。
2. **管道缓冲**：客户端必须**持续读取 stdout**，否则调试器输出堆积会阻塞 headless
   （死锁风险）。Python `bufsize=1`（行缓冲）逐行读。
3. **stdin 行缓冲**：每条请求必须**以换行结尾**并 `flush`。
4. **userdir 隔离**：每个会话用独立 `-userdir`，避免数据库/配置互相污染。
5. **异步命令**：`cmd` 的 `ok` 只表示"已入队"，不代表命令成功 —— 需要结果可靠时用
   `sync:true` 或配合 `wait`/`log` 事件判断。
6. **超时**：所有响应都应在合理时间内返回；`wait` 自带 timeout，客户端侧也建议设
   整体读超时，防止调试器卡死导致挂起。
7. **`.dp64` vs `.dp32`**：x64 用 `x64dbg.dll`/`xxx.dp64`，x32 用 `x32dbg.dll`/`xxx.dp32`，
   别混用。

---

## 7. 与 `-cf` 脚本模式的关系

| | `-cf script.txt` | `-rpc` |
|---|---|---|
| 交互 | 一次性脚本，跑完退出 | 长会话，一问一答 |
| AI 适用 | 批处理（已知完整流程） | 交互式决策（观察→决策→下一步） |
| 输出 | 日志流 | 结构化 JSON |
| 插件 | `-plugin` | `-plugin`（相同） |

两者互补：`-cf` 适合"跑一遍就完"的回归；`-rpc` 适合 AI 逐步分析。

---

## 8. 构建与测试

```
cmake --build build64 --config Release --target headless dbg   # x64
cmake --build build32 --config Release --target headless dbg   # x32
python src/headless/test_rpc.py --arch x64
python src/headless/test_rpc.py --arch x32
```

产物：`src/bin/<arch>/headless.exe`（配套同目录 `x64dbg.dll`/`x32dbg.dll`）。
`-rpc` 参数在 dbg 侧也需接受（`src/dbg/x64dbg.cpp` 的 `CommandlineArguments`）。

当前功能测试覆盖（`test_rpc.py`，x64/x32 均 23/23 通过）：
ping / get state / eval（数学、十六进制、非法表达式）/ init / bp / run / wait（含超时）/
读符号地址与内存 / sync 单步 ×2 / 插件命令 / 未知命令报错 / 干净退出 /
非法 JSON 与未知请求处理 / 全部 stdout 行为合法 JSON。

---

## 9. GUI 版与 CLI 版兼容性（一起发布）

**结论：完全兼容，可以放同一个目录、一起构建发布。**

### 9.1 模块关系

```
GUI 版：  x64dbg.exe  → x64gui.dll  → x64bridge.dll → x64dbg.dll（调试内核）
CLI 版：  headless.exe  ────────────→ x64bridge.dll → x64dbg.dll（同一个内核）
```

- `headless.exe` **不依赖 `x64gui.dll`**（它自己充当"无界面 GUI 模块"），只依赖
  `x64bridge.dll` + `x64dbg.dll` —— 与 GUI 版共用同一对内核 DLL。
- 因此 GUI 与 CLI **必须配套使用同一版本的内核 DLL**：把同一份 `x64bridge.dll` /
  `x64dbg.dll` 同时给 `x64dbg.exe` 和 `headless.exe` 用即可，两者互不干扰
  （各自独立进程、独立 `-userdir`）。
- 插件（`.dp64` / `.dp32`）两边通用：GUI 放插件目录，CLI 用 `-plugin <路径>` 加载，
  同一个插件文件。

### 9.2 发布目录布局（顶层 `bin\`）

```
F:\x64dbg\bin\
├── x96dbg.exe                # 架构选择启动器（GUI）
├── x64\
│   ├── x64dbg.exe            # GUI 主程序
│   ├── headless.exe          # CLI（-rpc / -cf / -testing）
│   ├── x64bridge.dll         # 共享内核（bridge）
│   ├── x64dbg.dll            # 共享内核（debugger）
│   ├── x64gui.dll            # GUI 专用
│   ├── TitanEngine.dll, capstone.dll, ...   # 依赖
│   └── themes\ translations\ platforms\ ... # 资源
└── x32\                      # 同名结构（x32dbg.exe / headless.exe / x32bridge.dll / x32dbg.dll / ...）
```

CLI 用法（在对应架构目录下执行，保证同目录内核 DLL 生效）：

```
cd F:\x64dbg\bin\x64
headless.exe -rpc -userdir C:\tmp\ud -plugin myplugin.dp64
```

### 9.3 一次构建，两个版本同时产出

```
cmake --build build64 --config Release    # x64：GUI + headless + tests
cmake --build build32 --config Release    # x32：同上
```

构建输出在 `src/bin/<arch>/`。发布时把整套（`*.exe`、`*.dll`、资源目录）复制到顶层
`bin\<arch>\` 即可；只需额外把 `headless.exe` 一起复制进去，GUI/CLI 就齐了。
（辅助 stub `*_bridge.dll` / `*_dbg.dll` 是 cmake 中间产物，不要发布。）

### 9.4 版本一致性提醒

- GUI 和 CLI 用的 `x64bridge.dll` / `x64dbg.dll` 必须来自**同一次构建**，否则可能出现
  接口不匹配。
- 升级时整目录替换，避免新旧混用。
- 数据库/配置：GUI 默认写 exe 旁的用户目录；CLI 用 `-userdir` 隔离，两者可共存，
  也可以指向同一目录共享数据库（注意并发写冲突）。

---

## 10. 插件线程命令支持（headless 命令泵）

headless 与 GUI 的一个关键差异：

- **GUI 版**：命令循环线程在等待脚本完成时，`GuiProcessEvents()` 处理 Qt 事件泵。
- **headless 版**（修复后）：`GUI_PROCESS_EVENTS` 会**排空调试器命令队列**（`gMsgQueue`），因此**插件独立线程通过 `DbgCmdExec()` 下发的命令在脚本运行期间也会被执行**（例如分析线程循环下发 `sti`/`read` 等命令）。

相关机制：

- `_dbg_processpendingcommands()`（dbg 导出）：非阻塞消费 `gMsgQueue` 中所有待处理命令。
- `DbgProcessPendingCommands()`（bridge API）：headless 从 `GUI_PROCESS_EVENTS` 调用。
- `-testing` 模式下不排空（测试脚本依赖严格命令顺序，如 `testfinalize` 必须在脚本之后）。
- 退出（stdin EOF）时 headless 会**等待 -cf 脚本完成**再销毁命令队列，避免插件线程命令在收尾时丢失。

因此"插件用独立线程下发 `DbgCmdExec`"的方案在 headless 下**可用**（GUI 与 CLI 行为一致）。

---

## 11. 插件线程单步（sti）的正确模式 —— 寄存器读取同步

x64dbg 的 step 命令（`sti`/`stepinto`）是**异步语义**：命令只负责"请求单步"
（`cbDebugRunInternal` 发出 `unlock(WAITID_RUN)` 后立即返回），**单步完成是异步的**。
因此插件线程不能这样做：

```c
// ❌ 错误：sti 可能还没完成就读取 → 读到旧寄存器快照（cip 卡在入口）
DbgCmdExec("sti");
Sleep(1);                 // Sleep(1) 不保证单步完成
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

- `DbgWaitForPause(timeoutMs)`（bridge API，新增）：轮询 `DbgIsRunning()`，
  调试器暂停（不再运行）时返回 `true`，超时返回 `false`。
- 时序不受 `Sleep` 粒度影响；每步可靠地同步到"单步完成 + 寄存器刷新"。
- GUI 下同样适用（GUI 手动 step 是交互同步的，线程场景必须显式等待）。

**为什么之前"不稳定"**：读取到旧快照 → cip 卡在入口 → 基于寄存器的停止条件
（如 `esp` 还原判断）对入口指令误判。等待暂停后读取稳定，停止条件不再误触发。

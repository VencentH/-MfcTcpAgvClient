# MfcTcpAgvClient — MFC TCP AGV 任务客户端

这是一个使用 **C++17、Visual Studio 2022、MFC Unicode、Winsock2 和 nlohmann/json** 的 x64 对话框程序。程序作为 TCP 客户端，向“网络调试助手”开启的 TCP 服务端发送单行 UTF-8 JSON，并等待一行 JSON 响应。

每次请求使用一次新的 TCP 连接；收到完整响应行或发生错误后立即关闭 socket，不自动重试。

## 1. 目录结构

```text
tcp_demo/
├── MfcTcpAgvClient.sln
├── README.md
├── MODULE_MAPPING.md
├── MfcTcpAgvClient/
│   ├── MfcTcpAgvClient.vcxproj
│   ├── MfcTcpAgvClient.vcxproj.filters
│   ├── MfcTcpAgvClientApp.h/.cpp
│   ├── TcpClientDlg.h/.cpp
│   ├── TcpWorker.h/.cpp
│   ├── StringUtil.h
│   ├── MfcTcpAgvClient.rc
│   ├── resource.h
│   └── third_party/nlohmann/json.hpp
└── x64/Debug|Release/                 构建后生成
```

`json.hpp` 已随工程提供，附加包含目录使用 `$(ProjectDir)third_party`，不需要参考工程、vcpkg 或系统级 JSON 安装。

## 2. 构建环境

需要：

- Visual Studio 2022；
- “使用 C++ 的桌面开发”工作负载；
- MSVC v143 x64 生成工具；
- “适用于最新 v143 生成工具的 C++ MFC（x86 和 x64）”组件；
- Windows 10/11 SDK。

若 Visual Studio 提示缺少 MFC，可打开 **Visual Studio Installer → Visual Studio 2022 → 修改 → 单个组件**，搜索并勾选 **MFC** 和 **v143** 对应组件，然后安装。

工程固定使用：

- `PlatformToolset = v143`；
- `LanguageStandard = stdcpp17`；
- `CharacterSet = Unicode`；
- 动态 MFC；
- `Debug|x64` 和 `Release|x64`；
- `/utf-8` 编译选项；
- 链接库 `ws2_32.lib`。

### 在 Visual Studio 中构建

1. 在 Visual Studio 中选择 **文件 → 打开 → 项目/解决方案**，打开 `MfcTcpAgvClient.sln`。
2. 在工具栏的“解决方案配置”中选择 **Debug**，“解决方案平台”中选择 **x64**。
3. 选择 **生成 → 生成解决方案**，或按 `Ctrl+Shift+B`。
4. 改为 **Release|x64**，再次生成解决方案。
5. 可执行文件分别生成到：
   - `x64/Debug/MfcTcpAgvClient.exe`
   - `x64/Release/MfcTcpAgvClient.exe`

### 在命令行构建

打开“Developer Command Prompt for VS 2022”，进入本目录后执行：

```bat
msbuild MfcTcpAgvClient.sln /p:Configuration=Debug /p:Platform=x64
msbuild MfcTcpAgvClient.sln /p:Configuration=Release /p:Platform=x64
```

若复制到其他未安装 Visual Studio 的电脑运行，还需安装相应版本的 **Microsoft Visual C++ Redistributable (x64)**。

## 3. 网络调试助手设置

不同版本“网络调试助手”的按钮名称可能略有差异，但配置项相同。

### 启动 TCP 服务端

1. 打开网络调试助手，在协议/工作模式中选择 **TCP Server（TCP 服务端）**，不要选择 TCP Client。
2. 设置监听地址：
   - 程序与助手在同一台电脑：监听 `127.0.0.1`，程序中的服务端地址也填 `127.0.0.1`。
   - 程序从另一台电脑连接：助手监听本机实际局域网 IP；若助手支持并可监听所有网卡，也可选 `0.0.0.0`。
   - **客户端目标地址不要填 `0.0.0.0`**，应填运行助手那台电脑的实际 IP 或可解析主机名。
3. 设置监听端口，例如 `9000`，然后点击 **开始监听/启动服务端**。
4. 程序中填写相同的 IP/主机名和端口。端口必须是 `1` 到 `65535` 的整数。
5. 跨电脑或跨防火墙测试时，确保服务端防火墙允许该端口的入站 TCP 连接。

启动客户端后，IP、端口和四个任务字段均为空，需要手动填写。

### 从助手返回 JSON

1. 点击客户端的 **发送 TCP**。助手应先显示一个客户端连接，并显示收到的 JSON 行。
2. 在助手的“发送区”选择 **文本/字符串发送**，编码选择 **UTF-8**，不要选择 HEX 发送。
3. 响应必须放在**同一行**。例如：

   ```json
   {"code":200,"message":"OK","requestId":"local-1","data":{"taskId":"TASK-2024","agvId":"AGV-008","taskStatus":"ACCEPTED","routeId":"ROUTE-A"}}
   ```

4. 开启助手的 **追加 CRLF / 加回车换行 / 发送回车换行** 选项，再执行发送。若助手使用转义模式，也可在 JSON 末尾添加 `\r\n` 并启用“解析转义字符”。
5. 客户端接受 **LF** 结尾，也接受 **CRLF** 结尾；因此助手只追加 LF 也可以。解析前会去掉行尾的一个 CR。
6. 不要发送 Markdown 代码围栏，不要把示例格式化成多行 JSON，也不要让助手把 `\r\n` 当作四个普通字符发送。

UTF-8 文本不应带 BOM。请求中的中文、引号和反斜杠由 nlohmann/json 正确编码；响应中的中文也应以 UTF-8 发送。

## 4. JSON 行协议

### 请求

请求 JSON 顶层必须是对象，且只包含四个字符串字段：

```json
{"taskId":"TASK-2024","agvId":"AGV-008","taskType":"LOOP_RUN","routeId":"ROUTE-A"}
```

实际发送字节为：

```text
UTF-8 JSON + CR + LF
```

也就是：

```text
{"taskId":"...","agvId":"...","taskType":"...","routeId":"..."}\r\n
```

规则：

- 四个字段均必填，类型均为字符串。
- 去除字段值两端的空间、Tab、CR、LF 后不能为空。
- 去空白仅用于校验，实际发送用户输入的原始字段值。
- 序列化后的 UTF-8 JSON 最大为 **1 MiB（1,048,576 字节）**，末尾 CRLF 不计入此上限。
- 发送循环处理部分发送，直到 JSON 和 CRLF 全部发完。

### 响应

响应是一行 UTF-8 JSON：

```json
{"code":200,"message":"OK","requestId":"local-1","data":{"taskId":"TASK-2024","agvId":"AGV-008","taskStatus":"ACCEPTED","routeId":"ROUTE-A"}}
```

- 以 LF 为消息边界；允许 LF 前有一个 CR。
- 接收方持续累积多次 `recv` 的数据，直到找到 LF，不假定一次 `recv` 就是一整个响应。
- 若同一批数据包含后续字节，只取第一个完整响应行，随后关闭本次连接。
- 响应 JSON 最大为 **1 MiB**；行尾 CR/LF 不计入 JSON 大小。
- 合法 JSON 用 2 空格缩进显示；非法 JSON 保留并显示原始响应文本。

### 超时

| 阶段 | 超时 |
| --- | ---: |
| 单个地址的 TCP 连接等待 | 5 秒 |
| socket 发送等待 | 10 秒 |
| socket 接收等待 | 30 秒 |

主机解析由 Windows `GetAddrInfoW` 完成，支持 IP 地址和主机名。连接采用非阻塞 `connect + select`，并用 `SO_ERROR` 检查最终连接状态，随后恢复阻塞模式并设置收发超时。

## 5. 响应业务校验

合法 JSON 按以下顺序校验：

1. `code` 必须存在、为整数且等于 `200`。
2. `data` 必须存在且为对象。
3. `data.taskStatus` 缺失或不是字符串时显示警告；为字符串时在消息区显示其值。
4. 将 `data.taskId`、`data.agvId`、`data.routeId` 与本次请求快照比较。
5. 回显字段缺失、类型错误或值不一致时追加警告；比较时忽略两端 ASCII 空白。
6. 不比较 `taskType`，因为响应格式不保证回显该字段。

业务警告只写入消息提示区，不会清空响应，也不会覆盖 TCP 连接/收发状态。

## 6. 界面与错误表现

| 场景 | TCP 状态区 | 响应区 | 消息提示区 |
| --- | --- | --- | --- |
| 主机为空、端口非法、任务字段为空白 | 保留此前状态 | 保留此前内容 | 指出校验错误，不建立连接 |
| 请求 JSON 超过 1 MiB | 保留此前状态 | 保留此前内容 | 显示实际大小和上限，不建立连接 |
| 主机解析失败 | 主机解析失败 | 清空 | 显示 Windows 错误码和描述 |
| 连接被拒绝或其他连接错误 | 连接失败 | 清空 | 显示 Windows 错误码和描述 |
| 连接超时 | 连接超时 | 清空 | 提示检查监听、地址、端口和防火墙 |
| 发送失败/发送超时 | 发送失败/发送超时 | 清空 | 显示具体发送错误 |
| 接收失败/接收超时 | 接收失败/接收超时 | 清空 | 显示具体接收错误或 30 秒超时说明 |
| 对端提前断开 | 对端断开 | 清空 | 说明未收到完整 LF 响应行 |
| 响应超过 1 MiB | 响应超长 | 清空 | 说明已停止读取并关闭连接 |
| 收到合法 JSON | 显示连接、发送字节数和接收字节数 | 2 空格缩进 JSON | 显示业务结果或警告 |
| 收到非法 JSON | 保留 TCP 收发成功结果 | 原始响应文本 | 显示 JSON 解析错误 |
| `code` 非 200 或 `data` 非对象 | 保留 TCP 收发成功结果 | 保留格式化 JSON | 显示业务校验失败 |
| 回显不一致或 `taskStatus` 异常 | 保留 TCP 收发成功结果 | 保留格式化 JSON | 追加业务警告 |

请求进行期间，发送按钮和六个输入框均禁用；重复点击会被忽略。标题栏关闭、Alt+F4、Esc 和 Enter 触发的关闭路径都会被拦截，直到工作线程回传结果。

## 7. 线程与资源管理

- UI 线程负责读取控件、校验、JSON 序列化、保存请求快照和显示结果。
- 独立 `std::thread` 负责 Winsock 初始化、解析、连接、发送、接收和清理，不访问 MFC 控件。
- 工作线程通过 `PostMessage(WM_TCP_REQUEST_DONE)` 回传 `TcpRequestResult*`。
- UI 线程使用 `std::unique_ptr` 接管结果并自动释放；若 `PostMessage` 失败，工作线程自动释放结果。
- 请求进行中禁止关窗，保证正常流程中的目标窗口存活。
- 每次请求均执行对应的 `WSAStartup/WSACleanup`，socket 由 RAII 关闭。

## 8. 建议的手工验收

1. 不启动助手，填写合法参数后发送，确认出现连接失败且界面仍可操作。
2. 启动助手但发送非法主机、端口或空白任务字段，确认没有新连接。
3. 输入中文、双引号和反斜杠，确认助手收到合法 JSON 且字段值保持原意。
4. 让助手延迟几秒响应，确认窗口可移动和重绘、控件禁用且不能关闭。
5. 让助手把响应拆成多段发送，最后再发送 LF，确认客户端能完整解析。
6. 分别测试 LF、CRLF、非法 JSON、`code != 200`、缺少 `taskStatus` 和回显不一致。
7. 让助手不接受连接或不返回 LF，分别验证连接/接收超时。
8. 让助手在 LF 前关闭连接，验证“对端断开”提示。

## 9. 本次交付验证

2026-10-30 已在本机使用 Visual Studio 2022 v143/MSBuild 实际生成：

- `Debug|x64`：成功，0 个警告，0 个错误；
- `Release|x64`：成功，0 个警告，0 个错误。

同时检查了工程引用路径、19 个控件 ID、源码 UTF-8 编码和 Release 二进制依赖；可执行文件确认依赖 `WS2_32.dll`，不依赖 OpenSSL。

本次未通过网络调试助手进行人工 GUI 联调，因此窗口布局、助手版本特定的发送选项以及实际拆包/粘包场景仍需按第 8 节验收。

# MODULE_MAPPING.md

## 模块代码映射表

| 代码文件路径 | 所属模块 | 功能描述 |
| --- | --- | --- |
| `MfcTcpAgvClient/MfcTcpAgvClientApp.h` | 应用入口 | 声明 MFC 应用类。 |
| `MfcTcpAgvClient/MfcTcpAgvClientApp.cpp` | 应用入口 | 初始化通用控件并运行主对话框。 |
| `MfcTcpAgvClient/TcpClientDlg.h` | 对话框与业务校验 | 声明控件交互、请求快照、忙碌状态和结果消息处理。 |
| `MfcTcpAgvClient/TcpClientDlg.cpp` | 对话框与业务校验 | 实现中文界面、输入校验、四字段 JSON、控件锁定、禁止关闭、响应格式化和业务回显比对。 |
| `MfcTcpAgvClient/TcpWorker.h` | TCP 工作线程 | 定义 TCP 参数、结果、完成消息和异步启动接口。 |
| `MfcTcpAgvClient/TcpWorker.cpp` | TCP 工作线程 | 实现 Winsock 生命周期、地址解析、超时连接、完整发送、按 LF 接收、大小限制、错误转换和 PostMessage 回传。 |
| `MfcTcpAgvClient/StringUtil.h` | 公共编码工具 | 提供 UTF-16/UTF-8、CString/std::string 转换及多行控件换行转换，被对话框和 TCP 模块依赖。 |
| `MfcTcpAgvClient/resource.h` | 对话框资源 | 定义对话框和全部控件 ID。 |
| `MfcTcpAgvClient/MfcTcpAgvClient.rc` | 对话框资源 | 定义主机、端口、任务字段、发送按钮、状态、响应和消息区布局。 |
| `MfcTcpAgvClient/third_party/nlohmann/json.hpp` | JSON 依赖 | 提供请求序列化、响应解析和格式化；以单头文件随工程交付。 |

## 工程文件映射

| 文件 | 用途 |
| --- | --- |
| `MfcTcpAgvClient.sln` | 提供 Debug\|x64 和 Release\|x64 解决方案配置。 |
| `MfcTcpAgvClient/MfcTcpAgvClient.vcxproj` | 配置 VS2022、v143、C++17、MFC Unicode、/utf-8、JSON 包含目录和 ws2_32.lib。 |
| `MfcTcpAgvClient/MfcTcpAgvClient.vcxproj.filters` | 在解决方案资源管理器中组织源文件、头文件、资源和第三方依赖。 |
| `README.md` | 构建、网络调试助手配置、协议、错误表现和手工验收说明。 |

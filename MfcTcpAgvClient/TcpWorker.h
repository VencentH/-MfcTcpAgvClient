#pragma once
// TcpWorker.h : TCP 请求工作线程接口（不依赖 MFC 对话框，可独立复用）。
//
// 线程模型：
//   StartTcpRequestAsync() 创建后台线程，完成主机解析、连接、发送和接收；
//   工作线程只处理 UTF-8 字符串和 Winsock，不访问任何 MFC 控件。
//   结果通过 ::PostMessage(WM_TCP_REQUEST_DONE) 交回 UI 线程。
//
// 一次请求使用一次 TCP 连接：
//   请求 JSON + CRLF -> 服务端 -> 直到 LF 的一行响应 -> 关闭 socket。
//   不自动重试，避免同一条 AGV 任务被意外提交多次。

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

// 请求参数（字符串均为 UTF-8 编码）。
struct TcpRequestParams
{
    std::string hostUtf8;           // 服务端 IP 或主机名，支持 IPv4、IPv6 和域名
    std::uint16_t port = 0;         // 服务端端口，UI 层已校验为 1–65535
    std::string bodyUtf8;           // 单行 JSON；CRLF 由工作线程发送时追加
};

// 工作线程回传的结果（堆上分配，UI 线程负责 delete）。
struct TcpRequestResult
{
    bool transportOk = false;       // true = 已收到一行响应；false = 解析/连接/收发失败
    std::string statusUtf8;         // TCP 连接/收发状态，失败时指出失败阶段
    std::string responseUtf8;       // 去掉行尾 CR/LF 后的单行响应（UTF-8）
    std::string errorUtf8;          // 失败原因；transportOk == false 时有效
    std::size_t sentBytes = 0;      // 实际发送字节数，包含末尾 CRLF
};

// 自定义消息：工作线程完成后由 ::PostMessage 发出。
// wParam = 0；lParam = TcpRequestResult*（接收方必须接管并释放）。
constexpr UINT WM_TCP_REQUEST_DONE = WM_APP + 1;

// 在后台线程发起一次 TCP 请求。
// hWndNotify：结果回传的目标窗口，一般传对话框的 GetSafeHwnd()。
// 注意：函数立即返回；所有阻塞操作都有明确超时。
void StartTcpRequestAsync(const TcpRequestParams& params, HWND hWndNotify);

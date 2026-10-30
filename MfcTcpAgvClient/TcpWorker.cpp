// TcpWorker.cpp : TCP 工作线程实现（Winsock2，不使用 MFC 控件）。
//
// 一次请求的调用链：
//   StartTcpRequestAsync()
//    └─ std::thread（后台线程）
//        ├─ WinSockSession           WSAStartup / WSACleanup
//        ├─ ResolveAndConnect()      GetAddrInfoW + 带超时的非阻塞 connect
//        ├─ SendAll()                循环 send，直到 JSON 和 CRLF 全部发出
//        ├─ ReceiveLine()            持续 recv 并累积，直到收到一个 LF
//        └─ ::PostMessage(WM_TCP_REQUEST_DONE, new TcpRequestResult)
//              └─ UI 线程用 unique_ptr 接管并释放结果
//
// TCP 是字节流，不能假定一次 send 对应一次 recv。本模块只在收到 LF 后
// 才认为响应结束；连接、发送、接收都设有超时，且一次请求只连接一次。

#include "TcpWorker.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <exception>
#include <memory>
#include <thread>

#include "StringUtil.h"

namespace {

// 各阶段超时。连接超时由 select 控制，发送/接收超时写入 socket 选项。
constexpr DWORD kConnectTimeoutMs = 5000;
constexpr DWORD kSendTimeoutMs = 10000;
constexpr DWORD kReceiveTimeoutMs = 30000;

// 响应 JSON 上限（UTF-8 字节数），行尾 CR/LF 不计入。
constexpr std::size_t kMaxResponseBytes = 1024 * 1024;

// 统一填写失败结果：status 给状态区，error 给消息区。
void Fail(TcpRequestResult& result, const std::string& status, const std::string& error)
{
    result.transportOk = false;
    result.statusUtf8 = status;
    result.errorUtf8 = error;
}

// 把 Winsock/Windows 错误码转换成可读的 UTF-8 中文消息。
// FormatMessageW 得到的系统文本通常带结尾 CRLF 和空格，需先清理再显示。
std::string FormatWindowsError(const wchar_t* operation, int errorCode)
{
    wchar_t* buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(errorCode), 0,
        reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);

    std::wstring detail;
    if (length > 0 && buffer != nullptr)
        detail.assign(buffer, length);
    if (buffer != nullptr)
        ::LocalFree(buffer);

    while (!detail.empty() &&
           (detail.back() == L'\r' || detail.back() == L'\n' || detail.back() == L' '))
    {
        detail.pop_back();
    }
    if (detail.empty())
        detail = L"未提供系统错误描述";

    std::wstring text = operation;
    text += L"失败（错误码 ";
    text += std::to_wstring(errorCode);
    text += L"）：";
    text += detail;
    return WideToUtf8(text);
}

// 阻塞 send/recv 在设置了 SO_SNDTIMEO / SO_RCVTIMEO 后，超时通常表现为
// WSAETIMEDOUT；部分情况下也可能返回 WSAEWOULDBLOCK。
bool IsTimeoutError(int errorCode)
{
    return errorCode == WSAETIMEDOUT || errorCode == WSAEWOULDBLOCK;
}

// Winsock 生命周期 RAII：每个工作线程独立初始化，线程退出时配对清理。
class WinSockSession
{
public:
    WinSockSession() : m_error(WSAStartup(MAKEWORD(2, 2), &m_data)) {}
    ~WinSockSession()
    {
        // 只有成功初始化才需要 WSACleanup。
        if (m_error == 0)
            WSACleanup();
    }

    bool IsReady() const { return m_error == 0; }
    int Error() const { return m_error; }

private:
    WSADATA m_data{};
    int m_error;
};

// socket 句柄 RAII：作用域退出时自动 closesocket，避免每条错误分支漏关。
class SocketHandle
{
public:
    explicit SocketHandle(SOCKET value = INVALID_SOCKET) : m_value(value) {}
    ~SocketHandle()
    {
        if (m_value != INVALID_SOCKET)
            closesocket(m_value);
    }

    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;

    SOCKET Get() const { return m_value; }

    // 把所有权交给调用方；本对象随后不再重复关闭该 socket。
    SOCKET Release()
    {
        const SOCKET value = m_value;
        m_value = INVALID_SOCKET;
        return value;
    }

private:
    SOCKET m_value;
};

// GetAddrInfoW 返回的地址链表需要 FreeAddrInfoW，使用 RAII 保证释放。
class AddrInfoHandle
{
public:
    ~AddrInfoHandle()
    {
        if (Value != nullptr)
            FreeAddrInfoW(Value);
    }

    PADDRINFOW Value = nullptr;
};

// 尝试连接链表中的一个地址。
//
// 连接超时实现要点：
//   1. 临时把 socket 设为非阻塞，connect 通常返回 WSAEWOULDBLOCK；
//   2. 用 select 同时等待可写和异常，最多 kConnectTimeoutMs；
//   3. select 有结果并不等于成功，必须用 getsockopt(SO_ERROR) 读取最终状态；
//   4. 成功后再恢复阻塞模式，并设置发送/接收超时供后续 send/recv 使用。
//
// 返回 false 时，lastError 带最后一处错误，timedOut 标记是否发生连接超时。
bool ConnectAddress(
    PADDRINFOW address, SOCKET& connected, int& lastError, bool& timedOut)
{
    SocketHandle socket(::socket(address->ai_family, SOCK_STREAM, IPPROTO_TCP));
    if (socket.Get() == INVALID_SOCKET)
    {
        lastError = WSAGetLastError();
        return false;
    }

    u_long nonBlocking = 1;
    if (ioctlsocket(socket.Get(), FIONBIO, &nonBlocking) == SOCKET_ERROR)
    {
        lastError = WSAGetLastError();
        return false;
    }

    const int connectResult = connect(
        socket.Get(), address->ai_addr, static_cast<int>(address->ai_addrlen));
    if (connectResult == SOCKET_ERROR)
    {
        lastError = WSAGetLastError();

        // 非阻塞 connect 正在后台进行时返回 WSAEWOULDBLOCK；
        // 其他错误（例如地址不可用）无需等待，直接交给外层尝试下一个地址。
        if (lastError != WSAEWOULDBLOCK)
            return false;

        fd_set writeSet;
        fd_set exceptSet;
        FD_ZERO(&writeSet);
        FD_ZERO(&exceptSet);
        FD_SET(socket.Get(), &writeSet);
        FD_SET(socket.Get(), &exceptSet);

        timeval timeout{};
        timeout.tv_sec = static_cast<long>(kConnectTimeoutMs / 1000);
        timeout.tv_usec = static_cast<long>((kConnectTimeoutMs % 1000) * 1000);
        const int selected = select(0, nullptr, &writeSet, &exceptSet, &timeout);
        if (selected == 0)
        {
            // select 超时：连接没有在 5 秒内完成。
            timedOut = true;
            lastError = WSAETIMEDOUT;
            return false;
        }
        if (selected == SOCKET_ERROR)
        {
            lastError = WSAGetLastError();
            return false;
        }

        // select 只说明连接尝试有结果，成功/失败必须以 SO_ERROR 为准。
        int socketError = 0;
        int socketErrorSize = sizeof(socketError);
        if (getsockopt(
                socket.Get(), SOL_SOCKET, SO_ERROR,
                reinterpret_cast<char*>(&socketError), &socketErrorSize) == SOCKET_ERROR)
        {
            lastError = WSAGetLastError();
            return false;
        }
        if (socketError != 0)
        {
            // 典型值包括 WSAECONNREFUSED、WSAETIMEDOUT、WSAENETUNREACH 等。
            lastError = socketError;
            return false;
        }
    }

    // 后续收发使用阻塞 socket，由超时 socket 选项限制每次等待时间。
    u_long blocking = 0;
    if (ioctlsocket(socket.Get(), FIONBIO, &blocking) == SOCKET_ERROR)
    {
        lastError = WSAGetLastError();
        return false;
    }

    const DWORD sendTimeout = kSendTimeoutMs;
    const DWORD receiveTimeout = kReceiveTimeoutMs;
    if (setsockopt(
            socket.Get(), SOL_SOCKET, SO_SNDTIMEO,
            reinterpret_cast<const char*>(&sendTimeout), sizeof(sendTimeout)) == SOCKET_ERROR ||
        setsockopt(
            socket.Get(), SOL_SOCKET, SO_RCVTIMEO,
            reinterpret_cast<const char*>(&receiveTimeout), sizeof(receiveTimeout)) == SOCKET_ERROR)
    {
        lastError = WSAGetLastError();
        return false;
    }

    // 只有完整配置成功后才把 socket 所有权移出本函数。
    connected = socket.Release();
    return true;
}

// 解析主机名并依次尝试返回的地址。
// GetAddrInfoW 支持 UTF-8 输入转成的 Unicode 主机名，也同时支持 IPv4/IPv6；
// 一个地址失败时继续尝试下一个，全部失败后输出连接失败或连接超时。
bool ResolveAndConnect(
    const TcpRequestParams& params, TcpRequestResult& result, SOCKET& connected)
{
    const std::wstring host = Utf8ToWide(params.hostUtf8);
    if (host.empty())
    {
        Fail(result, "主机解析失败", "主机名不是有效的 UTF-16 文本。");
        return false;
    }

    ADDRINFOW hints{};
    hints.ai_family = AF_UNSPEC;       // IPv4 和 IPv6 都允许
    hints.ai_socktype = SOCK_STREAM;   // TCP 字节流
    hints.ai_protocol = IPPROTO_TCP;

    AddrInfoHandle addresses;
    const std::wstring port = std::to_wstring(params.port);
    const int resolveResult = GetAddrInfoW(
        host.c_str(), port.c_str(), &hints, &addresses.Value);
    if (resolveResult != 0)
    {
        Fail(result, "主机解析失败", FormatWindowsError(L"主机解析", resolveResult));
        return false;
    }
    if (addresses.Value == nullptr)
    {
        Fail(result, "主机解析失败", "主机名没有返回可用的 TCP 地址。");
        return false;
    }

    int lastError = WSAEHOSTUNREACH;
    bool timedOut = false;
    for (PADDRINFOW address = addresses.Value; address != nullptr; address = address->ai_next)
    {
        if (ConnectAddress(address, connected, lastError, timedOut))
            return true;
    }

    if (timedOut && lastError == WSAETIMEDOUT)
    {
        Fail(result, "连接超时", "TCP 连接超时（5 秒），请检查服务端监听状态、IP、端口和防火墙。");
    }
    else
    {
        Fail(result, "连接失败", FormatWindowsError(L"TCP 连接", lastError));
    }
    return false;
}

// 循环发送，直到 wire（JSON + CRLF）全部写进 socket。
// send 可能只发送一部分数据，因此不能调用一次就认为请求完成。
bool SendAll(
    SOCKET socket, const std::string& wire, TcpRequestResult& result)
{
    std::size_t sent = 0;
    while (sent < wire.size())
    {
        const int count = send(
            socket, wire.data() + sent, static_cast<int>(wire.size() - sent), 0);
        if (count > 0)
        {
            // 只推进成功发送的字节数，剩余部分继续发送。
            sent += static_cast<std::size_t>(count);
            continue;
        }
        if (count == 0)
        {
            // 正常 TCP 发送不应返回 0；保守地按失败处理，避免死循环。
            Fail(result, "发送失败", "TCP 发送失败：socket 未发送任何数据。");
            return false;
        }

        const int errorCode = WSAGetLastError();
        if (IsTimeoutError(errorCode))
        {
            Fail(result, "发送超时", "TCP 发送超时（10 秒），完整 JSON 和 CRLF 尚未发完。");
        }
        else
        {
            Fail(result, "发送失败", FormatWindowsError(L"TCP 发送", errorCode));
        }
        return false;
    }

    result.sentBytes = sent;
    return true;
}

// 持续 recv 并累积数据，直到出现 LF，形成一条完整响应行。
//
// 不依赖一次 recv 就收到整行，因此正确处理拆包；
// 一批数据中出现多个字节时只取第一个 LF 之前的内容，天然处理粘包。
// 行尾若是 CRLF，解析前去掉 CR；响应超过 1 MiB 时立即停止并报错。
bool ReceiveLine(
    SOCKET socket, const TcpRequestParams& params, TcpRequestResult& result)
{
    std::string received;
    received.reserve(4096);
    char buffer[4096];

    for (;;)
    {
        const int count = recv(socket, buffer, static_cast<int>(sizeof(buffer)), 0);
        if (count == 0)
        {
            // TCP 对端正常关闭。没有 LF 就说明响应行不完整。
            Fail(result, "对端断开", "对端在发送完整响应行（LF）之前关闭了 TCP 连接。");
            return false;
        }
        if (count == SOCKET_ERROR)
        {
            const int errorCode = WSAGetLastError();
            if (IsTimeoutError(errorCode))
            {
                Fail(result, "接收超时", "TCP 接收超时（30 秒），尚未收到完整的 LF 响应行。");
            }
            else
            {
                Fail(result, "接收失败", FormatWindowsError(L"TCP 接收", errorCode));
            }
            return false;
        }

        // 把本次读到的片段追加到已收数据，下一次 recv 继续补齐。
        received.append(buffer, static_cast<std::size_t>(count));
        const std::size_t lineFeed = received.find('\n');
        if (lineFeed != std::string::npos)
        {
            // 只取 LF 之前的 JSON；若有行尾 CR，去掉一个 CR。
            std::size_t jsonBytes = lineFeed;
            if (jsonBytes > 0 && received[jsonBytes - 1] == '\r')
                --jsonBytes;
            if (jsonBytes > kMaxResponseBytes)
            {
                Fail(result, "响应超长", "响应 JSON 超过 1 MiB，已停止接收并关闭连接。");
                return false;
            }

            // LF 后的多余字节不再处理；返回后 SocketHandle 会关闭本次连接。
            result.responseUtf8 = received.substr(0, jsonBytes);
            result.transportOk = true;
            result.statusUtf8 =
                "TCP 收发成功：已连接 " + params.hostUtf8 + ":" +
                std::to_string(params.port) + "；已发送 " +
                std::to_string(result.sentBytes) + " 字节（含 CRLF）；已接收 " +
                std::to_string(jsonBytes) + " 字节 JSON。";
            return true;
        }

        // 尚未收到 LF 时也要限制内存占用。
        // 末尾暂存的 CR 可能属于下一段数据开头的 LF，不计入 JSON 大小。
        std::size_t potentialJsonBytes = received.size();
        if (potentialJsonBytes > 0 && received.back() == '\r')
            --potentialJsonBytes;
        if (potentialJsonBytes > kMaxResponseBytes)
        {
            Fail(result, "响应超长", "响应超过 1 MiB 且尚未收到 LF，已停止接收并关闭连接。");
            return false;
        }
    }
}

// 工作线程真正执行的阻塞流程。
// 任一步失败都立即返回；socket 在函数退出时由 RAII 关闭，不自动重试。
void DoTcpRequest(const TcpRequestParams& params, TcpRequestResult& result)
{
    WinSockSession winSock;
    if (!winSock.IsReady())
    {
        Fail(result, "Winsock 初始化失败", FormatWindowsError(L"初始化 Winsock", winSock.Error()));
        return;
    }

    SOCKET connected = INVALID_SOCKET;
    if (!ResolveAndConnect(params, result, connected))
        return;
    SocketHandle socket(connected);

    // 请求消息边界：UTF-8 单行 JSON 后追加 CRLF。
    const std::string wire = params.bodyUtf8 + "\r\n";
    if (!SendAll(socket.Get(), wire, result))
        return;
    ReceiveLine(socket.Get(), params, result);
}

} // namespace

// 启动并分离后台线程。
// 结果对象在启动线程前创建；若线程创建抛异常，unique_ptr 自动释放并让 UI
// 捕获异常。消息投递成功时 release() 把所有权交给 UI，失败时由 unique_ptr
// 自动 delete，避免泄漏。
void StartTcpRequestAsync(const TcpRequestParams& params, HWND hWndNotify)
{
    auto result = std::make_unique<TcpRequestResult>();
    std::thread(
        [params, hWndNotify, result = std::move(result)]() mutable {
            try
            {
                DoTcpRequest(params, *result);
            }
            catch (const std::exception& error)
            {
                Fail(*result, "TCP 内部错误", std::string("TCP 工作线程异常：") + error.what());
            }
            catch (...)
            {
                Fail(*result, "TCP 内部错误", "TCP 工作线程发生未知异常。");
            }

            if (::PostMessageW(
                    hWndNotify, WM_TCP_REQUEST_DONE, 0,
                    reinterpret_cast<LPARAM>(result.get())))
            {
                result.release();
            }
            // PostMessageW 失败时 result 仍由 unique_ptr 管理，离开作用域即释放。
        })
        .detach();
}

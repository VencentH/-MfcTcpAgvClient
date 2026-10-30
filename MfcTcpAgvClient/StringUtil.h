#pragma once
// StringUtil.h : CString/std::string 与 UTF-16/UTF-8 转换工具。
//
// 为什么需要转换：
//   MFC 控件使用 UTF-16（CString / std::wstring），而 JSON 报文、TCP 字节流
//   和 nlohmann/json 使用 UTF-8。所有进出网络和 JSON 的字符串都要经过这里，
//   否则中文会变成乱码。
//
// 另提供 LfToCrLf()：只用于把多行编辑框的显示文本转换为 Windows 换行，
//   不得用它修改实际发送的 JSON。

#include <afxwin.h>
#include <windows.h>

#include <string>

// std::wstring(UTF-16) -> std::string(UTF-8)。
// 空输入返回空串；系统转换失败时也返回空串。
inline std::string WideToUtf8(const std::wstring& text)
{
    if (text.empty())
        return {};

    // 第一次调用只取得所需缓冲区长度，第二次才写入实际字符。
    const int length = ::WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return {};

    std::string utf8(static_cast<size_t>(length), '\0');
    ::WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        utf8.data(), length, nullptr, nullptr);
    return utf8;
}

// std::string(UTF-8) -> std::wstring(UTF-16)。
// TCP 接收到的 UTF-8 JSON、主机名和 Windows 错误文本都可通过本函数进入 MFC。
inline std::wstring Utf8ToWide(const std::string& utf8)
{
    if (utf8.empty())
        return {};

    // 先查询 UTF-16 字符数，再分配并转换，避免手工估算缓冲区大小。
    const int length = ::MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (length <= 0)
        return {};

    std::wstring text(static_cast<size_t>(length), L'\0');
    ::MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
        text.data(), length);
    return text;
}

// CString(UTF-16) -> std::string(UTF-8)，用于把编辑框原始内容交给 JSON/TCP。
inline std::string CStringToUtf8(const CString& text)
{
    return WideToUtf8(std::wstring(text.GetString(), text.GetLength()));
}

// std::string(UTF-8) -> CString(UTF-16)，用于显示响应、状态和错误消息。
inline CString Utf8ToCString(const std::string& utf8)
{
    const std::wstring text = Utf8ToWide(utf8);
    return CString(text.data(), static_cast<int>(text.size()));
}

// 把裸 "\n" 转成 "\r\n"。
// 原因：Win32 多行 EDIT 控件只把 CRLF 识别为换行，而 nlohmann::json::dump()
// 输出的换行是 "\n"。凡是要显示进编辑框的文本都先经过本函数；TCP 请求体和
// 响应原文不受影响。
inline std::string LfToCrLf(const std::string& text)
{
    std::string output;
    output.reserve(text.size() + text.size() / 8);
    for (size_t i = 0; i < text.size(); ++i)
    {
        // 已经有 CR 的 LF 不重复补 CR，避免出现 CRCRLF。
        if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r'))
            output += "\r\n";
        else
            output += text[i];
    }
    return output;
}

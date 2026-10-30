// TcpClientDlg.cpp : 主对话框实现。
//
// 请求流程（与 README 的协议说明对应）：
//   [UI 线程] OnBnClickedSend()
//     ├─ 读取并校验主机、端口和四个任务字段
//     ├─ nlohmann::json 序列化，并检查 1 MiB 上限
//     ├─ 保存 taskId/agvId/routeId 请求快照
//     ├─ SetUiBusy(true)，禁用按钮和输入框
//     └─ StartTcpRequestAsync()
//          └─ [工作线程] 连接/发送/接收 -> ::PostMessage(WM_TCP_REQUEST_DONE)
//               └─ [UI 线程] OnTcpRequestDone()
//                    ├─ unique_ptr 接管并释放结果
//                    ├─ SetUiBusy(false)，恢复控件
//                    └─ ShowResponseResult()，显示 TCP 状态、响应和业务校验
//
// 本文件含中文字符串，依赖工程中的 /utf-8 编译选项。
// 校验失败只更新消息区，不启动线程、不建立 TCP 连接。

#include "TcpClientDlg.h"

#include <exception>
#include <memory>

#include <nlohmann/json.hpp>

#include "StringUtil.h"

namespace {

// 请求 JSON 上限（UTF-8 字节数）；末尾 CRLF 不属于 JSON，不计入。
constexpr std::size_t kMaxRequestBytes = 1024 * 1024;

// 去掉两端 ASCII 空白（空格/Tab/CR/LF）。
// 只用于“是否为空”的校验和响应回显比较；调用方不得把结果写回请求体，
// 实际发送的仍是用户输入的原始字段值。
std::string TrimAsciiWhitespace(const std::string& text)
{
    const char* const whitespace = " \t\r\n";
    const std::size_t begin = text.find_first_not_of(whitespace);
    if (begin == std::string::npos)
        return {};
    const std::size_t end = text.find_last_not_of(whitespace);
    return text.substr(begin, end - begin + 1);
}

// 字段值去首尾空白后为空时，把字段名追加到 blankFields（顿号分隔）。
// 只用于发送前校验；请求体仍使用用户输入的原始字段值。
void AppendBlankField(
    CString& blankFields, const char* field, const std::string& valueUtf8)
{
    if (!TrimAsciiWhitespace(valueUtf8).empty())
        return;
    if (!blankFields.IsEmpty())
        blankFields += _T("、");
    blankFields += Utf8ToCString(field);
}

// 严格解析端口：只接受 ASCII 数字，范围 1–65535。
// 不使用 _ttoi 一类的宽松转换，避免 "80abc"、"+80" 被误当成合法端口。
bool TryParsePort(const CString& text, std::uint16_t& port)
{
    CString digits = text;
    digits.Trim();
    if (digits.IsEmpty())
        return false;

    unsigned value = 0;
    for (int i = 0; i < digits.GetLength(); ++i)
    {
        const wchar_t ch = digits[i];
        if (ch < L'0' || ch > L'9')
            return false;
        value = value * 10 + static_cast<unsigned>(ch - L'0');
        if (value > 65535)
            return false;
    }
    if (value == 0)
        return false;

    port = static_cast<std::uint16_t>(value);
    return true;
}

// 比对单个响应回显字段，不一致或类型无效时追加警告。
// 仅比较 taskId、agvId、routeId；taskType 的回显不在响应契约内。
void AppendEchoWarning(
    const nlohmann::json& data, const char* field,
    const std::string& requested, CString& text)
{
    if (!data.contains(field) || !data[field].is_string())
    {
        text += _T("；警告：响应 data.") + Utf8ToCString(field) +
            _T(" 缺失或不是字符串，无法与请求比对。");
        return;
    }

    // 比较时忽略两端 ASCII 空白，不改变界面中展示的原始响应。
    const std::string echoed = TrimAsciiWhitespace(data[field].get<std::string>());
    const std::string wanted = TrimAsciiWhitespace(requested);
    if (echoed != wanted)
    {
        text += _T("；警告：data.") + Utf8ToCString(field) +
            _T(" 与请求不一致（请求 ") + Utf8ToCString(wanted) +
            _T("，响应 ") + Utf8ToCString(echoed) + _T("）。");
    }
}

// 业务校验与响应回显比对。
// 规则：
//   code 必须是整数且等于 200；
//   data 必须是对象；
//   taskStatus 缺失/类型无效为警告，类型正确时显示其值；
//   taskId、agvId、routeId 与请求快照比较，不一致为警告。
// 该函数只生成消息文本，不修改 TCP 状态或响应框内容。
CString DescribeBusinessResult(
    const nlohmann::json& parsed, const TcpRequestSnapshot& request)
{
    const bool hasCode = parsed.contains("code") && parsed["code"].is_number_integer();

    // message 是辅助信息；缺失或类型不符时不阻断业务校验。
    CString messageText;
    if (parsed.contains("message") && parsed["message"].is_string())
        messageText = _T("，message=") + Utf8ToCString(parsed["message"].get<std::string>());

    if (!hasCode)
        return _T("业务校验失败：响应 code 缺失或不是整数。") + messageText;
    if (parsed["code"] != 200)
    {
        // 使用 dump() 而非 get<int>()，避免异常大的整数导致转换溢出。
        return _T("业务校验失败：code=") + Utf8ToCString(parsed["code"].dump()) +
            _T("（约定成功值为 200）。") + messageText;
    }
    if (!parsed.contains("data") || !parsed["data"].is_object())
        return _T("业务校验失败：code=200，但响应 data 缺失或不是对象。") + messageText;

    CString text = _T("业务校验：code=200") + messageText;
    const auto& data = parsed["data"];

    // taskStatus 缺失或类型错误只警告，不影响已经收到的响应内容。
    if (!data.contains("taskStatus"))
    {
        text += _T("；警告：响应缺少 data.taskStatus，无法确认任务状态。");
    }
    else if (!data["taskStatus"].is_string())
    {
        text += _T("；警告：data.taskStatus 不是字符串，无法确认任务状态。");
    }
    else
    {
        text += _T("，taskStatus=") + Utf8ToCString(data["taskStatus"].get<std::string>());
    }

    // 回显比对只覆盖响应契约中保证可能出现的三个字段。
    AppendEchoWarning(data, "taskId", request.taskId, text);
    AppendEchoWarning(data, "agvId", request.agvId, text);
    AppendEchoWarning(data, "routeId", request.routeId, text);
    return text;
}

// 请求期间的统一关闭提示。
void ShowCloseBlockedMessage()
{
    AfxMessageBox(
        _T("TCP 请求正在进行中，请等待完成后再关闭窗口。"),
        MB_ICONWARNING | MB_OK);
}

} // namespace

BEGIN_MESSAGE_MAP(CTcpClientDlg, CDialogEx)
    ON_WM_CLOSE()
    ON_BN_CLICKED(IDC_BTN_SEND, &CTcpClientDlg::OnBnClickedSend)
    ON_MESSAGE(WM_TCP_REQUEST_DONE, &CTcpClientDlg::OnTcpRequestDone)
END_MESSAGE_MAP()

CTcpClientDlg::CTcpClientDlg(CWnd* pParent)
    : CDialogEx(IDD_MAIN, pParent)
{
}

BOOL CTcpClientDlg::OnInitDialog()
{
    CDialogEx::OnInitDialog();

    // .rc 中的控件文字保持英文（规避 rc.exe 代码页问题），
    // 运行时统一覆盖为中文。
    SetWindowText(_T("MFC TCP AGV 任务客户端"));
    SetDlgItemText(IDC_LABEL_HOST, _T("服务端 IP/主机名："));
    SetDlgItemText(IDC_LABEL_PORT, _T("端口："));
    SetDlgItemText(IDC_LABEL_TASK_ID, _T("taskId："));
    SetDlgItemText(IDC_LABEL_AGV_ID, _T("agvId："));
    SetDlgItemText(IDC_LABEL_TASK_TYPE, _T("taskType："));
    SetDlgItemText(IDC_LABEL_ROUTE_ID, _T("routeId："));
    SetDlgItemText(IDC_BTN_SEND, _T("发送 TCP"));
    SetDlgItemText(IDC_LABEL_STATUS, _T("TCP 连接/收发状态："));
    SetDlgItemText(IDC_LABEL_RESPONSE, _T("响应（合法 JSON 以 2 空格缩进显示）："));
    SetDlgItemText(IDC_LABEL_MESSAGE, _T("消息提示："));

    // 限制单行输入框的最大长度；四个任务字段保留很大上限，
    // 使超长粘贴仍能进入 JSON 后被 1 MiB 规则明确拒绝，而不是被控件静默截断。
    GetDlgItem(IDC_EDIT_HOST)->SendMessage(EM_SETLIMITTEXT, 255, 0);
    GetDlgItem(IDC_EDIT_PORT)->SendMessage(EM_SETLIMITTEXT, 5, 0);
    const int requestEditIds[] = {
        IDC_EDIT_TASK_ID, IDC_EDIT_AGV_ID, IDC_EDIT_TASK_TYPE, IDC_EDIT_ROUTE_ID
    };
    for (int id : requestEditIds)
        GetDlgItem(id)->SendMessage(EM_SETLIMITTEXT, 0x7FFEFC00, 0);

    // 所有输入框均由 .rc 保持空值，不预填示例。
    SetDlgItemText(IDC_EDIT_STATUS, _T("未连接。"));
    ShowMessage(
        _T("就绪。请填写 TCP 服务端地址、端口和四个任务字段，然后点击“发送 TCP”。")
        _T("每次请求使用一次 TCP 连接，请求末尾自动追加 CRLF。"));
    return TRUE;
}

// 请求期间禁用六个输入框和发送按钮，阻止重复提交；
// 请求结束后恢复。响应区和消息区始终只读，不在此处切换。
void CTcpClientDlg::SetUiBusy(bool busy)
{
    m_busy = busy;
    const int inputIds[] = {
        IDC_EDIT_HOST, IDC_EDIT_PORT, IDC_EDIT_TASK_ID, IDC_EDIT_AGV_ID,
        IDC_EDIT_TASK_TYPE, IDC_EDIT_ROUTE_ID, IDC_BTN_SEND
    };
    for (int id : inputIds)
        GetDlgItem(id)->EnableWindow(!busy);
}

void CTcpClientDlg::ShowMessage(const CString& text)
{
    SetDlgItemText(IDC_EDIT_MESSAGE, text);
}

void CTcpClientDlg::OnBnClickedSend()
{
    // m_busy 是第二道防重复提交保护；正常情况下按钮已被禁用。
    if (m_busy)
        return;

    // ---- 1. 校验 TCP 服务端地址和端口 ----
    CString hostText;
    CString portText;
    GetDlgItemText(IDC_EDIT_HOST, hostText);
    GetDlgItemText(IDC_EDIT_PORT, portText);
    hostText.Trim();
    if (hostText.IsEmpty())
    {
        ShowMessage(_T("错误：TCP 服务端 IP 或主机名不能为空，未建立连接。"));
        return;
    }

    std::uint16_t port = 0;
    if (!TryParsePort(portText, port))
    {
        ShowMessage(_T("错误：端口必须是 1 到 65535 的整数，未建立连接。"));
        return;
    }

    // ---- 2. 读取四个任务字段的原始值，编码为 UTF-8 ----
    // 不做 Trim，保证通过校验后发送的是用户原始输入。
    CString taskIdText;
    CString agvIdText;
    CString taskTypeText;
    CString routeIdText;
    GetDlgItemText(IDC_EDIT_TASK_ID, taskIdText);
    GetDlgItemText(IDC_EDIT_AGV_ID, agvIdText);
    GetDlgItemText(IDC_EDIT_TASK_TYPE, taskTypeText);
    GetDlgItemText(IDC_EDIT_ROUTE_ID, routeIdText);

    const std::string taskIdUtf8 = CStringToUtf8(taskIdText);
    const std::string agvIdUtf8 = CStringToUtf8(agvIdText);
    const std::string taskTypeUtf8 = CStringToUtf8(taskTypeText);
    const std::string routeIdUtf8 = CStringToUtf8(routeIdText);

    // ---- 3. 由 nlohmann::json 序列化，自动转义引号、反斜杠和中文 ----
    std::string bodyUtf8;
    try
    {
        nlohmann::json requestJson = nlohmann::json::object();
        requestJson["taskId"] = taskIdUtf8;
        requestJson["agvId"] = agvIdUtf8;
        requestJson["taskType"] = taskTypeUtf8;
        requestJson["routeId"] = routeIdUtf8;
        bodyUtf8 = requestJson.dump();
    }
    catch (const std::exception& error)
    {
        ShowMessage(_T("请求 JSON 构造失败，未发送：") + Utf8ToCString(error.what()));
        return;
    }

    // 大小限制针对 JSON 本身；工作线程稍后追加的 CRLF 不计入。
    if (bodyUtf8.size() > kMaxRequestBytes)
    {
        CString message;
        message.Format(
            _T("错误：UTF-8 请求 JSON 为 %llu 字节，超过 1 MiB（%llu 字节），未发送。"),
            static_cast<unsigned long long>(bodyUtf8.size()),
            static_cast<unsigned long long>(kMaxRequestBytes));
        ShowMessage(message);
        return;
    }

    // ---- 4. 四个字段去首尾空白后不能为空 ----
    CString blankFields;
    AppendBlankField(blankFields, "taskId", taskIdUtf8);
    AppendBlankField(blankFields, "agvId", agvIdUtf8);
    AppendBlankField(blankFields, "taskType", taskTypeUtf8);
    AppendBlankField(blankFields, "routeId", routeIdUtf8);
    if (!blankFields.IsEmpty())
    {
        ShowMessage(_T("请求参数不能为空：") + blankFields + _T(" 去首尾空白后为空，未发送。"));
        return;
    }

    // ---- 5. 保存本次实际发送值的快照，供响应回显比对 ----
    // 快照保存原始值；比较时才忽略两端 ASCII 空白。
    m_lastRequest.taskId = taskIdUtf8;
    m_lastRequest.agvId = agvIdUtf8;
    m_lastRequest.routeId = routeIdUtf8;

    TcpRequestParams params;
    params.hostUtf8 = CStringToUtf8(hostText);
    params.port = port;
    params.bodyUtf8 = bodyUtf8;

    // ---- 6. 进入忙碌状态并启动工作线程 ----
    SetUiBusy(true);
    SetDlgItemText(IDC_EDIT_STATUS, _T("正在后台线程连接并收发 TCP 数据，请稍候…"));
    SetDlgItemText(IDC_EDIT_RESPONSE, _T(""));
    ShowMessage(_T("正在发送单行 UTF-8 JSON（末尾 CRLF），并等待 LF 结束的完整响应行…"));

    try
    {
        StartTcpRequestAsync(params, GetSafeHwnd());
    }
    catch (const std::exception& error)
    {
        // 线程创建失败时必须恢复控件，否则界面会永久停留在忙碌状态。
        SetUiBusy(false);
        SetDlgItemText(IDC_EDIT_STATUS, _T("TCP 工作线程启动失败。"));
        ShowMessage(_T("无法启动 TCP 工作线程：") + Utf8ToCString(error.what()));
    }
}

LRESULT CTcpClientDlg::OnTcpRequestDone(WPARAM, LPARAM lParam)
{
    // 接管工作线程 new 出来的结果对象，函数返回时自动释放。
    const std::unique_ptr<TcpRequestResult> result(
        reinterpret_cast<TcpRequestResult*>(lParam));

    SetUiBusy(false);
    ShowResponseResult(*result);
    return 0;
}

void CTcpClientDlg::ShowResponseResult(const TcpRequestResult& result)
{
    // 状态区始终展示 TCP 层结果；后续业务警告不得覆盖它。
    SetDlgItemText(IDC_EDIT_STATUS, Utf8ToCString(result.statusUtf8));

    // ---- 情况 1：解析、连接、发送、接收等 TCP 层失败 ----
    if (!result.transportOk)
    {
        SetDlgItemText(IDC_EDIT_RESPONSE, _T(""));
        ShowMessage(Utf8ToCString(result.errorUtf8));
        return;
    }

    // ---- 情况 2：已收到一行响应，优先按 JSON 解析 ----
    nlohmann::json parsed;
    try
    {
        // dump(2) 使用 2 空格缩进；写入多行编辑框前把 LF 转成 CRLF。
        parsed = nlohmann::json::parse(result.responseUtf8);
        SetDlgItemText(
            IDC_EDIT_RESPONSE,
            Utf8ToCString(LfToCrLf(parsed.dump(2))));
    }
    catch (const nlohmann::json::parse_error& error)
    {
        // 非法 JSON 保留原文展示，但 TCP 收发状态不能被改写。
        SetDlgItemText(
            IDC_EDIT_RESPONSE,
            Utf8ToCString(LfToCrLf(result.responseUtf8)));
        ShowMessage(
            _T("TCP 收发成功，但响应不是合法 JSON，已显示原始响应文本。解析错误：") +
            Utf8ToCString(error.what()));
        return;
    }

    // ---- 情况 3：合法 JSON，执行 code/data/taskStatus/回显业务校验 ----
    // 业务警告只写消息区，响应框和 TCP 状态保持原样。
    ShowMessage(DescribeBusinessResult(parsed, m_lastRequest));
}

// Enter 可能触发 OnOK；请求进行中同样需要阻止窗口关闭。
void CTcpClientDlg::OnOK()
{
    if (m_busy)
    {
        ShowCloseBlockedMessage();
        return;
    }
    CDialogEx::OnOK();
}

// Esc 或标题栏关闭最终可能进入 OnCancel。
void CTcpClientDlg::OnCancel()
{
    if (m_busy)
    {
        ShowCloseBlockedMessage();
        return;
    }
    CDialogEx::OnCancel();
}

// 显式处理 WM_CLOSE（标题栏 X、Alt+F4），确保忙碌时不会绕过 OnCancel。
void CTcpClientDlg::OnClose()
{
    if (m_busy)
    {
        ShowCloseBlockedMessage();
        return;
    }
    CDialogEx::OnClose();
}

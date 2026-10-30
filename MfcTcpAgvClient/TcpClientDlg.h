#pragma once
// TcpClientDlg.h : 主对话框类声明。
//
// 对话框负责所有 UI 工作：读取输入、校验、构建请求 JSON、保存请求快照、
// 禁用控件和展示工作线程结果。网络 I/O 全部委托给 TcpWorker。
//
// 结果回传链：
//   StartTcpRequestAsync() -> 工作线程 -> WM_TCP_REQUEST_DONE -> OnTcpRequestDone()

#include <afxwin.h>
#include <afxdialogex.h>

#include <string>

#include "TcpWorker.h"
#include "resource.h"

// 本次请求中响应会回显的三个字段快照；仅 UI 线程读写。
// 保存的是用户原始输入（未裁剪），比较时再由业务校验临时去 ASCII 空白。
struct TcpRequestSnapshot
{
    std::string taskId;
    std::string agvId;
    std::string routeId;
};

class CTcpClientDlg : public CDialogEx
{
public:
    explicit CTcpClientDlg(CWnd* pParent = nullptr);

    enum { IDD = IDD_MAIN };

protected:
    BOOL OnInitDialog() override;

    // 三种关闭路径都要检查忙碌状态，防止工作线程回传时目标窗口已销毁。
    void OnOK() override;
    void OnCancel() override;

    // 标题栏关闭 / Alt+F4
    afx_msg void OnClose();

    // “发送 TCP”按钮：在 UI 线程校验和组装请求，再启动后台线程。
    afx_msg void OnBnClickedSend();

    // 工作线程回传结果（WM_TCP_REQUEST_DONE，lParam = TcpRequestResult*）。
    afx_msg LRESULT OnTcpRequestDone(WPARAM wParam, LPARAM lParam);
    DECLARE_MESSAGE_MAP()

private:
    // 请求进行中标志：禁用按钮和输入框、阻止重复发送和关闭窗口。
    bool m_busy = false;

    // 最近一次实际发送 JSON 的 taskId/agvId/routeId 快照，用于响应回显比对。
    TcpRequestSnapshot m_lastRequest;

    // 根据忙碌状态统一启用/禁用发送按钮及六个输入框。
    void SetUiBusy(bool busy);

    // 把文本写入底部消息提示区。
    void ShowMessage(const CString& text);

    // 根据工作线程结果更新 TCP 状态、响应区和业务消息。
    void ShowResponseResult(const TcpRequestResult& result);
};

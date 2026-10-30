// MfcTcpAgvClientApp.cpp : 应用程序入口。
// 标准 MFC 对话框程序骨架：InitInstance 中弹出主对话框，关闭即退出。

#include "MfcTcpAgvClientApp.h"

#include <commctrl.h>

#include "TcpClientDlg.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

// 唯一的 App 对象。
CMfcTcpAgvClientApp theApp;

BEGIN_MESSAGE_MAP(CMfcTcpAgvClientApp, CWinApp)
END_MESSAGE_MAP()

BOOL CMfcTcpAgvClientApp::InitInstance()
{
    // 初始化通用控件（多行编辑框的滚动条等依赖它）。
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_WIN95_CLASSES;
    InitCommonControlsEx(&controls);

    CWinApp::InitInstance();

    // 模态主对话框：程序的生命周期 = 对话框的生命周期。
    CTcpClientDlg dlg;
    m_pMainWnd = &dlg;
    static_cast<void>(dlg.DoModal());

    // 对话框已关闭，返回 FALSE 让程序退出。
    return FALSE;
}

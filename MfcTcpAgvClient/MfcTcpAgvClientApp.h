#pragma once
// MfcTcpAgvClientApp.h : 应用程序类声明。

#include <afxwin.h>

class CMfcTcpAgvClientApp : public CWinApp
{
public:
    // 创建并运行模态主对话框；对话框关闭后程序退出。
    BOOL InitInstance() override;

private:
    DECLARE_MESSAGE_MAP()
};

// 全局唯一的 MFC 应用对象。
extern CMfcTcpAgvClientApp theApp;

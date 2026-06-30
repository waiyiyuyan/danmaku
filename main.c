#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <strsafe.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ws2_32.lib")

#define PORT 12345
#define TRANSPARENT_COLOR 0x00FF00FF
#define TIMER_ID 1
// #define IDI_MAIN_ICON 1
#define IDI_MAIN_ICON 100
// 控件 ID 定义
#define IDC_EDIT_INPUT     1000
#define IDOK                1
#define IDC_COMBO_IP       1005  // IP下拉选择框ID
#define ID_BTN_REFRESH     1006  // 弹窗内搜寻节点按钮ID
#define IDC_COMBO_COLOR    1007  // 💡 控制台颜色下拉框ID
#define IDC_COMBO_SIZE     1008  // 💡 控制台大小下拉框ID
#define IDC_COMBO_SPEED    1009  // 💡 控制台速度下拉框ID

#define ID_SEND_DANMAKU    1001
#define ID_EXIT_APP        1002

#define WM_USER_CREATE_DANMAKU (WM_USER + 2)

// ==========================================
// 💡 局域网对等体点名册数据结构
// ==========================================
#define MAX_PEERS 200
struct sockaddr_in g_onlineUsers[MAX_PEERS];
int g_userCount = 0;
CRITICAL_SECTION g_peerListCS; 

// 💡 弹幕节点结构体升级：支持单条弹幕独立拥有特定属性
typedef struct DanmakuData {
    HWND hWnd;
    char text[256];
    COLORREF color; // 💡 独立颜色
    int size;       // 💡 独立大小
    int speed;      // 💡 独立速度
    struct DanmakuData* next;
} DanmakuData;

DanmakuData* g_danmakuList = NULL;
HINSTANCE g_hInst;
NOTIFYICONDATAW g_nid;
HWND g_hwndTray;
HMENU g_hMenu;
HICON g_hTrayIcon;

// 目标发送 IP 缓存变量
struct sockaddr_in g_targetAddr;
BOOL g_isBroadcastMode = TRUE; 

// 💡 临时解析缓存（供新窗体创建瞬间抓取属性）
COLORREF g_parsedColor = RGB(255, 255, 0);
int g_parsedSize = 32;
int g_parsedSpeed = 4;

LRESULT CALLBACK DanmakuWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK TrayWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK InputDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);
void CreateDanmakuWindow(const char* text, COLORREF col, int sz, int spd);
void AddDanmakuNode(HWND hWnd, const char* text, COLORREF col, int sz, int spd);
void RemoveDanmakuNode(HWND hWnd);
void InitTrayIcon();
BOOL ShowInputDialog(char* outBuf, int bufLen);
void SendDanmakuToPeers(const char* text); 
void BroadcastPing();                               
void AddUserIfNew(struct sockaddr_in* addr); 
void CenterWindow(HWND hWnd);
void GetLocalIP(char* ipOut, int bufSize);
void RefreshIPComboBox(HWND hCombo); 

DWORD WINAPI UdpRecvThread(LPVOID param);

#define DANMAKU_CLASS L"DanmakuWindowClass"
#define INPUT_DLG_CLASS L"InputDlgClass"

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPSTR cmd, int show)
{
    g_hInst = hInstance;
    srand(GetTickCount());

    InitializeCriticalSection(&g_peerListCS);

    g_hTrayIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_MAIN_ICON));
	if (g_hTrayIcon == NULL) {
		DWORD err = GetLastError();
		WCHAR msg[256];
		StringCchPrintfW(msg, ARRAYSIZE(msg), L"图标加载失败，错误码：%d", err);
		MessageBoxW(NULL, msg, L"资源错误", MB_ICONERROR);
		g_hTrayIcon = LoadIconA(NULL, IDI_APPLICATION);
	}

    WNDCLASSEXW wc = {0};
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc   = DanmakuWndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = DANMAKU_CLASS;
    RegisterClassExW(&wc);

    WNDCLASSEXW dlgWc = {0};
    dlgWc.cbSize        = sizeof(WNDCLASSEXW);
    dlgWc.lpfnWndProc   = InputDlgProc;
    dlgWc.hInstance     = hInstance;
    dlgWc.lpszClassName = INPUT_DLG_CLASS;
    dlgWc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    dlgWc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassExW(&dlgWc);

    WNDCLASSEXA trayWc = {0};
    trayWc.cbSize        = sizeof(WNDCLASSEXA);
    trayWc.lpfnWndProc   = TrayWndProc;
    trayWc.hInstance     = hInstance;
    trayWc.lpszClassName = "TrayWndClass";
    RegisterClassExA(&trayWc);

    g_hwndTray = CreateWindowExA(0, "TrayWndClass", "Tray", 0,
        0,0,0,0, HWND_MESSAGE, NULL, hInstance, NULL);
    InitTrayIcon();

    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);

    BroadcastPing();

    CreateThread(NULL, 0, UdpRecvThread, NULL, 0, NULL);

    MSG msg;
    while(GetMessageW(&msg, NULL, 0, 0)){
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    WSACleanup();
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if(g_hTrayIcon) DestroyIcon(g_hTrayIcon);
    DestroyMenu(g_hMenu);
    
    DeleteCriticalSection(&g_peerListCS);
    return 0;
}

LRESULT CALLBACK TrayWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch(msg)
    {
        case WM_CREATE:
            g_hMenu = CreatePopupMenu();
            AppendMenuW(g_hMenu, MF_STRING, ID_SEND_DANMAKU, L"发送弹幕控制台");
            AppendMenuW(g_hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(g_hMenu, MF_STRING, ID_EXIT_APP, L"退出");
            break;

        case WM_COMMAND:
        {
            int id = LOWORD(wParam);
            if(id == ID_SEND_DANMAKU){
                char contentBuf[256] = {0};
                // 💡 因为现在是窗口常驻模式，发射数据移到了窗口内部逻辑，所以外面这里不需要处理返回值了
                ShowInputDialog(contentBuf, 256);
            }
            else if(id == ID_EXIT_APP){
                PostQuitMessage(0);
            }
            break;
        }

        case WM_USER+1:
            if(lParam == WM_LBUTTONUP || lParam == WM_RBUTTONUP){
                POINT pt;
                GetCursorPos(&pt);
                SetForegroundWindow(hWnd);
                TrackPopupMenu(g_hMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hWnd, NULL);
                PostMessage(hWnd, WM_NULL,0,0);
            }
            break;

        case WM_USER_CREATE_DANMAKU:
        {
            char* text = (char*)lParam;
            if(text){
                CreateDanmakuWindow(text, g_parsedColor, g_parsedSize, g_parsedSpeed);
                free(text);
            }
            break;
        }
    }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

void InitTrayIcon()
{
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = g_hwndTray;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_TIP | NIF_ICON;
    g_nid.uCallbackMessage = WM_USER+1;
    g_nid.hIcon = g_hTrayIcon;
    StringCchCopyW(g_nid.szTip, ARRAYSIZE(g_nid.szTip), L"局域网P2P弹幕工具");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

void RefreshIPComboBox(HWND hCombo)
{
    SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"📢 列表内所有在线IP群发 (默认)");

    char localIP[64] = {0};
    GetLocalIP(localIP, 64);

    EnterCriticalSection(&g_peerListCS);
    for (int i = 0; i < g_userCount; i++) {
        char ipStr[32] = {0};
        StringCchPrintfA(ipStr, sizeof(ipStr), "%s", inet_ntoa(g_onlineUsers[i].sin_addr));
        
        WCHAR wIpStr[64];
        MultiByteToWideChar(CP_UTF8, 0, ipStr, -1, wIpStr, 64);
        
        WCHAR listItem[128];
        if (strcmp(ipStr, localIP) == 0) {
            StringCchPrintfW(listItem, 128, L"🖥️ 局域网节点 [%s] (本机)", wIpStr);
        } else {
            StringCchPrintfW(listItem, 128, L"💻 局域网节点 [%s]", wIpStr);
        }
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)listItem);
    }
    LeaveCriticalSection(&g_peerListCS);
    SendMessageW(hCombo, CB_SETCURSEL, 0, 0); 
}

static char* s_contentBuf = NULL;
static int s_contentMax = 0;
static BOOL s_submitOk = FALSE;

// 样式全局中转
static COLORREF s_selectedColor = RGB(255, 255, 0);
static int s_selectedSize = 32;
static int s_selectedSpeed = 4;

BOOL ShowInputDialog(char* outContent, int contentMaxLen)
{
    s_contentBuf = outContent;
    s_contentMax = contentMaxLen;
    s_submitOk = FALSE;

    // 💡 样式调整：去掉 WS_EX_DLGMODALFRAME，加入标准重叠窗口属性，使右上角能正常展示「最小化」和「关闭」
    HWND hDlg = CreateWindowExW(
        WS_EX_TOPMOST,
        INPUT_DLG_CLASS,
        L"🚀 局域网弹幕控制台",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, // 💡 注入 WS_MINIMIZEBOX 支持最小化
        0, 0, 510, 212, 
        NULL, NULL, g_hInst, NULL
    );

    CenterWindow(hDlg);
    ShowWindow(hDlg, SW_SHOW);
    
    HFONT hSysFont = GetStockObject(DEFAULT_GUI_FONT);
    SendMessageW(hDlg, WM_SETFONT, (WPARAM)hSysFont, MAKELPARAM(TRUE,0));
    UpdateWindow(hDlg);

    MSG msg;
    while(GetMessageW(&msg, NULL, 0, 0)){
        if(!IsWindow(hDlg)) break;
        
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            HWND hEdit = GetDlgItem(hDlg, IDC_EDIT_INPUT);
            if (msg.hwnd == hEdit) {
                SendMessageW(hDlg, WM_COMMAND, IDOK, 0);
                continue; 
            }
        }

        if(!IsDialogMessageW(hDlg, &msg)){
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return s_submitOk;
}

LRESULT CALLBACK InputDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    static HFONT s_hDlgFont = NULL;
    static HWND hComboIP = NULL;
    static HWND hComboColor = NULL, hComboSize = NULL, hComboSpeed = NULL;

    switch(msg)
    {
        case WM_CREATE:
        {
            s_hDlgFont = CreateFontW(18, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, FF_DONTCARE, L"Microsoft YaHei");

            // --- 第一行：发送目标选择 ---
            HWND hStaticTarget = CreateWindowW(L"STATIC", L"发送目标：",
                WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 15, 80, 20, hDlg, NULL, g_hInst, NULL);

            hComboIP = CreateWindowW(L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                100, 12, 280, 200, hDlg, (HMENU)IDC_COMBO_IP, g_hInst, NULL);

            HWND hBtnRefresh = CreateWindowW(L"BUTTON", L"🔍 搜寻节点",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                395, 10, 90, 28, hDlg, (HMENU)ID_BTN_REFRESH, g_hInst, NULL);

            // --- 第二行：三大样式下拉框 ---
            HWND hStaticStyle = CreateWindowW(L"STATIC", L"弹幕样式：",
                WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 58, 80, 20, hDlg, NULL, g_hInst, NULL);

            hComboColor = CreateWindowW(L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                100, 55, 110, 200, hDlg, (HMENU)IDC_COMBO_COLOR, g_hInst, NULL);
            SendMessageW(hComboColor, CB_ADDSTRING, 0, (LPARAM)L"亮黄色(默认)");
            SendMessageW(hComboColor, CB_ADDSTRING, 0, (LPARAM)L"大红色");
            SendMessageW(hComboColor, CB_ADDSTRING, 0, (LPARAM)L"橙橙色");
            SendMessageW(hComboColor, CB_ADDSTRING, 0, (LPARAM)L"鲜绿色");
            SendMessageW(hComboColor, CB_ADDSTRING, 0, (LPARAM)L"青蓝色");
            SendMessageW(hComboColor, CB_ADDSTRING, 0, (LPARAM)L"深蓝色");
            SendMessageW(hComboColor, CB_ADDSTRING, 0, (LPARAM)L"紫罗兰");
            SendMessageW(hComboColor, CB_SETCURSEL, 0, 0);

            hComboSize = CreateWindowW(L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                220, 55, 110, 200, hDlg, (HMENU)IDC_COMBO_SIZE, g_hInst, NULL);
            SendMessageW(hComboSize, CB_ADDSTRING, 0, (LPARAM)L"中字 32号");
            SendMessageW(hComboSize, CB_ADDSTRING, 0, (LPARAM)L"小字 24号");
            SendMessageW(hComboSize, CB_ADDSTRING, 0, (LPARAM)L"大字 40号");
            SendMessageW(hComboSize, CB_ADDSTRING, 0, (LPARAM)L"巨字 48号");
            SendMessageW(hComboSize, CB_SETCURSEL, 0, 0);

            hComboSpeed = CreateWindowW(L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                340, 55, 110, 200, hDlg, (HMENU)IDC_COMBO_SPEED, g_hInst, NULL);
            SendMessageW(hComboSpeed, CB_ADDSTRING, 0, (LPARAM)L"中速 4px");
            SendMessageW(hComboSpeed, CB_ADDSTRING, 0, (LPARAM)L"慢速 2px");
            SendMessageW(hComboSpeed, CB_ADDSTRING, 0, (LPARAM)L"快速 6px");
            SendMessageW(hComboSpeed, CB_ADDSTRING, 0, (LPARAM)L"极速 9px");
            SendMessageW(hComboSpeed, CB_SETCURSEL, 0, 0);

            // --- 第三行：内容与发送 ---
            HWND hStaticMsg = CreateWindowW(L"STATIC", L"弹幕内容：",
                WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 103, 80, 20, hDlg, NULL, g_hInst, NULL);

            HWND hEdit = CreateWindowW(L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 
                20, 130, 360, 32, hDlg, (HMENU)IDC_EDIT_INPUT, g_hInst, NULL);

            HWND hBtnSend = CreateWindowW(L"BUTTON", L"发射🚀",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                395, 130, 90, 32, hDlg, (HMENU)IDOK, g_hInst, NULL);

            if (s_hDlgFont) {
                SendMessageW(hStaticTarget, WM_SETFONT, (WPARAM)s_hDlgFont, TRUE);
                SendMessageW(hComboIP, WM_SETFONT, (WPARAM)s_hDlgFont, TRUE);
                SendMessageW(hBtnRefresh, WM_SETFONT, (WPARAM)s_hDlgFont, TRUE);
                SendMessageW(hStaticStyle, WM_SETFONT, (WPARAM)s_hDlgFont, TRUE);
                SendMessageW(hComboColor, WM_SETFONT, (WPARAM)s_hDlgFont, TRUE);
                SendMessageW(hComboSize, WM_SETFONT, (WPARAM)s_hDlgFont, TRUE);
                SendMessageW(hComboSpeed, WM_SETFONT, (WPARAM)s_hDlgFont, TRUE);
                SendMessageW(hStaticMsg, WM_SETFONT, (WPARAM)s_hDlgFont, TRUE);
                SendMessageW(hEdit, WM_SETFONT, (WPARAM)s_hDlgFont, TRUE);
                SendMessageW(hBtnSend, WM_SETFONT, (WPARAM)s_hDlgFont, TRUE);
            } 

            RefreshIPComboBox(hComboIP);
            SetFocus(hEdit);
            break;
        }

        case WM_COMMAND:
        {
            int id = LOWORD(wParam);
            
            if (id == ID_BTN_REFRESH) {
                BroadcastPing(); 
                Sleep(200);      
                RefreshIPComboBox(hComboIP); 
            }
            else if (id == IDOK) 
            {
                // 1. 读取目标网卡路由项
                int selIdx = (int)SendMessageW(hComboIP, CB_GETCURSEL, 0, 0);
                if (selIdx == 0 || selIdx == CB_ERR) {
                    g_isBroadcastMode = TRUE;
                } else {
                    g_isBroadcastMode = FALSE;
                    EnterCriticalSection(&g_peerListCS);
                    g_targetAddr = g_onlineUsers[selIdx - 1]; 
                    LeaveCriticalSection(&g_peerListCS);
                }

                // 2. 读取控制台当前选定的颜色值
                int colorSel = (int)SendMessageW(hComboColor, CB_GETCURSEL, 0, 0);
                switch(colorSel){
                    case 0: s_selectedColor = RGB(255, 255, 0);   break; 
                    case 1: s_selectedColor = RGB(255, 0, 0);     break; 
                    case 2: s_selectedColor = RGB(255, 128, 0);   break; 
                    case 3: s_selectedColor = RGB(0, 255, 64);    break; 
                    case 4: s_selectedColor = RGB(0, 255, 255);   break; 
                    case 5: s_selectedColor = RGB(0, 128, 255);   break; 
                    case 6: s_selectedColor = RGB(180, 80, 255);  break; 
                }

                // 3. 读取控制台当前选定的字号大小
                int sizeSel = (int)SendMessageW(hComboSize, CB_GETCURSEL, 0, 0);
                switch(sizeSel){
                    case 0: s_selectedSize = 32; break; 
                    case 1: s_selectedSize = 24; break; 
                    case 2: s_selectedSize = 40; break; 
                    case 3: s_selectedSize = 48; break; 
                }

                // 4. 读取控制台当前选定的滚动速度
                int speedSel = (int)SendMessageW(hComboSpeed, CB_GETCURSEL, 0, 0);
                switch(speedSel){
                    case 0: s_selectedSpeed = 4; break; 
                    case 1: s_selectedSpeed = 2; break; 
                    case 2: s_selectedSpeed = 6; break; 
                    case 3: s_selectedSpeed = 9; break; 
                }

                // 5. 提取并转码文字
                HWND hEdit = GetDlgItem(hDlg, IDC_EDIT_INPUT);
                WCHAR wBuf[256] = {0};
                GetWindowTextW(hEdit, wBuf, ARRAYSIZE(wBuf));
                
                // 💡 防止发空弹幕
                if (lstrlenW(wBuf) > 0) {
                    WideCharToMultiByte(CP_UTF8, 0, wBuf, -1, s_contentBuf, s_contentMax, NULL, NULL);
                    
                    // 💡 【核心修改点】不再执行销毁，直接在窗口内把网络UDP数据包发射给局域网对等体！
                    SendDanmakuToPeers(s_contentBuf);
                    
                    // 💡 发射完毕后，立刻清空编辑框输入内容，方便连续编写下一条
                    SetWindowTextW(hEdit, L"");
                }
                
                // 💡 重新将光标焦点锁回输入框，无需手动再用鼠标去点
                SetFocus(hEdit);
            }
            break;
        }

        case WM_CLOSE:
            // 💡 「X」关闭动作原封不动：依然直接销毁，退出对话框消息循环
            DestroyWindow(hDlg);
            break;

        case WM_DESTROY:
            if (s_hDlgFont) {
                DeleteObject(s_hDlgFont);
                s_hDlgFont = NULL;
            }
            break;
    }
    return DefWindowProcW(hDlg, msg, wParam, lParam);
}

void CenterWindow(HWND hWnd)
{
    RECT rc;
    GetWindowRect(hWnd, &rc);
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int x = (sw - (rc.right - rc.left)) / 2;
    int y = (sh - (rc.bottom - rc.top)) / 2;
    MoveWindow(hWnd, x, y, rc.right - rc.left, rc.bottom - rc.top, TRUE);
}

void GetLocalIP(char* ipOut, int bufSize)
{
    ZeroMemory(ipOut, bufSize);
    char hostName[128] = {0};
    gethostname(hostName, sizeof(hostName));
    struct hostent* host = gethostbyname(hostName);
    if(!host) return;
    for(int i = 0; host->h_addr_list[i]; i++){
        char* ip = inet_ntoa(*(struct in_addr*)host->h_addr_list[i]);
        if(strcmp(ip, "127.0.0.1") != 0){
            StringCchCopyA(ipOut, bufSize, ip);
            return;
        }
    }
    StringCchCopyA(ipOut, bufSize, "127.0.0.1");
}

void AddUserIfNew(struct sockaddr_in* addr)
{
    EnterCriticalSection(&g_peerListCS);
    for (int i = 0; i < g_userCount; i++) {
        if (g_onlineUsers[i].sin_addr.s_addr == addr->sin_addr.s_addr) {
            LeaveCriticalSection(&g_peerListCS);
            return;
        }
    }
    if (g_userCount < MAX_PEERS) {
        g_onlineUsers[g_userCount] = *addr;
        g_onlineUsers[g_userCount].sin_port = htons(PORT); 
        g_userCount++;
    }
    LeaveCriticalSection(&g_peerListCS);
}

void BroadcastPing()
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if(s == INVALID_SOCKET) return;
    BOOL opt = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (char*)&opt, sizeof(opt));
    
    struct sockaddr_in broadcastAddr = {0};
    broadcastAddr.sin_family = AF_INET;
    broadcastAddr.sin_port = htons(PORT);
    broadcastAddr.sin_addr.s_addr = INADDR_BROADCAST;
    
    const char* pingMsg = "_DANMAKU_PING_";
    sendto(s, pingMsg, (int)strlen(pingMsg), 0, (const struct sockaddr*)&broadcastAddr, sizeof(broadcastAddr));
    closesocket(s);
}

void SendDanmakuToPeers(const char* plainText)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if(s == INVALID_SOCKET) return;

    char localIP[64] = {0};
    GetLocalIP(localIP, 64);

    char protocolPayload[1024] = {0};
    StringCchPrintfA(protocolPayload, sizeof(protocolPayload), 
                 "_STYLE_MSG_|%06X|%d|%d|%s", 
                 s_selectedColor, s_selectedSize, s_selectedSpeed, plainText);

    if (!g_isBroadcastMode) {
        sendto(s, protocolPayload, (int)strlen(protocolPayload), 0, (const struct sockaddr*)&g_targetAddr, sizeof(g_targetAddr));
    } 
    else {
        EnterCriticalSection(&g_peerListCS);
        if (g_userCount == 0) {
            LeaveCriticalSection(&g_peerListCS);
            
            SOCKET sB = socket(AF_INET, SOCK_DGRAM, 0);
            BOOL opt = TRUE;
            setsockopt(sB, SOL_SOCKET, SO_BROADCAST, (char*)&opt, sizeof(opt));
            struct sockaddr_in bAddr = {0};
            bAddr.sin_family = AF_INET;
            bAddr.sin_port = htons(PORT);
            bAddr.sin_addr.s_addr = INADDR_BROADCAST;
            
            sendto(sB, protocolPayload, (int)strlen(protocolPayload), 0, (const struct sockaddr*)&bAddr, sizeof(bAddr));
            const char* pingMsg = "_DANMAKU_PING_";
            sendto(sB, pingMsg, (int)strlen(pingMsg), 0, (const struct sockaddr*)&bAddr, sizeof(bAddr));
            closesocket(sB);
        } 
        else {
            for (int i = 0; i < g_userCount; i++) {
                sendto(s, protocolPayload, (int)strlen(protocolPayload), 0, (const struct sockaddr*)&g_onlineUsers[i], sizeof(g_onlineUsers[i]));
            }
            LeaveCriticalSection(&g_peerListCS);
        }
    }
    closesocket(s);
}

DWORD WINAPI UdpRecvThread(LPVOID param)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    BOOL opt = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    bind(s, (const struct sockaddr*)&addr, sizeof(addr));
    
    char buf[1024];
    struct sockaddr_in remote;
    int remoteLen;
    
    while(1)
    {
        ZeroMemory(buf, sizeof(buf));
        remoteLen = sizeof(remote);
        
        int n = recvfrom(s, buf, sizeof(buf)-1, 0, (struct sockaddr*)&remote, &remoteLen);
        if(n <= 0) continue;
        buf[n] = '\0';

        if (strcmp(buf, "_DANMAKU_PING_") == 0) {
            AddUserIfNew(&remote); 
            
            SOCKET sReply = socket(AF_INET, SOCK_DGRAM, 0);
            if (sReply != INVALID_SOCKET) {
                const char* pongMsg = "_DANMAKU_PONG_";
                sendto(sReply, pongMsg, (int)strlen(pongMsg), 0, (const struct sockaddr*)&remote, sizeof(remote));
                closesocket(sReply);
            }
            continue; 
        }
        else if (strcmp(buf, "_DANMAKU_PONG_") == 0) {
            AddUserIfNew(&remote); 
            continue; 
        }
        else if (strncmp(buf, "_STYLE_MSG_|", 12) == 0) {
			AddUserIfNew(&remote);

			char hexColor[16] = {0};
			int rSize = 32, rSpeed = 4;
			char realText[512] = {0};

			char* ctx = NULL;
			char* token = strtok_s(buf, "|", &ctx); 
			
			if (ctx) token = strtok_s(NULL, "|", &ctx); 
			if (token) StringCchCopyA(hexColor, sizeof(hexColor), token);
			
			if (ctx) token = strtok_s(NULL, "|", &ctx); 
			if (token) rSize = atoi(token);
			
			if (ctx) token = strtok_s(NULL, "|", &ctx); 
			if (token) rSpeed = atoi(token);
			
			if (ctx) StringCchCopyA(realText, sizeof(realText), ctx);

			unsigned int hexVal = 0;
			sscanf_s(hexColor, "%X", &hexVal);
			g_parsedColor = (COLORREF)hexVal;
			g_parsedSize = rSize;
			g_parsedSpeed = rSpeed;

			// ===== 新增：从remote获取真实发送IP，拼接文本 =====
			char srcIpBuf[32] = {0};
			strcpy(srcIpBuf, inet_ntoa(remote.sin_addr));
			char finalShowText[600] = {0};
			StringCchPrintfA(finalShowText, ARRAYSIZE(finalShowText), "%s: %s", srcIpBuf, realText);

			char* textCopy = malloc(strlen(finalShowText) + 1);
			if (textCopy) {
				strcpy(textCopy, finalShowText);
				PostMessageA(g_hwndTray, WM_USER_CREATE_DANMAKU, 0, (LPARAM)textCopy);
			}
		}
        else {
            AddUserIfNew(&remote); 
            g_parsedColor = RGB(255, 255, 0); 
            g_parsedSize = 32;
            g_parsedSpeed = 4;

            char* textCopy = malloc(strlen(buf) + 1);
            if (textCopy) {
                strcpy(textCopy, buf);
                PostMessageA(g_hwndTray, WM_USER_CREATE_DANMAKU, 0, (LPARAM)textCopy);
            }
        }
    }
    closesocket(s);
    return 0;
}

void CreateDanmakuWindow(const char* text, COLORREF col, int sz, int spd)
{
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int textLen = MultiByteToWideChar(CP_UTF8,0,text,-1,NULL,0);
    int winW = textLen * (sz / 2) + 60; 
    if(winW < 200) winW = 200;
    int winH = sz + 20;
    int minY = sh / 10;
    int maxY = sh / 2;
    int randY = minY + (rand() % (maxY - minY));
    DWORD exStyle = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TRANSPARENT
                  | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    HWND hWnd = CreateWindowExW(
        exStyle,
        DANMAKU_CLASS,
        L"",
        WS_POPUP,
        sw, randY, winW, winH,
        NULL, NULL, g_hInst, NULL
    );
    if(hWnd == NULL) return;
    
    AddDanmakuNode(hWnd, text, col, sz, spd);
    
    SetLayeredWindowAttributes(hWnd, TRANSPARENT_COLOR, 0, LWA_COLORKEY);
    ShowWindow(hWnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hWnd);
    SetTimer(hWnd, TIMER_ID, 20, NULL);
}

LRESULT CALLBACK DanmakuWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    DanmakuData* node = NULL;
    int currentSpeed = 4;
    int currentSize = 32;
    COLORREF currentColor = RGB(255, 255, 0);

    for(node = g_danmakuList; node; node = node->next) {
        if(node->hWnd == hWnd){
            currentSpeed = node->speed;
            currentSize = node->size;
            currentColor = node->color;
            break;
        }
    }

    switch(msg)
    {
        case WM_TIMER:
        {
            RECT rc;
            GetWindowRect(hWnd, &rc);
            int w = rc.right - rc.left;
            int newX = rc.left - currentSpeed; 
            if(newX < -w)
            {
                KillTimer(hWnd, TIMER_ID);
                DestroyWindow(hWnd);
                return 0;
            }
            MoveWindow(hWnd, newX, rc.top, w, rc.bottom - rc.top, TRUE);
            break;
        }
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT client;
            GetClientRect(hWnd, &client);
            HBRUSH hBrush = CreateSolidBrush(TRANSPARENT_COLOR);
            FillRect(hdc, &client, hBrush);
            DeleteObject(hBrush);
            
            SetBkMode(hdc, TRANSPARENT);

            HFONT hDrawFont = CreateFontW(
                currentSize, 0, 0, 0, FW_BOLD, 0, 0, 0, 
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                ANTIALIASED_QUALITY, FF_DONTCARE, L"Microsoft YaHei" 
            );
            if (!hDrawFont) {
                hDrawFont = CreateFontW(currentSize,0,0,0,FW_BOLD,0,0,0,
                    DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
                    ANTIALIASED_QUALITY,FF_DONTCARE,L"");
            }
            HFONT oldFont = (HFONT)SelectObject(hdc, hDrawFont);

            char textBuf[256] = {0};
            for(node = g_danmakuList; node; node = node->next) {
                if(node->hWnd == hWnd){
                    StringCchCopyA(textBuf, ARRAYSIZE(textBuf), node->text);
                    break;
                }
            }
            WCHAR wText[256];
            MultiByteToWideChar(CP_UTF8,0,textBuf,-1,wText,ARRAYSIZE(wText));
            int textY = (client.bottom - client.top - currentSize) / 2;
            int len = lstrlenW(wText);
            
            SetTextColor(hdc, RGB(0, 0, 0));
            for (int dx = -2; dx <= 2; dx++) {
                for (int dy = -2; dy <= 2; dy++) {
                    if (dx == 0 && dy == 0) continue;
                    if (abs(dx) == 2 && abs(dy) == 2) continue; 
                    TextOutW(hdc, 15 + dx, textY + dy, wText, len);
                }
            }

            SetTextColor(hdc, currentColor);
            TextOutW(hdc, 15, textY, wText, len);

            SelectObject(hdc, oldFont);
            DeleteObject(hDrawFont);
            EndPaint(hWnd, &ps);
            break;
        }
        case WM_DESTROY:
            RemoveDanmakuNode(hWnd);
            break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void AddDanmakuNode(HWND hWnd, const char* text, COLORREF col, int sz, int spd)
{
    DanmakuData* newNode = malloc(sizeof(DanmakuData));
    if (newNode) {
        ZeroMemory(newNode, sizeof(DanmakuData));
        newNode->hWnd = hWnd;
        StringCchCopyA(newNode->text, ARRAYSIZE(newNode->text), text);
        newNode->color = col; 
        newNode->size = sz;
        newNode->speed = spd;
        newNode->next = g_danmakuList;
        g_danmakuList = newNode;
    }
}

void RemoveDanmakuNode(HWND hWnd)
{
    DanmakuData **p = &g_danmakuList;
    while(*p)
    {
        if((*p)->hWnd == hWnd)
        {
            DanmakuData* del = *p;
            *p = del->next;
            free(del);
            break;
        }
        p = &(*p)->next;
    }
}
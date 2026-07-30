#include "stdafx.h"
#include "TaskbarButtonSpacer.h"
#include <dwmapi.h>
#include <uiautomation.h>
#include <atlbase.h>
#include <initguid.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shobjidl.h>

#pragma comment(lib, "dwmapi.lib")

CTaskbarButtonSpacer* CTaskbarButtonSpacer::m_instance{};


//占位窗口的窗口类名
static const wchar_t* SPACER_WINDOW_CLASS = L"TrafficMonitorTaskbarSpacer";
//占位窗口标题使用的空白字符（盲文空格U+2800，在任务栏按钮标签中显示为空白）
static const wchar_t SPACER_TITLE_CHAR = L'⠀';
#ifndef DWMWA_CLOAK
#define DWMWA_CLOAK 13
#endif

CTaskbarButtonSpacer::~CTaskbarButtonSpacer()
{
    Destroy();
}

CString CTaskbarButtonSpacer::GetSpacerTitle(int index)
{
    //每个占位窗口的标题使用不同数量的空白字符，便于在UI自动化接口中区分每个按钮
    return CString(SPACER_TITLE_CHAR, index + 1);
}

HICON CTaskbarButtonSpacer::CreateTransparentIcon()
{
    //创建一个32x32的完全透明的图标
    const int size = 32;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(BITMAPV5HEADER);
    bi.bV5Width = size;
    bi.bV5Height = size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    HDC hdc = ::GetDC(nullptr);
    void* bits{};
    HBITMAP color_bitmap = CreateDIBSection(hdc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ::ReleaseDC(nullptr, hdc);
    if (color_bitmap == nullptr)
        return nullptr;
    if (bits != nullptr)
    {
        //所有像素都设置为alpha为1的黑色。
        //如果所有像素的alpha都为0，图标会被系统当作没有alpha通道的旧式图标处理，显示为黑色方块；
        //而只把个别像素的alpha设置为非0值，缩放图标时该像素会被放大成一个可见的小点。
        //这里让整个图标都使用一个极小的alpha值（不透明度约0.4%），既能被识别为带alpha通道的图标，
        //又不会显示出任何可见的内容
        BYTE* pixel = static_cast<BYTE*>(bits);
        for (int i = 0; i < size * size; i++)
        {
            pixel[i * 4 + 0] = 0;   //B
            pixel[i * 4 + 1] = 0;   //G
            pixel[i * 4 + 2] = 0;   //R
            pixel[i * 4 + 3] = 1;   //A
        }
    }
    //掩码位图所有位都为1（透明）。注意不能使用未初始化的掩码
    std::vector<BYTE> mask_bits(size * size / 8, 0xFF);
    HBITMAP mask_bitmap = CreateBitmap(size, size, 1, 1, mask_bits.data());

    ICONINFO icon_info{};
    icon_info.fIcon = TRUE;
    icon_info.hbmColor = color_bitmap;
    icon_info.hbmMask = mask_bitmap;
    HICON icon = CreateIconIndirect(&icon_info);

    DeleteObject(color_bitmap);
    DeleteObject(mask_bitmap);
    return icon;
}

void CTaskbarButtonSpacer::RegisterSpacerWindowClass()
{
    static bool registered{ false };
    if (registered)
        return;
    //设置完全透明的图标，使任务栏按钮显示为空白
    HICON transparent_icon = CreateTransparentIcon();
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = AfxGetInstanceHandle();
    wc.lpszClassName = SPACER_WINDOW_CLASS;
    wc.hIcon = transparent_icon;
    wc.hIconSm = transparent_icon;
    RegisterClassExW(&wc);
    registered = true;
}

HWND CTaskbarButtonSpacer::CreateSpacerWindow(int index)
{
    RegisterSpacerWindowClass();
    //创建一个位于屏幕外的小窗口。WS_EX_APPWINDOW确保窗口显示在任务栏中
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, SPACER_WINDOW_CLASS, GetSpacerTitle(index).GetString(),
        WS_POPUP, -32000, -32000, 1, 1, nullptr, nullptr, AfxGetInstanceHandle(), nullptr);
    if (hwnd == nullptr)
        return nullptr;
    //为窗口设置带序号和代数的AppUserModelID。
    //任务栏按照AppUserModelID记忆按钮的位置，如果不设置，重新创建的窗口的按钮会回到原来的位置；
    //设置一个新的AppUserModelID后，任务栏会把按钮排列到所有按钮的最右侧。
    //每个窗口使用不同的AppUserModelID，防止在“合并任务栏按钮”开启时占位按钮被合并成一个
    CString app_id;
    app_id.Format(L"TrafficMonitor.TaskbarSpacer.%d.%u", index, m_generation);
    CComPtr<IPropertyStore> property_store;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&property_store))))
    {
        PROPVARIANT var;
        if (SUCCEEDED(InitPropVariantFromString(app_id.GetString(), &var)))
        {
            property_store->SetValue(PKEY_AppUserModel_ID, var);
            property_store->Commit();
            PropVariantClear(&var);
        }
    }
    ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    //使用DWM将窗口隐藏（Cloak）。窗口在屏幕、Alt+Tab和任务视图中都不可见，
    //但任务栏中的按钮仍然保留（与挂起的UWP应用保持任务栏按钮的机制相同）
    BOOL cloak = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
    return hwnd;
}

void CALLBACK CTaskbarButtonSpacer::WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG id_object, LONG id_child, DWORD event_thread, DWORD event_time)
{
    //任务栏布局发生了变化，立即唤醒后台查询线程
    if (m_instance != nullptr && m_instance->m_wake_event != nullptr)
        SetEvent(m_instance->m_wake_event);
}

void CTaskbarButtonSpacer::EnsureQueryThread()
{
    if (m_query_thread.joinable())
        return;
    m_thread_exit = false;
    if (m_wake_event == nullptr)
        m_wake_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    //安装WinEvent钩子，在任务栏布局变化时立即唤醒后台查询线程。
    //使用WINEVENT_OUTOFCONTEXT模式，事件在本进程中接收，不会向Explorer注入任何代码
    if (m_win_event_hook == nullptr)
    {
        HWND hTaskbar = ::FindWindowW(L"Shell_TrayWnd", nullptr);
        DWORD taskbar_process_id{};
        if (hTaskbar != nullptr)
            ::GetWindowThreadProcessId(hTaskbar, &taskbar_process_id);
        m_instance = this;
        m_win_event_hook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, WinEventProc,
            taskbar_process_id, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    }
    m_query_thread = std::thread(&CTaskbarButtonSpacer::QueryThreadProc, this);
}

void CTaskbarButtonSpacer::QueryThreadProc()
{
    //在MTA中初始化COM，UI自动化的调用不会抽取界面线程的消息队列
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        CComPtr<IUIAutomation> uia;
        CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation), reinterpret_cast<void**>(&uia));
        std::vector<CRect> last_rects;
        int stable_count{};
        int query_interval{ QUERY_INTERVAL_FAST };
        ULONGLONG last_query_tick{};
        while (!m_thread_exit)
        {
            last_query_tick = GetTickCount64();
            int count = m_window_count;
            if (uia != nullptr && count > 0)
            {
                std::vector<CRect> rects(count);
                std::vector<CRect> foreign_rects;   //其它程序的任务栏按钮的矩形区域
                //查找任务栏中的所有按钮
                HWND hTaskbar = ::FindWindowW(L"Shell_TrayWnd", nullptr);
                CComPtr<IUIAutomationElement> taskbar_element;
                if (hTaskbar != nullptr && SUCCEEDED(uia->ElementFromHandle(hTaskbar, &taskbar_element)) && taskbar_element != nullptr)
                {
                    CComPtr<IUIAutomationCondition> condition;
                    VARIANT var{};
                    var.vt = VT_I4;
                    var.lVal = UIA_ButtonControlTypeId;
                    CComPtr<IUIAutomationElementArray> buttons;
                    if (SUCCEEDED(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, var, &condition))
                        && SUCCEEDED(taskbar_element->FindAll(TreeScope_Descendants, condition, &buttons)) && buttons != nullptr)
                    {
                        int button_count{};
                        buttons->get_Length(&button_count);
                        for (int i = 0; i < button_count; i++)
                        {
                            CComPtr<IUIAutomationElement> button;
                            if (FAILED(buttons->GetElement(i, &button)) || button == nullptr)
                                continue;
                            CComBSTR name;
                            if (FAILED(button->get_CurrentName(&name)) || name == nullptr)
                                continue;
                            //按钮名称以窗口标题开头（后面可能跟有系统附加的说明文字）。
                            //统计名称开头的标题字符数量即可得到对应的占位窗口序号
                            CString name_str(name);
                            int char_count{};
                            while (char_count < name_str.GetLength() && name_str[char_count] == SPACER_TITLE_CHAR)
                                char_count++;
                            int index = char_count - 1;
                            RECT rc{};
                            if (FAILED(button->get_CurrentBoundingRectangle(&rc)))
                                continue;
                            if (index >= 0 && index < count)
                                rects[index] = rc;
                            else if (index < 0)
                                foreign_rects.push_back(CRect(rc));
                        }
                    }
                }
                //检查是否有其它程序的任务栏按钮出现在占位按钮的右侧（只检查任务按钮区域中的按钮，
                //即完全位于通知区域左侧的按钮，排除通知区域图标、时钟等）
                bool foreign_on_right{ false };
                LONG leftmost_spacer{ LONG_MAX };
                bool all_spacers_found{ true };
                for (const auto& rc : rects)
                {
                    if (rc.IsRectEmpty())
                        all_spacers_found = false;
                    else if (rc.left < leftmost_spacer)
                        leftmost_spacer = rc.left;
                }
                if (all_spacers_found && count > 0)
                {
                    CRect rc_notify;
                    HWND hNotify = ::FindWindowExW(hTaskbar, nullptr, L"TrayNotifyWnd", nullptr);
                    if (hNotify != nullptr && ::GetWindowRect(hNotify, rc_notify))
                    {
                        for (const auto& rc : foreign_rects)
                        {
                            //跳过通知区域中的按钮（托盘图标、“显示隐藏的图标”按钮、时钟等）。
                            //只要按钮延伸到了通知区域的左边界右侧，就认为它属于通知区域。
                            //注意不能只判断按钮的左边界：“显示隐藏的图标”按钮的左边界
                            //可能比TrayNotifyWnd的左边界小一两个像素，
                            //只判断左边界会把它当成任务按钮，导致一直误判为有按钮位于占位按钮右侧
                            if (rc.IsRectEmpty() || rc.right > rc_notify.left)
                                continue;
                            //其它程序的按钮进入了占位按钮的区域或者位于占位按钮的右侧
                            if (rc.right > leftmost_spacer)
                            {
                                foreign_on_right = true;
                                break;
                            }
                        }
                    }
                }
                //检测到有按钮进入占位按钮的区域时立即通知任务栏窗口，
                //使窗口不必等待定时器的下一次触发就能重新调整位置
                bool was_foreign_on_right = m_foreign_on_right.exchange(foreign_on_right);
                if (foreign_on_right && !was_foreign_on_right)
                {
                    HWND notify_wnd = m_notify_wnd;
                    if (notify_wnd != nullptr && ::IsWindow(notify_wnd))
                        ::PostMessageW(notify_wnd, WM_SPACER_LAYOUT_CHANGED, 0, 0);
                }
                //保存查询结果
                {
                    std::lock_guard<std::mutex> lock(m_rects_mutex);
                    m_button_rects = rects;
                }
                //根据任务栏布局是否稳定调整查询间隔：
                //布局连续多次没有变化时使用较长的间隔以节省电量，布局变化时立即切换回较短的间隔
                bool rects_changed = (rects.size() != last_rects.size());
                if (!rects_changed)
                {
                    for (size_t i = 0; i < rects.size(); i++)
                    {
                        if (rects[i] != last_rects[i])
                        {
                            rects_changed = true;
                            break;
                        }
                    }
                }
                last_rects = rects;
                if (rects_changed || foreign_on_right)
                {
                    stable_count = 0;
                    query_interval = QUERY_INTERVAL_FAST;
                }
                else if (++stable_count >= STABLE_COUNT_FOR_SLOW)
                {
                    query_interval = QUERY_INTERVAL_SLOW;
                }
            }
            //等待下一次查询。任务栏布局变化时会被WinEvent钩子立即唤醒，否则按当前间隔轮询
            if (m_wake_event != nullptr)
            {
                WaitForSingleObject(m_wake_event, query_interval);
                //限制查询频率：任务栏动画期间钩子会产生大量事件，
                //这里保证两次查询之间至少间隔MIN_QUERY_GAP，避免频繁查询占用CPU
                ULONGLONG elapsed = GetTickCount64() - last_query_tick;
                if (!m_thread_exit && elapsed < MIN_QUERY_GAP)
                    Sleep(static_cast<DWORD>(MIN_QUERY_GAP - elapsed));
            }
            else
            {
                for (int i = 0; i < query_interval / 50 && !m_thread_exit; i++)
                    Sleep(50);
            }
        }
    }
    CoUninitialize();
}

void CTaskbarButtonSpacer::SetRequiredWidth(int width)
{
    m_required_width = width;

    //清理已经失效的窗口（例如用户通过任务栏右键菜单关闭了窗口），下面会重新创建
    for (auto& hwnd : m_windows)
    {
        if (hwnd != nullptr && !::IsWindow(hwnd))
            hwnd = nullptr;
    }

    //测量单个占位按钮的宽度。
    //只接受合理范围内的宽度：任务栏按钮的宽度不会小于按钮的高度的一半，也不会超过按钮高度的3倍。
    //如果使用了不合理的宽度（例如任务栏正在重新布局时查询到的中间状态），
    //计算得到的占位按钮数量会过多，在任务栏中留下一大片空白
    {
        std::lock_guard<std::mutex> lock(m_rects_mutex);
        for (const auto& rc : m_button_rects)
        {
            if (!rc.IsRectEmpty() && rc.Width() > 0 && rc.Height() > 0
                && rc.Width() * 2 >= rc.Height() && rc.Width() <= rc.Height() * 3)
            {
                m_measured_button_width = rc.Width();
                break;
            }
        }
    }

    //计算需要的按钮数量（单个按钮宽度未知时先创建一个按钮用于测量）。
    //这里使用四舍五入而不是向上取整：向上取整时预留的区域可能比任务栏窗口宽很多，
    //窗口覆盖不到的部分会在任务栏上留下一片空白
    int target_count{ 1 };
    if (m_measured_button_width > 0)
        target_count = (width + m_measured_button_width / 2) / m_measured_button_width;
    if (target_count > MAX_SPACER_BUTTONS)
        target_count = MAX_SPACER_BUTTONS;
    if (target_count < 1)
        target_count = 1;

    //创建缺少的窗口
    while (static_cast<int>(m_windows.size()) < target_count)
        m_windows.push_back(CreateSpacerWindow(static_cast<int>(m_windows.size())));
    //重新创建已经失效的窗口
    for (size_t i = 0; i < m_windows.size(); i++)
    {
        if (m_windows[i] == nullptr)
            m_windows[i] = CreateSpacerWindow(static_cast<int>(i));
    }
    //移除多余的窗口
    while (static_cast<int>(m_windows.size()) > target_count)
    {
        if (m_windows.back() != nullptr)
            ::DestroyWindow(m_windows.back());
        m_windows.pop_back();
    }
    m_window_count = static_cast<int>(m_windows.size());

    //如果有其它程序的任务栏按钮出现在占位按钮的右侧（例如新打开的窗口，
    //或者用户把其它按钮拖动到了占位按钮的右侧），使用新的AppUserModelID重新创建占位窗口。
    //任务栏把使用新AppUserModelID的窗口当作新启动的应用程序，按钮总是排列在所有按钮的最右侧，
    //因此占位按钮（以及显示在它上面的任务栏窗口）会立即回到任务按钮区域的最右侧
    if (m_foreign_on_right && !m_windows.empty())
    {
        ULONGLONG current_tick = GetTickCount64();
        if (current_tick - m_last_recreate_tick > RECREATE_COOLDOWN)
        {
            m_last_recreate_tick = current_tick;
            m_foreign_on_right = false;
            m_generation++;
            //先创建新的占位窗口，再销毁原来的窗口，
            //使任务按钮区域中始终存在占位按钮，避免任务栏按钮的位置来回变化
            std::vector<HWND> old_windows{ m_windows };
            for (size_t i = 0; i < m_windows.size(); i++)
                m_windows[i] = CreateSpacerWindow(static_cast<int>(i));
            for (HWND hwnd : old_windows)
            {
                if (hwnd != nullptr)
                    ::DestroyWindow(hwnd);
            }
        }
    }

    //启动后台查询线程
    EnsureQueryThread();
}

bool CTaskbarButtonSpacer::GetReservedRect(CRect& rect) const
{
    if (m_windows.empty())
        return false;
    std::vector<CRect> rects;
    {
        std::lock_guard<std::mutex> lock(m_rects_mutex);
        rects = m_button_rects;
    }
    if (rects.size() != m_windows.size())
        return false;
    CRect union_rect;
    int total_width{};
    for (size_t i = 0; i < m_windows.size(); i++)
    {
        if (m_windows[i] == nullptr)
            return false;
        const CRect& rc = rects[i];
        if (rc.IsRectEmpty())
            return false;
        total_width += rc.Width();
        if (union_rect.IsRectEmpty())
            union_rect = rc;
        else
            union_rect.UnionRect(union_rect, rc);
    }
    //如果并集的宽度明显大于所有按钮宽度之和，说明占位按钮中间夹杂了其它程序的按钮，
    //此时不能使用这个区域，否则会遮挡其它程序的按钮
    if (m_measured_button_width > 0 && union_rect.Width() > total_width + m_measured_button_width)
        return false;
    rect = union_rect;
    return true;
}

void CTaskbarButtonSpacer::Destroy()
{
    //移除WinEvent钩子
    if (m_win_event_hook != nullptr)
    {
        UnhookWinEvent(m_win_event_hook);
        m_win_event_hook = nullptr;
    }
    m_instance = nullptr;
    //停止后台查询线程
    if (m_query_thread.joinable())
    {
        m_thread_exit = true;
        if (m_wake_event != nullptr)
            SetEvent(m_wake_event);
        m_query_thread.join();
    }
    if (m_wake_event != nullptr)
    {
        CloseHandle(m_wake_event);
        m_wake_event = nullptr;
    }
    for (HWND hwnd : m_windows)
    {
        if (hwnd != nullptr && ::IsWindow(hwnd))
            ::DestroyWindow(hwnd);
    }
    m_windows.clear();
    m_window_count = 0;
    {
        std::lock_guard<std::mutex> lock(m_rects_mutex);
        m_button_rects.clear();
    }
    m_measured_button_width = 0;
}

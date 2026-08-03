#include "stdafx.h"
#include "TaskbarButtonSpacer.h"
#include <uiautomation.h>
#include <shellapi.h>
#include <strsafe.h>
#include <algorithm>

CTaskbarTrayReserve* CTaskbarTrayReserve::m_instance{};

//占位图标宿主窗口的窗口类名 / window class of the hidden icon-owner window
static const wchar_t* TRAY_RESERVE_WINDOW_CLASS = L"TrafficMonitorTrayReserve";
//占位图标的提示文本。故意取一个独一无二的名字，便于在UI自动化中把它们找出来。
//Tooltip of the placeholder icons - deliberately unique so UI Automation can find them.
static const wchar_t* TRAY_RESERVE_TIP = L"TrafficMonitor reserved space";

GUID CTaskbarTrayReserve::MakeIconGuid(int index)
{
    //每个槽位使用一个固定的GUID，序号放在最后两个字节里，
    //这样重启之后同一个槽位仍然对应同一个图标。
    //One fixed GUID per slot, with the index in the last two bytes, so a slot maps to the
    //same icon across restarts.
    GUID guid{ 0x7f3a9c41, 0x6d18, 0x4b52, { 0x9e, 0x64, 0xa1, 0x27, 0x00, 0x00 } };
    guid.Data4[6] = static_cast<unsigned char>((index >> 8) & 0xFF);
    guid.Data4[7] = static_cast<unsigned char>(index & 0xFF);
    return guid;
}

//创建一个完全透明的图标，使占位图标在通知区域中不可见
//A fully transparent icon, so the placeholders are invisible in the tray
static HICON CreateBlankTrayIcon()
{
    const int size = 32;
    HDC screen_dc = ::GetDC(nullptr);
    if (screen_dc == nullptr)
        return nullptr;
    HICON result{};
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits{};
    HBITMAP color = CreateDIBSection(screen_dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (color != nullptr)
    {
        if (bits != nullptr)
            memset(bits, 0, static_cast<size_t>(size) * size * 4);  //alpha 0 everywhere
        //掩码必须显式填成全1（表示完全透明）。传nullptr时掩码的内容是未初始化的，
        //取到0的位会被画成实心黑块，通知区域里就会出现一排黑方块。
        //The AND mask must be explicitly filled with 1s (= fully transparent). Passing nullptr
        //leaves it uninitialised, and zeroed mask bits draw as solid black squares in the tray.
        const int mask_stride = ((size + 15) / 16) * 2;         //1bpp rows are WORD aligned
        std::vector<BYTE> mask_bits(static_cast<size_t>(mask_stride) * size, 0xFF);
        HBITMAP mask = CreateBitmap(size, size, 1, 1, mask_bits.data());
        if (mask != nullptr)
        {
            ICONINFO ii{};
            ii.fIcon = TRUE;
            ii.hbmColor = color;
            ii.hbmMask = mask;
            result = CreateIconIndirect(&ii);
            DeleteObject(mask);
        }
        DeleteObject(color);
    }
    ::ReleaseDC(nullptr, screen_dc);
    return result;
}

CTaskbarTrayReserve::~CTaskbarTrayReserve()
{
    Destroy();
}

bool CTaskbarTrayReserve::EnsureWindow()
{
    if (m_wnd != nullptr && ::IsWindow(m_wnd))
        return true;

    static bool registered{ false };
    if (!registered)
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = AfxGetInstanceHandle();
        wc.lpszClassName = TRAY_RESERVE_WINDOW_CLASS;
        RegisterClassExW(&wc);
        registered = true;
    }
    //一个真实存在、但从不显示的顶层窗口。绝对不能用仅消息窗口：
    //外壳会拒绝由HWND_MESSAGE窗口发起的Shell_NotifyIcon，导致每次添加图标都失败。
    //A real (but never shown) top-level window. This must NOT be a message-only window: the
    //shell rejects Shell_NotifyIcon from HWND_MESSAGE owners and every add fails.
    m_wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, TRAY_RESERVE_WINDOW_CLASS,
        L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, AfxGetInstanceHandle(), nullptr);
    if (m_wnd == nullptr)
        return false;
    if (m_icon == nullptr)
        m_icon = CreateBlankTrayIcon();
    return true;
}

bool CTaskbarTrayReserve::AddIcon(int index)
{
    if (!EnsureWindow())
        return false;

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_wnd;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_GUID;
    nid.guidItem = MakeIconGuid(index);
    nid.hIcon = m_icon;
    StringCchCopyW(nid.szTip, ARRAYSIZE(nid.szTip), TRAY_RESERVE_TIP);

    if (!Shell_NotifyIconW(NIM_ADD, &nid))
    {
        //GUID可能还被上一次没有正常退出的实例占着，删掉之后重试一次
        //The GUID may still be held by an instance that did not exit cleanly - drop and retry
        Shell_NotifyIconW(NIM_DELETE, &nid);
        if (!Shell_NotifyIconW(NIM_ADD, &nid))
            return false;
    }
    return true;
}

void CTaskbarTrayReserve::RemoveIcon(int index)
{
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_wnd;
    nid.uFlags = NIF_GUID;
    nid.guidItem = MakeIconGuid(index);
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void CTaskbarTrayReserve::RefineSlotWidth(int reserved_width, int icon_count)
{
    if (icon_count <= 0 || reserved_width <= 0)
        return;
    const int measured = reserved_width / icon_count;
    if (measured <= 0)
        return;
    //只允许把估计值改小，理由见头文件 / only ever narrow it; see the header for why
    if (m_slot_width == 0 || measured < m_slot_width)
        m_slot_width = measured;
}

int CTaskbarTrayReserve::GetSlotWidth() const
{
    //图标已经显示出来之后，它们真实的间距就是已知的，应当优先采用。
    //实测GetDeviceCaps可能报告144，而通知区域图标的实际间距是42像素，
    //只按DPI估算会少预留三分之一，窗口永远放不进预留区域。
    //Once the icons are on screen their real pitch is known; trust it over the DPI guess.
    //GetDeviceCaps can report 144 while tray icons are genuinely 42px apart, so the guess
    //alone under-reserves by a third and the window never fits the region.
    if (m_slot_width > 0)
        return m_slot_width;
    UINT dpi = 96;
    HDC dc = ::GetDC(nullptr);
    if (dc != nullptr)
    {
        dpi = static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX));
        ::ReleaseDC(nullptr, dc);
    }
    const int slot = ICON_SLOT_WIDTH * static_cast<int>(dpi) / 96;
    return slot > 0 ? slot : ICON_SLOT_WIDTH;
}

bool CTaskbarTrayReserve::IsHealthy() const
{
    if (m_ever_valid)
        return true;                //成功过就不再怀疑 / proven to work here
    if (m_first_icon_tick == 0)
        return true;                //还没开始 / nothing attempted yet
    return (GetTickCount64() - m_first_icon_tick) < HEALTH_GRACE;
}

void CTaskbarTrayReserve::SetReservedWidth(int width)
{
    int target{};
    if (width > 0)
    {
        int slot{};
        {
            std::lock_guard<std::mutex> lock(m_rect_mutex);
            if (m_reserved_valid)
                RefineSlotWidth(m_reserved_rect.Width(), m_reserved_count);
            slot = GetSlotWidth();
        }
        //向上取整：多预留一点无伤大雅，少预留则会让窗口放不进预留区域。
        //Round up: over-reserving slightly is harmless, under-reserving means the window does
        //not fit and lands back on top of taskbar buttons.
        target = (width + slot - 1) / slot;
        if (target > MAX_ICONS)
            target = MAX_ICONS;
    }

    if (target != static_cast<int>(m_icons.size()))
    {
        const ULONGLONG now = GetTickCount64();
        //每次只增加一个图标，而不是一口气全部添加。连续添加时外壳会丢掉其中一部分，
        //而且所有图标同时出现会让通知区域明显抖动。调用方是定时器驱动的，仍然很快能填满。
        //Grow by a single icon per call rather than in a burst: adding several in a tight loop
        //makes the shell drop some, and the tray visibly thrashes as they all appear at once.
        if (static_cast<int>(m_icons.size()) < target && now >= m_retry_after)
        {
            const int index = static_cast<int>(m_icons.size());
            if (AddIcon(index))
            {
                m_failed_attempts = 0;
                m_icons.push_back(index);
                if (m_first_icon_tick == 0)
                    m_first_icon_tick = now;
            }
            else
            {
                m_failed_attempts++;
                const int steps = (m_failed_attempts < MAX_BACKOFF_STEPS)
                    ? m_failed_attempts : MAX_BACKOFF_STEPS;
                m_retry_after = now + RETRY_COOLDOWN * steps;
            }
        }
        while (static_cast<int>(m_icons.size()) > target)
        {
            RemoveIcon(m_icons.back());
            m_icons.pop_back();
        }
        m_icon_count = static_cast<int>(m_icons.size());
        {
            //数量变了，缓存的区域不再反映现实 / the count changed, the cached region is stale
            std::lock_guard<std::mutex> lock(m_rect_mutex);
            m_reserved_valid = false;
        }
    }

    if (!m_icons.empty())
        EnsureQueryThread();
}

bool CTaskbarTrayReserve::GetReservedRect(CRect& rect) const
{
    std::lock_guard<std::mutex> lock(m_rect_mutex);
    if (!m_reserved_valid)
        return false;
    rect = m_reserved_rect;
    return true;
}

void CALLBACK CTaskbarTrayReserve::WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
    LONG id_object, LONG id_child, DWORD event_thread, DWORD event_time)
{
    //任务栏布局变了，立刻唤醒后台线程 / the taskbar layout changed - wake the query thread
    if (m_instance != nullptr && m_instance->m_wake_event != nullptr)
        SetEvent(m_instance->m_wake_event);
}

void CTaskbarTrayReserve::EnsureQueryThread()
{
    if (m_query_thread.joinable())
        return;
    m_thread_exit = false;
    if (m_wake_event == nullptr)
        m_wake_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (m_win_event_hook == nullptr)
    {
        HWND taskbar = ::FindWindowW(L"Shell_TrayWnd", nullptr);
        DWORD pid{};
        if (taskbar != nullptr)
            ::GetWindowThreadProcessId(taskbar, &pid);
        m_instance = this;
        //WINEVENT_OUTOFCONTEXT：事件在本进程中接收，不会向Explorer注入任何代码
        //OUTOFCONTEXT: events arrive in our own process; nothing is injected into Explorer
        m_win_event_hook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_LOCATIONCHANGE,
            nullptr, WinEventProc, pid, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    }
    m_query_thread = std::thread(&CTaskbarTrayReserve::QueryThreadProc, this);
}

void CTaskbarTrayReserve::QueryThreadProc()
{
    //在MTA中初始化COM：UI自动化的跨进程调用会抽取消息队列，
    //在界面线程中调用会造成重入。
    //COM in an MTA: UI Automation's cross-process calls pump the message queue, which would
    //re-enter the UI thread if this ran there.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        CComPtr<IUIAutomation> uia;
        CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
            __uuidof(IUIAutomation), reinterpret_cast<void**>(&uia));

        struct TrayItem { CRect rect; bool ours; };
        CRect last_rect;
        int stable_count{};
        int query_interval{ QUERY_INTERVAL_FAST };
        while (!m_thread_exit)
        {
            const ULONGLONG query_start = GetTickCount64();
            const int expected = m_icon_count;

            std::vector<TrayItem> tray_items;
            if (uia != nullptr && expected > 0)
            {
                HWND taskbar = ::FindWindowW(L"Shell_TrayWnd", nullptr);
                CRect rc_taskbar;
                const bool taskbar_ok = (taskbar != nullptr
                    && ::GetWindowRect(taskbar, rc_taskbar) && !rc_taskbar.IsRectEmpty());
                CComPtr<IUIAutomationElement> taskbar_element;
                if (taskbar_ok && SUCCEEDED(uia->ElementFromHandle(taskbar, &taskbar_element))
                    && taskbar_element != nullptr)
                {
                    //枚举任务栏里的所有按钮，而不只是自己的占位图标：
                    //要判断哪一段占位图标是真正相邻的，必须知道它们之间有没有别的图标。
                    //Enumerate every button in the taskbar, not just our own placeholders:
                    //deciding which placeholders are genuinely adjacent requires knowing
                    //whether anything else sits between them.
                    CComPtr<IUIAutomationCondition> condition;
                    VARIANT var{};
                    var.vt = VT_I4;
                    var.lVal = UIA_ButtonControlTypeId;
                    CComPtr<IUIAutomationElementArray> elements;
                    if (SUCCEEDED(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, var, &condition))
                        && SUCCEEDED(taskbar_element->FindAll(TreeScope_Descendants, condition, &elements))
                        && elements != nullptr)
                    {
                        int length{};
                        elements->get_Length(&length);
                        for (int i = 0; i < length; i++)
                        {
                            CComPtr<IUIAutomationElement> element;
                            if (FAILED(elements->GetElement(i, &element)) || element == nullptr)
                                continue;
                            RECT rc{};
                            if (FAILED(element->get_CurrentBoundingRectangle(&rc)))
                                continue;
                            //丢弃明显不合理的结果：任务栏切换位置时UI自动化会短暂返回
                            //空矩形或者根本不在任务栏里的元素，采信它们会把窗口摆到屏幕外。
                            //Discard nonsense: while the taskbar is being moved UI Automation
                            //briefly returns empty rects and elements outside it entirely.
                            if (rc.right <= rc.left || rc.bottom <= rc.top)
                                continue;
                            CRect rc_hit;
                            if (!rc_hit.IntersectRect(rc_taskbar, CRect(rc)))
                                continue;
                            //用包含关系而不是完全相等来认自己的图标：
                            //系统可能在提示文本前后加上其它内容，严格相等会一个也认不出来。
                            //Match by substring rather than equality: the shell may decorate
                            //the tooltip, and strict equality would then recognise none.
                            CComBSTR name;
                            const bool is_ours = (SUCCEEDED(element->get_CurrentName(&name))
                                && name != nullptr && wcsstr(name, TRAY_RESERVE_TIP) != nullptr);
                            tray_items.push_back({ CRect(rc), is_ours });
                        }
                    }
                    VariantClear(&var);
                }
            }

            //UI自动化有时会暂时枚举不到任务栏里的任何东西——最典型的就是
            //在任务栏上点右键、弹出那个菜单的时候。这只是暂时看不见，
            //并不代表占位图标真的没了，必须保持上一次的结果不变，
            //否则窗口会以为预留区域没了而淡出去。
            //UI Automation intermittently enumerates nothing at all in the taskbar - most
            //reproducibly while the taskbar's own right-click menu is open. That is a blackout,
            //not a real loss, so the previous result must be kept; otherwise the window
            //concludes the region has vanished and dims out from under the user.
            if (tray_items.empty())
            {
                if (m_wake_event != nullptr)
                    WaitForSingleObject(m_wake_event, query_interval);
                else
                    Sleep(query_interval);
                continue;
            }

            //只采用连续的一段占位图标。用户可以拖动通知区域的图标重新排序，
            //别的程序的图标因此可能被拖到两个占位图标之间。若直接取并集，
            //预留区域就会把那个图标包在里面，窗口一盖上去就把它藏住了。
            //不能改用间距判断：通知区域会把图标重新排紧，中间根本不留缝隙。
            //Use only a contiguous run of placeholders. Tray icons can be reordered by
            //dragging, so another program's icon can land between two of ours; taking the
            //plain union would enclose it and the window would hide it. A gap test does not
            //work either - the tray re-packs its icons and leaves no space at all.
            std::sort(tray_items.begin(), tray_items.end(),
                [](const TrayItem& a, const TrayItem& b) { return a.rect.left < b.rect.left; });
            CRect union_rect;
            int found{};
            {
                size_t best_begin = 0, best_len = 0, run_begin = 0, run_len = 0;
                for (size_t i = 0; i < tray_items.size(); i++)
                {
                    if (!tray_items[i].ours)
                    {
                        run_len = 0;
                        continue;
                    }
                    if (run_len == 0)
                        run_begin = i;
                    run_len++;
                    if (run_len > best_len)
                    {
                        best_len = run_len;
                        best_begin = run_begin;
                    }
                }
                for (size_t i = best_begin; i < best_begin + best_len; i++)
                {
                    if (found == 0)
                        union_rect = tray_items[i].rect;
                    else
                        union_rect.UnionRect(union_rect, tray_items[i].rect);
                    found++;
                }
            }

            const bool valid = (found > 0 && !union_rect.IsRectEmpty());
            bool changed{};
            {
                std::lock_guard<std::mutex> lock(m_rect_mutex);
                changed = (valid != m_reserved_valid) || (valid && union_rect != m_reserved_rect);
                m_reserved_valid = valid;
                if (valid)
                {
                    m_reserved_rect = union_rect;
                    //必须用“真正构成这块区域的图标数”去除，才能得到正确的间距。
                    //Dividing by the icons that actually form the region is the only way to
                    //get the true pitch; using the requested count under-measures.
                    m_reserved_count = found;
                    m_ever_valid = true;
                }
            }
            if (changed)
            {
                HWND notify = m_notify_wnd;
                if (notify != nullptr && ::IsWindow(notify))
                    ::PostMessageW(notify, WM_SPACER_LAYOUT_CHANGED, 0, 0);
            }

            //位置稳定下来之后拉长查询间隔以省电，一有变化立刻恢复到较短的间隔
            //Lengthen the interval once the position settles; snap back the moment it moves
            if (changed || union_rect != last_rect)
            {
                stable_count = 0;
                query_interval = QUERY_INTERVAL_FAST;
            }
            else if (++stable_count >= STABLE_COUNT_FOR_SLOW)
            {
                query_interval = QUERY_INTERVAL_SLOW;
            }
            last_rect = union_rect;

            if (m_wake_event != nullptr)
            {
                WaitForSingleObject(m_wake_event, query_interval);
                //限制查询频率：任务栏动画期间钩子会产生大量事件
                //Throttle: taskbar animations make the hook fire a flood of events
                const ULONGLONG elapsed = GetTickCount64() - query_start;
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

void CTaskbarTrayReserve::Destroy()
{
    m_thread_exit = true;
    if (m_wake_event != nullptr)
        SetEvent(m_wake_event);
    if (m_query_thread.joinable())
        m_query_thread.join();
    if (m_wake_event != nullptr)
    {
        CloseHandle(m_wake_event);
        m_wake_event = nullptr;
    }
    if (m_win_event_hook != nullptr)
    {
        UnhookWinEvent(m_win_event_hook);
        m_win_event_hook = nullptr;
    }
    if (m_instance == this)
        m_instance = nullptr;

    for (int index : m_icons)
        RemoveIcon(index);
    m_icons.clear();
    m_icon_count = 0;
    m_first_icon_tick = 0;
    m_failed_attempts = 0;
    m_retry_after = 0;

    if (m_wnd != nullptr)
    {
        if (::IsWindow(m_wnd))
            ::DestroyWindow(m_wnd);
        m_wnd = nullptr;
    }
    if (m_icon != nullptr)
    {
        DestroyIcon(m_icon);
        m_icon = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(m_rect_mutex);
        m_reserved_valid = false;
        m_reserved_count = 0;
        m_reserved_rect.SetRectEmpty();
    }
}

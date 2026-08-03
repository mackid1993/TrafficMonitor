#include "stdafx.h"
#include "TaskbarButtonSpacer.h"
#include <uiautomation.h>
#include <shellapi.h>
#include <strsafe.h>
#include <algorithm>

CTaskbarTrayReserve* CTaskbarTrayReserve::m_instance{};

//占位图标宿主窗口的窗口类名 / window class of the hidden icon-owner window
static const wchar_t* TRAY_RESERVE_WINDOW_CLASS = L"TrafficMonitorTrayReserve";
//占位图标的提示文本。故意取一个独一无二的名字，便于在UI自动化中找出它们。
//Tooltip of the placeholder icons - deliberately unique so UI Automation can find them.
static const wchar_t* TRAY_RESERVE_TIP = L"TrafficMonitor reserved space";
//存放通知区域图标显示设置的注册表路径 / registry path for tray icon visibility
static const wchar_t* NOTIFY_ICON_SETTINGS_KEY = L"Control Panel\\NotifyIconSettings";

GUID CTaskbarTrayReserve::MakeIconGuid(int index)
{
    //每个槽位使用一个固定的GUID，序号放在最后两个字节里，
    //这样重启之后同一个槽位仍然对应注册表中的同一项。
    //One fixed GUID per slot, with the index in the last two bytes, so a slot maps to the
    //same registry entry across restarts instead of littering a new one every launch.
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
        //掩码必须显式填成全1（表示完全透明）。传nullptr时掩码未初始化，
        //取到0的位会被画成实心黑块，通知区域里就会出现一排黑方块。
        //The AND mask must be explicitly filled with 1s (= fully transparent). Passing
        //nullptr leaves it uninitialised, and zeroed mask bits draw as solid black squares
        //in the tray instead of nothing at all.
        const int mask_stride = ((size + 15) / 16) * 2;             //1bpp rows are WORD aligned
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
    //A real (but never shown) top-level window. This must NOT be a message-only window:
    //the shell rejects Shell_NotifyIcon from HWND_MESSAGE owners, so every add failed.
    //WS_EX_TOOLWINDOW keeps it out of the taskbar and Alt+Tab.
    m_wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, TRAY_RESERVE_WINDOW_CLASS,
        L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, AfxGetInstanceHandle(), nullptr);
    if (m_wnd == nullptr)
        return false;
    if (m_icon == nullptr)
        m_icon = CreateBlankTrayIcon();

    //清理上一次运行遗留下来的占位图标注册表项。正常退出时Destroy()会清理，
    //但崩溃或者被强制结束进程时不会，这些残留会一直堆积在用户的设置里。
    //Delete placeholder entries left over by a previous run. Destroy() cleans up on a normal
    //exit, but a crash or a force-kill skips it, and those orphans would otherwise pile up in
    //the notification-area settings forever.
    HKEY root{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, NOTIFY_ICON_SETTINGS_KEY, 0, KEY_READ | KEY_SET_VALUE, &root) == ERROR_SUCCESS)
    {
        for (const auto& name : EnumNotifyIconKeys())
        {
            HKEY sub{};
            if (RegOpenKeyExW(root, name.c_str(), 0, KEY_READ, &sub) != ERROR_SUCCESS)
                continue;
            wchar_t tip[256]{};
            DWORD size = sizeof(tip);
            DWORD type{};
            const bool is_ours = (RegQueryValueExW(sub, L"InitialTooltip", nullptr, &type,
                reinterpret_cast<LPBYTE>(tip), &size) == ERROR_SUCCESS
                && type == REG_SZ && wcscmp(tip, TRAY_RESERVE_TIP) == 0);
            RegCloseKey(sub);
            if (is_ours)
                RegDeleteKeyW(root, name.c_str());
        }
        RegCloseKey(root);
    }
    return true;
}

std::vector<std::wstring> CTaskbarTrayReserve::EnumNotifyIconKeys()
{
    std::vector<std::wstring> keys;
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, NOTIFY_ICON_SETTINGS_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return keys;
    wchar_t name[256]{};
    DWORD index{};
    DWORD name_len = ARRAYSIZE(name);
    while (RegEnumKeyExW(key, index, name, &name_len, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
    {
        keys.push_back(name);
        index++;
        name_len = ARRAYSIZE(name);
    }
    RegCloseKey(key);
    return keys;
}

bool CTaskbarTrayReserve::PromoteIcon(const std::vector<std::wstring>& keys_before)
{
    //Find the key that just appeared and mark it always-visible. Its name must be recorded:
    //removing an icon only blanks the key's values and leaves the empty key behind, so
    //matching on contents (e.g. ExecutablePath) during cleanup would silently find nothing.
    bool promoted{ false };
    for (const auto& name : EnumNotifyIconKeys())
    {
        if (std::find(keys_before.begin(), keys_before.end(), name) != keys_before.end())
            continue;
        std::wstring path{ NOTIFY_ICON_SETTINGS_KEY };
        path += L"\\";
        path += name;
        HKEY key{};
        if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS)
        {
            DWORD value = 1;
            if (RegSetValueExW(key, L"IsPromoted", 0, REG_DWORD,
                reinterpret_cast<const BYTE*>(&value), sizeof(value)) == ERROR_SUCCESS)
            {
                promoted = true;
            }
            RegCloseKey(key);
        }
        if (std::find(m_created_keys.begin(), m_created_keys.end(), name) == m_created_keys.end())
            m_created_keys.push_back(name);
    }
    return promoted;
}

bool CTaskbarTrayReserve::FindKeyForIcon(int index, std::wstring& key_name)
{
    const GUID guid = MakeIconGuid(index);
    wchar_t wanted[64]{};
    StringFromGUID2(guid, wanted, ARRAYSIZE(wanted));

    HKEY root{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, NOTIFY_ICON_SETTINGS_KEY, 0, KEY_READ, &root) != ERROR_SUCCESS)
        return false;
    bool found{};
    for (const auto& name : EnumNotifyIconKeys())
    {
        HKEY sub{};
        if (RegOpenKeyExW(root, name.c_str(), 0, KEY_READ, &sub) != ERROR_SUCCESS)
            continue;
        wchar_t value[64]{};
        DWORD size = sizeof(value);
        DWORD type{};
        if (RegQueryValueExW(sub, L"IconGuid", nullptr, &type, reinterpret_cast<LPBYTE>(value), &size) == ERROR_SUCCESS
            && type == REG_SZ && _wcsicmp(value, wanted) == 0)
        {
            key_name = name;
            found = true;
        }
        RegCloseKey(sub);
        if (found)
            break;
    }
    RegCloseKey(root);
    return found;
}

void CTaskbarTrayReserve::EnsurePromoted()
{
    if (m_pending_promote.empty())
        return;
    std::vector<int> still_pending;
    for (int index : m_pending_promote)
    {
        std::wstring key_name;
        if (!FindKeyForIcon(index, key_name))
        {
            //Explorer has not written the key yet - try again on the next call
            still_pending.push_back(index);
            continue;
        }
        std::wstring path{ NOTIFY_ICON_SETTINGS_KEY };
        path += L"\\";
        path += key_name;
        HKEY key{};
        if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ | KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        {
            still_pending.push_back(index);
            continue;
        }
        DWORD promoted{};
        DWORD size = sizeof(promoted);
        DWORD type{};
        const bool already = (RegQueryValueExW(key, L"IsPromoted", nullptr, &type,
            reinterpret_cast<LPBYTE>(&promoted), &size) == ERROR_SUCCESS && promoted == 1);
        if (!already)
        {
            DWORD value = 1;
            RegSetValueExW(key, L"IsPromoted", 0, REG_DWORD,
                reinterpret_cast<const BYTE*>(&value), sizeof(value));
        }
        RegCloseKey(key);

        if (std::find(m_created_keys.begin(), m_created_keys.end(), key_name) == m_created_keys.end())
            m_created_keys.push_back(key_name);

        if (!already)
        {
            //Re-add so the shell picks the new visibility up; it only reads this when the
            //icon is registered.
            NOTIFYICONDATAW nid{};
            nid.cbSize = sizeof(nid);
            nid.hWnd = m_wnd;
            nid.uFlags = NIF_ICON | NIF_TIP | NIF_GUID;
            nid.guidItem = MakeIconGuid(index);
            nid.hIcon = m_icon;
            StringCchCopyW(nid.szTip, ARRAYSIZE(nid.szTip), TRAY_RESERVE_TIP);
            Shell_NotifyIconW(NIM_DELETE, &nid);
            Shell_NotifyIconW(NIM_ADD, &nid);
        }
    }
    m_pending_promote = std::move(still_pending);
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

    //Step 1: add once so Windows creates the registry entry. That entry does not exist until
    //the icon has been added at least once, so IsPromoted cannot simply be written up front.
    const std::vector<std::wstring> keys_before = EnumNotifyIconKeys();
    if (!Shell_NotifyIconW(NIM_ADD, &nid))
    {
        //The GUID may still be held by an instance that did not exit cleanly - drop it and retry
        Shell_NotifyIconW(NIM_DELETE, &nid);
        if (!Shell_NotifyIconW(NIM_ADD, &nid))
            return false;
    }

    //Step 2: mark the new entry always-visible, then delete and re-add so it takes effect.
    //Without this the icon sits in the "Show Hidden Icons" flyout and reserves nothing at all.
    if (PromoteIcon(keys_before))
    {
        Shell_NotifyIconW(NIM_DELETE, &nid);
        Shell_NotifyIconW(NIM_ADD, &nid);
    }
    else
    {
        //The key was not there yet. Explorer writes it asynchronously, so retry the
        //promotion later - otherwise this icon stays in the hidden flyout and reserves
        //nothing, and the region never becomes wide enough to hold the window.
        m_pending_promote.push_back(index);
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
    //只允许把估计值改小。改大会导致图标变少、区域变窄、估计值又变大，
    //形成来回震荡，通知区域和窗口都会不停抖动。
    //Only ever narrow the estimate. Growing it would reduce the icon count, shrink the
    //region and re-grow the estimate - the oscillation that made the tray thrash before.
    if (m_slot_width == 0 || measured < m_slot_width)
        m_slot_width = measured;
}

int CTaskbarTrayReserve::GetSlotWidth() const
{
    //Once the icons are on screen their real pitch is known; trust it over the DPI guess.
    //GetDeviceCaps reports 144 on this machine while tray icons are genuinely 42px apart,
    //so the guess alone under-reserves by a third and the window never fits the region.
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

void CTaskbarTrayReserve::SetReservedWidth(int width)
{
    //Catch up on any icon whose always-visible flag could not be written when it was added
    EnsurePromoted();

    //Correct the slot estimate from the icons that are actually on screen
    {
        std::lock_guard<std::mutex> lock(m_rect_mutex);
        if (m_reserved_valid)
            RefineSlotWidth(m_reserved_rect.Width(), m_reserved_count);
    }

    int target{};
    if (width > 0)
    {
        const int slot = GetSlotWidth();
        //向上取整：多预留一点无伤大雅，少预留则会让窗口放不进预留区域，
        //结果又压回到任务栏按钮上。
        //Round up: reserving slightly more than asked for is harmless, whereas reserving less
        //means the window does not fit the region and lands back on top of taskbar buttons.
        target = (width + slot - 1) / slot;
        if (target > MAX_ICONS)
            target = MAX_ICONS;
    }

    if (target != static_cast<int>(m_icons.size()))
    {
        const ULONGLONG now = GetTickCount64();
        //Only attempt to grow if we are not in a back-off window and have not given up.
        //Without this a persistent failure is retried on every timer tick, and because each
        //attempt adds, promotes, deletes and re-adds an icon, the whole tray flickers.
        //Back off after a failure, but never give up for good: adds fail transiently while
        //the shell is rebuilding the tray, and stopping permanently leaves the region too
        //small forever, so the window can never move onto it.
        const bool may_add = (now >= m_retry_after);
        //每次只增加一个图标，而不是一口气全部添加。连续添加时外壳会丢掉一部分，
        //而且所有图标同时出现会让通知区域明显抖动。调用方是定时器驱动的，仍然很快能填满。
        //Grow by a single icon per call rather than in one burst. Adding a handful in a tight
        //loop makes the shell drop some of them, and the tray visibly thrashes while they all
        //appear at once. The caller runs on a timer, so the region still fills in quickly.
        if (may_add && static_cast<int>(m_icons.size()) < target)
        {
            const int index = static_cast<int>(m_icons.size());
            if (AddIcon(index))
            {
                m_failed_attempts = 0;
                m_icons.push_back(index);
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
            const int index = m_icons.back();
            RemoveIcon(index);
            m_icons.pop_back();
            m_pending_promote.erase(
                std::remove(m_pending_promote.begin(), m_pending_promote.end(), index),
                m_pending_promote.end());
        }
        m_icon_count = static_cast<int>(m_icons.size());
        {
            //The count changed, so the cached region no longer describes reality
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
    //The taskbar layout changed - wake the query thread at once
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
        //OUTOFCONTEXT: events arrive in our own process; nothing is injected into Explorer
        m_win_event_hook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_LOCATIONCHANGE,
            nullptr, WinEventProc, pid, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    }
    m_query_thread = std::thread(&CTaskbarTrayReserve::QueryThreadProc, this);
}

void CTaskbarTrayReserve::QueryThreadProc()
{
    //COM in an MTA: UI Automation's cross-process calls pump the message queue, which would
    //re-enter the UI thread if this ran there.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        CComPtr<IUIAutomation> uia;
        CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
            __uuidof(IUIAutomation), reinterpret_cast<void**>(&uia));

        CRect last_rect;
        int stable_count{};
        int query_interval{ QUERY_INTERVAL_FAST };
        while (!m_thread_exit)
        {
            const ULONGLONG query_start = GetTickCount64();
            const int expected = m_icon_count;

            CRect union_rect;
            std::vector<CRect> others;      //别人的托盘图标 / other programs' tray icons
            int found{};
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
                    //枚举任务栏里所有的按钮，而不是只找自己的占位图标。
                    //只找自己的就看不见别人：用户可以把别的程序的托盘图标拖到占位块中间，
                    //那个图标落在预留区域里面，窗口一摆上去就正好把它盖住，
                    //既看不见也点不到。要发现这种情况，就必须知道区域里还有没有别人的图标。
                    //Enumerate every button in the taskbar, not just our own placeholders.
                    //Looking only for our own makes other icons invisible to us: the user can
                    //drag another program's tray icon into the middle of the placeholder block,
                    //where it lands inside the reserved region and the window covers it - unable
                    //to be seen or clicked. Spotting that requires knowing what else is in there.
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
                            //Discard nonsense: while the taskbar is being moved, UI Automation
                            //briefly returns empty rects and elements that are not inside it at
                            //all. Trusting those throws the window off-screen.
                            if (rc.right <= rc.left || rc.bottom <= rc.top)
                                continue;
                            CRect rc_hit;
                            if (!rc_hit.IntersectRect(rc_taskbar, CRect(rc)))
                                continue;
                            //用"包含"而不是"完全相等"来认自己的图标：外壳有可能在提示文本
                            //前后加上别的内容，严格相等就会一个也认不出来。
                            //Match by substring rather than exact equality: the shell may
                            //decorate the tooltip, and strict equality would recognise none.
                            CComBSTR name;
                            const bool is_ours = (SUCCEEDED(element->get_CurrentName(&name))
                                && name != nullptr && wcsstr(name, TRAY_RESERVE_TIP) != nullptr);
                            if (is_ours)
                            {
                                if (found == 0)
                                    union_rect = rc;
                                else
                                    union_rect.UnionRect(union_rect, CRect(rc));
                                found++;
                            }
                            else
                            {
                                others.push_back(CRect(rc));
                            }
                        }
                    }
                    VariantClear(&var);
                }
            }

            //Use whatever placeholders are genuinely on screen. Requiring all of them to be
            //found makes the region permanently invalid whenever a single icon lags behind or
            //stays hidden, and the window then never moves onto the space that does exist.
            //The caller checks the width, so a partially filled region simply fits less.
            const bool valid = (found > 0 && !union_rect.IsRectEmpty());

            //预留区域里有没有夹着别人的图标。通知区域的图标可以被用户拖动重排，
            //别的程序的图标完全可能被拖到两个占位图标中间，落在预留区域内部。
            //这时窗口如果照常摆上去，就正好把那个图标盖在下面——用户既看不见它，
            //也点不到它，只能把窗口挪开才能取回。调用方发现这个标志就把窗口藏起来，
            //图标被拖走之后标志自动清除，窗口再自己回来。
            //Whether someone else's icon is sitting inside the reserved region. Tray icons can
            //be reordered by dragging, so another program's icon can land between two
            //placeholders, inside the region. Placing the window there as usual would cover that
            //icon - invisible and unclickable until the window moves away. The caller hides the
            //window while this is set; once the icon is dragged out it clears and the window
            //comes back on its own.
            bool obstructed{};
            if (valid)
            {
                for (const CRect& other : others)
                {
                    CRect hit;
                    if (hit.IntersectRect(union_rect, other) && !hit.IsRectEmpty())
                    {
                        obstructed = true;
                        break;
                    }
                }
            }
            m_obstructed = obstructed;
            bool changed{};
            {
                std::lock_guard<std::mutex> lock(m_rect_mutex);
                changed = (valid != m_reserved_valid) || (valid && union_rect != m_reserved_rect);
                m_reserved_valid = valid;
                if (valid)
                {
                    m_reserved_rect = union_rect;
                    //Dividing the region by the number of icons that actually form it is the
                    //only way to get the true slot pitch. Using the requested count instead
                    //under-measures whenever an icon is still hidden, and the estimate then
                    //spirals down and over-reserves.
                    m_reserved_count = found;
                }
            }
            if (changed)
            {
                HWND notify = m_notify_wnd;
                if (notify != nullptr && ::IsWindow(notify))
                    ::PostMessageW(notify, WM_SPACER_LAYOUT_CHANGED, 0, 0);
            }

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

    //按记录下来的键名删除注册表项。删除图标时系统只会清空键里的值、
    //把空键留在原地，靠内容匹配是找不到它们的。
    //Delete the registry entries by recorded name. Removing an icon only blanks the values
    //and leaves an empty key behind, which content matching would never find.
    if (!m_created_keys.empty())
    {
        HKEY key{};
        if (RegOpenKeyExW(HKEY_CURRENT_USER, NOTIFY_ICON_SETTINGS_KEY, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS)
        {
            for (const auto& name : m_created_keys)
                RegDeleteKeyW(key, name.c_str());
            RegCloseKey(key);
        }
        m_created_keys.clear();
    }

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
        m_reserved_rect.SetRectEmpty();
    }
}

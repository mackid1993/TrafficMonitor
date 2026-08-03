#include "stdafx.h"
#include "Win11TaskbarDlg.h"
#include "WindowsSettingHelper.h"

void CWin11TaskbarDlg::AdjustTaskbarWndPos(bool force_adjust)
{
    ::GetWindowRect(m_hNotify, m_rcNotify);
    ::GetWindowRect(m_hStart, m_rcStart);
    m_rcStart.MoveToXY(m_rcStart.left - m_rcTaskbar.left, m_rcStart.top - m_rcTaskbar.top);

    //Window size
    m_rect.right = m_rect.left + m_window_width;
    m_rect.bottom = m_rect.top + m_window_height;

    //竖向任务栏不支持预留空间。竖着放时通知区域是按每行几个的网格排布的，
    //把托盘"拉宽"根本不会在水平方向上腾出任何空间，占位图标一点作用都没有，
    //连续的一段也凑不出一块干净的矩形，反而会把别人的图标圈进去。
    //这种情况下彻底不做预留，回到普通的摆放方式；设置里的开关也会相应置灰并说明原因。
    //Reserving space is not supported on a vertical taskbar. Docked vertically, the tray is laid
    //out as a grid several icons wide, so widening it frees no horizontal space at all and the
    //placeholders accomplish nothing; a contiguous run also cannot form a clean rectangle there
    //and would enclose other icons. Skip the reservation entirely and fall back to ordinary
    //placement; the settings checkbox is greyed out and says why.
    if (theApp.m_taskbar_data.reserve_taskbar_space && m_taskbar_on_top_or_bottom)
    {
        //Start the timer that tracks the reserved region
        if (!m_tray_timer_started && GetSafeHwnd() != nullptr)
        {
            SetTimer(SPACER_ADJUST_TIMER, SPACER_ADJUST_INTERVAL, nullptr);
            m_tray_timer_started = true;
        }
        //有别的图标被拖进了预留区域里。这时把窗口藏起来，绝不能照常摆上去——
        //摆上去就正好盖在那个图标上面，用户既看不见它也点不到它，
        //看起来就像图标被卡在窗口底下了。图标被拖走之后自动恢复显示。
        //Another icon has been dragged into the reserved region. Hide the window rather than
        //placing it as usual: placing it would cover that icon, which could then be neither
        //seen nor clicked - it looks like the icon is trapped underneath. It comes back by
        //itself once the icon is dragged away.
        if (m_tray_reserve.IsInited() && m_tray_reserve.IsObstructed())
        {
            if (IsWindowVisible())
                ShowWindow(SW_HIDE);
            return;
        }
        //Once the placeholders exist the window sits on them and nothing else applies
        if (UpdateTrayReserve())
            return;
        //The reservation is still filling in. Hold the current position instead of falling
        //back: every icon added widens the tray, which moves the fallback target, so
        //recomputing it each tick makes the window skitter across the taskbar while the
        //region grows. Only give up and re-place once reservation has actually failed.
        //只有在窗口已经正常摆放过一次之后，"保持原位"才有意义。第一次进来时m_rect还是
        //初始值{0,0,宽,高}，保持原位等于把窗口钉在任务栏最左端、压住开始按钮。
        //Holding only makes sense once the window has been placed normally at least once. On the
        //first pass m_rect is still {0,0,w,h}, so holding pins the window to the far left of the
        //taskbar, covering the Start button.
        if (m_tray_reserve.IsInited() && m_fallback_placed)
        {
            if (!IsWindowVisible())
                ShowWindow(SW_SHOWNA);
            //Hold the position, but keep the size current. The displayed items can change
            //while the region is still filling in, and freezing the window at its old width
            //leaves it clipped - or blank - until the reservation happens to complete.
            MoveWindow(m_rect);
            return;
        }
    }
    else
    {
        //Option switched off: drop the placeholders and stop the timer
        if (m_tray_reserve.IsInited())
            m_tray_reserve.Destroy();
        if (m_tray_timer_started)
        {
            KillTimer(SPACER_ADJUST_TIMER);
            m_tray_timer_started = false;
        }
        m_last_reserved_rect.SetRectEmpty();
    }

    //Make sure the window is visible
    if (!IsWindowVisible())
        ShowWindow(SW_SHOWNA);

    if (force_adjust || m_rcNotify.Width() != m_last_notify_width || m_rcStart.left != m_last_start_pos)
    {
        m_last_notify_width = m_rcNotify.Width();
        m_last_start_pos = m_rcStart.left;

        //Place the window on the left or the right of the taskbar
        if (!ShouldMoveToLeftForWidgets())
        {
            //Horizontal position of the notification area
            int notify_x_pos = m_rcNotify.left;
            if (notify_x_pos == 0)
            {
                //Secondary Win11 displays have no tray window; use a fixed 88px clock area
                if (m_is_secondary_display)
                    notify_x_pos = m_rcTaskbar.Width() - DPI(88);
                //Otherwise fall back to the configured taskbar_right_space_win11
                else
                    notify_x_pos = m_rcTaskbar.Width() - DPI(theApp.m_taskbar_data.taskbar_right_space_win11);
            }
            //With "avoid overlapping right widgets" ticked, leave room for the widgets
            if (IsAvoidingRightWidgets())
                notify_x_pos -= DPI(theApp.m_taskbar_data.taskbar_left_space_win11);
            m_rect.MoveToX(notify_x_pos - m_rect.Width() + 2);
        }
        else
        {
            //Snug against the Start button, or hard against the left edge
            if (theApp.m_taskbar_data.tbar_wnd_snap)
                m_rect.MoveToX(m_rcStart.left - m_rect.Width() - 2);
            else
                m_rect.MoveToX(2);
        }
        //Horizontal offset from the settings
        m_rect.MoveToX(m_rect.left + DPI(theApp.m_taskbar_data.window_offset_left));

        //Vertical position. The (m_rcTaskbar.Height() - m_rcStart.Height()) term corrects
        //touch devices on Win11 22621+, where the taskbar rect is taller than the bar itself.
        //rcStart is used rather than m_rcBar because the latter collapses to zero height when
        //the taskbar holds no icons at all.
        m_rect.MoveToY((m_rcStart.Height() - m_rect.Height()) / 2
            + (m_rcTaskbar.Height() - m_rcStart.Height())
            + DPI(theApp.m_taskbar_data.window_offset_top));

        MoveWindow(m_rect);
        //到这里m_rect才是真正算出来的位置，从现在起"保持原位"才是安全的
        //Only now is m_rect a genuinely computed position, so holding it is safe from here on
        m_fallback_placed = true;
    }
}

bool CWin11TaskbarDlg::UpdateTrayReserve()
{
    //副显示器没有自己的通知区域，无法在上面预留空间
    //Secondary displays have no tray of their own, so nothing can be reserved there
    if (m_is_secondary_display)
    {
        if (m_tray_reserve.IsInited())
            m_tray_reserve.Destroy();
        return false;
    }

    m_tray_reserve.SetNotifyWindow(GetSafeHwnd());
    //按窗口实际需要的宽度预留，两边各留一点空白。
    //这里用的是实际算出来的窗口宽度，因此无论配置了多少个显示项目都能自动适应。
    //Reserve whatever the window actually needs, plus a little breathing room on each side.
    //Driven by the measured window width, so it scales with however many display items are
    //configured rather than assuming any particular number.
    m_tray_reserve.SetReservedWidth(m_window_width + DPI(RESERVE_PADDING) * 2);

    CRect rc_reserved{ 0, 0, 0, 0 };    //CRect's default constructor does not zero it
    const bool have_rect = m_tray_reserve.GetReservedRect(rc_reserved);
    if (!have_rect)
        return false;                   //placeholders not in the tray yet
    if (rc_reserved.Width() < m_window_width)
        return false;                   //region still smaller than the window; wait for it

    if (!IsWindowVisible())
        ShowWindow(SW_SHOWNA);

    //把窗口居中放在预留区域里。预留区域是按整个图标槽位凑出来的，
    //因此通常会比窗口稍宽一点。
    //Centre the window inside the reserved region. The region is made of whole icon slots,
    //so it is usually a little wider than the window.
    CRect rc_target{ rc_reserved };
    rc_target.OffsetRect(-m_rcTaskbar.left, -m_rcTaskbar.top);
    const int x = rc_target.left + (rc_target.Width() - m_window_width) / 2
        + DPI(theApp.m_taskbar_data.window_offset_left);
    const int y = rc_target.top + (rc_target.Height() - m_window_height) / 2
        + DPI(theApp.m_taskbar_data.window_offset_top);

    m_rect.MoveToXY(x, y);
    if (rc_reserved != m_last_reserved_rect || m_rect.Width() != m_window_width)
    {
        m_last_reserved_rect = rc_reserved;
        MoveWindow(m_rect);
    }
    return true;
}

bool CWin11TaskbarDlg::IsAvoidingRightWidgets() const
{
    //Purely the user's checkbox. The old extra conditions ("taskbar not centred" and "system
    //reports widgets shown") each silently disabled the option: the latter reads TaskbarDa,
    //which is 0 whenever widgets are off, so ticking the box did nothing at all.
    return theApp.m_taskbar_data.avoid_overlap_with_widgets;
}

bool CWin11TaskbarDlg::ShouldMoveToLeftForWidgets() const
{
    //Move to the left side only when:
    //1. the taskbar is centred (only then is there free space before the first button);
    //2. the "taskbar window on left" option is ticked;
    //3. that free space genuinely fits the whole window - never overlap a button for this.
    if (!CWindowsSettingHelper::IsTaskbarCenterAlign())
        return false;
    if (!theApp.m_taskbar_data.tbar_wnd_on_left)
        return false;
    //With a centred taskbar the Start button is the first button of the strip
    const int free_left = m_rcStart.left - DPI(RESERVE_PADDING);
    return m_window_width <= free_left;
}

void CWin11TaskbarDlg::OnCancel()
{
    //Drop the placeholder icons before the window goes away
    if (m_tray_timer_started && GetSafeHwnd() != nullptr)
    {
        KillTimer(SPACER_ADJUST_TIMER);
        m_tray_timer_started = false;
    }
    m_tray_reserve.Destroy();
    CTaskBarDlg::OnCancel();
}

BEGIN_MESSAGE_MAP(CWin11TaskbarDlg, CTaskBarDlg)
    ON_WM_TIMER()
    ON_MESSAGE(WM_SPACER_LAYOUT_CHANGED, &CWin11TaskbarDlg::OnSpacerLayoutChanged)
END_MESSAGE_MAP()

LRESULT CWin11TaskbarDlg::OnSpacerLayoutChanged(WPARAM wParam, LPARAM lParam)
{
    //The reserved region moved - follow it immediately instead of waiting for the timer
    if (theApp.m_taskbar_data.reserve_taskbar_space && !m_menu_popuped)
        AdjustWindowPos();
    return 0;
}

void CWin11TaskbarDlg::OnTimer(UINT_PTR nIDEvent)
{
    if (nIDEvent == SPACER_ADJUST_TIMER)
    {
        //Backstop for the notification above. Skipped while a context menu is up so the
        //window does not move out from under it.
        if (theApp.m_taskbar_data.reserve_taskbar_space && !m_menu_popuped)
            AdjustWindowPos();
        return;
    }
    CTaskBarDlg::OnTimer(nIDEvent);
}

void CWin11TaskbarDlg::InitTaskbarWnd()
{
    m_hNotify = ::FindWindowEx(m_hTaskbar, 0, L"TrayNotifyWnd", NULL);
    m_hStart = ::FindWindowEx(m_hTaskbar, nullptr, L"Start", NULL);
    ::GetWindowRect(m_hNotify, m_rcNotify);
}

void CWin11TaskbarDlg::ResetTaskbarPos()
{
}

HWND CWin11TaskbarDlg::GetParentHwnd()
{
    return m_hTaskbar;
}

bool CWin11TaskbarDlg::IsTaskbarVertical()
{
    CRect rc_taskbar;
    HWND taskbar = ::FindWindowW(L"Shell_TrayWnd", nullptr);
    if (taskbar == nullptr || !::GetWindowRect(taskbar, rc_taskbar) || rc_taskbar.IsRectEmpty())
        return false;
    return rc_taskbar.Height() > rc_taskbar.Width();
}

void CWin11TaskbarDlg::CheckTaskbarOnTopOrBottom()
{
    //Do not assume the taskbar is horizontal: third-party tools (and newer Windows 11
    //builds) can dock it to the left or right edge, and every horizontal calculation then
    //puts the window off-screen, which looks like it has vanished. Judge by its shape.
    CRect rc_taskbar;
    if (m_hTaskbar != nullptr && ::GetWindowRect(m_hTaskbar, rc_taskbar) && !rc_taskbar.IsRectEmpty())
        m_taskbar_on_top_or_bottom = (rc_taskbar.Width() >= rc_taskbar.Height());
    else
        m_taskbar_on_top_or_bottom = true;
}

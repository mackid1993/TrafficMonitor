#include "stdafx.h"
#include "Win11TaskbarDlg.h"
#include "WindowsSettingHelper.h"
#include "TaskBarDlgDrawCommon.h"

void CWin11TaskbarDlg::AdjustTaskbarWndPos(bool force_adjust)
{
    ::GetWindowRect(m_hNotify, m_rcNotify);
    ::GetWindowRect(m_hStart, m_rcStart);
    m_rcStart.MoveToXY(m_rcStart.left - m_rcTaskbar.left, m_rcStart.top - m_rcTaskbar.top);

    //Window size
    m_rect.right = m_rect.left + m_window_width;
    m_rect.bottom = m_rect.top + m_window_height;

    //竖向任务栏不支持预留空间。Windows11把竖向任务栏的通知区域排成了每行三个的网格，
    //连续的一段占位图标凑不出一块干净的矩形区域，反而会把别人的图标圈进去盖住。
    //这种情况下彻底不做预留，回到普通的摆放方式；设置里的开关也会相应置灰。
    //Reserving space is not supported on a vertical taskbar: Win11 lays that tray out as a
    //grid three icons wide, so a contiguous run of placeholders does not form a clean
    //rectangle and would enclose (and cover) other people's icons. Skip the reservation
    //entirely and fall back to ordinary placement; the settings checkbox is greyed out to match.
    if (theApp.m_taskbar_data.reserve_taskbar_space && m_taskbar_on_top_or_bottom)
    {
        //Start the timer that tracks the reserved region
        if (!m_tray_timer_started && GetSafeHwnd() != nullptr)
        {
            SetTimer(SPACER_ADJUST_TIMER, SPACER_ADJUST_INTERVAL, nullptr);
            m_tray_timer_started = true;
        }
        //Once the placeholders exist the window sits on them and nothing else applies
        if (UpdateTrayReserve())
            return;
        //Hold position only while the reservation is still being built for the first time.
        //Every icon added widens the tray and moves the fallback target, so re-placing on
        //each tick would make the window skitter across the taskbar as the region grows.
        //
        //Once it HAS been placed on the region, holding is exactly the wrong thing: if the
        //region later becomes unusable - the user drags a tray icon into the middle of the
        //block and splits it - staying put leaves the window sitting on top of that icon,
        //which is what makes an icon look trapped underneath. In that case fall through and
        //re-place normally, so the window vacates the tray instead of covering someone.
        if (m_tray_reserve.IsInited() && !m_ever_placed_on_region)
        {
            if (!IsWindowVisible())
                ShowWindow(SW_SHOWNA);
            //Hold the position, but keep the size current: the displayed items can change
            //while the region fills in, and freezing the old width leaves the window clipped.
            MoveWindow(m_rect);
            return;
        }
        //预留区域用不了了（例如用户把别的图标拖进了块里）。
        //这时直接把窗口隐藏起来，而不是跳回任务栏上的默认位置：
        //那个位置在另一头，窗口会突然飞很远，有时还会压到别的东西上。
        //隐藏安静得多，区域一可用就自动重新显示。
        //The region is no longer usable (e.g. an icon was dragged into the block). Hide the
        //window rather than jumping back to the default taskbar position: that position is way
        //off at the other end, so the window flies across the screen and can land on top of
        //something else. Hiding is quiet, and it reappears the moment the region is usable.
        if (m_tray_reserve.IsInited())
        {
            SetDimmed(true);
            //淡出期间让后台线程保持高频查询，区域一恢复就能立刻亮回来
            //Keep the background thread polling briskly while dimmed, so the window comes
            //back the moment the region is usable again instead of up to five seconds later
            m_tray_reserve.Poke();
            return;
        }
    }
    else
    {
        //Option switched off (or unsupported here): drop the placeholders and stop the timer.
        //取消淡出必须放在最前面：淡出时窗口是半透明、而且对鼠标完全穿透的，
        //如果就这么关掉开关，窗口会永远停在这个状态——看得见却点不着，只能重启程序。
        //Un-dim first: a dimmed window is semi-transparent AND click-through, so switching the
        //option off while dimmed would strand it that way forever - visible but unclickable,
        //recoverable only by restarting the app.
        SetDimmed(false);
        if (m_tray_reserve.IsInited())
            m_tray_reserve.Destroy();
        if (m_tray_timer_started)
        {
            KillTimer(SPACER_ADJUST_TIMER);
            m_tray_timer_started = false;
        }
        m_last_reserved_rect.SetRectEmpty();
        m_ever_placed_on_region = false;
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
    }
}

bool CWin11TaskbarDlg::UpdateTrayReserve()
{
    //副显示器没有自己的通知区域，无法在上面预留空间
    //Secondary displays have no tray of their own, so nothing can be reserved there
    if (m_is_secondary_display)
    {
        SetDimmed(false);
        if (m_tray_reserve.IsInited())
            m_tray_reserve.Destroy();
        return false;
    }

    //预留机制在当前系统上失效时（例如Windows改了IsPromoted的行为），
    //彻底放弃并清理掉已经添加的占位图标，回到普通的摆放方式。
    //When the mechanism turns out not to work on this system (say a Windows update changes
    //how IsPromoted behaves), give up for good, clean the placeholders away and fall back to
    //ordinary placement rather than leaving the window lost or the tray littered.
    if (!m_tray_reserve.IsHealthy())
    {
        SetDimmed(false);
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
    m_ever_placed_on_region = true;
    SetDimmed(false);
    return true;
}

void CWin11TaskbarDlg::SetDimmed(bool dimmed)
{
    if (dimmed == m_dimmed)
        return;
    if (GetSafeHwnd() == nullptr)
        return;
    const LONG ex_style = ::GetWindowLong(m_hWnd, GWL_EXSTYLE);
    const bool layered = ((ex_style & WS_EX_LAYERED) != 0);

    //每种绘制方式设置不透明度的接口都不一样，用错一个就会把渲染搞坏，因此按绘制方式分开处理。
    //Each render path has exactly one correct way to set opacity, and using another one breaks
    //rendering outright - so dispatch on the render path rather than guessing.
    bool faded{ false };
    if (layered)
    {
        switch (m_supported_render_enums.GetAutoFitEnum())
        {
            using namespace DrawCommonHelper;
        case RenderType::D2D1:
            //这条路径是用UpdateLayeredWindowIndirect呈现的，必须改混合函数里的整体alpha。
            //绝对不能用SetLayeredWindowAttributes：这两个接口互斥，一旦调用了后者，
            //UpdateLayeredWindowIndirect就会一直返回失败，程序会据此认定D2D不可用、
            //弹出错误提示并退回GDI绘图——淡出一次就把渲染方式降级了。
            //This path presents with UpdateLayeredWindowIndirect, so the whole-window alpha
            //belongs in the blend function. SetLayeredWindowAttributes must NOT be used: the
            //two are mutually exclusive, and once it has been called UpdateLayeredWindowIndirect
            //fails forever. The app reads that failure as "D2D is broken", shows an error and
            //drops to GDI - so a single fade would permanently downgrade rendering.
            CTaskBarDlgDrawBuffer::SetConstantAlpha(dimmed ? DIM_ALPHA : 0xFF);
            faded = true;
            break;
        case RenderType::DEFAULT:
            //GDI色键抠像，这里SetLayeredWindowAttributes才是正确的接口
            //The GDI colour-key path - here SetLayeredWindowAttributes is the right tool
            if (dimmed)
            {
                const bool use_colorkey = (theApp.m_taskbar_data.transparent_color != 0
                    && theApp.m_taksbar_transparent_color_enable);
                ::SetLayeredWindowAttributes(m_hWnd, theApp.m_taskbar_data.transparent_color,
                    DIM_ALPHA, use_colorkey ? (LWA_COLORKEY | LWA_ALPHA) : LWA_ALPHA);
            }
            else
            {
                ApplyWindowTransparentColor();      //restores the plain colour key
            }
            faded = true;
            break;
        default:
            //DComposition呈现，没有可以直接调的整体不透明度 / no whole-window alpha to set
            break;
        }
    }

    if (faded)
    {
        if (!IsWindowVisible())
            ShowWindow(SW_SHOWNA);
    }
    else
    {
        //这条绘制路径调不了不透明度，只能退而求其次把窗口藏起来
        //Opacity is not available on this path, so fall back to hiding the window
        if (dimmed)
        {
            if (IsWindowVisible())
                ShowWindow(SW_HIDE);
        }
        else if (!IsWindowVisible())
        {
            ShowWindow(SW_SHOWNA);
        }
    }

    //淡出期间让窗口对鼠标完全穿透，否则下面的图标看得见却拖不出来。
    //取消淡出时无论走的是哪条分支都必须把这个样式清掉：漏掉一次，
    //窗口就会一直点不着，用户只能重启程序。
    //Click-through while dimmed, or the icon underneath is visible but cannot be dragged back
    //out. On un-dim the style must be cleared on EVERY path - miss it once and the window stays
    //permanently unclickable with no way back short of restarting the app.
    if (dimmed)
        ::SetWindowLong(m_hWnd, GWL_EXSTYLE, ex_style | WS_EX_TRANSPARENT);
    else
        ::SetWindowLong(m_hWnd, GWL_EXSTYLE, ex_style & ~WS_EX_TRANSPARENT);

    //强制重画，使新的不透明度立刻生效 / repaint so the new opacity takes effect at once
    Invalidate();
    UpdateWindow();
    m_dimmed = dimmed;
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

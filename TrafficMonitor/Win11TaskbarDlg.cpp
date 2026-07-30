#include "stdafx.h"
#include "Win11TaskbarDlg.h"
#include "WindowsSettingHelper.h"

void CWin11TaskbarDlg::AdjustTaskbarWndPos(bool force_adjust)
{
    ::GetWindowRect(m_hNotify, m_rcNotify);
    ::GetWindowRect(m_hStart, m_rcStart);
    m_rcStart.MoveToXY(m_rcStart.left - m_rcTaskbar.left, m_rcStart.top - m_rcTaskbar.top);

    //设置窗口大小
    m_rect.right = m_rect.left + m_window_width;
    m_rect.bottom = m_rect.top + m_window_height;

    //启用了“为任务栏窗口预留空间”时，通过任务栏占位按钮预留空间。
    //预留成功时窗口已被移动到预留的区域上，不再执行下面的调整逻辑；
    //预留还未生效时（例如占位按钮还没有显示出来），仍按原来的方式调整位置。
    if (theApp.m_taskbar_data.reserve_taskbar_space)
    {
        //启动跟踪占位按钮位置变化的定时器
        if (!m_spacer_timer_started && GetSafeHwnd() != nullptr)
        {
            SetTimer(SPACER_ADJUST_TIMER, SPACER_ADJUST_INTERVAL, nullptr);
            m_spacer_timer_started = true;
        }
        if (AdjustWndPosBySpacer(force_adjust))
        {
            m_positioned_by_spacer = true;
            m_spacer_fail_count = 0;
            return;
        }
        //预留区域暂时失效时（例如任务栏正在重新布局或者正在重新创建占位按钮），
        //短时间内保持窗口位置不变，避免窗口在预留的位置和原来的位置之间来回跳动
        if (m_positioned_by_spacer)
        {
            m_spacer_fail_count++;
            if (m_spacer_fail_count <= SPACER_FAIL_GRACE_COUNT)
                return;
            m_positioned_by_spacer = false;
        }
    }
    else
    {
        //选项被关闭时删除占位按钮并停止定时器
        if (m_button_spacer.IsInited())
            m_button_spacer.Destroy();
        if (m_spacer_timer_started)
        {
            KillTimer(SPACER_ADJUST_TIMER);
            m_spacer_timer_started = false;
        }
        m_positioned_by_spacer = false;
    }

    //确保窗口可见（窗口可能在调整占位按钮期间被隐藏）
    if (!IsWindowVisible())
        ShowWindow(SW_SHOWNA);

    if (force_adjust || m_rcNotify.Width() != m_last_notify_width || m_rcStart.left != m_last_start_pos)   //如果最小化窗口的宽度改变了，重新设置任务栏窗口的位置
    {
        m_last_notify_width = m_rcNotify.Width();
        m_last_start_pos = m_rcStart.left;
        //任务窗口显示在右侧时，或者Windows11下任务栏左对齐时
        //（Windows11下，如果任务栏设置为左对齐，即使在“任务栏窗口设置”中设置了任务窗口显示在左边，窗口仍然显示在右边）
        if (!theApp.m_taskbar_data.tbar_wnd_on_left || !CWindowsSettingHelper::IsTaskbarCenterAlign())
        {
            ////靠近任务栏图标的情况
            //if (theApp.m_taskbar_data.tbar_wnd_snap && IsTaskbarCloseToIconEnable(theApp.m_taskbar_data.tbar_wnd_on_left))
            //{
            //    m_rect.MoveToX(m_rcMin.right + 2);
            //}
            ////靠近通知区的情况
            //else
            //{
            //通知区窗口的水平位置
            int notify_x_pos = m_rcNotify.left;
            //没有获取到通知区位置的情况
            if (notify_x_pos == 0)
            {
                //Win11副屏没有通知区窗口，这里使用固定的值（88像素的系统时间区域）
                if (m_is_secondary_display)
                    notify_x_pos = m_rcTaskbar.Width() - DPI(88);
                //如果不是副屏，但是仍然没有获取到通知区域的位置，使用配置文件中taskbar_right_space_win11指定的值
                else
                    notify_x_pos = m_rcTaskbar.Width() - DPI(theApp.m_taskbar_data.taskbar_right_space_win11);
            }
            //如果显示了小组件，并且任务栏靠左显示，则留出小组件的位置
            if (theApp.m_taskbar_data.avoid_overlap_with_widgets && CWindowsSettingHelper::IsTaskbarWidgetsBtnShown() && !CWindowsSettingHelper::IsTaskbarCenterAlign())
                m_rect.MoveToX(notify_x_pos - m_rect.Width() + 2 - DPI(theApp.m_taskbar_data.taskbar_left_space_win11));
            else
                m_rect.MoveToX(notify_x_pos - m_rect.Width() + 2);
            //}
        }
        //任务栏窗口显示在左侧时
        else
        {
            //靠近“开始”按钮
            if (theApp.m_taskbar_data.tbar_wnd_snap)
            {
                m_rect.MoveToX(m_rcStart.left - m_rect.Width() - 2);
            }
            //靠近最左侧
            else
            {
                if (CWindowsSettingHelper::IsTaskbarWidgetsBtnShown())
                    m_rect.MoveToX(2 + DPI(theApp.m_taskbar_data.taskbar_left_space_win11));
                else
                    m_rect.MoveToX(2);
            }
        }
        //水平偏移
        m_rect.MoveToX(m_rect.left + DPI(theApp.m_taskbar_data.window_offset_left));
        ////确保水平方向不超出屏幕边界
        //if (m_rect.left < 0)
        //    m_rect.MoveToX(0);
        //if (m_rcTaskbar.Width() > m_rect.Width() && m_rect.right > m_rcTaskbar.Width())
        //    m_rect.MoveToX(m_rcTaskbar.Width() - m_rect.Width());

        //设置任务栏窗口的垂直位置
        //注：这里加上(m_rcTaskbar.Height() - rcStart.Height())用于修正Windows11 build 22621版本后触屏设备任务栏窗口位置不正确的问题。
        //在这种情况下m_rcTaskbar的高度要大于m_rcBar的高度，正常情况下，它们的高度相同
        //但是当任务栏上没有任何图标时，m_rcBar的高度会变为0，因此使用rcStart代替
        m_rect.MoveToY((m_rcStart.Height() - m_rect.Height()) / 2 + (m_rcTaskbar.Height() - m_rcStart.Height()) + DPI(theApp.m_taskbar_data.window_offset_top));
        ////确保垂直方向不超出屏幕边界
        //if (m_rect.top < 0)
        //    m_rect.MoveToY(0);
        //if (m_rcTaskbar.Height() > m_rect.Height() && m_rect.bottom > m_rcTaskbar.Height())
        //    m_rect.MoveToY(m_rcTaskbar.Height() - m_rect.Height());

        MoveWindow(m_rect);
    }
}

bool CWin11TaskbarDlg::AdjustWndPosBySpacer(bool force_adjust)
{
    //占位按钮只在主显示器的任务栏中使用
    if (m_is_secondary_display)
        return false;
    //需要预留的宽度加上少量边距，避免窗口紧贴两侧的按钮
    m_button_spacer.SetNotifyWindow(GetSafeHwnd());
    m_button_spacer.SetRequiredWidth(m_rect.Width() + DPI(4));
    CRect rc_reserved;
    if (!m_button_spacer.GetReservedRect(rc_reserved))
        return false;
    //屏幕坐标转换为相对任务栏的坐标
    int target_x = rc_reserved.left - m_rcTaskbar.left;
    int target_y = rc_reserved.top - m_rcTaskbar.top;

    //让窗口完全覆盖占位按钮所在的区域。
    //宽度：向右扩展到通知区域的左边界，一方面遮挡住占位按钮，
    //另一方面不在占位按钮和通知区域之间留下空隙（空隙可以作为拖动其它按钮时的放置位置）
    int right_limit = m_rcNotify.left - m_rcTaskbar.left;
    if (right_limit > target_x && right_limit - target_x > m_rect.Width())
        m_rect.right = m_rect.left + (right_limit - target_x);
    else if (rc_reserved.Width() > m_rect.Width())
        m_rect.right = m_rect.left + rc_reserved.Width();
    //高度：向下扩展到任务栏的底部。任务栏在按钮的下方绘制表示“正在运行”的指示器，
    //指示器可能位于按钮矩形区域的外面（任务栏使用默认大小时更明显），
    //因此窗口需要一直延伸到任务栏底部才能完全遮挡住指示器
    int bottom_limit = m_rcTaskbar.Height();
    if (bottom_limit > target_y && bottom_limit - target_y > m_rect.Height())
        m_rect.bottom = m_rect.top + (bottom_limit - target_y);
    if (force_adjust || target_x != m_rect.left || target_y != m_rect.top)
    {
        m_rect.MoveToXY(target_x, target_y);
        MoveWindow(m_rect);
    }
    return true;
}

void CWin11TaskbarDlg::OnCancel()
{
    //关闭窗口前删除任务栏占位按钮
    if (m_spacer_timer_started && GetSafeHwnd() != nullptr)
    {
        KillTimer(SPACER_ADJUST_TIMER);
        m_spacer_timer_started = false;
    }
    m_button_spacer.Destroy();
    CTaskBarDlg::OnCancel();
}

BEGIN_MESSAGE_MAP(CWin11TaskbarDlg, CTaskBarDlg)
    ON_WM_TIMER()
    ON_MESSAGE(WM_SPACER_LAYOUT_CHANGED, &CWin11TaskbarDlg::OnSpacerLayoutChanged)
END_MESSAGE_MAP()

LRESULT CWin11TaskbarDlg::OnSpacerLayoutChanged(WPARAM wParam, LPARAM lParam)
{
    //后台线程检测到有其它程序的按钮进入了占位按钮的区域，立即重新调整窗口位置
    if (theApp.m_taskbar_data.reserve_taskbar_space && !m_menu_popuped)
        AdjustWindowPos();
    return 0;
}

void CWin11TaskbarDlg::OnTimer(UINT_PTR nIDEvent)
{
    if (nIDEvent == SPACER_ADJUST_TIMER)
    {
        //占位按钮的位置会随着其它任务栏按钮的增减而变化，
        //这里以比主定时器更短的间隔调整窗口位置，减少窗口位置变化的滞后。
        //右键菜单弹出时不调整窗口位置，避免菜单显示过程中窗口移动
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

void CWin11TaskbarDlg::CheckTaskbarOnTopOrBottom()
{
    m_taskbar_on_top_or_bottom = true;
}

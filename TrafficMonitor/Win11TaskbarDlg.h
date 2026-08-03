#pragma once
#include "TaskBarDlg.h"
#include "TaskbarButtonSpacer.h"
class CWin11TaskbarDlg :
    public CTaskBarDlg
{
public:
    virtual void OnCancel() override;

    //任务栏是否竖着放（在屏幕左边或右边）。按任务栏窗口的形状判断，不读注册表：
    //竖向任务栏目前还是预览功能，相关的注册表项随时可能改名或改含义，
    //而窗口的形状是实打实的，怎么改都不会错。
    //Whether the taskbar is docked vertically (screen left or right). Judged from the shape of
    //the taskbar window rather than the registry: the vertical taskbar is still a preview
    //feature whose registry values can be renamed or redefined at any time, while the window's
    //shape is ground truth and cannot be wrong.
    static bool IsTaskbarVertical();

protected:
    afx_msg void OnTimer(UINT_PTR nIDEvent);
    //Reposition the taskbar window as soon as the reserved region moves
    afx_msg LRESULT OnSpacerLayoutChanged(WPARAM wParam, LPARAM lParam);
    DECLARE_MESSAGE_MAP()

private:
    //Timer that tracks the reserved region. The background thread posts
    //WM_SPACER_LAYOUT_CHANGED on any change, so this is only a backstop - hence the long
    //interval, which keeps power use down.
    static constexpr UINT_PTR SPACER_ADJUST_TIMER = 1961;
    static constexpr UINT SPACER_ADJUST_INTERVAL = 1000;
    //窗口两侧额外预留的空白（逻辑像素，会按DPI缩放），使内容不至于紧贴旁边的托盘图标。
    //这个值要留得很小：预留区域是按整个图标槽位（约42像素）向上取整的，
    //多留几像素就可能多占用一整个槽位，反而在两边留下很大的空隙。
    //Extra padding reserved on each side of the window (logical px, DPI scaled) so its
    //contents do not sit flush against the neighbouring tray icons. Keep this small: the
    //region is rounded up to whole icon slots (~42px), so a few extra pixels can cost an
    //entire slot and leave a conspicuous gap on both sides.
    static constexpr int RESERVE_PADDING = 2;

    // inherited from CTaskBarDlg
    void InitTaskbarWnd() override;
    virtual void AdjustTaskbarWndPos(bool force_adjust) override;
    void ResetTaskbarPos() override;
    virtual HWND GetParentHwnd() override;
    void CheckTaskbarOnTopOrBottom() override;

    //Reserve space in the tray and place the window in it. Returns true once the window has
    //been placed; returns false while the reserved region has not appeared yet, so the
    //caller falls back to the original placement.
    bool UpdateTrayReserve();

    //Whether to leave room for widgets on the right. Driven purely by the user's checkbox:
    //widget detection relies on a registry value that can read 0 even when widgets are
    //shown, so second-guessing the user only makes the option do nothing.
    bool IsAvoidingRightWidgets() const;
    //Whether to move the window to the taskbar's left side. Only when the taskbar is
    //centred, the option is ticked, AND the free space there fits the whole window -
    //otherwise it stays on the right rather than overlapping any button.
    bool ShouldMoveToLeftForWidgets() const;

    HWND m_hNotify;     //handle of the notification area
    HWND m_hStart;      //handle of the Start button
    CRect m_rcNotify;   //rect of the notification area
    CRect m_rcStart;    //rect of the Start button
    int m_last_notify_width{};
    int m_last_start_pos{};

    //Placeholder tray icons reserving the space
    CTaskbarTrayReserve m_tray_reserve;
    bool m_tray_timer_started{ false };
    //Where the reserved region was last time we placed the window, to detect movement
    CRect m_last_reserved_rect{ 0, 0, 0, 0 };
    //窗口是否曾经成功摆放到预留区域上。这是一个只会置位、不会复位的标志：
    //一旦成功过，之后再拿不到可用的区域就必须隐藏，而不能"保持原位"——
    //保持原位那条分支会把窗口重新显示出来，正好压在刚被拖进来的那个图标上。
    //Whether the window has ever been placed on the reserved region. This latches on and is
    //never cleared: once it has been placed, losing the region must hide the window rather
    //than hold position, because the hold branch re-shows it at its old rect - directly on
    //top of whatever was just dragged in.
    bool m_ever_placed_on_region{ false };

    //预留区域暂时不能用时，把窗口淡下去而不是直接消失，看起来柔和一些。
    //只有窗口确实是分层窗口时才能这么做：D2D+DComposition的绘制方式下窗口没有
    //WS_EX_LAYERED，硬加上去会破坏它的绘制，这种情况下退回到直接隐藏。
    //Dim the window instead of making it vanish outright when the region is briefly
    //unusable - it reads much better. Only possible while the window is actually layered:
    //under the D2D + DComposition render path it has no WS_EX_LAYERED and forcing one on
    //breaks its drawing, so that case falls back to hiding.
    void SetDimmed(bool dimmed);
    bool m_dimmed{ false };
    //淡出后的不透明度（0~255）/ opacity when dimmed (0-255)
    static constexpr BYTE DIM_ALPHA = 60;
};

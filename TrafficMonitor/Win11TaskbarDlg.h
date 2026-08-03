#pragma once
#include "TaskBarDlg.h"
#include "TaskbarButtonSpacer.h"
class CWin11TaskbarDlg :
    public CTaskBarDlg
{
public:
    virtual void OnCancel() override;

    //任务栏是否竖着放（贴在屏幕左边或右边）。按任务栏窗口的形状判断，不读注册表：
    //竖向任务栏目前还是预览功能，相关的注册表项随时可能改名或改含义
    //（实测竖着放时StuckRects3里记录的仍然是"底部"），而窗口的形状是实打实的。
    //Whether the taskbar is docked vertically (screen left or right). Judged from the shape of
    //the taskbar window rather than the registry: the vertical taskbar is still a preview
    //feature whose registry values can be renamed or redefined at any time (measured: with the
    //bar docked vertically, StuckRects3 still recorded "bottom"), while the window's shape is
    //ground truth.
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
    //窗口两侧额外预留的空白（逻辑像素，按DPI缩放），使内容不至于紧贴旁边的托盘图标。
    //这个值要留得很小：预留区域是按整个图标槽位（约42像素）向上取整的，
    //多留几像素就可能多占一整个槽位，反而在窗口两边留下很大的空隙。
    //Extra padding reserved on each side of the window (logical px, DPI scaled) so its contents
    //do not sit flush against the neighbouring tray icons. Keep this small: the region is
    //rounded up to whole icon slots (~42px), so a few extra pixels can cost an entire slot and
    //leave a conspicuous gap on both sides of the window.
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
    //预留区域建好之前，窗口保持隐藏的最长时间。
    //实测占位图标一个一个加进去、区域建成大约要两秒，所以正常情况下只会隐藏这么久。
    //一直建不起来时（占位图标没能被标记为始终显示、用户在设置里把它们关掉、
    //外壳拒绝添加等）不能就这样一直不显示，超过这个时限就回到普通的摆放方式。
    //取8秒：既给了正常情况四倍的余量，万一失败也不会让用户对着空任务栏发呆太久。
    //Longest the window stays hidden while the reserved region is being built. Measured, the
    //placeholders go in one per tick and the region is ready in about two seconds, so that is
    //all the delay there normally is. If it never becomes ready - placeholders not marked
    //always-visible, the user switching them off in Settings, the shell refusing adds - staying
    //invisible forever is not acceptable, so past this deadline it falls back to ordinary
    //placement. Eight seconds gives the normal case 4x headroom without leaving the user staring
    //at an empty taskbar for long when it fails.
    static constexpr ULONGLONG HOLD_TIMEOUT = 8000;
    ULONGLONG m_hold_deadline{};
    //首次调用时开始计时，之后返回同一个截止时间 / starts the clock on first call
    ULONGLONG HoldDeadline();
};

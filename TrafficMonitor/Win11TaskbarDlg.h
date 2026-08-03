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
    //窗口是否已经按普通方式摆放过一次。还没摆放过时m_rect仍是初始的{0,0,宽,高}，
    //也就是任务栏最左端。"保持原位"那条分支会照着这个矩形显示窗口，于是窗口压在
    //开始按钮上面，而且一直压到预留区域建成为止——建不成就是永远。
    //资源管理器重启时对话框会被销毁重建，走的正是这条路径。
    //Whether the window has been placed the ordinary way at least once. Before that m_rect is
    //still its initial {0,0,w,h} - the far left of the taskbar. The hold branch shows the window
    //at that rect, so it covers the Start button until the reserved region is built, which is
    //forever if the reservation never completes. An Explorer restart destroys and rebuilds this
    //dialog and takes exactly that path.
    bool m_fallback_placed{ false };
};

#pragma once
#include "TaskBarDlg.h"
#include "TaskbarButtonSpacer.h"
class CWin11TaskbarDlg :
    public CTaskBarDlg
{
public:
    virtual void OnCancel() override;

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
    //Extra padding reserved around the window (logical px, DPI scaled) so its contents do
    //not sit flush against the neighbouring tray icons
    static constexpr int RESERVE_PADDING = 8;

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
};

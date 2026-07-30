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
    //占位按钮的位置需要调整时立即调整任务栏窗口的位置
    afx_msg LRESULT OnSpacerLayoutChanged(WPARAM wParam, LPARAM lParam);
    DECLARE_MESSAGE_MAP()

private:
    //用于跟踪占位按钮位置变化的定时器。
    //占位按钮的位置发生变化时，后台线程会立即发送WM_SPACER_LAYOUT_CHANGED消息来调整窗口位置，
    //这个定时器只作为兜底手段，因此使用较长的间隔以减少耗电
    static constexpr UINT_PTR SPACER_ADJUST_TIMER = 1961;
    static constexpr UINT SPACER_ADJUST_INTERVAL = 1000;
    //预留空间暂时失效时（例如任务栏正在重新布局），保持窗口位置不变的最大次数，
    //避免窗口在预留位置和原来的位置之间来回跳动
    static constexpr int SPACER_FAIL_GRACE_COUNT = 20;
    // 通过 CTaskBarDlg 继承
    void InitTaskbarWnd() override;
    virtual void AdjustTaskbarWndPos(bool force_adjust) override;
    void ResetTaskbarPos() override;
    virtual HWND GetParentHwnd() override;

    //在启用了“为任务栏窗口预留空间”时通过通知区域占位图标预留空间，
    //成功预留时把窗口移动到预留区域上并返回true，否则返回false
    bool AdjustWndPosBySpacer(bool force_adjust);

    HWND m_hNotify;     //任务栏通知区域的句柄
    HWND m_hStart;      //开始按钮的句柄
    CRect m_rcNotify;   //任务栏通知区域的矩形区域
    CRect m_rcStart;     //开始按钮的矩形区域
    int m_last_notify_width{};
    int m_last_start_pos{};

    CTaskbarButtonSpacer m_button_spacer;   //任务栏占位按钮，用于为任务栏窗口预留空间
    bool m_positioned_by_spacer{ false };   //窗口当前是否位于占位按钮预留的位置上
    int m_spacer_fail_count{};              //连续获取预留区域失败的次数
    bool m_spacer_timer_started{ false };   //跟踪占位按钮位置的定时器是否已启动

    // 通过 CTaskBarDlg 继承
    void CheckTaskbarOnTopOrBottom() override;

};


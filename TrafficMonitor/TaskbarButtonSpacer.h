#pragma once
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <atlbase.h>

//通过在通知区域中添加若干个完全透明的占位图标，为任务栏窗口预留空间。
//Reserve space for the taskbar window by adding fully transparent placeholder icons to the
//notification area (system tray).
//
//原理：Windows11的任务栏是一个两列的布局，任务按钮区域的右边界就是通知区域的左边界。
//通知区域变宽时，任务按钮区域会相应变窄、按钮重新排布，绝不会侵占通知区域的地盘。
//How it works: the Win11 taskbar is a two-column layout and the task button area's right
//edge IS the tray's left edge. Widen the tray and the button strip repacks to stay clear
//of it - the strip never encroaches on the tray's territory.
//
//这一点和任务按钮完全不同：任务栏排满时占位按钮会被系统收进溢出菜单（“...”），
//而通知区域的图标不会被挤掉。因此这是唯一能真正预留出空间的办法。
//This differs fundamentally from placeholder task buttons: when the taskbar fills up Windows
//demotes those into the overflow flyout, whereas tray icons are never pushed out. It is the
//only mechanism that genuinely reserves space.
//
//只使用公开的接口：占位图标用Shell_NotifyIcon注册，位置用UI自动化在后台线程中查询
//（UI自动化的跨进程调用会抽取消息队列，在界面线程中调用会导致重入问题）。
//不写注册表、不注入任何进程。
//Only public interfaces are used: the placeholders are registered with Shell_NotifyIcon and
//their positions are queried through UI Automation on a background thread (its cross-process
//calls pump the message queue, which would re-enter the UI thread). Nothing is written to the
//registry and nothing is injected into any other process.
//
//注意：通知区域图标是否显示由用户设置决定
//（设置 - 个性化 - 任务栏 - 其它系统托盘图标）。实测新注册的图标默认就是显示的，
//但用户把它关掉时占位图标就预留不到任何宽度；这种情况下IsHealthy会返回false，
//调用方应当放弃预留、回到普通的摆放方式。
//Note: whether a tray icon is shown is the user's setting (Settings - Personalisation -
//Taskbar - Other system tray icons). Newly registered icons are shown by default in practice,
//but if the user turns them off the placeholders reserve no width at all; IsHealthy then
//reports false and the caller should give up and fall back to normal placement.
class CTaskbarTrayReserve
{
public:
    ~CTaskbarTrayReserve();

    //设置需要预留的宽度（物理像素）。宽度为0时移除所有占位图标。需要周期性调用。
    //Set how much to reserve, in physical pixels. 0 removes every placeholder icon.
    //Call this periodically.
    void SetReservedWidth(int width);

    //取得占位图标当前占据的屏幕区域。还没有任何占位图标出现在通知区域中时返回false。
    //Get the screen rect the placeholder icons currently occupy. Returns false until at
    //least one of them has actually appeared in the tray.
    bool GetReservedRect(CRect& rect) const;

    //设置接收通知的窗口。预留区域的位置发生变化时会立即发送WM_SPACER_LAYOUT_CHANGED，
    //使任务栏窗口不必等待定时器的下一次触发就能跟上。
    //Window to notify. When the reserved region moves, WM_SPACER_LAYOUT_CHANGED is posted so
    //the taskbar window can follow immediately instead of waiting for the next timer tick.
    void SetNotifyWindow(HWND hwnd) { m_notify_wnd = hwnd; }

    bool IsInited() const { return !m_icons.empty(); }

    //移除所有占位图标 / remove every placeholder icon
    void Destroy();

    //催后台线程立刻重新查询一次。预留区域稳定之后轮询间隔会拉长到数秒，
    //窗口正等着区域恢复时，这个延迟会让它迟迟不恢复。
    //Nudge the background thread to re-query at once. Once the region settles the poll
    //interval stretches to seconds, which makes recovery feel stuck when the window is
    //waiting for the region to come back.
    void Poke() { if (m_wake_event != nullptr) SetEvent(m_wake_event); }

    //这套机制在当前系统上是否还能用。图标都添加了却始终拿不到可用的区域时，
    //说明它们没有显示出来，调用方应当彻底放弃预留、回到普通的摆放方式——
    //宁可没有预留，也不能把用户的窗口搞丢。
    //Whether the mechanism still works here. If the icons were added but a usable region never
    //appears they are not being shown, and the caller should abandon the reservation and fall
    //back to ordinary placement - better no reservation than a lost window.
    bool IsHealthy() const;

private:
    //最多添加多少个占位图标。窗口的显示项目可以很多，因此上限留得宽裕。
    //Maximum placeholder icons; the window can carry a lot of display items.
    static constexpr int MAX_ICONS = 48;
    //通知区域中单个图标占用的宽度，仅作为初值，之后按实测值修正
    //Width one tray icon occupies; a seed value only, corrected by measurement
    static constexpr int ICON_SLOT_WIDTH = 42;
    //后台线程查询图标位置的间隔（毫秒）。布局变化时WinEvent钩子会立即唤醒它，
    //因此轮询只是兜底：变化后用较短的间隔，稳定后用很长的间隔以省电。
    //Query interval (ms). A WinEvent hook wakes the thread on layout changes, so polling is
    //only a backstop: short interval after a change, long once things settle.
    static constexpr int QUERY_INTERVAL_FAST = 250;
    static constexpr int QUERY_INTERVAL_SLOW = 5000;
    static constexpr int STABLE_COUNT_FOR_SLOW = 8;
    //两次查询之间的最小间隔（毫秒），避免任务栏动画期间频繁查询占用CPU
    //Minimum gap between queries (ms) so taskbar animations don't burn CPU
    static constexpr ULONGLONG MIN_QUERY_GAP = 200;
    //添加图标可能会暂时失败（外壳还没准备好等）。失败后退避，间隔逐次加长，
    //但绝不永久放弃，否则预留区域会永远不够宽。
    //Adding an icon can fail transiently. Back off with a growing delay after a failure, but
    //never give up for good or the region stays too small forever.
    static constexpr ULONGLONG RETRY_COOLDOWN = 2000;
    static constexpr int MAX_BACKOFF_STEPS = 8;
    //添加图标后等多久仍拿不到可用区域就认定机制不可用（毫秒）
    //How long after adding icons to declare the mechanism unavailable (ms)
    static constexpr ULONGLONG HEALTH_GRACE = 30000;

    bool EnsureWindow();
    bool AddIcon(int index);
    void RemoveIcon(int index);
    static GUID MakeIconGuid(int index);

    //单个占位图标实际占用的宽度。先用按DPI缩放的估计值，再按屏幕上真实的图标修正，
    //而且只允许改小：改小意味着需要更多图标、区域变宽，量出来的间距不变，能够收敛；
    //允许改大则会删掉图标、区域变窄、间距又变大，来回震荡。
    //Width a single placeholder buys. Seeded from a DPI-scaled guess, then corrected from the
    //icons actually on screen - but only ever downwards, which makes it converge: a smaller
    //slot means more icons and a wider region, which measures the same pitch again. Letting it
    //grow would remove icons, narrow the region and start it oscillating.
    int GetSlotWidth() const;
    void RefineSlotWidth(int reserved_width, int icon_count);

    void EnsureQueryThread();
    void QueryThreadProc();
    //WinEvent钩子回调（WINEVENT_OUTOFCONTEXT模式，事件在本进程中接收，不注入其它进程）
    //WinEvent hook callback (OUTOFCONTEXT - received in our process, nothing is injected)
    static void CALLBACK WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
        LONG id_object, LONG id_child, DWORD event_thread, DWORD event_time);

    HWND m_wnd{};                               //拥有占位图标的隐藏窗口 / hidden owner window
    HICON m_icon{};                             //完全透明的图标 / the fully transparent icon
    std::vector<int> m_icons;                   //已添加的占位图标序号 / indices currently added

    std::thread m_query_thread;
    std::atomic<bool> m_thread_exit{ false };
    std::atomic<int> m_icon_count{ 0 };         //供后台线程读取 / read by the background thread
    std::atomic<HWND> m_notify_wnd{};
    HANDLE m_wake_event{};
    HWINEVENTHOOK m_win_event_hook{};
    static CTaskbarTrayReserve* m_instance;     //供WinEvent回调使用 / for the WinEvent callback

    int m_failed_attempts{};                    //连续添加失败的次数 / consecutive add failures
    ULONGLONG m_retry_after{};                  //在此之前不再重试 / do not retry before this
    ULONGLONG m_first_icon_tick{};              //第一个图标添加的时间 / when the first icon appeared
    std::atomic<bool> m_ever_valid{ false };    //是否曾拿到过可用区域 / a usable region was seen

    mutable std::mutex m_rect_mutex;
    mutable int m_slot_width{};                 //未测量时为0 / 0 until measured
    int m_reserved_count{};                     //构成预留区域的图标数 / icons forming the region
    CRect m_reserved_rect;                      //占位图标的并集，访问时加锁 / union, locked
    bool m_reserved_valid{ false };
};

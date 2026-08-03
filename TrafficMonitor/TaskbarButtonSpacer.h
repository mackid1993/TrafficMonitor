#pragma once
#include <vector>
#include <string>
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
//而通知区域的图标不会被挤掉。因此这是唯一能真正预留出空间的办法，
//也正因为如此，窗口不可能再和任何任务栏按钮重叠。
//This differs fundamentally from placeholder task buttons: when the taskbar fills up Windows
//demotes those into the overflow flyout, whereas tray icons are never pushed out. It is the
//only mechanism that genuinely reserves space - and because of that, the window can no
//longer overlap any taskbar button at all.
//
//占位图标默认会被收进“显示隐藏的图标”里，必须在注册表中把它们标记为始终显示
//（HKCU\Control Panel\NotifyIconSettings\<键名>\IsPromoted），才会真正占用通知区域的宽度。
//By default a new tray icon hides inside the "Show Hidden Icons" flyout and takes up no
//width. It must be marked always-visible in the registry
//(HKCU\Control Panel\NotifyIconSettings\<key>\IsPromoted) before it reserves anything.
//
//占位图标的实际位置通过UI自动化在后台线程中查询
//（UI自动化的跨进程调用会抽取消息队列，在界面线程中调用会导致重入问题）。
//The icons' real position is queried on a background thread through UI Automation (its
//cross-process calls pump the message queue, which would re-enter the UI thread).
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

    //预留区域里是否夹着别的程序的托盘图标。为true时调用方应当把窗口藏起来，
    //否则窗口正好盖在那个图标上面，用户看不见也点不到它。
    //图标被拖走之后这个标志会自动清除，窗口随即恢复显示。
    //Whether another program's tray icon is sitting inside the reserved region. While true the
    //caller should hide the window; otherwise it covers that icon, which can then be neither
    //seen nor clicked. It clears by itself once the icon is dragged away, and the window returns.
    bool IsObstructed() const { return m_obstructed; }

    //移除所有占位图标，并清理它们在注册表中留下的键
    //Remove every placeholder icon and clean up the registry keys they left behind
    void Destroy();

private:
    //最多添加多少个占位图标。窗口的显示项目可以很多，因此这个上限要留得宽裕
    //（每个约42像素，48个可以覆盖两千像素左右）
    //Maximum placeholder icons. The window can carry a lot of display items, so this has to
    //be generous - at ~42px a slot, 48 covers roughly 2000px of readout.
    static constexpr int MAX_ICONS = 48;
    //通知区域中单个图标占用的宽度，仅作为初值，之后会按实测值修正
    //Width one tray icon occupies; only a seed value, corrected by measurement later
    static constexpr int ICON_SLOT_WIDTH = 42;
    //后台线程查询图标位置的间隔（毫秒）。布局变化时WinEvent钩子会立即唤醒它，
    //因此轮询只是兜底：变化后一段时间用较短的间隔，稳定后用很长的间隔以省电。
    //Query interval (ms). A WinEvent hook wakes the thread on layout changes, so polling is
    //only a backstop: short interval right after a change, long once things settle.
    static constexpr int QUERY_INTERVAL_FAST = 250;
    static constexpr int QUERY_INTERVAL_SLOW = 5000;
    static constexpr int STABLE_COUNT_FOR_SLOW = 8;
    //两次查询之间的最小间隔（毫秒），避免任务栏动画期间频繁查询占用CPU
    //Minimum gap between queries (ms) so taskbar animations don't burn CPU
    static constexpr ULONGLONG MIN_QUERY_GAP = 200;

    //添加图标可能会失败（外壳暂时拒绝、通知区域还没准备好等）。
    //每个定时器周期都重试会让通知区域剧烈闪烁，因此失败后退避，退避时间逐次加长；
    //但绝不永久放弃，否则预留区域会永远不够宽，窗口也就永远搬不过去。
    //Adding an icon can fail (the shell refuses it, the tray is not ready yet, ...). Retrying
    //every tick makes the tray flicker violently, so failures back off with a growing delay -
    //but never give up for good, or the region stays too small forever and the window can
    //never move onto it.
    //连续多少次测到重叠才认定预留区域真的被别人的图标占了。
    //取2：查询间隔最短250毫秒，也就是要求重叠持续约半秒，
    //足以滤掉展开折叠按钮时那一瞬间的重新排布，又远短于用户真的拖动图标的时间。
    //How many consecutive samples must see an overlap before the region counts as obstructed.
    //Two: the fastest query interval is 250ms, so this requires roughly half a second of
    //overlap - long enough to filter out the momentary re-layout when the overflow chevron is
    //clicked, and far shorter than any real drag.
    static constexpr int OBSTRUCT_CONFIRM = 2;
    //退出时删除占位图标的时间上限（毫秒）。外壳没响应时单次Shell_NotifyIcon要等好几秒，
    //不设上限的话48个图标能把界面线程冻住几分钟，连关机都会被拖住。
    //Time budget for removing placeholders on shutdown (ms). A single Shell_NotifyIcon waits
    //seconds when the shell is unresponsive; without a cap, 48 of them can freeze the UI thread
    //for minutes and stall even a Windows shutdown.
    static constexpr ULONGLONG REMOVE_BUDGET = 2000;
    static constexpr ULONGLONG RETRY_COOLDOWN = 2000;
    static constexpr int MAX_BACKOFF_STEPS = 8;     //退避上限 / caps the wait

    bool EnsureWindow();
    //添加第index个占位图标，并顺便把它标记为始终显示
    //Add placeholder icon #index; on first add it is also marked always-visible
    bool AddIcon(int index);
    void RemoveIcon(int index);
    //把占位图标标记为始终显示。注册表中的键要等图标第一次被添加之后才会出现，
    //因此采用“先添加→找出新增的键→写入IsPromoted→删除图标→重新添加”的两步做法。
    //Mark the icon always-visible. The registry key only exists after the icon has been added
    //once, hence: add -> diff to find the new key -> write IsPromoted -> delete -> add again.
    //index用来核对新出现的键确实属于本次注册的那个图标，绝不能只凭"新出现"就认定是自己的
    //index is used to verify a newly appeared key really belongs to the icon just registered;
    //"newly appeared" alone must never be treated as proof of ownership
    bool PromoteIcon(int index, const std::vector<std::wstring>& keys_before);
    //对第一次没能标记成功的图标再试一次。系统写入注册表键是异步的，刚添加完往往还查不到，
    //那些图标就会留在“显示隐藏的图标”里，一点空间也预留不到。
    //这里按每个槽位固定的GUID去匹配，不需要做差集，键什么时候出现都能补上。
    //Second promotion pass for icons the first attempt missed. Explorer writes the registry
    //key asynchronously, so right after NIM_ADD it often does not exist yet; those icons stay
    //hidden in the overflow flyout and reserve nothing. Matched here by our per-slot GUID,
    //which needs no diff and works however late the key shows up.
    void EnsurePromoted();
    //找出第index个占位图标对应的注册表键（系统还没写入时返回false）
    //Locate the registry key belonging to slot #index, if Explorer has written it yet
    static bool FindKeyForIcon(int index, std::wstring& key_name);
    //枚举HKCU\Control Panel\NotifyIconSettings下的所有子键名
    //Enumerate every subkey name under HKCU\Control Panel\NotifyIconSettings
    static std::vector<std::wstring> EnumNotifyIconKeys();
    static GUID MakeIconGuid(int index);

    //单个占位图标实际占用的宽度。先用一个按DPI缩放的估计值，再按屏幕上真实的图标修正，
    //而且只允许改小：改小意味着需要更多图标、预留区域变宽，量出来的间距不变，能够收敛；
    //反过来允许改大则会删掉图标、区域变窄、间距又变大，来回震荡。
    //Width a single placeholder buys. Seeded from a DPI-scaled guess, then corrected from the
    //icons actually on screen - but only ever downwards, which is what makes it converge: a
    //smaller slot means more icons and a wider region, which measures the same pitch again.
    //Letting it grow would remove icons, narrow the region and start it oscillating.
    int GetSlotWidth() const;
    void RefineSlotWidth(int reserved_width, int icon_count);
    //按DPI缩放的初始估计值 / the DPI-scaled starting guess
    static int GetDpiSeedWidth();
    //测量值的下限，低于此值的样本一律丢弃 / floor below which a sample is discarded
    static int GetSlotWidthFloor();

    //启动在后台查询占位图标位置的线程 / start the background position-query thread
    void EnsureQueryThread();
    void QueryThreadProc();
    //WinEvent钩子回调（WINEVENT_OUTOFCONTEXT模式，事件在本进程中接收，不会注入其它进程）
    //WinEvent hook callback (OUTOFCONTEXT - received in our process, nothing is injected)
    static void CALLBACK WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
        LONG id_object, LONG id_child, DWORD event_thread, DWORD event_time);

    HWND m_wnd{};                               //拥有占位图标的隐藏窗口 / hidden owner window
    HICON m_icon{};                             //完全透明的图标 / the fully transparent icon
    std::vector<int> m_icons;                   //已添加的占位图标序号 / indices currently added
    //本程序在注册表中创建的键名。退出时必须按记录下来的名字删除：
    //删除图标时系统只会清空键里的值、把空键留在原地，事后靠内容匹配是找不到它们的。
    //Registry keys we created. Cleanup MUST delete them by recorded name: removing an icon
    //only blanks the key's values and leaves the empty key behind, so matching on contents
    //afterwards silently finds nothing.
    std::vector<std::wstring> m_created_keys;

    std::thread m_query_thread;
    std::atomic<bool> m_thread_exit{ false };
    std::atomic<int> m_icon_count{ 0 };         //供后台线程读取 / read by the background thread
    std::atomic<HWND> m_notify_wnd{};
    //预留区域内是否夹着别人的图标，由后台线程写、界面线程读
    //Whether a foreign icon lies inside the region; written by the query thread, read by the UI
    std::atomic<bool> m_obstructed{ false };
    HANDLE m_wake_event{};
    HWINEVENTHOOK m_win_event_hook{};
    static CTaskbarTrayReserve* m_instance;     //供WinEvent回调使用 / for the WinEvent callback

    mutable int m_slot_width{};                 //未测量时为0 / 0 until measured
    int m_reserved_count{};                     //构成预留区域的图标数 / icons forming the region
    std::vector<int> m_pending_promote;         //已添加但尚未确认始终显示 / awaiting promotion
    int m_failed_attempts{};                    //连续添加失败的次数 / consecutive add failures
    ULONGLONG m_retry_after{};                  //在此之前不再重试 / do not retry before this

    mutable std::mutex m_rect_mutex;
    CRect m_reserved_rect;                      //占位图标的并集，访问时加锁 / union, locked
    bool m_reserved_valid{ false };
};

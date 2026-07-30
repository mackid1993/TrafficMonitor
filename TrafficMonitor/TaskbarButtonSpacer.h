#pragma once
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>

//通过在任务栏的任务按钮区域创建隐形的占位按钮，为任务栏窗口预留空间。
//原理：创建若干个被DWM隐藏（Cloak）的顶层窗口，窗口本身在屏幕上不可见，
//但任务栏仍然会为它们显示任务按钮（与挂起的UWP应用保持任务栏按钮的机制相同）。
//任务栏的布局系统不会让其它按钮与这些按钮重叠，
//再把任务栏窗口移动到这些按钮上方，即可实现类似Windows10工具栏(DeskBand)的空间预留效果。
//按钮的位置通过UI自动化(UI Automation)接口在后台线程中获取
//（UI自动化的跨进程调用会抽取消息队列，在界面线程中调用会导致重入问题）。
//不需要注入Explorer，只使用公开的窗口、DWM和UI自动化接口。
class CTaskbarButtonSpacer
{
public:
    ~CTaskbarButtonSpacer();

    //设置需要预留的宽度（物理像素）。需要周期性调用，以便增减占位按钮。
    void SetRequiredWidth(int width);

    //设置接收通知的窗口。检测到其它程序的按钮位于占位按钮右侧时，
    //会立即向该窗口发送WM_SPACER_LAYOUT_CHANGED消息，使任务栏窗口能够立即调整位置，
    //不需要等待定时器的下一次触发
    void SetNotifyWindow(HWND hwnd) { m_notify_wnd = hwnd; }

    //获取所有占位按钮当前的屏幕矩形区域（并集）。
    //占位按钮还没有全部显示到任务栏中时返回false。
    bool GetReservedRect(CRect& rect) const;

    void Destroy();

    bool IsInited() const { return !m_windows.empty(); }

    //是否有其它程序的按钮出现在占位按钮的位置上（此时预留区域暂时失效，正在重新调整占位按钮）
    bool IsAdjustingButtons() const { return m_foreign_on_right; }

private:
    //占位按钮的最大数量
    static constexpr int MAX_SPACER_BUTTONS = 8;
    //后台线程查询占位按钮位置的间隔（毫秒）。
    //任务栏布局变化时WinEvent钩子会立即唤醒后台线程，因此这里的轮询只作为兜底手段：
    //布局变化后的一段时间内使用较短的间隔，布局稳定后使用很长的间隔，以尽量减少耗电
    static constexpr int QUERY_INTERVAL_FAST = 400;
    static constexpr int QUERY_INTERVAL_SLOW = 5000;
    //连续多少次查询结果不变后切换到较长的查询间隔
    static constexpr int STABLE_COUNT_FOR_SLOW = 5;
    //两次查询之间的最小间隔（毫秒）。
    //任务栏动画期间WinEvent钩子会产生大量事件，这里限制查询的频率以避免占用过多CPU
    static constexpr ULONGLONG MIN_QUERY_GAP = 200;
    //重新创建占位窗口（使占位按钮回到最右侧）的最小间隔（毫秒）。
    //这个间隔只用于防止任务栏布局剧烈变化时反复重新创建窗口，
    //正常情况下检测到其它按钮位于占位按钮右侧后会立即重新创建
    static constexpr ULONGLONG RECREATE_COOLDOWN = 150;

    //创建一个占位窗口（第index个）
    HWND CreateSpacerWindow(int index);
    //获取占位窗口的标题（使用不同数量的盲文空格字符，使每个按钮的标题都不同且显示为空白）
    static CString GetSpacerTitle(int index);
    static void RegisterSpacerWindowClass();
    //创建完全透明的图标（用作占位窗口的图标，使任务栏按钮显示为空白）
    static HICON CreateTransparentIcon();

    //启动在后台查询占位按钮位置的线程
    void EnsureQueryThread();
    //后台线程：周期性通过UI自动化接口查询占位按钮的位置，保存到m_button_rects中
    void QueryThreadProc();
    //WinEvent钩子回调（WINEVENT_OUTOFCONTEXT模式，事件在本进程中接收，不会注入其它进程）。
    //任务栏布局变化时立即唤醒后台查询线程
    static void CALLBACK WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG id_object, LONG id_child, DWORD event_thread, DWORD event_time);

    std::vector<HWND> m_windows;
    std::atomic<int> m_window_count{ 0 };   //占位窗口数量（供后台线程读取）
    std::thread m_query_thread;
    std::atomic<bool> m_thread_exit{ false };
    HWINEVENTHOOK m_win_event_hook{};       //任务栏变化事件的钩子
    std::atomic<HWND> m_notify_wnd{};       //检测到需要调整时通知的窗口
    HANDLE m_wake_event{};                  //用于唤醒后台查询线程的事件
    static CTaskbarButtonSpacer* m_instance;    //唯一实例（供WinEvent钩子回调使用）
    std::atomic<bool> m_foreign_on_right{ false };  //任务按钮区域中占位按钮的右侧是否出现了其它程序的按钮
    ULONGLONG m_last_recreate_tick{};       //上次重新创建占位窗口的时间
    UINT m_generation{ static_cast<UINT>(GetTickCount64() / 1000) };    //AppUserModelID的代数，每次重新创建占位窗口时加一
    mutable std::mutex m_rects_mutex;
    std::vector<CRect> m_button_rects;      //每个占位按钮的屏幕矩形区域（空矩形表示没有找到），访问时需要加锁
    int m_required_width{};
    int m_measured_button_width{};          //实际测量到的单个按钮宽度
};

// ==WindhawkMod==
// @id              win10-native-action-center-controls
// @name            Windows 10 Native Action Center Controls
// @description     Independent screen brightness and unified AC/DC display-off time inside the native Win10 Action Center.
// @version         0.1.0
// @author          qqq694637644
// @github          https://github.com/qqq694637644
// @include         ShellExperienceHost.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lruntimeobject -luuid -luser32
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
Native Win+A panel on Windows 10 build 19045, not a separate flyout.

The independent brightness slider never inherits the disabled state of the
Windows stock slider. Hardware brightness writes use WMI and can still fail
if the driver is unavailable; the new control stays enabled and reports error.

The display-off selector writes AC and DC together, without touching sleep.
Lock-screen display-off is only changed with the optional checkbox enabled.

A separate unelevated per-user companion executable must be started with
the argument "broker". Build it from the two tools/win10_action_center_*.cs
files; it provides the full-trust settings API over a limited WM_COPYDATA
channel. This experimental mod must not be enabled on a fragile production
host until XAML insertion has been validated on a disposable Win10 VM.
*/
// ==/WindhawkModReadme==

#include <windows.h>
#undef GetCurrentTime
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace wf = winrt::Windows::Foundation;
namespace wux = winrt::Windows::UI::Xaml;
namespace wuxc = winrt::Windows::UI::Xaml::Controls;
namespace wuxp = winrt::Windows::UI::Xaml::Controls::Primitives;
namespace wuxm = winrt::Windows::UI::Xaml::Media;
namespace wuxa = winrt::Windows::UI::Xaml::Automation;

static constexpr wchar_t kBrokerTitle[] =
    L"Windhawk Win10 ActionCenter Controls Broker v1";
static constexpr ULONG_PTR kMagic = 0x57484143;
static constexpr uint32_t kEmpty = 0xFFFFFFFFu;

struct Packet { uint32_t version, command, value, flags; };
struct Reply { bool ok = false; uint32_t a = 0, b = 0; };

static std::atomic<bool> g_stopping{false};
static std::atomic<bool> g_attached{false};
static std::atomic<HWND> g_window{nullptr};
static std::atomic<uint32_t> g_brightnessRequest{kEmpty};
static std::atomic<uint32_t> g_timeoutRequest{kEmpty};
static std::atomic<uint32_t> g_syncLockRequest{0};
static std::atomic<ULONGLONG> g_brightnessChangeTick{0};
static std::thread g_worker;
static std::mutex g_stateMutex;

struct Status {
    int brightness = -1;
    int ac = -1;
    int dc = -1;
    uint64_t generation = 0;
    std::wstring text = L"等待连接设置代理…";
};
static Status g_status;
static uint64_t g_uiGeneration = 0;

static winrt::weak_ref<wuxc::Grid> g_target;
// Owned controls must have stable references until the UI-thread unload
// callback releases them. Their weak_ref projections expired after AttachUi
// returned, so status and readback changes never reached the visible panel.
static wuxc::StackPanel g_card{nullptr};
// Keep the specific added rows alive until they have been detached. Otherwise
// a vanished weak reference can leave a permanent empty Grid row behind.
static wuxc::RowDefinition g_reservationRow{nullptr};
static wuxc::RowDefinition g_fallbackNativeRow{nullptr};
static wuxc::Slider g_slider{nullptr};
static wuxc::ComboBox g_combo{nullptr};
static wuxc::CheckBox g_lockCheckbox{nullptr};
static wuxc::TextBlock g_feedback{nullptr};
static wux::DispatcherTimer g_timer{nullptr};
static winrt::event_token g_sliderToken{}, g_comboToken{}, g_timerToken{};
static bool g_updatingUi = false;
static unsigned int g_uiTimerDiagnosticTicks = 0;

static void Publish(std::wstring text, int brightness = -1,
                    int ac = -1, int dc = -1) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_status.text = std::move(text);
    if (brightness >= 0) g_status.brightness = brightness;
    if (ac >= 0) g_status.ac = ac;
    if (dc >= 0) g_status.dc = dc;
    ++g_status.generation;
}

static Reply BrokerCall(uint32_t command, uint32_t value = 0,
                        uint32_t flags = 0) {
    HWND broker = FindWindowW(nullptr, kBrokerTitle);
    if (!broker) return {};

    Packet packet{1, command, value, flags};
    COPYDATASTRUCT data{};
    data.dwData = kMagic;
    data.cbData = sizeof(packet);
    data.lpData = &packet;
    HWND sender = g_window.load();
    if (!sender || !IsWindow(sender)) sender = nullptr;

    DWORD_PTR raw = 0;
    // Called only on the private worker thread, never from the XAML UI.
    const LRESULT sent = SendMessageTimeoutW(
        broker, WM_COPYDATA, reinterpret_cast<WPARAM>(sender),
        reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG | SMTO_BLOCK,
        1800, &raw);
    if (!sent || ((raw >> 56) & 0xFF) != 0xA5 ||
        ((raw >> 48) & 0xFF) != 0) {
        return {};
    }
    return {true,
        static_cast<uint32_t>((raw >> 24) & 0xFFFFFF),
        static_cast<uint32_t>(raw & 0xFFFFFF)};
}

static bool IsNativeActionCenter(HWND hwnd) {
    if (!hwnd) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return false;
    wchar_t cls[128]{}, title[256]{};
    GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
    GetWindowTextW(hwnd, title, ARRAYSIZE(title));
    return _wcsicmp(cls, L"Windows.UI.Core.CoreWindow") == 0 &&
           (wcsstr(title, L"操作中心") != nullptr ||
            wcsstr(title, L"Action Center") != nullptr);
}

static bool FindView(wux::DependencyObject const& node,
                     wux::FrameworkElement& found,
                     int depth, int& count) {
    if (!node || depth > 30 || ++count > 3000) return false;
    try {
        const auto name = winrt::get_class_name(node);
        if (name == L"ControlCenter.ControlCenterView" ||
            name == L"ActionCenter.ActionCenterView") {
            found = node.try_as<wux::FrameworkElement>();
            if (found) return true;
        }
        int children = wuxm::VisualTreeHelper::GetChildrenCount(node);
        for (int i = 0; i < children; ++i) {
            if (FindView(wuxm::VisualTreeHelper::GetChild(node, i),
                         found, depth + 1, count)) return true;
        }
    } catch (...) {}
    return false;
}

static void FindLargestGrid(wux::DependencyObject const& node,
                            wuxc::Grid& result, double& area,
                            int depth, int& count) {
    if (!node || depth > 30 || ++count > 3000) return;
    try {
        if (auto grid = node.try_as<wuxc::Grid>()) {
            double w = grid.ActualWidth(), h = grid.ActualHeight();
            if (w > 220 && h > 180 && w*h > area) {
                result = grid;
                area = w*h;
            }
        }
        int children = wuxm::VisualTreeHelper::GetChildrenCount(node);
        for (int i = 0; i < children; ++i)
            FindLargestGrid(wuxm::VisualTreeHelper::GetChild(node, i),
                            result, area, depth + 1, count);
    } catch (...) {}
}

static wuxc::TextBlock Label(winrt::hstring const& value, int size = 13) {
    wuxc::TextBlock label;
    label.Text(value);
    label.FontSize(size);
    label.Margin(wux::Thickness{0, 4, 0, 4});
    label.Foreground(wuxm::SolidColorBrush(
        winrt::Windows::UI::Color{255, 248, 249, 251}));
    return label;
}

static void AppendOption(wuxc::ComboBox const& selector,
                         wchar_t const* text, int seconds) {
    wuxc::ComboBoxItem item;
    item.Content(winrt::box_value(winrt::hstring(text)));
    item.Tag(winrt::box_value(static_cast<int32_t>(seconds)));
    selector.Items().Append(item);
}

static void OnUiTick() {
    if (g_stopping.load()) return;
    Status data;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        if (g_uiGeneration == g_status.generation) return;
        data = g_status;
        g_uiGeneration = data.generation;
    }
    Wh_Log(L"Native controls UI state refresh generation=%llu",
           static_cast<unsigned long long>(data.generation));
    try {
        if (auto label = g_feedback) {
            std::wstring message = data.text;
            if (data.ac >= 0 && data.dc >= 0 && data.ac != data.dc)
                message += L" · 当前 AC/DC 不一致，选一次即可统一";
            label.Text(winrt::hstring(message));
            Wh_Log(L"Feedback TextBlock updated at generation=%llu, length=%u",
                   static_cast<unsigned long long>(data.generation),
                   static_cast<unsigned>(label.Text().size()));
        } else {
            Wh_Log(L"Feedback TextBlock missing at generation=%llu",
                   static_cast<unsigned long long>(data.generation));
        }
        if (auto slider = g_slider) {
            if (data.brightness >= 0 &&
                g_brightnessRequest.load() == kEmpty &&
                GetTickCount64() - g_brightnessChangeTick.load() > 1500) {
                g_updatingUi = true;
                slider.Value(data.brightness);
                g_updatingUi = false;
            }
            slider.IsEnabled(true);
        }
        if (auto combo = g_combo) {
            if (data.ac >= 0 && data.dc >= 0 &&
                g_timeoutRequest.load() == kEmpty) {
                int index = -1;
                if (data.ac == data.dc) {
                    auto items = combo.Items();
                    for (uint32_t i = 0; i < items.Size(); ++i) {
                        if (auto item = items.GetAt(i).try_as<wuxc::ComboBoxItem>()) {
                            if (winrt::unbox_value<int32_t>(item.Tag()) == data.ac) {
                                index = static_cast<int>(i);
                                break;
                            }
                        }
                    }
                }
                g_updatingUi = true;
                combo.SelectedIndex(index);
                g_updatingUi = false;
            }
        }
    } catch (...) {
        g_updatingUi = false;
        Wh_Log(L"Native controls UI refresh failed with HRESULT=0x%08X",
               static_cast<unsigned>(winrt::to_hresult().value));
    }
}

static void RemoveUi() {
    try {
        if (g_timer) {
            g_timer.Stop();
            g_timer.Tick(g_timerToken);
            g_timer = nullptr;
        }
    } catch (...) {
        Wh_Log(L"Native controls timer cleanup reported an exception");
    }
    try {
        if (auto slider = g_slider) slider.ValueChanged(g_sliderToken);
    } catch (...) {
        Wh_Log(L"Native controls slider handler cleanup reported an exception");
    }
    try {
        if (auto combo = g_combo) combo.SelectionChanged(g_comboToken);
    } catch (...) {
        Wh_Log(L"Native controls selector handler cleanup reported an exception");
    }
    if (auto parent = g_target.get()) {
        Wh_Log(L"Native controls cleanup beginning: rows=%u, children=%u",
               parent.RowDefinitions().Size(), parent.Children().Size());
        try {
            if (auto card = g_card) {
                uint32_t pos = 0;
                if (parent.Children().IndexOf(card, pos))
                    parent.Children().RemoveAt(pos);
            }
        } catch (...) {
            Wh_Log(L"Native controls card removal reported an exception");
        }
        try {
            if (g_reservationRow) {
                uint32_t pos = 0;
                if (parent.RowDefinitions().IndexOf(g_reservationRow, pos))
                    parent.RowDefinitions().RemoveAt(pos);
            }
        } catch (...) {
            Wh_Log(L"Native controls reserved row removal reported an exception");
        }
        try {
            if (g_fallbackNativeRow) {
                uint32_t pos = 0;
                if (parent.RowDefinitions().IndexOf(g_fallbackNativeRow, pos))
                    parent.RowDefinitions().RemoveAt(pos);
            }
        } catch (...) {
            Wh_Log(L"Native controls fallback row removal reported an exception");
        }
        Wh_Log(L"Native controls cleanup finished: rows=%u, children=%u",
               parent.RowDefinitions().Size(), parent.Children().Size());
    } else {
        Wh_Log(L"Native controls cleanup skipped: native Grid no longer exists");
    }
    g_slider = nullptr;
    g_combo = nullptr;
    g_card = nullptr;
    g_target = {};
    g_reservationRow = nullptr;
    g_fallbackNativeRow = nullptr;
    g_feedback = nullptr;
    g_lockCheckbox = nullptr;
    g_attached.store(false);
}

static bool AttachUi() {
    if (g_attached.load() || g_stopping.load()) return false;
    try {
        auto window = wux::Window::Current();
        if (!window || !window.Content()) return false;

        wux::FrameworkElement view{nullptr};
        int visits = 0;
        if (!FindView(window.Content(), view, 0, visits)) return false;

        wuxc::Grid target{nullptr};
        visits = 0;
        double area = 0;
        FindLargestGrid(view, target, area, 0, visits);
        if (!target) return false;
        const auto nativeRows = target.RowDefinitions().Size();
        Wh_Log(L"Native Action Center content grid: rows=%u, columns=%u, "
               L"children=%u, width=%f, height=%f",
               nativeRows, target.ColumnDefinitions().Size(),
               target.Children().Size(),
               target.ActualWidth(), target.ActualHeight());

        wuxc::StackPanel card;
        card.Name(L"WHWin10DisplaySettings");
        card.Orientation(wuxc::Orientation::Vertical);
        card.Margin(wux::Thickness{12, 8, 12, 8});
        card.Padding(wux::Thickness{12, 10, 12, 10});
        card.HorizontalAlignment(wux::HorizontalAlignment::Stretch);
        card.VerticalAlignment(wux::VerticalAlignment::Stretch);
        card.Background(wuxm::SolidColorBrush(
            winrt::Windows::UI::Color{246, 38, 42, 51}));

        card.Children().Append(Label(L"亮度（独立控制）"));
        wuxc::Slider slider;
        slider.Minimum(0);
        slider.Maximum(100);
        slider.SmallChange(1);
        slider.LargeChange(10);
        slider.IsEnabled(true);
        slider.Value(50);
        wuxa::AutomationProperties::SetName(slider, L"独立亮度调节");
        card.Children().Append(slider);

        card.Children().Append(Label(L"屏幕自动关闭（充电／电池统一）"));
        wuxc::ComboBox combo;
        combo.PlaceholderText(L"选择时间");
        combo.HorizontalAlignment(wux::HorizontalAlignment::Stretch);
        AppendOption(combo, L"1 分钟", 60);
        AppendOption(combo, L"2 分钟", 120);
        AppendOption(combo, L"3 分钟", 180);
        AppendOption(combo, L"5 分钟", 300);
        AppendOption(combo, L"10 分钟", 600);
        AppendOption(combo, L"15 分钟", 900);
        AppendOption(combo, L"20 分钟", 1200);
        AppendOption(combo, L"30 分钟", 1800);
        AppendOption(combo, L"1 小时", 3600);
        AppendOption(combo, L"2 小时", 7200);
        AppendOption(combo, L"从不", 0);
        card.Children().Append(combo);

        wuxc::CheckBox check;
        check.Content(winrt::box_value(
            winrt::hstring(L"更改时同步锁屏熄屏时间")));
        check.IsChecked(false);
        card.Children().Append(check);

        auto feedback = Label(L"正在连接设置代理…", 11);
        feedback.TextWrapping(wux::TextWrapping::Wrap);
        card.Children().Append(feedback);

        // Reserve real layout space in the existing native Action Center.
        // Do not span existing rows or overlay the built-in Quick Actions and
        // brightness controls. The added row is removed on mod unload.
        g_target = winrt::make_weak(target);
        if (nativeRows == 0) {
            wuxc::RowDefinition nativeRow;
            nativeRow.Height(wux::GridLength{1.0, wux::GridUnitType::Star});
            target.RowDefinitions().Append(nativeRow);
            g_fallbackNativeRow = nativeRow;
        }
        wuxc::RowDefinition reservedRow;
        reservedRow.Height(wux::GridLength{0.0, wux::GridUnitType::Auto});
        target.RowDefinitions().Append(reservedRow);
        g_reservationRow = reservedRow;
        wuxc::Grid::SetRow(card, target.RowDefinitions().Size() - 1);
        const auto columns = target.ColumnDefinitions().Size();
        if (columns) wuxc::Grid::SetColumnSpan(card, columns);
        g_card = card;
        target.Children().Append(card);

        g_slider = slider;
        g_combo = combo;
        g_lockCheckbox = check;
        g_feedback = feedback;

        g_sliderToken = slider.ValueChanged(
            [](wf::IInspectable const&, wuxp::RangeBaseValueChangedEventArgs const& ev) {
                if (g_stopping.load() || g_updatingUi) return;
                int value = static_cast<int>(std::lround(ev.NewValue()));
                if (value < 0 || value > 100) return;
                g_brightnessRequest.store(value);
                g_brightnessChangeTick.store(GetTickCount64());
            });
        g_comboToken = combo.SelectionChanged(
            [](wf::IInspectable const& sender, wuxc::SelectionChangedEventArgs const&) {
                if (g_stopping.load() || g_updatingUi) return;
                try {
                    auto item = sender.as<wuxc::ComboBox>().SelectedItem()
                        .try_as<wuxc::ComboBoxItem>();
                    if (!item) return;
                    const int seconds = winrt::unbox_value<int32_t>(item.Tag());
                    if (seconds < 0 || seconds > 86400) return;
                    uint32_t sync = 0;
                    if (auto check = g_lockCheckbox) {
                        auto checked = check.IsChecked();
                        if (checked && checked.Value()) sync = 1;
                    }
                    g_syncLockRequest.store(sync);
                    g_timeoutRequest.store(seconds);
                } catch (...) {}
            });

        g_timer = wux::DispatcherTimer();
        g_timer.Interval(std::chrono::milliseconds(350));
        g_uiTimerDiagnosticTicks = 0;
        g_timerToken = g_timer.Tick(
            [](wf::IInspectable const&, wf::IInspectable const&) {
                if (++g_uiTimerDiagnosticTicks <= 3)
                    Wh_Log(L"Native controls DispatcherTimer tick #%u",
                           g_uiTimerDiagnosticTicks);
                OnUiTick();
            });
        g_timer.Start();
        g_attached.store(true);
        OnUiTick();
        Wh_Log(L"Native brightness and AC/DC timeout controls attached in a reserved Grid row");
        return true;
    } catch (...) {
        Wh_Log(L"XAML insertion failed: %08X",
            static_cast<unsigned>(winrt::to_hresult().value));
        RemoveUi();
        return false;
    }
}

struct UiInvocation {
    const std::function<void()>* callback;
    bool executed;
};
static UINT g_customMessage = 0;

static LRESULT CALLBACK XamlThreadHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION) {
        auto const* msg = reinterpret_cast<CWPSTRUCT const*>(lParam);
        if (msg->message == g_customMessage && msg->lParam) {
            auto* call = reinterpret_cast<UiInvocation*>(msg->lParam);
            if (!call->executed) {
                call->executed = true;
                try { (*call->callback)(); } catch (...) {}
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

static bool RunOnUiThread(HWND hwnd, std::function<void()> const& callback) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    DWORD tid = GetWindowThreadProcessId(hwnd, nullptr);
    if (!tid) return false;
    if (tid == GetCurrentThreadId()) { callback(); return true; }
    HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, XamlThreadHook, nullptr, tid);
    if (!hook) return false;
    UiInvocation call{&callback, false};
    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(hwnd, g_customMessage, 0,
        reinterpret_cast<LPARAM>(&call),
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 800, &ignored);
    UnhookWindowsHookEx(hook);
    return call.executed;
}

static void Worker() {
    auto brightness = BrokerCall(1);
    auto timeouts = BrokerCall(3);
    if (brightness.ok && timeouts.ok)
        Publish(L"设置代理已连接", brightness.a, timeouts.a, timeouts.b);
    else
        Publish(L"设置代理未连接，请先运行辅助程序");

    ULONGLONG lastAttach = 0, lastRefresh = GetTickCount64();
    while (!g_stopping.load()) {
        HWND foreground = GetForegroundWindow();
        if (IsNativeActionCenter(foreground)) {
            g_window.store(foreground);
            if (!g_attached.load() && GetTickCount64()-lastAttach > 1000) {
                lastAttach = GetTickCount64();
                RunOnUiThread(foreground, [] { AttachUi(); });
            }
        }

        uint32_t pendingBrightness = g_brightnessRequest.load();
        if (pendingBrightness != kEmpty &&
            GetTickCount64()-g_brightnessChangeTick.load() > 200) {
            uint32_t value = g_brightnessRequest.exchange(kEmpty);
            if (value != kEmpty) {
                auto result = BrokerCall(2, value);
                Wh_Log(L"Brightness control request=%u, broker accepted=%d",
                       value, result.ok ? 1 : 0);
                Publish(result.ok ? L"亮度已应用" : L"亮度设置失败，请重试");
            }
        }

        uint32_t seconds = g_timeoutRequest.exchange(kEmpty);
        if (seconds != kEmpty) {
            const uint32_t sync = g_syncLockRequest.load();
            auto result = BrokerCall(4, seconds, sync);
            Wh_Log(L"Unified display-off request=%u seconds, syncLock=%u, "
                   L"broker accepted=%d",
                   seconds, sync, result.ok ? 1 : 0);
            if (result.ok)
                Publish(L"接电源与电池已统一设置", -1, result.a, result.b);
            else
                Publish(L"屏幕关闭时间未写入，请检查代理并重试");
        }

        if (GetTickCount64()-lastRefresh > 6500) {
            lastRefresh = GetTickCount64();
            if (g_brightnessRequest.load() == kEmpty &&
                g_timeoutRequest.load() == kEmpty) {
                auto current = BrokerCall(1);
                auto timeout = BrokerCall(3);
                if (current.ok && timeout.ok) {
                    std::lock_guard<std::mutex> lock(g_stateMutex);
                    g_status.brightness = current.a;
                    g_status.ac = timeout.a;
                    g_status.dc = timeout.b;
                    ++g_status.generation;
                }
            }
        }
        Sleep(100);
    }
}

BOOL Wh_ModInit() {
    wchar_t file[MAX_PATH]{};
    GetModuleFileNameW(nullptr, file, ARRAYSIZE(file));
    wchar_t const* base = wcsrchr(file, L'\\');
    base = base ? base+1 : file;
    if (_wcsicmp(base, L"ShellExperienceHost.exe") != 0) return FALSE;

    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    auto rtlGetVersion = reinterpret_cast<LONG(WINAPI*)(OSVERSIONINFOW*)>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    if (!rtlGetVersion || rtlGetVersion(&version) != 0 ||
        version.dwMajorVersion != 10 || version.dwBuildNumber != 19045)
        return FALSE;

    g_customMessage = RegisterWindowMessageW(L"WH_Win10_ActionCenter_Xaml_Callback_1");
    if (!g_customMessage) return FALSE;
    g_stopping.store(false);
    g_worker = std::thread(Worker);
    Wh_Log(L"Native Win10 settings mod worker started, no global hooks");
    return TRUE;
}

void Wh_ModUninit() {
    g_stopping.store(true);
    if (g_worker.joinable()) g_worker.join();
    HWND hwnd = g_window.load();
    if (hwnd && IsWindow(hwnd)) {
        const bool detached = RunOnUiThread(hwnd, [] { RemoveUi(); });
        Wh_Log(L"Native controls UI-thread detach callback completed=%d",
               detached ? 1 : 0);
    }
    Wh_Log(L"Native Win10 controls unloaded");
}

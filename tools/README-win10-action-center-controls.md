# Windows 10 原生操作中心：独立亮度与统一熄屏设置

本分支新增的两个功能专门针对 Windows 10 22H2 / build 19045 的系统原生 Win+A
操作中心，而不是独立浮窗或 Windows 11 控制中心。

## 功能

- **独立亮度滑块（0–100%）**：不继承 Windows 自带滑块的灰色或禁用状态，
  通过单独设置代理调用 WMI 的 WmiSetBrightness。每次操作重新建立 WMI
  连接，不缓存睡眠或驱动恢复后可能失效的对象句柄。如果显示驱动不响应，
  滑块仍保持可操作，但明确报告写入失败，不能保证物理亮度永远能改变。
- **统一屏幕关闭时间**：1、2、3、5、10、15、20、30、60、120 分钟和“从不”。
  选择一次同时更新当前电源方案的 AC 与 DC VIDEOIDLE 设置，
  然后调用 PowerSetActiveScheme 应用并验证。不改变睡眠时间。
- **可选锁屏同步**：只有勾选复选框并修改熄屏时间时，才另外同步
  VIDEOCONLOCK 的 AC 与 DC 设置。
- 不修改 Windows 系统文件，不安装驱动，也不单独模拟操作中心浮窗。

## 组件

- mods/win10-native-action-center-controls.wh.cpp：
  原生 XAML 控件、后台通信、状态和撤销逻辑。
- tools/win10_action_center_helper.cs：
  WMI 亮度以及 Windows Power API 读写、同步校验与回滚。
- tools/win10_action_center_broker.cs：
  在普通桌面用户会话运行的不可见 broker，处理 WM_COPYDATA 请求。
- .github/workflows/win10-action-center-controls-build.yml：
  Windows GitHub Runner 编译 DLL 和辅助 EXE，绝不加载 Shell DLL。

ShellExperienceHost.exe 属于 Windows AppContainer。为了不在其 UI 线程执行
WMI 操作，本实现使用独立非管理员桌面 broker。客户端发固定长度的
WM_COPYDATA 请求，broker 仅对指定窗口放宽 UIPI 的 WM_COPYDATA 过滤。
写请求必须携带当前交互式用户会话的 ShellExperienceHost 窗口。
该协议不提供提权安全边界，因此 broker **禁止以管理员身份运行**。

## 验证与部署边界

当前是实验性 MVP。此前成功验证过 Windhawk 在这台 Win10 19045 上加载
零 Hook 诊断模组，但新 XAML 控件在真实操作中心中的挂载和卸载、以及
AppContainer 到 broker 的跨进程调用，仍需要运行时验证。

因为此机器曾多次整机硬冻结，应先在可以还原快照的 Windows 10 19045
测试虚拟机上执行首次新模组注入，避免直接在生产 Surface 启用。

隔离测试部署步骤：

1. 从 GitHub Actions Artifact 获取 native-controls-x64.dll 和
   win10-controls-helper.exe。
2. 在相同的已登录用户会话，以非管理员身份运行
   win10-controls-helper.exe broker。此程序没有可见 UI，也不安装自启动。
3. 运行 win10-controls-helper.exe broker-readonly-smoke，验证只读 IPC。
4. Windhawk 引擎配置为 Exclude=* 且 Include=ShellExperienceHost.exe，
   安装本分支的 mod 源码或构建产物。
5. 使用任务栏的原生操作中心按钮打开 Win+A 面板，查看新控件、
   控件位置和报错，随后测试卸载。
6. 通过 UI 生命周期测试后，再测试真实亮度写入与 AC/DC 时间同步。

PowerShell 只读测试方式（项目根目录）：

~~~powershell
$csc = "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
& $csc /nologo /target:exe /platform:x64 /optimize+ /out:win10-controls-helper.exe /reference:System.Management.dll /reference:System.Windows.Forms.dll tools/win10_action_center_helper.cs tools/win10_action_center_broker.cs
.\win10-controls-helper.exe self-test
.\win10-controls-helper.exe read-brightness
.\win10-controls-helper.exe read-display-timeout
~~~

用于特权开发环境的 broker-readonly 启动模式会禁止所有写操作。
生产 broker 模式拒绝管理员令牌。

## 已知风险

首版 XAML 控件会插入原生操作中心最大的 Grid，作为底部对齐的可撤销区域。
在尚未核对实际可见布局前，可能与原有快捷按钮或通知内容重叠。
此 UI 不是最终排版，需要在隔离 VM 中调试 XAML 视觉树后优化。

不能将一轮零 Hook 注入成功等同于长时间运行稳定。
当 ShellExperienceHost 因生命周期管理被挂起时，远程线程创建成功
不代表 DLL 已完成加载。不要强制恢复系统线程。

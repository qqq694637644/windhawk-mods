# Windows 10 原生操作中心：独立亮度与统一熄屏设置

本分支新增的两个功能专门针对 Windows 10 22H2 / build 19045 的系统原生 Win+A
操作中心，而不是独立浮窗或 Windows 11 控制中心。

## 功能

- **独立亮度滑块（0–100%）**：不继承 Windows 自带滑块的灰色或禁用状态，
  通过单独设置代理调用 WMI 的 WmiSetBrightness。每次操作重新建立 WMI
  连接，不缓存睡眠或驱动恢复后可能失效的对象句柄。如果显示驱动不响应，
  滑块仍保持可操作，但明确报告写入失败，不能保证物理亮度永远能改变。
  实机发现 Surface Pro 7 可读取当前亮度，但 WMI 写入当前值也可能失败。
  模组添加了 WinRT BrightnessOverride 备用通道：仅当普通用户 broker
  返回 WMI 操作失败（状态码 4）时，才在原生操作中心的 XAML UI 线程
  尝试 GetForCurrentView / SetBrightnessLevel / StartOverride。
  检查 IsOverrideActive 后再尝试 SaveForSystemAsync；此方法需要
  systemManagement 受限权限。经实机读取确认，当前 ShellExperienceHost
  的 AppxManifest.xml **并未声明这项权限**，因此不应指望永久保存。
  如覆盖有效但保存不被授权，明确显示仅临时生效。测试代理状态码 5
  绝不触发此备用通道。
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
  Windows GitHub Runner 编译 DLL、辅助 EXE，并依照 Windhawk 官方规则
  打包 x64 C++ 运行库，绝不加载 Shell DLL。

ShellExperienceHost.exe 属于 Windows AppContainer。为了不在其 UI 线程执行
WMI 操作，本实现使用独立非管理员桌面 broker。客户端发固定长度的
WM_COPYDATA 请求，broker 仅对指定窗口放宽 UIPI 的 WM_COPYDATA 过滤。
写请求必须携带当前交互式用户会话的 ShellExperienceHost 窗口。
该协议不提供提权安全边界，因此 broker **禁止以管理员身份运行**。

## 验证与部署边界

当前是实验性 MVP。2026-10-26 已在 Surface Pro 7 / Windows 10 19045
实机验证原生 Win+A 操作中心 XAML 控件挂载、显示、滑块与下拉框事件、
AppContainer 到只读 broker 的 IPC、状态反馈和正常卸载。Grid 的布局行
在卸载时恢复，系统原生快捷操作和原亮度滑块不被覆盖。以上测试不能证明
长期稳定，也不能代替真实硬件亮度写入验证。

实际调用 WmiSetBrightness 将亮度写回当前 17% 时失败，读回保持 17%。
原生 CoreWindow 中 BrightnessOverride.GetForCurrentView 返回
IsSupported=True，IsOverrideActive=False；未验证 StartOverride 的
硬件效果，也未验证 SaveForSystemAsync 是否有权限。**不要把“备用接口
受支持”等同于“亮度永远可调”。**

因为此机器曾多次整机硬冻结，应先在可以还原快照的 Windows 10 19045
测试虚拟机上执行首次新模组注入，避免直接在生产 Surface 启用。

隔离测试部署步骤：

1. 从 GitHub Actions Artifact 获取 native-controls-x64.dll、
   win10-controls-helper.exe、libc++.whl、libunwind.whl 和
   windhawk-mod-shim.dll。三个运行库要与模组 DLL 一起放入
   Windhawk Engine/Mods/64 目录；缺少前两个时会导致 0x8007007E，
   即使模组 DLL 本身已编译成功。
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

首版叠加布局曾遮挡原生快捷操作；已替换为 Grid 预留自动高度行，并且
在 Surface 实机截图确认不再遮挡。仍需要长时间运行与多次睡眠恢复验证。
2026-10-26 进一步完成了真实 AC/DC Power API 测试：先将当前
AC=DC=0（从不）原值写回并读回验证；随后将 AC/DC 临时设置为 7200 秒
（2 小时），读回两者均为 7200 秒，并在同一条本机命令的 finally
清理阶段恢复 AC=DC=0。两轮测试都未改变锁屏熄屏时间
（AC=DC=30 秒）、活动电源方案（平衡），Shell 保持响应。
**普通用户 broker 对 Windows Power API 的端到端实际写入尚未实测**；
以上真实写入是通过同一辅助程序的命令行接口完成的。

WinRT 备用方案已提交至测试分支（GitHub Actions run 40268477416 成功），
并通过原生操作中心的只读代理实机回归：亮度调整请求被 status=5 拒绝，
且没有启动任何 WinRT 写入。真正调用 StartOverride 后的硬件亮度效果
尚未验证；系统亮度长期可用性不应宣称已解决。

不能将一轮零 Hook 注入成功等同于长时间运行稳定。
当 ShellExperienceHost 因生命周期管理被挂起时，远程线程创建成功
不代表 DLL 已完成加载。不要强制恢复系统线程。

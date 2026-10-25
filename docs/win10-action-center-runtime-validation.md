# Windows 10 native Action Center: live runtime validation

## Context

Validated on 2026-10-25 using Windows 10 Pro 22H2 (19045.6466),
Surface Pro 7, and the official Windhawk 1.7.3 x64 engine.

The GitHub Actions build of
mods/win10-action-center-injection-probe.wh.cpp succeeded. The probe uses
zero API hooks and does not alter the native XAML tree or brightness.

## Observed evidence

| Test | Observation |
| --- | --- |
| Build with official Windhawk 1.7.3 compiler | Pass |
| Wildcard engine exclusion with only ShellExperienceHost.exe included | Pass |
| Native Action Center activation resumes suspended host process | Pass |
| Windhawk engine DLL visible in the host module list | Pass |
| Diagnostic mod DLL visible in the host module list | Pass |
| Wh_ModInit ran and recorded the native Shell host | Pass |
| Wh_ModUninit ran on exit | Pass |
| Daemon exited; exclude-all, empty include and disabled mod restored | Pass |
| XAML insertion, UI hooks, or actual brightness writes | Not tested |
| Long-term stability across standby, GPU and display transitions | Not tested |

At the beginning of the successful test, all 19 host threads were reported
as Wait/Suspended. Clicking the real native taskbar Action Center icon
(identified through UI Automation) resumed them. Only then did the zero-hook
probe load correctly.

Representative lines from the authenticated local Windhawk debug capture:

~~~text
19:50:05 ShellExperienceHost.exe  [WH] LL
19:50:05 ShellExperienceHost.exe  [WH] GPA
19:50:05 ShellExperienceHost.exe  [WH] II
19:50:05 ShellExperienceHost.exe  [WH] [InjectInit]: Running InjectInit
19:50:05 ShellExperienceHost.exe  [Wh_ModInit]: Probe: loaded, with zero hooks and zero UI changes
19:50:09 ShellExperienceHost.exe  [Wh_ModUninit]: Probe: unloaded
~~~

Earlier attempts while ShellExperienceHost was suspended logged that a
remote injection thread was created, but showed neither injected DLL in
the target. This is a critical distinction: successful remote-thread
creation does not, by itself, prove module initialization.

The host is a Windows AppContainer process. Several status/task persistence
operations logged 0x80070005 (access denied), but the mod initialization
and unloading both completed. Status-file writing and actual mod loading
are separate validation criteria.

The display is scaled to 200%. UI Automation returned physical coordinates
on a 2736 x 1824 desktop, whereas some screenshot operations were
DPI-virtualized to 1368 x 912. Mouse/keyboard tests must normalize
the coordinate system before clicking the actual taskbar icon.

## Native modules and controls

Direct module enumeration of the active ShellExperienceHost process found:

~~~text
C:\Windows\SystemApps\ShellExperienceHost_cw5n1h2txyewy\Windows.UI.ActionCenter.dll
C:\Windows\ShellExperiences\QuickActions.dll
C:\Windows\System32\QuickActionsDataModel.dll
C:\Windows\System32\Windows.UI.Xaml.dll
C:\Windows\System32\Windows.UI.Xaml.Controls.dll
~~~

The actual foreground native Action Center is a
Windows.UI.Core.CoreWindow with the localized title "操作中心".

UI Automation exposed native quick actions for tablet mode, battery
saver, Bluetooth, night light, rotation lock, all settings, focus assist,
screen snip, WLAN, and connect.

A native brightness Slider already exists. Its UI Automation
RangeValuePattern supports values 0 through 100, and the sampled
brightness agreed with root/wmi:WmiMonitorBrightness. No brightness
setting was written or changed. For this reason, the design should
evaluate fixing/reusing the existing brightness control before
adding a duplicate slider.

## Power policy mapping for future controls

The hardware supports S0 Modern Standby, not conventional S3.

| Target UX | Power subgroup GUID | Power setting GUID |
| --- | --- | --- |
| Display off after idle (VIDEOIDLE) | 7516b95f-f776-4464-8c53-06167f40cc99 | 3c0bc021-c8a8-4e07-a973-6b14cbcb2b7e |
| Sleep after idle (STANDBYIDLE) | 238c9fa8-0aad-41ed-83f4-97be242c8f20 | 29f6c1db-86da-48c5-9fdb-f2b67b1f44da |
| Locked-screen display off (VIDEOCONLOCK) | 7516b95f-f776-4464-8c53-06167f40cc99 | 8ec4b3a5-6868-48c2-be75-4f3044be88a7 |

Read-only commands for confirming the active profile:

~~~powershell
powercfg /a
powercfg /query SCHEME_CURRENT SUB_VIDEO VIDEOIDLE
powercfg /query SCHEME_CURRENT SUB_SLEEP STANDBYIDLE
powercfg /qh SCHEME_CURRENT SUB_VIDEO VIDEOCONLOCK
~~~

The desired future UX keeps display-off and sleep timeouts separate,
synchronizes each setting across AC and DC, and modifies VIDEOCONLOCK
only when an explicit lock-screen-sync checkbox is enabled.
None of those settings were modified during this validation.

## Safety boundary

This was a short, diagnostics-only test on a machine with a history
of unrecoverable hangs. A single successful run is not proof of
long-term stability. Do not enable Windhawk's default unrestricted
process injection. Do not force-resume suspended shell threads.

Future native XAML changes and brightness writes should be tested
first on a disposable Windows 10 VM with snapshots. Only perform
single-process, reversible tests, with explicit cleanup. No startup
persistence or system-driver installation was used in this validation.

This report deliberately excludes full private desktop screenshots,
unrelated installed applications, user-specific workspace paths and
memory dump content.

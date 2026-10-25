# Windhawk Windows 10 Action Center: verification boundaries

## Goal

Evaluate whether a Windhawk mod can load in the native Windows 10 Action Center
host (ShellExperienceHost.exe) on Windows 10 22H2 build 19045. The eventual
product goal is to add a brightness slider and unified screen-off/sleep controls
inside the existing Action Center, not a separate panel.

## Verified by GitHub Actions

1. GitHub Actions compilation only using the official Windhawk 1.7.3
   portable compiler on an ephemeral GitHub-hosted Windows runner.
2. Probe DLL is limited to logging OS version, process path, and PID.
3. No GUI injection, process launch, brightness write, shell restart, or
   local auto-start is included in the CI.

## Verified by a short live-host test (2026-10-25)

With explicit authorization, the x64 probe was loaded on Windows 10 Pro
22H2 build 19045.6466 (Surface Pro 7). After activating the native
Action Center using the taskbar's accessible Action Center button:

1. ShellExperienceHost.exe resumed from Windows app lifecycle suspension.
2. Windhawk 1.7.3 injected its engine and the x64 probe into the host.
3. The module list showed both the Windhawk engine and probe DLL.
4. Debug logs confirmed Wh_ModInit and Wh_ModUninit ran.
5. The background daemon exited and exclude-all configuration was restored.

The target was strictly scoped by Engine settings: Exclude=* and
Include=ShellExperienceHost.exe. No hooks or XAML mutations were installed.
See [runtime validation](win10-action-center-runtime-validation.md) for
the actual evidence and native component locations.

## Not established by compilation or the short runtime test

- Creating or modifying native XAML controls.
- Long-term stability or compatibility across GPU, TCON, and power transitions.
- Safety of a mod that installs hooks or changes brightness.

## Runtime safety gate

The live host previously experienced unrecoverable freezes. A single
authorized, diagnostics-only test passed; this does not mean broad injection
is safe. For future invasive runtime tests prefer a disposable Windows 10 22H2
VM snapshot or dedicated machine with rollback.
Windhawk defaults to broad process injection even for narrowly targeted mods:
set a full process exclusion (*) with a selective inclusion only after the
configuration can be verified independently.

One successful short injection is not evidence of long-term compatibility.

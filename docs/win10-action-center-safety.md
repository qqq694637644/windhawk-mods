# Windhawk Windows 10 Action Center: verification boundaries

## Goal

Evaluate whether a Windhawk mod can load in the native Windows 10 Action Center
host (ShellExperienceHost.exe) on Windows 10 22H2 build 19045. The eventual
product goal is to add a brightness slider and unified screen-off/sleep controls
inside the existing Action Center, not a separate panel.

## Tested in this branch

1. GitHub Actions compilation only using the official Windhawk 1.7.3
   portable compiler on an ephemeral GitHub-hosted Windows runner.
2. Probe DLL is limited to logging OS version, process path, and PID.
3. No GUI injection, process launch, brightness write, shell restart, or
   local auto-start is included in the CI.

## Not tested by a successful build

- Loading Windhawk into a real Windows 10 19045 desktop.
- Mod injection into ShellExperienceHost.exe.
- Creating or modifying native XAML controls.
- Reliability or compatibility with a particular GPU, TCON, or power driver.

## Runtime safety gate

The live host previously experienced unrecoverable freezes. Do not run any
Windhawk injection on that host. Runtime tests should use a disposable
Windows 10 22H2 VM snapshot or dedicated test machine with rollback.
Windhawk defaults to broad process injection even for narrowly targeted mods:
set a full process exclusion (*) with a selective inclusion only after the
configuration can be verified independently.

Compilation is not evidence of runtime compatibility.

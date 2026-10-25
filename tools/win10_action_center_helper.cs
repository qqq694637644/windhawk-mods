// Win10 Action Center controls: full-trust setting helper (Windows 10 / .NET Framework).
// Safe to compile and invoke with "self-test", "read-brightness",
// and "read-display-timeout" without changing any system settings.
//
// The optional "broker" command hosts a private, invisible window in this
// full-trust user process. The injected Action Center can request settings
// changes through a narrowly scoped WM_COPYDATA protocol.
using System;
using System.Globalization;
using System.Management;
using System.Runtime.InteropServices;

internal static class Win10ActionCenterHelper
{
    // https://learn.microsoft.com/windows/win32/api/powersetting/
    private static readonly Guid VideoSubgroup =
        new Guid("7516b95f-f776-4464-8c53-06167f40cc99");
    private static readonly Guid DisplayIdle =
        new Guid("3c0bc021-c8a8-4e07-a973-6b14cbcb2b7e");
    private static readonly Guid ConsoleLockDisplayIdle =
        new Guid("8ec4b3a5-6868-48c2-be75-4f3044be88a7");

    [DllImport("PowrProf.dll", SetLastError = false)]
    private static extern uint PowerGetActiveScheme(IntPtr root, out IntPtr activeScheme);

    [DllImport("kernel32.dll", SetLastError = false)]
    private static extern IntPtr LocalFree(IntPtr memory);

    [DllImport("PowrProf.dll", SetLastError = false)]
    private static extern uint PowerReadACValueIndex(
        IntPtr root, ref Guid scheme, ref Guid subgroup, ref Guid setting, out uint index);

    [DllImport("PowrProf.dll", SetLastError = false)]
    private static extern uint PowerReadDCValueIndex(
        IntPtr root, ref Guid scheme, ref Guid subgroup, ref Guid setting, out uint index);

    [DllImport("PowrProf.dll", SetLastError = false)]
    private static extern uint PowerWriteACValueIndex(
        IntPtr root, ref Guid scheme, ref Guid subgroup, ref Guid setting, uint index);

    [DllImport("PowrProf.dll", SetLastError = false)]
    private static extern uint PowerWriteDCValueIndex(
        IntPtr root, ref Guid scheme, ref Guid subgroup, ref Guid setting, uint index);

    [DllImport("PowrProf.dll", SetLastError = false)]
    private static extern uint PowerSetActiveScheme(IntPtr root, ref Guid scheme);

    private sealed class ActiveScheme : IDisposable
    {
        internal readonly Guid Id;

        internal ActiveScheme()
        {
            IntPtr native;
            uint result = PowerGetActiveScheme(IntPtr.Zero, out native);
            if (result != 0 || native == IntPtr.Zero)
                throw new InvalidOperationException("PowerGetActiveScheme failed: Win32 " + result);
            try { Id = (Guid)Marshal.PtrToStructure(native, typeof(Guid)); }
            finally { LocalFree(native); }
        }

        public void Dispose() { }
    }

    private static void CheckWin32(uint error, string what)
    {
        if (error != 0)
            throw new InvalidOperationException(what + " failed: Win32 " + error);
    }

    private static uint ReadPowerIndex(Guid scheme, Guid subgroup, Guid setting, bool ac)
    {
        uint result;
        uint code = ac
            ? PowerReadACValueIndex(IntPtr.Zero, ref scheme, ref subgroup, ref setting, out result)
            : PowerReadDCValueIndex(IntPtr.Zero, ref scheme, ref subgroup, ref setting, out result);
        CheckWin32(code, ac ? "PowerReadACValueIndex" : "PowerReadDCValueIndex");
        return result;
    }

    internal static void GetDisplayTimeout(out uint ac, out uint dc)
    {
        using (var active = new ActiveScheme())
        {
            ac = ReadPowerIndex(active.Id, VideoSubgroup, DisplayIdle, true);
            dc = ReadPowerIndex(active.Id, VideoSubgroup, DisplayIdle, false);
        }
    }

    private static void WritePowerIndex(Guid scheme, Guid subgroup, Guid setting, bool ac, uint index)
    {
        uint code = ac
            ? PowerWriteACValueIndex(IntPtr.Zero, ref scheme, ref subgroup, ref setting, index)
            : PowerWriteDCValueIndex(IntPtr.Zero, ref scheme, ref subgroup, ref setting, index);
        CheckWin32(code, ac ? "PowerWriteACValueIndex" : "PowerWriteDCValueIndex");
    }

    private static void PrintTimeout(Guid scheme)
    {
        uint ac = ReadPowerIndex(scheme, VideoSubgroup, DisplayIdle, true);
        uint dc = ReadPowerIndex(scheme, VideoSubgroup, DisplayIdle, false);
        Console.WriteLine("DISPLAY_OFF_SECONDS_AC=" + ac);
        Console.WriteLine("DISPLAY_OFF_SECONDS_DC=" + dc);
        Console.WriteLine("UNIFIED=" + (ac == dc ? "1" : "0"));
        uint lockAc = ReadPowerIndex(scheme, VideoSubgroup, ConsoleLockDisplayIdle, true);
        uint lockDc = ReadPowerIndex(scheme, VideoSubgroup, ConsoleLockDisplayIdle, false);
        Console.WriteLine("LOCK_SCREEN_OFF_SECONDS_AC=" + lockAc);
        Console.WriteLine("LOCK_SCREEN_OFF_SECONDS_DC=" + lockDc);
    }

    // Each setting is a pair. Read both before changing either; on failure,
    // restore both and re-activate the active scheme. The lock-screen pair
    // is not touched unless the optional switch is specified.
    internal static void WriteUnifiedDisplayTimeout(uint seconds, bool syncLockScreen)
    {
        using (var active = new ActiveScheme())
        {
            Guid scheme = active.Id;
            uint originalAc = ReadPowerIndex(scheme, VideoSubgroup, DisplayIdle, true);
            uint originalDc = ReadPowerIndex(scheme, VideoSubgroup, DisplayIdle, false);
            uint originalLockAc = 0, originalLockDc = 0;
            if (syncLockScreen)
            {
                originalLockAc = ReadPowerIndex(scheme, VideoSubgroup, ConsoleLockDisplayIdle, true);
                originalLockDc = ReadPowerIndex(scheme, VideoSubgroup, ConsoleLockDisplayIdle, false);
            }

            // The regular Display idle setting is written for BOTH sources.
            // A zero value means "Never".
            try
            {
                WritePowerIndex(scheme, VideoSubgroup, DisplayIdle, true, seconds);
                WritePowerIndex(scheme, VideoSubgroup, DisplayIdle, false, seconds);
                if (syncLockScreen)
                {
                    WritePowerIndex(scheme, VideoSubgroup, ConsoleLockDisplayIdle, true, seconds);
                    WritePowerIndex(scheme, VideoSubgroup, ConsoleLockDisplayIdle, false, seconds);
                }
                // Writes to the active power scheme do not take effect until
                // this explicit apply call.
                CheckWin32(PowerSetActiveScheme(IntPtr.Zero, ref scheme),
                    "PowerSetActiveScheme");

                // Verify the desired indices were persisted.
                uint appliedAc = ReadPowerIndex(scheme, VideoSubgroup, DisplayIdle, true);
                uint appliedDc = ReadPowerIndex(scheme, VideoSubgroup, DisplayIdle, false);
                if (appliedAc != seconds || appliedDc != seconds)
                    throw new InvalidOperationException("Timeout verification failed");
                if (syncLockScreen &&
                    (ReadPowerIndex(scheme, VideoSubgroup, ConsoleLockDisplayIdle, true) != seconds ||
                     ReadPowerIndex(scheme, VideoSubgroup, ConsoleLockDisplayIdle, false) != seconds))
                    throw new InvalidOperationException("Lock-screen timeout verification failed");
                Console.WriteLine("APPLIED_BOTH_AC_DC=1");
                Console.WriteLine("DISPLAY_OFF_SECONDS=" + seconds);
                Console.WriteLine("SYNC_LOCK_SCREEN=" + (syncLockScreen ? "1" : "0"));
            }
            catch
            {
                try
                {
                    WritePowerIndex(scheme, VideoSubgroup, DisplayIdle, true, originalAc);
                    WritePowerIndex(scheme, VideoSubgroup, DisplayIdle, false, originalDc);
                    if (syncLockScreen)
                    {
                        WritePowerIndex(scheme, VideoSubgroup, ConsoleLockDisplayIdle, true, originalLockAc);
                        WritePowerIndex(scheme, VideoSubgroup, ConsoleLockDisplayIdle, false, originalLockDc);
                    }
                    CheckWin32(PowerSetActiveScheme(IntPtr.Zero, ref scheme),
                        "PowerSetActiveScheme rollback");
                }
                catch (Exception rollbackError)
                {
                    Console.Error.WriteLine("ROLLBACK_FAILED=" + rollbackError.Message);
                }
                throw;
            }
        }
    }

    private static ManagementScope ConnectWmi()
    {
        var scope = new ManagementScope(@"\\.\root\wmi");
        scope.Connect();
        return scope;
    }

    internal static byte ReadBrightness()
    {
        ManagementScope scope = ConnectWmi();
        using (var searcher = new ManagementObjectSearcher(scope,
            new ObjectQuery("SELECT Active, CurrentBrightness FROM WmiMonitorBrightness")))
        using (ManagementObjectCollection result = searcher.Get())
        {
            foreach (ManagementObject obj in result)
            {
                using (obj)
                {
                    if (Convert.ToBoolean(obj["Active"], CultureInfo.InvariantCulture))
                        return Convert.ToByte(obj["CurrentBrightness"], CultureInfo.InvariantCulture);
                }
            }
        }
        throw new InvalidOperationException("No active internal monitor brightness interface");
    }

    internal static void WriteBrightness(byte brightness)
    {
        ManagementScope scope = ConnectWmi();
        using (var searcher = new ManagementObjectSearcher(scope,
            new ObjectQuery("SELECT * FROM WmiMonitorBrightnessMethods")))
        using (ManagementObjectCollection collection = searcher.Get())
        {
            foreach (ManagementObject obj in collection)
            {
                using (obj)
                {
                    if (!Convert.ToBoolean(obj["Active"], CultureInfo.InvariantCulture))
                        continue;
                    // Resolve the provider afresh on every operation: do not
                    // cache an object across sleep / display-driver recovery.
                    ManagementBaseObject args = obj.GetMethodParameters("WmiSetBrightness");
                    args["Timeout"] = (uint)0;
                    args["Brightness"] = brightness;
                    using (ManagementBaseObject result = obj.InvokeMethod("WmiSetBrightness", args, null))
                    {
                        uint code = Convert.ToUInt32(result["ReturnValue"], CultureInfo.InvariantCulture);
                        if (code != 0)
                            throw new InvalidOperationException("WmiSetBrightness failed: " + code);
                    }
                    Console.WriteLine("BRIGHTNESS_WRITE_ACCEPTED=" + brightness);
                    return;
                }
            }
        }
        throw new InvalidOperationException("No active monitor exposes WmiSetBrightness");
    }

    private static uint ParseWholeNumber(string value, uint min, uint max)
    {
        uint parsed;
        if (!uint.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out parsed) ||
            parsed < min || parsed > max)
            throw new ArgumentException("Value must be an integer in range " + min + ".." + max);
        return parsed;
    }

    private static int SelfTest()
    {
        if (ParseWholeNumber("0", 0, 100) != 0) throw new Exception("Zero parse failed");
        if (ParseWholeNumber("100", 0, 100) != 100) throw new Exception("Max parse failed");
        bool rejected = false;
        try { ParseWholeNumber("1000", 0, 100); } catch (ArgumentException) { rejected = true; }
        if (!rejected) throw new Exception("Out-of-range value was accepted");
        rejected = false;
        try { ParseWholeNumber("-1", 0, 100); } catch (ArgumentException) { rejected = true; }
        if (!rejected) throw new Exception("Negative value was accepted");
        if (VideoSubgroup == DisplayIdle) throw new Exception("GUID mapping invalid");
        if (ConsoleLockDisplayIdle == DisplayIdle) throw new Exception("Lock timeout is not independent");
        if (!Win10ActionCenterBroker.ProtocolSelfTest())
            throw new Exception("Broker request/response protocol failed");
        Console.WriteLine("SELF_TEST=PASS");
        return 0;
    }

    private static int Main(string[] args)
    {
        try
        {
            if (args.Length == 1 && args[0] == "broker")
                return Win10ActionCenterBroker.Run();
            if (args.Length == 1 && args[0] == "broker-readonly")
                return Win10ActionCenterBroker.Run(true);
            if (args.Length == 1 && args[0] == "broker-readonly-smoke")
                return Win10ActionCenterBroker.ReadOnlySmokeTest();
            if (args.Length == 1 && args[0] == "self-test") return SelfTest();
            if (args.Length == 1 && args[0] == "read-brightness")
            {
                Console.WriteLine("BRIGHTNESS=" + ReadBrightness());
                return 0;
            }
            if (args.Length == 2 && args[0] == "set-brightness")
            {
                WriteBrightness((byte)ParseWholeNumber(args[1], 0, 100));
                return 0;
            }
            if (args.Length == 1 && args[0] == "read-display-timeout")
            {
                using (var active = new ActiveScheme()) PrintTimeout(active.Id);
                return 0;
            }
            if (args.Length >= 2 && args.Length <= 3 &&
                args[0] == "set-display-timeout")
            {
                uint seconds = ParseWholeNumber(args[1], 0, 86400);
                bool syncLock = args.Length == 3 && args[2] == "--sync-lock-screen";
                if (args.Length == 3 && !syncLock)
                    throw new ArgumentException("Unexpected option: " + args[2]);
                WriteUnifiedDisplayTimeout(seconds, syncLock);
                return 0;
            }
            Console.Error.WriteLine(
                "Usage: helper.exe self-test | read-brightness | set-brightness <0..100> | " +
                "read-display-timeout | set-display-timeout <seconds> [--sync-lock-screen]");
            return 2;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine("ERROR=" + error.GetType().Name + ": " + error.Message);
            return 1;
        }
    }
}

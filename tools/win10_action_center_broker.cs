// Full-trust per-user broker for the Windows 10 native Action Center mod.
//
// The mod runs inside ShellExperienceHost.exe (AppContainer). The broker runs
// in a separate, unelevated desktop user process and owns a hidden HWND.
// WM_COPYDATA messages use a small fixed-length protocol. No listening TCP
// ports, admin rights, drivers, scheduled tasks, or registry persistence.
//
// Protocol v1:
//   COPYDATASTRUCT.dwData = 0x57484143 ("WHAC")
//   Packet { uint version=1, command, value, flags }
//   Commands: 1 read brightness, 2 set brightness, 3 read AC/DC display idle,
//             4 write same display idle AC+DC.
//   Response LRESULT 64-bit: 0xA5SSAAAAAABBBBBB
//   SS is 0 on success; A and B are unsigned 24-bit output fields.
//
// Use "win10-controls-helper.exe broker" as an ordinary unelevated user.
// The broker MUST NOT be installed as a Windows service or run as admin.
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;

internal static class Win10ActionCenterBroker
{
    private const int WmCopyData = 0x004A;
    private const int MsgfltAllow = 1;
    private const ulong PacketMagic = 0x57484143UL;
    internal const string WindowTitle = "Windhawk Win10 ActionCenter Controls Broker v1";
    private static bool s_readOnlyTestMode;

    [StructLayout(LayoutKind.Sequential)]
    private struct CopyData
    {
        public IntPtr dwData;
        public int cbData;
        public IntPtr lpData;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Packet
    {
        public uint version;
        public uint command;
        public uint value;
        public uint flags;
    }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool ChangeWindowMessageFilterEx(
        IntPtr window, uint message, uint action, IntPtr changeFilterStruct);

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr FindWindow(string className, string windowName);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint processId);

    [DllImport("user32.dll")]
    private static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr SendMessageTimeout(
        IntPtr window, uint message, IntPtr wParam,
        ref CopyData data, uint flags, uint milliseconds, out UIntPtr result);

    private static ulong Encode(byte status, uint a, uint b)
    {
        return (0xA5UL << 56) | ((ulong)status << 48) |
            (((ulong)a & 0xFFFFFFUL) << 24) | ((ulong)b & 0xFFFFFFUL);
    }

    private static bool IsTrustedShellWindow(IntPtr sender)
    {
        if (sender == IntPtr.Zero)
            return false;
        uint pid;
        if (GetWindowThreadProcessId(sender, out pid) == 0 || pid == 0)
            return false;

        try
        {
            using (Process process = Process.GetProcessById((int)pid))
            using (Process self = Process.GetCurrentProcess())
            {
                if (!String.Equals(process.ProcessName, "ShellExperienceHost",
                    StringComparison.OrdinalIgnoreCase) ||
                    process.SessionId != self.SessionId)
                    return false;
            }
        }
        catch { return false; }

        // WM_COPYDATA is accepted from lower-integrity callers specifically
        // for a real Action Center window in this session. Do not accept a
        // fabricated sender handle belonging to an unrelated program.
        uint foregroundPid;
        GetWindowThreadProcessId(GetForegroundWindow(), out foregroundPid);
        return foregroundPid == pid;
    }

    private sealed class BrokerWindow : NativeWindow, IDisposable
    {
        internal BrokerWindow()
        {
            var cp = new CreateParams();
            cp.Caption = WindowTitle;
            cp.Style = 0;
            cp.ExStyle = 0;
            CreateHandle(cp);
            if (Handle == IntPtr.Zero)
                throw new InvalidOperationException("Cannot create settings broker window");
            if (!ChangeWindowMessageFilterEx(Handle, WmCopyData, MsgfltAllow, IntPtr.Zero))
                throw new InvalidOperationException("Cannot allow WM_COPYDATA: " +
                    Marshal.GetLastWin32Error());
        }

        protected override void WndProc(ref Message m)
        {
            if (m.Msg != WmCopyData)
            {
                base.WndProc(ref m);
                return;
            }

            ulong response = Encode(1, 0, 0);
            try
            {
                var copyData = (CopyData)Marshal.PtrToStructure(
                    m.LParam, typeof(CopyData));
                if ((ulong)copyData.dwData.ToInt64() != PacketMagic ||
                    copyData.cbData != Marshal.SizeOf(typeof(Packet)) ||
                    copyData.lpData == IntPtr.Zero)
                {
                    m.Result = new IntPtr(unchecked((long)Encode(2, 0, 0)));
                    return;
                }

                var packet = (Packet)Marshal.PtrToStructure(
                    copyData.lpData, typeof(Packet));
                if (packet.version != 1 || packet.command < 1 || packet.command > 4)
                {
                    m.Result = new IntPtr(unchecked((long)Encode(2, 0, 0)));
                    return;
                }
                if (s_readOnlyTestMode &&
                    (packet.command == 2 || packet.command == 4))
                {
                    m.Result = new IntPtr(unchecked((long)Encode(5, 0, 0)));
                    return;
                }

                // Reading settings is harmless, so it can be exercised by a
                // testing client. Mutating commands require an active native
                // ShellExperienceHost window in the same interactive session.
                if ((packet.command == 2 || packet.command == 4) &&
                    !IsTrustedShellWindow(m.WParam))
                {
                    m.Result = new IntPtr(unchecked((long)Encode(3, 0, 0)));
                    return;
                }

                switch (packet.command)
                {
                    case 1:
                        response = Encode(0, Win10ActionCenterHelper.ReadBrightness(), 0);
                        break;
                    case 2:
                        if (packet.value > 100)
                            throw new ArgumentOutOfRangeException("brightness");
                        Win10ActionCenterHelper.WriteBrightness((byte)packet.value);
                        response = Encode(0, packet.value, 0);
                        break;
                    case 3:
                        uint ac, dc;
                        Win10ActionCenterHelper.GetDisplayTimeout(out ac, out dc);
                        response = Encode(0, ac, dc);
                        break;
                    case 4:
                        if (packet.value > 86400 || (packet.flags & ~1U) != 0)
                            throw new ArgumentOutOfRangeException("display timeout");
                        Win10ActionCenterHelper.WriteUnifiedDisplayTimeout(
                            packet.value, (packet.flags & 1U) != 0);
                        response = Encode(0, packet.value, packet.value);
                        break;
                }
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine("BROKER_OPERATION_ERROR=" + ex.Message);
                response = Encode(4, 0, 0);
            }
            m.Result = new IntPtr(unchecked((long)response));
        }

        public void Dispose()
        {
            if (Handle != IntPtr.Zero)
                DestroyHandle();
        }
    }

    internal static int Run(bool readOnlyTestMode = false)
    {
        if (IntPtr.Size != 8)
        {
            Console.Error.WriteLine("64-bit Windows is required.");
            return 2;
        }

        s_readOnlyTestMode = readOnlyTestMode;
        using (var mutex = new Mutex(false, @"Local\WindhawkWin10ActionCenterControlsBrokerV1"))
        {
            bool acquired;
            try { acquired = mutex.WaitOne(0, false); }
            catch (AbandonedMutexException) { acquired = true; }
            if (!acquired)
            {
                Console.Error.WriteLine("BROKER_ALREADY_RUNNING");
                return 3;
            }
            try
            {
                // Do not run this user-level process elevated. An elevated
                // server would unnecessarily widen the privilege boundary.
                using (var identity = System.Security.Principal.WindowsIdentity.GetCurrent())
                {
                    var principal = new System.Security.Principal.WindowsPrincipal(identity);
                    if (!readOnlyTestMode &&
                        principal.IsInRole(System.Security.Principal.WindowsBuiltInRole.Administrator))
                    {
                        Console.Error.WriteLine("BROKER_REFUSES_ADMINISTRATOR_TOKEN");
                        return 4;
                    }
                }
                if (FindWindow(null, WindowTitle) != IntPtr.Zero)
                {
                    Console.Error.WriteLine("BROKER_WINDOW_ALREADY_EXISTS");
                    return 5;
                }

                using (var window = new BrokerWindow())
                {
                    if (readOnlyTestMode)
                        Console.WriteLine("BROKER_READ_ONLY_TEST_MODE=1");
                    Console.WriteLine("BROKER_READY=" + WindowTitle);
                    Console.Out.Flush();
                    Application.Run(new ApplicationContext());
                }
                return 0;
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine("BROKER_INIT_FAILED=" + ex.Message);
                return 1;
            }
            finally
            {
                mutex.ReleaseMutex();
            }
        }
    }

    internal static bool ProtocolSelfTest()
    {
        // No window, no writes: verify the encoding and bounds used by C++
        // on the other side of the WM_COPYDATA protocol.
        ulong r = Encode(0, 86400, 30);
        return (byte)(r >> 56) == 0xA5 &&
            (byte)(r >> 48) == 0 &&
            ((r >> 24) & 0xFFFFFF) == 86400 &&
            (r & 0xFFFFFF) == 30 &&
            (byte)(Encode(3, 0, 0) >> 48) == 3;
    }

    internal static int ReadOnlySmokeTest()
    {
        IntPtr window = FindWindow(null, WindowTitle);
        if (window == IntPtr.Zero)
        {
            Console.Error.WriteLine("BROKER_NOT_FOUND");
            return 1;
        }

        foreach (uint command in new uint[] { 1, 3 })
        {
            IntPtr memory = IntPtr.Zero;
            try
            {
                var packet = new Packet { version = 1, command = command };
                memory = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(Packet)));
                Marshal.StructureToPtr(packet, memory, false);
                var data = new CopyData
                {
                    dwData = new IntPtr(unchecked((long)PacketMagic)),
                    cbData = Marshal.SizeOf(typeof(Packet)),
                    lpData = memory
                };
                UIntPtr returned;
                if (SendMessageTimeout(window, WmCopyData, IntPtr.Zero,
                    ref data, 2, 2000, out returned) == IntPtr.Zero)
                {
                    Console.Error.WriteLine("BROKER_CALL_FAILED=" + Marshal.GetLastWin32Error());
                    return 1;
                }
                ulong result = returned.ToUInt64();
                byte magic = (byte)(result >> 56);
                byte status = (byte)(result >> 48);
                if (magic != 0xA5 || status != 0)
                {
                    Console.Error.WriteLine("BROKER_STATUS=" + status + " MAGIC=" + magic);
                    return 1;
                }
                uint a = (uint)((result >> 24) & 0xFFFFFF);
                uint b = (uint)(result & 0xFFFFFF);
                if (command == 1)
                    Console.WriteLine("BROKER_BRIGHTNESS=" + a);
                else
                {
                    Console.WriteLine("BROKER_DISPLAY_OFF_AC=" + a);
                    Console.WriteLine("BROKER_DISPLAY_OFF_DC=" + b);
                }
            }
            finally
            {
                if (memory != IntPtr.Zero) Marshal.FreeHGlobal(memory);
            }
        }
        Console.WriteLine("BROKER_READ_ONLY_SMOKE=PASS");
        return 0;
    }
}

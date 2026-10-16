import { useEffect, useState } from "react";
import { Cpu, Wifi, HardDrive, Clock, Fingerprint, RotateCw, CalendarClock, CheckCircle2, XCircle, AlertTriangle, RefreshCw } from "lucide-react";
import { supabase, type DeviceHeartbeat, type DeviceCommand, type Device } from "@/lib/supabase";
import { Card, LoadingSpinner, EmptyState, ErrorState, Badge, timeAgo, isStale } from "@/components/ui";
import { useAuth } from "@/context/AuthContext";

export function DevicePage() {
  const { user } = useAuth();
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [heartbeat, setHeartbeat] = useState<DeviceHeartbeat | null>(null);
  const [device, setDevice] = useState<Device | null>(null);
  const [commands, setCommands] = useState<DeviceCommand[]>([]);
  const [sendingCmd, setSendingCmd] = useState(false);

  async function load() {
    try {
      const [hbRes, devRes, cmdRes] = await Promise.all([
        supabase.from("device_heartbeats").select("*").order("received_at", { ascending: false }).limit(1).maybeSingle(),
        supabase.from("devices").select("*").order("created_at", { ascending: false }).limit(1).maybeSingle(),
        supabase.from("device_commands").select("*").order("created_at", { ascending: false }).limit(20),
      ]);
      if (hbRes.error) throw new Error(hbRes.error.message);
      if (devRes.error) throw new Error(devRes.error.message);
      setHeartbeat((hbRes.data as DeviceHeartbeat) ?? null);
      setDevice((devRes.data as Device) ?? null);
      setCommands((cmdRes.data as DeviceCommand[]) ?? []);
    } catch (err) { setError(err instanceof Error ? err.message : "Failed to load device data"); } finally { setLoading(false); }
  }

  useEffect(() => { load(); const interval = setInterval(load, 15000); return () => clearInterval(interval); }, []);

  const sendCommand = async (command: "restart" | "sync_time") => {
    setSendingCmd(true);
    try {
      const { error: cmdErr } = await supabase.from("device_commands").insert({ device_id: device?.device_id ?? heartbeat?.device_id ?? "default", command, payload: {}, status: "pending", created_by: user?.id });
      if (cmdErr) throw new Error(cmdErr.message);
      await load();
    } catch (err) { setError(err instanceof Error ? err.message : "Failed to send command"); } finally { setSendingCmd(false); }
  };

  if (loading) return <LoadingSpinner label="Loading device status..." />;
  const deviceOnline = heartbeat?.wifi_connected && !isStale(heartbeat?.received_at ?? null);
  const deviceStale = heartbeat && isStale(heartbeat?.received_at ?? null);

  return (
    <div className="space-y-6">
      {error && <Card className="p-4 border-rose-200 bg-rose-50"><p className="text-sm text-rose-700">{error}</p></Card>}
      <Card className="p-6">
        <div className="flex items-center justify-between flex-wrap gap-4 mb-4">
          <div className="flex items-center gap-4">
            <div className={`w-14 h-14 rounded-xl flex items-center justify-center ${deviceOnline ? "bg-emerald-50 text-emerald-600" : deviceStale ? "bg-amber-50 text-amber-600" : "bg-slate-100 text-slate-400"}`}>
              {deviceOnline ? <CheckCircle2 className="w-8 h-8" /> : <AlertTriangle className="w-8 h-8" />}
            </div>
            <div>
              <h3 className="font-semibold text-slate-800 text-lg">{device?.name ?? heartbeat?.device_name ?? "ESP32 Device"}</h3>
              <p className="text-sm text-slate-500">{deviceOnline ? "Online — connected and reporting" : deviceStale ? `Stale — last heartbeat ${timeAgo(heartbeat?.received_at ?? null)}` : "No device data — waiting for first connection"}</p>
              {device && <p className="text-xs text-slate-400 font-mono mt-1">ID: {device.device_id}</p>}
            </div>
          </div>
          <button onClick={() => load()} className="flex items-center gap-2 px-3 py-2 text-sm font-medium rounded-lg border border-slate-200 text-slate-600 hover:bg-slate-50 transition-colors"><RefreshCw className="w-4 h-4" />Refresh</button>
        </div>
        {heartbeat && (
          <div className="flex flex-wrap gap-2">
            <Badge className={heartbeat.wifi_connected ? "bg-emerald-50 text-emerald-700 border-emerald-200" : "bg-rose-50 text-rose-700 border-rose-200"}><Wifi className="w-3 h-3 mr-1" />{heartbeat.wifi_connected ? "Wi-Fi Connected" : "Wi-Fi Disconnected"}</Badge>
            <Badge className={heartbeat.sd_available ? "bg-emerald-50 text-emerald-700 border-emerald-200" : "bg-rose-50 text-rose-700 border-rose-200"}><HardDrive className="w-3 h-3 mr-1" />{heartbeat.sd_available ? "SD Card OK" : "SD Card Missing"}</Badge>
            <Badge className={heartbeat.fingerprint_ok ? "bg-emerald-50 text-emerald-700 border-emerald-200" : "bg-rose-50 text-rose-700 border-rose-200"}><Fingerprint className="w-3 h-3 mr-1" />{heartbeat.fingerprint_ok ? "Sensor Ready" : "Sensor Not Found"}</Badge>
            <Badge className={heartbeat.rtc_time_valid ? "bg-emerald-50 text-emerald-700 border-emerald-200" : "bg-amber-50 text-amber-700 border-amber-200"}><Clock className="w-3 h-3 mr-1" />{heartbeat.rtc_time_valid ? "RTC Valid" : "RTC Invalid"}</Badge>
          </div>
        )}
      </Card>
      {!heartbeat && !error && <Card><EmptyState icon={<Cpu className="w-7 h-7" />} title="No device connected yet" description="Once your ESP32 is configured with cloud credentials and uploads a heartbeat, its full status will appear here." /></Card>}
      {heartbeat && (
        <>
          <div className="grid grid-cols-1 lg:grid-cols-2 gap-4">
            <Card className="p-5"><h3 className="text-sm font-semibold text-slate-700 mb-4 flex items-center gap-2"><Cpu className="w-4 h-4 text-slate-400" /> System Information</h3><dl className="space-y-3"><div className="flex justify-between text-sm"><dt className="text-slate-500">Firmware Version</dt><dd className="font-mono text-xs text-slate-700">{heartbeat.firmware_version ?? "—"}</dd></div><div className="flex justify-between text-sm"><dt className="text-slate-500">Device Name</dt><dd className="font-medium text-slate-700">{heartbeat.device_name ?? "—"}</dd></div><div className="flex justify-between text-sm"><dt className="text-slate-500">IP Address</dt><dd className="font-mono text-xs text-slate-700">{heartbeat.ip_address ?? "—"}</dd></div><div className="flex justify-between text-sm"><dt className="text-slate-500">Wi-Fi Signal (RSSI)</dt><dd className="font-medium text-slate-700">{heartbeat.rssi ? `${heartbeat.rssi} dBm` : "—"}</dd></div><div className="flex justify-between text-sm"><dt className="text-slate-500">Free Heap</dt><dd className="font-medium text-slate-700">{heartbeat.free_heap ? `${(heartbeat.free_heap / 1024).toFixed(0)} KB` : "—"}</dd></div></dl></Card>
            <Card className="p-5"><h3 className="text-sm font-semibold text-slate-700 mb-4 flex items-center gap-2"><Fingerprint className="w-4 h-4 text-slate-400" /> Fingerprint & Attendance</h3><dl className="space-y-3"><div className="flex justify-between text-sm"><dt className="text-slate-500">Enrolled Templates</dt><dd className="font-medium text-slate-700">{heartbeat.templates} / {heartbeat.capacity}</dd></div><div className="flex justify-between text-sm"><dt className="text-slate-500">Currently Enrolling</dt><dd className="font-medium text-slate-700">{heartbeat.is_enrolling ? "Yes" : "No"}</dd></div><div className="flex justify-between text-sm"><dt className="text-slate-500">Total Check-ins</dt><dd className="font-medium text-emerald-600">{heartbeat.entry_count}</dd></div><div className="flex justify-between text-sm"><dt className="text-slate-500">Total Check-outs</dt><dd className="font-medium text-blue-600">{heartbeat.exit_count}</dd></div><div className="flex justify-between text-sm"><dt className="text-slate-500">Pending Sync Queue</dt><dd className="font-medium text-slate-700">{heartbeat.pending_count}</dd></div></dl></Card>
          </div>
          <div className="grid grid-cols-1 lg:grid-cols-2 gap-4">
            <Card className="p-5"><h3 className="text-sm font-semibold text-slate-700 mb-4 flex items-center gap-2"><CalendarClock className="w-4 h-4 text-slate-400" /> Schedule</h3><dl className="space-y-3"><div className="flex justify-between text-sm"><dt className="text-slate-500">Check-in Time</dt><dd className="font-medium text-slate-700">{heartbeat.checkin_time ?? "—"}</dd></div><div className="flex justify-between text-sm"><dt className="text-slate-500">Check-out Time</dt><dd className="font-medium text-slate-700">{heartbeat.checkout_time ?? "—"}</dd></div><div className="flex justify-between text-sm"><dt className="text-slate-500">Last Heartbeat</dt><dd className="font-medium text-slate-700">{timeAgo(heartbeat.received_at)}</dd></div></dl></Card>
            <Card className="p-5"><h3 className="text-sm font-semibold text-slate-700 mb-4 flex items-center gap-2"><RotateCw className="w-4 h-4 text-slate-400" /> Device Commands</h3><div className="flex gap-2 mb-4"><button onClick={() => sendCommand("sync_time")} disabled={sendingCmd} className="flex items-center gap-2 px-3 py-2 text-sm font-medium rounded-lg border border-slate-200 text-slate-600 hover:bg-slate-50 disabled:opacity-50 transition-colors"><Clock className="w-4 h-4" />Sync Time</button><button onClick={() => sendCommand("restart")} disabled={sendingCmd} className="flex items-center gap-2 px-3 py-2 text-sm font-medium rounded-lg border border-amber-200 text-amber-600 hover:bg-amber-50 disabled:opacity-50 transition-colors"><RotateCw className="w-4 h-4" />Restart</button></div><p className="text-xs text-slate-400">Commands are delivered when the device polls the cloud. Only safe, allowlisted actions are supported.</p></Card>
          </div>
          {commands.length > 0 && (
            <Card className="overflow-hidden">
              <div className="px-5 py-3 border-b border-slate-200"><h3 className="text-sm font-semibold text-slate-700">Command History</h3></div>
              <div className="overflow-x-auto"><table className="w-full text-sm"><thead><tr className="border-b border-slate-200 bg-slate-50"><th className="text-left px-4 py-3 font-medium text-slate-600">Command</th><th className="text-left px-4 py-3 font-medium text-slate-600">Status</th><th className="text-left px-4 py-3 font-medium text-slate-600">Created</th><th className="text-left px-4 py-3 font-medium text-slate-600 hidden sm:table-cell">Acknowledged</th></tr></thead><tbody className="divide-y divide-slate-100">{commands.map((cmd) => (<tr key={cmd.id} className="hover:bg-slate-50 transition-colors"><td className="px-4 py-3 text-slate-700 capitalize font-medium">{cmd.command}</td><td className="px-4 py-3">{cmd.status === "pending" && <Badge className="bg-amber-50 text-amber-700 border-amber-200"><Clock className="w-3 h-3 mr-1" />Pending</Badge>}{cmd.status === "acknowledged" && <Badge className="bg-emerald-50 text-emerald-700 border-emerald-200"><CheckCircle2 className="w-3 h-3 mr-1" />Acknowledged</Badge>}{cmd.status === "failed" && <Badge className="bg-rose-50 text-rose-700 border-rose-200"><XCircle className="w-3 h-3 mr-1" />Failed</Badge>}</td><td className="px-4 py-3 text-slate-500 text-xs">{timeAgo(cmd.created_at)}</td><td className="px-4 py-3 text-slate-500 text-xs hidden sm:table-cell">{cmd.acknowledged_at ? timeAgo(cmd.acknowledged_at) : "—"}</td></tr>))}</tbody></table></div>
            </Card>
          )}
        </>
      )}
    </div>
  );
}

import { useEffect, useState } from "react";
import {
  CalendarCheck,
  LogIn,
  LogOut,
  HardDrive,
  Wifi,
  Cpu,
  Clock,
  AlertTriangle,
  CheckCircle2,
} from "lucide-react";
import { supabase, type DeviceHeartbeat, type AttendanceEvent } from "@/lib/supabase";
import { Card, StatCard, LoadingSpinner, EmptyState, Badge, timeAgo, isStale } from "@/components/ui";

export function OverviewPage() {
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [heartbeat, setHeartbeat] = useState<DeviceHeartbeat | null>(null);
  const [todayCount, setTodayCount] = useState(0);
  const [totalRecords, setTotalRecords] = useState(0);
  const [checkInsToday, setCheckInsToday] = useState(0);
  const [checkOutsToday, setCheckOutsToday] = useState(0);

  useEffect(() => {
    async function load() {
      try {
        const today = new Date().toISOString().slice(0, 10);
        const [hbRes, todayRes, totalRes, todayInRes, todayOutRes] = await Promise.all([
          supabase.from("device_heartbeats").select("*").order("received_at", { ascending: false }).limit(1).maybeSingle(),
          supabase.from("attendance_events").select("id", { count: "exact", head: true }).eq("event_date", today),
          supabase.from("attendance_events").select("id", { count: "exact", head: true }),
          supabase.from("attendance_events").select("id", { count: "exact", head: true }).eq("event_date", today).eq("event_type", "CHECK_IN"),
          supabase.from("attendance_events").select("id", { count: "exact", head: true }).eq("event_date", today).eq("event_type", "CHECK_OUT"),
        ]);
        if (hbRes.error) throw new Error(hbRes.error.message);
        setHeartbeat((hbRes.data as DeviceHeartbeat) ?? null);
        setTodayCount(todayRes.count ?? 0);
        setTotalRecords(totalRes.count ?? 0);
        setCheckInsToday(todayInRes.count ?? 0);
        setCheckOutsToday(todayOutRes.count ?? 0);
      } catch (err) {
        setError(err instanceof Error ? err.message : "Failed to load data");
      } finally {
        setLoading(false);
      }
    }
    load();
    const interval = setInterval(load, 30000);
    return () => clearInterval(interval);
  }, []);

  if (loading) return <LoadingSpinner label="Loading dashboard..." />;

  const deviceOnline = heartbeat?.wifi_connected && !isStale(heartbeat?.received_at ?? null);
  const deviceStale = heartbeat && isStale(heartbeat?.received_at ?? null);

  return (
    <div className="space-y-6">
      <Card className="p-5">
        <div className="flex items-center justify-between flex-wrap gap-4">
          <div className="flex items-center gap-4">
            <div className={`w-12 h-12 rounded-xl flex items-center justify-center ${
              deviceOnline ? "bg-emerald-50 text-emerald-600" : deviceStale ? "bg-amber-50 text-amber-600" : "bg-slate-100 text-slate-400"
            }`}>
              {deviceOnline ? <CheckCircle2 className="w-7 h-7" /> : <AlertTriangle className="w-7 h-7" />}
            </div>
            <div>
              <p className="font-semibold text-slate-800">{heartbeat?.device_name ?? "ESP32 Device"}</p>
              <p className="text-sm text-slate-500">
                {deviceOnline ? "Online — connected and reporting" : deviceStale ? `Stale — last seen ${timeAgo(heartbeat?.received_at ?? null)}` : "No device data yet — waiting for first heartbeat"}
              </p>
            </div>
          </div>
          {heartbeat && (
            <div className="flex items-center gap-2 flex-wrap">
              <Badge className={heartbeat.wifi_connected ? "bg-emerald-50 text-emerald-700 border-emerald-200" : "bg-rose-50 text-rose-700 border-rose-200"}>
                <Wifi className="w-3 h-3 mr-1" />{heartbeat.wifi_connected ? "Wi-Fi Connected" : "Wi-Fi Disconnected"}
              </Badge>
              <Badge className={heartbeat.sd_available ? "bg-emerald-50 text-emerald-700 border-emerald-200" : "bg-rose-50 text-rose-700 border-rose-200"}>
                <HardDrive className="w-3 h-3 mr-1" />{heartbeat.sd_available ? "SD Card OK" : "SD Card Missing"}
              </Badge>
              <Badge className={heartbeat.rtc_time_valid ? "bg-emerald-50 text-emerald-700 border-emerald-200" : "bg-amber-50 text-amber-700 border-amber-200"}>
                <Clock className="w-3 h-3 mr-1" />{heartbeat.rtc_time_valid ? "RTC Valid" : "RTC Invalid"}
              </Badge>
            </div>
          )}
        </div>
      </Card>

      {error && <Card className="p-4 border-rose-200 bg-rose-50"><p className="text-sm text-rose-700">{error}</p></Card>}

      <div className="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-4 gap-4">
        <StatCard label="Today's Records" value={todayCount} icon={<CalendarCheck className="w-6 h-6" />} accent="teal" subtitle={new Date().toLocaleDateString("en-US", { weekday: "long", month: "short", day: "numeric" })} />
        <StatCard label="Check-ins Today" value={checkInsToday} icon={<LogIn className="w-6 h-6" />} accent="emerald" />
        <StatCard label="Check-outs Today" value={checkOutsToday} icon={<LogOut className="w-6 h-6" />} accent="blue" />
        <StatCard label="Total Cloud Records" value={totalRecords} icon={<Cpu className="w-6 h-6" />} accent="slate" subtitle="All synced attendance events" />
      </div>

      {heartbeat ? (
        <div className="grid grid-cols-1 lg:grid-cols-3 gap-4">
          <Card className="p-5">
            <h3 className="text-sm font-semibold text-slate-700 mb-4">Fingerprint Sensor</h3>
            <dl className="space-y-3">
              <div className="flex justify-between text-sm"><dt className="text-slate-500">Status</dt><dd className="font-medium text-slate-700">{heartbeat.fingerprint_ok ? "Ready" : "Not Found"}</dd></div>
              <div className="flex justify-between text-sm"><dt className="text-slate-500">Enrolled Templates</dt><dd className="font-medium text-slate-700">{heartbeat.templates} / {heartbeat.capacity}</dd></div>
              <div className="flex justify-between text-sm"><dt className="text-slate-500">Enrolling</dt><dd className="font-medium text-slate-700">{heartbeat.is_enrolling ? "Yes" : "No"}</dd></div>
            </dl>
          </Card>
          <Card className="p-5">
            <h3 className="text-sm font-semibold text-slate-700 mb-4">Network & Storage</h3>
            <dl className="space-y-3">
              <div className="flex justify-between text-sm"><dt className="text-slate-500">IP Address</dt><dd className="font-medium text-slate-700 font-mono text-xs">{heartbeat.ip_address ?? "—"}</dd></div>
              <div className="flex justify-between text-sm"><dt className="text-slate-500">Wi-Fi Signal</dt><dd className="font-medium text-slate-700">{heartbeat.rssi ? `${heartbeat.rssi} dBm` : "—"}</dd></div>
              <div className="flex justify-between text-sm"><dt className="text-slate-500">Free Heap</dt><dd className="font-medium text-slate-700">{heartbeat.free_heap ? `${(heartbeat.free_heap / 1024).toFixed(0)} KB` : "—"}</dd></div>
            </dl>
          </Card>
          <Card className="p-5">
            <h3 className="text-sm font-semibold text-slate-700 mb-4">Attendance Totals</h3>
            <dl className="space-y-3">
              <div className="flex justify-between text-sm"><dt className="text-slate-500">Total Check-ins</dt><dd className="font-medium text-slate-700">{heartbeat.entry_count}</dd></div>
              <div className="flex justify-between text-sm"><dt className="text-slate-500">Total Check-outs</dt><dd className="font-medium text-slate-700">{heartbeat.exit_count}</dd></div>
              <div className="flex justify-between text-sm"><dt className="text-slate-500">Pending Sync</dt><dd className="font-medium text-slate-700">{heartbeat.pending_count}</dd></div>
              <div className="flex justify-between text-sm"><dt className="text-slate-500">Firmware</dt><dd className="font-medium text-slate-700 font-mono text-xs">{heartbeat.firmware_version ?? "—"}</dd></div>
            </dl>
          </Card>
        </div>
      ) : (
        !error && (
          <Card>
            <EmptyState icon={<Cpu className="w-7 h-7" />} title="No device heartbeats yet" description="Once your ESP32 is configured with cloud credentials and connected to Wi-Fi, its status will appear here in real time." />
          </Card>
        )
      )}
    </div>
  );
}

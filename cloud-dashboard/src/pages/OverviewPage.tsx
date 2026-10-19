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
  Users,
  UserCheck,
  UserX,
  AlertCircle,
} from "lucide-react";
import { supabase, type DeviceHeartbeat, type AttendanceEvent } from "@/lib/supabase";
import { Card, StatCard, LoadingSpinner, EmptyState, Badge, timeAgo, isStale } from "@/components/ui";

type RegisteredTeacher = {
  fingerprint_id: number;
  name: string | null;
  college_id: string | null;
  role: string | null;
  department: string | null;
};

type DailyTeacherRow = RegisteredTeacher & {
  checkedIn: boolean;
  checkedOut: boolean;
  dailyStatus: "complete" | "partial" | "absent";
};

function kigaliDateKey(date = new Date()): string {
  const parts = new Intl.DateTimeFormat("en-GB", {
    timeZone: "Africa/Kigali",
    year: "numeric",
    month: "2-digit",
    day: "2-digit",
  }).formatToParts(date);
  const part = (kind: string) => parts.find((item) => item.type === kind)?.value ?? "";
  return `${part("year")}-${part("month")}-${part("day")}`;
}

function dailyTeacherStatus(teachers: RegisteredTeacher[], events: Pick<AttendanceEvent, "fingerprint_id" | "event_type">[]): DailyTeacherRow[] {
  const byFingerprint = new Map<number, Set<"CHECK_IN" | "CHECK_OUT">>();
  for (const event of events) {
    if (event.event_type !== "CHECK_IN" && event.event_type !== "CHECK_OUT") continue;
    const set = byFingerprint.get(event.fingerprint_id) ?? new Set<"CHECK_IN" | "CHECK_OUT">();
    set.add(event.event_type);
    byFingerprint.set(event.fingerprint_id, set);
  }

  return teachers
    .filter((teacher) => (teacher.role ?? "").trim().toLowerCase() !== "administrator")
    .map((teacher) => {
      const marks = byFingerprint.get(teacher.fingerprint_id);
      const checkedIn = marks?.has("CHECK_IN") ?? false;
      const checkedOut = marks?.has("CHECK_OUT") ?? false;
      const dailyStatus = checkedIn && checkedOut ? "complete" : checkedIn || checkedOut ? "partial" : "absent";
      return { ...teacher, checkedIn, checkedOut, dailyStatus };
    })
    .sort((a, b) => (a.fingerprint_id ?? 0) - (b.fingerprint_id ?? 0));
}

export function OverviewPage() {
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [heartbeat, setHeartbeat] = useState<DeviceHeartbeat | null>(null);
  const [todayCount, setTodayCount] = useState(0);
  const [totalRecords, setTotalRecords] = useState(0);
  const [checkInsToday, setCheckInsToday] = useState(0);
  const [checkOutsToday, setCheckOutsToday] = useState(0);
  const [dailyTeachers, setDailyTeachers] = useState<DailyTeacherRow[]>([]);
  const [dailyAttendanceError, setDailyAttendanceError] = useState<string | null>(null);

  useEffect(() => {
    async function load() {
      try {
        const today = kigaliDateKey();
        const [hbRes, todayRes, totalRes, todayInRes, todayOutRes, teachersRes, todayEventsRes] = await Promise.all([
          supabase.from("device_heartbeats").select("*").order("received_at", { ascending: false }).limit(1).maybeSingle(),
          supabase.from("attendance_events").select("id", { count: "exact", head: true }).eq("event_date", today),
          supabase.from("attendance_events").select("id", { count: "exact", head: true }),
          supabase.from("attendance_events").select("id", { count: "exact", head: true }).eq("event_date", today).eq("event_type", "CHECK_IN"),
          supabase.from("attendance_events").select("id", { count: "exact", head: true }).eq("event_date", today).eq("event_type", "CHECK_OUT"),
          supabase.from("teachers").select("fingerprint_id, name, college_id, role, department").order("fingerprint_id", { ascending: true }).limit(1000),
          supabase.from("attendance_events").select("fingerprint_id, event_type").eq("event_date", today).limit(1000),
        ]);
        if (hbRes.error) throw new Error(hbRes.error.message);
        setHeartbeat((hbRes.data as DeviceHeartbeat) ?? null);
        setTodayCount(todayRes.count ?? 0);
        setTotalRecords(totalRes.count ?? 0);
        setCheckInsToday(todayInRes.count ?? 0);
        setCheckOutsToday(todayOutRes.count ?? 0);

        if (teachersRes.error || todayEventsRes.error) {
          setDailyAttendanceError(
            teachersRes.error?.message ?? todayEventsRes.error?.message ?? "Daily attendance summary could not be loaded."
          );
          setDailyTeachers([]);
        } else {
          setDailyAttendanceError(null);
          setDailyTeachers(dailyTeacherStatus(
            (teachersRes.data ?? []) as RegisteredTeacher[],
            (todayEventsRes.data ?? []) as Pick<AttendanceEvent, "fingerprint_id" | "event_type">[]
          ));
        }
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

      <Card className="overflow-hidden">
        <div className="px-5 py-4 border-b border-slate-200 flex items-center justify-between gap-3 flex-wrap">
          <div>
            <h3 className="font-semibold text-slate-800">Today's Teacher Attendance</h3>
            <p className="text-sm text-slate-500 mt-1">Rwanda time · {kigaliDateKey()} · one check-in and one check-out per teacher per day</p>
          </div>
          <span className="text-xs text-slate-500">Green = both · Yellow = one · Red = none</span>
        </div>
        {dailyAttendanceError ? (
          <div className="p-4 text-sm text-rose-700 bg-rose-50">Could not load teacher attendance: {dailyAttendanceError}</div>
        ) : (
          <>
            <div className="grid grid-cols-1 sm:grid-cols-2 xl:grid-cols-5 gap-3 p-4 bg-slate-50">
              <div className="rounded-lg border border-slate-200 bg-white p-4">
                <div className="flex items-center gap-2 text-slate-500 text-sm"><Users className="w-4 h-4" /> Registered teachers</div>
                <p className="text-2xl font-semibold text-slate-800 mt-2">{dailyTeachers.length}</p>
              </div>
              <div className="rounded-lg border border-teal-200 bg-teal-50 p-4">
                <div className="flex items-center gap-2 text-teal-700 text-sm"><UserCheck className="w-4 h-4" /> Attended at least once</div>
                <p className="text-2xl font-semibold text-teal-800 mt-2">{dailyTeachers.filter((teacher) => teacher.dailyStatus !== "absent").length}</p>
              </div>
              <div className="rounded-lg border border-emerald-200 bg-emerald-50 p-4">
                <div className="flex items-center gap-2 text-emerald-700 text-sm"><UserCheck className="w-4 h-4" /> Completed (green)</div>
                <p className="text-2xl font-semibold text-emerald-800 mt-2">{dailyTeachers.filter((teacher) => teacher.dailyStatus === "complete").length}</p>
              </div>
              <div className="rounded-lg border border-amber-200 bg-amber-50 p-4">
                <div className="flex items-center gap-2 text-amber-700 text-sm"><AlertCircle className="w-4 h-4" /> One event (yellow)</div>
                <p className="text-2xl font-semibold text-amber-800 mt-2">{dailyTeachers.filter((teacher) => teacher.dailyStatus === "partial").length}</p>
              </div>
              <div className="rounded-lg border border-rose-200 bg-rose-50 p-4">
                <div className="flex items-center gap-2 text-rose-700 text-sm"><UserX className="w-4 h-4" /> No attendance (red)</div>
                <p className="text-2xl font-semibold text-rose-800 mt-2">{dailyTeachers.filter((teacher) => teacher.dailyStatus === "absent").length}</p>
              </div>
            </div>
            {dailyTeachers.length === 0 ? (
              <div className="p-5 text-sm text-slate-500">No registered teachers found.</div>
            ) : (
              <div className="overflow-x-auto">
                <table className="w-full text-sm">
                  <thead><tr className="border-b border-slate-200 bg-white">
                    <th className="text-left px-4 py-3 font-medium text-slate-600">Fingerprint ID</th>
                    <th className="text-left px-4 py-3 font-medium text-slate-600">Teacher</th>
                    <th className="text-left px-4 py-3 font-medium text-slate-600 hidden md:table-cell">Department</th>
                    <th className="text-center px-4 py-3 font-medium text-slate-600">Check-in</th>
                    <th className="text-center px-4 py-3 font-medium text-slate-600">Check-out</th>
                    <th className="text-left px-4 py-3 font-medium text-slate-600">Daily status</th>
                  </tr></thead>
                  <tbody className="divide-y divide-slate-100">
                    {dailyTeachers.map((teacher) => {
                      const palette = teacher.dailyStatus === "complete"
                        ? "bg-emerald-50 text-emerald-700 border-emerald-200"
                        : teacher.dailyStatus === "partial"
                          ? "bg-amber-50 text-amber-800 border-amber-200"
                          : "bg-rose-50 text-rose-700 border-rose-200";
                      const label = teacher.dailyStatus === "complete" ? "Complete" : teacher.dailyStatus === "partial" ? "One event only" : "Absent";
                      return (
                        <tr key={teacher.fingerprint_id} className={teacher.dailyStatus === "complete" ? "bg-emerald-50/40" : teacher.dailyStatus === "partial" ? "bg-amber-50/40" : "bg-rose-50/40"}>
                          <td className="px-4 py-3 font-mono text-slate-600">{teacher.fingerprint_id}</td>
                          <td className="px-4 py-3 font-medium text-slate-800">{teacher.name?.trim() || `ID ${teacher.fingerprint_id}`}</td>
                          <td className="px-4 py-3 text-slate-500 hidden md:table-cell">{teacher.department?.trim() || "—"}</td>
                          <td className="px-4 py-3 text-center">{teacher.checkedIn ? <span className="text-emerald-700 font-semibold">✓ Yes</span> : <span className="text-slate-400">— No</span>}</td>
                          <td className="px-4 py-3 text-center">{teacher.checkedOut ? <span className="text-emerald-700 font-semibold">✓ Yes</span> : <span className="text-slate-400">— No</span>}</td>
                          <td className="px-4 py-3"><span className={`inline-flex rounded-full border px-2.5 py-1 text-xs font-semibold ${palette}`}>{label}</span></td>
                        </tr>
                      );
                    })}
                  </tbody>
                </table>
              </div>
            )}
          </>
        )}
      </Card>

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

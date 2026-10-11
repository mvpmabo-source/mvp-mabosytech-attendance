import { useEffect, useState, useMemo } from "react";
import { FileBarChart, Download, Calendar } from "lucide-react";
import { supabase, type AttendanceEvent, STATUS_LABELS } from "@/lib/supabase";
import { Card, LoadingSpinner, EmptyState, ErrorState } from "@/components/ui";

type DailySummary = { date: string; total: number; check_ins: number; check_outs: number; on_time: number; late: number; early: number; left_early: number; overtime: number };

export function ReportsPage() {
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [events, setEvents] = useState<AttendanceEvent[]>([]);
  const [fromDate, setFromDate] = useState("");
  const [toDate, setToDate] = useState("");

  useEffect(() => {
    async function load() {
      setLoading(true); setError(null);
      try {
        let query = supabase.from("attendance_events").select("*").order("event_date", { ascending: false }).order("event_time", { ascending: false });
        if (fromDate) query = query.gte("event_date", fromDate);
        if (toDate) query = query.lte("event_date", toDate);
        const { data, error: qErr } = await query;
        if (qErr) throw new Error(qErr.message);
        setEvents((data as AttendanceEvent[]) ?? []);
      } catch (err) { setError(err instanceof Error ? err.message : "Failed to load reports"); } finally { setLoading(false); }
    }
    load();
  }, [fromDate, toDate]);

  const dailySummaries = useMemo<DailySummary[]>(() => {
    const map = new Map<string, DailySummary>();
    for (const e of events) {
      let s = map.get(e.event_date);
      if (!s) { s = { date: e.event_date, total: 0, check_ins: 0, check_outs: 0, on_time: 0, late: 0, early: 0, left_early: 0, overtime: 0 }; map.set(e.event_date, s); }
      s.total++;
      if (e.event_type === "CHECK_IN") s.check_ins++; else if (e.event_type === "CHECK_OUT") s.check_outs++;
      if (e.status === "ON_TIME") s.on_time++; else if (e.status === "LATE") s.late++; else if (e.status === "EARLY") s.early++; else if (e.status === "LEFT_EARLY") s.left_early++; else if (e.status === "OVERTIME") s.overtime++;
    }
    return Array.from(map.values()).sort((a, b) => b.date.localeCompare(a.date));
  }, [events]);

  const totals = useMemo(() => dailySummaries.reduce((acc, s) => ({ total: acc.total + s.total, check_ins: acc.check_ins + s.check_ins, check_outs: acc.check_outs + s.check_outs, on_time: acc.on_time + s.on_time, late: acc.late + s.late, early: acc.early + s.early, left_early: acc.left_early + s.left_early, overtime: acc.overtime + s.overtime }), { total: 0, check_ins: 0, check_outs: 0, on_time: 0, late: 0, early: 0, left_early: 0, overtime: 0 }), [dailySummaries]);

  const handleExport = () => {
    const header = "date,total,check_ins,check_outs,on_time,late,early,left_early,overtime\n";
    const rows = dailySummaries.map((s) => `${s.date},${s.total},${s.check_ins},${s.check_outs},${s.on_time},${s.late},${s.early},${s.left_early},${s.overtime}`).join("\n");
    const blob = new Blob([header + rows], { type: "text/csv" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a"); a.href = url; a.download = `daily_summary_${new Date().toISOString().slice(0, 10)}.csv`; a.click(); URL.revokeObjectURL(url);
  };

  return (
    <div className="space-y-4">
      <div className="flex flex-col sm:flex-row gap-3 items-start sm:items-center justify-between">
        <div className="flex items-center gap-3 flex-wrap">
          <div className="flex items-center gap-2">
            <Calendar className="w-4 h-4 text-slate-400" />
            <input type="date" value={fromDate} onChange={(e) => setFromDate(e.target.value)} className="px-3 py-2 text-sm border border-slate-200 rounded-lg focus:outline-none focus:border-teal-500" />
            <span className="text-slate-400 text-sm">to</span>
            <input type="date" value={toDate} onChange={(e) => setToDate(e.target.value)} className="px-3 py-2 text-sm border border-slate-200 rounded-lg focus:outline-none focus:border-teal-500" />
          </div>
          {(fromDate || toDate) && <button onClick={() => { setFromDate(""); setToDate(""); }} className="text-xs text-slate-500 hover:text-slate-700">Clear</button>}
        </div>
        <button onClick={handleExport} disabled={dailySummaries.length === 0} className="flex items-center gap-2 px-3 py-2 text-sm font-medium rounded-lg border border-slate-200 text-slate-600 hover:bg-slate-50 disabled:opacity-50 transition-colors"><Download className="w-4 h-4" />Export Summary</button>
      </div>

      {!loading && !error && dailySummaries.length > 0 && (
        <div className="grid grid-cols-2 sm:grid-cols-4 lg:grid-cols-8 gap-3">
          {[{ label: "Total", value: totals.total, color: "text-slate-800" }, { label: "Check-ins", value: totals.check_ins, color: "text-emerald-600" }, { label: "Check-outs", value: totals.check_outs, color: "text-blue-600" }, { label: "On Time", value: totals.on_time, color: "text-emerald-600" }, { label: "Late", value: totals.late, color: "text-amber-600" }, { label: "Early", value: totals.early, color: "text-sky-600" }, { label: "Left Early", value: totals.left_early, color: "text-orange-600" }, { label: "Overtime", value: totals.overtime, color: "text-violet-600" }].map((stat) => (
            <Card key={stat.label} className="p-3"><p className="text-xs text-slate-500 font-medium">{stat.label}</p><p className={`text-xl font-bold ${stat.color} mt-1`}>{stat.value}</p></Card>
          ))}
        </div>
      )}

      <Card className="overflow-hidden">
        {loading ? <LoadingSpinner label="Loading reports..." /> : error ? <ErrorState message={error} /> : dailySummaries.length === 0 ? (
          <EmptyState icon={<FileBarChart className="w-7 h-7" />} title="No data for selected range" description="Select a date range or wait for attendance events to be synced from the ESP32 device." />
        ) : (
          <div className="overflow-x-auto">
            <table className="w-full text-sm">
              <thead><tr className="border-b border-slate-200 bg-slate-50">
                <th className="text-left px-4 py-3 font-medium text-slate-600">Date</th>
                <th className="text-center px-4 py-3 font-medium text-slate-600">Total</th>
                <th className="text-center px-4 py-3 font-medium text-slate-600">In</th>
                <th className="text-center px-4 py-3 font-medium text-slate-600">Out</th>
                <th className="text-center px-4 py-3 font-medium text-slate-600 hidden sm:table-cell">On Time</th>
                <th className="text-center px-4 py-3 font-medium text-slate-600 hidden sm:table-cell">Late</th>
                <th className="text-center px-4 py-3 font-medium text-slate-600 hidden md:table-cell">Early</th>
                <th className="text-center px-4 py-3 font-medium text-slate-600 hidden md:table-cell">Left Early</th>
                <th className="text-center px-4 py-3 font-medium text-slate-600 hidden md:table-cell">Overtime</th>
              </tr></thead>
              <tbody className="divide-y divide-slate-100">
                {dailySummaries.map((s) => (
                  <tr key={s.date} className="hover:bg-slate-50 transition-colors">
                    <td className="px-4 py-3 text-slate-700 font-medium whitespace-nowrap">{s.date}</td>
                    <td className="px-4 py-3 text-center font-semibold text-slate-800">{s.total}</td>
                    <td className="px-4 py-3 text-center text-emerald-600">{s.check_ins}</td>
                    <td className="px-4 py-3 text-center text-blue-600">{s.check_outs}</td>
                    <td className="px-4 py-3 text-center text-slate-600 hidden sm:table-cell">{s.on_time}</td>
                    <td className="px-4 py-3 text-center text-amber-600 hidden sm:table-cell">{s.late}</td>
                    <td className="px-4 py-3 text-center text-sky-600 hidden md:table-cell">{s.early}</td>
                    <td className="px-4 py-3 text-center text-orange-600 hidden md:table-cell">{s.left_early}</td>
                    <td className="px-4 py-3 text-center text-violet-600 hidden md:table-cell">{s.overtime}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </Card>
    </div>
  );
}

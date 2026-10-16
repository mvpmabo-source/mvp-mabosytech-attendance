import { useEffect, useState, useMemo } from "react";
import { CalendarCheck, Search, ChevronLeft, ChevronRight, Download, Filter, X } from "lucide-react";
import { supabase, type AttendanceEvent, type Teacher, STATUS_LABELS, STATUS_COLORS, EVENT_LABELS } from "@/lib/supabase";
import { Card, LoadingSpinner, EmptyState, ErrorState, Badge } from "@/components/ui";

const PAGE_SIZE = 25;

type TeacherMap = Map<number, Teacher>;

function isGenericName(name: string): boolean {
  return !name || name.trim() === "" || /^ID\s*\d+$/i.test(name.trim());
}

function enrichEvent(event: AttendanceEvent, teachers: TeacherMap): AttendanceEvent {
  const teacher = teachers.get(event.fingerprint_id);
  if (!teacher) return event;
  const genericName = isGenericName(event.name);
  const needsCollegeId = !event.college_id || !event.college_id.trim() || ["-", "—"].includes(event.college_id.trim());
  const needsRole = !event.role || !event.role.trim() || ["-", "—"].includes(event.role.trim());
  const needsDepartment = !event.department || !event.department.trim() || ["-", "—"].includes(event.department.trim());
  if (!genericName && !needsCollegeId && !needsRole && !needsDepartment) return event;
  return {
    ...event,
    name: genericName ? (teacher.name || event.name) : event.name,
    college_id: needsCollegeId ? (teacher.college_id || event.college_id) : event.college_id,
    role: needsRole ? (teacher.role || event.role) : event.role,
    department: needsDepartment ? (teacher.department || event.department) : event.department,
  };
}

export function AttendancePage() {
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [events, setEvents] = useState<AttendanceEvent[]>([]);
  const [total, setTotal] = useState(0);
  const [page, setPage] = useState(0);
  const [search, setSearch] = useState("");
  const [dateFilter, setDateFilter] = useState("");
  const [typeFilter, setTypeFilter] = useState("");
  const [statusFilter, setStatusFilter] = useState("");
  const [showFilters, setShowFilters] = useState(false);
  const [teachers, setTeachers] = useState<TeacherMap>(new Map());

  const from = page * PAGE_SIZE;
  const to = from + PAGE_SIZE - 1;

  useEffect(() => {
    async function load() {
      setLoading(true);
      setError(null);
      try {
        let query = supabase.from("attendance_events").select("*", { count: "exact" }).order("event_date", { ascending: false }).order("event_time", { ascending: false }).range(from, to);
        if (dateFilter) query = query.eq("event_date", dateFilter);
        if (typeFilter) query = query.eq("event_type", typeFilter);
        if (statusFilter) query = query.eq("status", statusFilter);
        const { data, error: qErr, count } = await query;
        if (qErr) throw new Error(qErr.message);
        setEvents((data as AttendanceEvent[]) ?? []);
        setTotal(count ?? 0);

        // Profile lookup is best-effort: a teachers-table issue must not break attendance loading.
        const { data: teacherRows, error: teacherError } = await supabase.from("teachers").select("*");
        if (!teacherError && teacherRows) {
          const teacherMap: TeacherMap = new Map();
          for (const teacher of teacherRows as Teacher[]) {
            teacherMap.set(teacher.fingerprint_id, teacher);
          }
          setTeachers(teacherMap);
        }
      } catch (err) {
        setError(err instanceof Error ? err.message : "Failed to load attendance records");
      } finally {
        setLoading(false);
      }
    }
    load();
  }, [from, to, dateFilter, typeFilter, statusFilter]);

  const filteredEvents = useMemo(() => {
    const enriched = events.map((event) => enrichEvent(event, teachers));
    if (!search.trim()) return enriched;
    const q = search.toLowerCase();
    return enriched.filter((e) => e.name.toLowerCase().includes(q) || e.college_id.toLowerCase().includes(q) || e.department.toLowerCase().includes(q) || String(e.fingerprint_id).includes(q));
  }, [events, search, teachers]);

  const totalPages = Math.ceil(total / PAGE_SIZE);

  const handleExport = () => {
    const header = "date,time,fingerprint_id,name,college_id,role,department,event_type,status\n";
    const rows = filteredEvents.map((e) => `${e.event_date},${e.event_time},${e.fingerprint_id},"${e.name}","${e.college_id}","${e.role}","${e.department}",${e.event_type},${e.status}`).join("\n");
    const blob = new Blob([header + rows], { type: "text/csv" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = `attendance_${new Date().toISOString().slice(0, 10)}.csv`;
    a.click();
    URL.revokeObjectURL(url);
  };

  const clearFilters = () => { setDateFilter(""); setTypeFilter(""); setStatusFilter(""); setSearch(""); setPage(0); };
  const hasFilters = dateFilter || typeFilter || statusFilter || search;

  return (
    <div className="space-y-4">
      <div className="flex flex-col sm:flex-row gap-3 items-start sm:items-center justify-between">
        <div className="flex items-center gap-3 flex-1 min-w-0">
          <div className="relative flex-1 max-w-xs">
            <Search className="absolute left-3 top-1/2 -translate-y-1/2 w-4 h-4 text-slate-400" />
            <input type="text" placeholder="Search name, ID, dept..." value={search} onChange={(e) => setSearch(e.target.value)} className="w-full pl-10 pr-4 py-2 text-sm border border-slate-200 rounded-lg focus:outline-none focus:border-teal-500 focus:ring-1 focus:ring-teal-500" />
          </div>
          <button onClick={() => setShowFilters(!showFilters)} className={`flex items-center gap-2 px-3 py-2 text-sm font-medium rounded-lg border transition-colors ${showFilters || hasFilters ? "border-teal-500 text-teal-600 bg-teal-50" : "border-slate-200 text-slate-600 hover:bg-slate-50"}`}>
            <Filter className="w-4 h-4" />Filters
          </button>
        </div>
        <button onClick={handleExport} disabled={filteredEvents.length === 0} className="flex items-center gap-2 px-3 py-2 text-sm font-medium rounded-lg border border-slate-200 text-slate-600 hover:bg-slate-50 disabled:opacity-50 transition-colors">
          <Download className="w-4 h-4" />Export CSV
        </button>
      </div>

      {showFilters && (
        <Card className="p-4">
          <div className="grid grid-cols-1 sm:grid-cols-3 gap-3">
            <div>
              <label className="block text-xs font-medium text-slate-500 mb-1">Date</label>
              <input type="date" value={dateFilter} onChange={(e) => { setDateFilter(e.target.value); setPage(0); }} className="w-full px-3 py-2 text-sm border border-slate-200 rounded-lg focus:outline-none focus:border-teal-500" />
            </div>
            <div>
              <label className="block text-xs font-medium text-slate-500 mb-1">Event Type</label>
              <select value={typeFilter} onChange={(e) => { setTypeFilter(e.target.value); setPage(0); }} className="w-full px-3 py-2 text-sm border border-slate-200 rounded-lg focus:outline-none focus:border-teal-500 bg-white">
                <option value="">All</option><option value="CHECK_IN">Check In</option><option value="CHECK_OUT">Check Out</option>
              </select>
            </div>
            <div>
              <label className="block text-xs font-medium text-slate-500 mb-1">Status</label>
              <select value={statusFilter} onChange={(e) => { setStatusFilter(e.target.value); setPage(0); }} className="w-full px-3 py-2 text-sm border border-slate-200 rounded-lg focus:outline-none focus:border-teal-500 bg-white">
                <option value="">All</option>
                {Object.entries(STATUS_LABELS).map(([val, label]) => (<option key={val} value={val}>{label}</option>))}
              </select>
            </div>
          </div>
          {hasFilters && <button onClick={clearFilters} className="mt-3 flex items-center gap-1 text-xs text-slate-500 hover:text-slate-700"><X className="w-3 h-3" />Clear all filters</button>}
        </Card>
      )}

      <Card className="overflow-hidden">
        {loading ? <LoadingSpinner label="Loading attendance records..." /> : error ? <ErrorState message={error} /> : filteredEvents.length === 0 ? (
          <EmptyState icon={<CalendarCheck className="w-7 h-7" />} title={hasFilters ? "No matching records" : "No attendance records yet"} description={hasFilters ? "Try adjusting your filters or search terms." : "Attendance events will appear here once your ESP32 device syncs them to the cloud."} />
        ) : (
          <div className="overflow-x-auto">
            <table className="w-full text-sm">
              <thead><tr className="border-b border-slate-200 bg-slate-50">
                <th className="text-left px-4 py-3 font-medium text-slate-600">Date</th>
                <th className="text-left px-4 py-3 font-medium text-slate-600">Time</th>
                <th className="text-left px-4 py-3 font-medium text-slate-600">ID</th>
                <th className="text-left px-4 py-3 font-medium text-slate-600">Name</th>
                <th className="text-left px-4 py-3 font-medium text-slate-600 hidden md:table-cell">College ID</th>
                <th className="text-left px-4 py-3 font-medium text-slate-600 hidden lg:table-cell">Department</th>
                <th className="text-left px-4 py-3 font-medium text-slate-600">Type</th>
                <th className="text-left px-4 py-3 font-medium text-slate-600">Status</th>
              </tr></thead>
              <tbody className="divide-y divide-slate-100">
                {filteredEvents.map((e) => (
                  <tr key={e.id} className="hover:bg-slate-50 transition-colors">
                    <td className="px-4 py-3 text-slate-700 whitespace-nowrap">{e.event_date}</td>
                    <td className="px-4 py-3 text-slate-700 font-mono text-xs whitespace-nowrap">{e.event_time}</td>
                    <td className="px-4 py-3 text-slate-700 whitespace-nowrap">{e.fingerprint_id}</td>
                    <td className="px-4 py-3 text-slate-700 whitespace-nowrap">{e.name || `ID ${e.fingerprint_id}`}</td>
                    <td className="px-4 py-3 text-slate-500 hidden md:table-cell whitespace-nowrap">{e.college_id || "—"}</td>
                    <td className="px-4 py-3 text-slate-500 hidden lg:table-cell whitespace-nowrap">{e.department || "—"}</td>
                    <td className="px-4 py-3 whitespace-nowrap"><Badge className={e.event_type === "CHECK_IN" ? "bg-emerald-50 text-emerald-700 border-emerald-200" : "bg-blue-50 text-blue-700 border-blue-200"}>{EVENT_LABELS[e.event_type] ?? e.event_type}</Badge></td>
                    <td className="px-4 py-3 whitespace-nowrap"><Badge className={STATUS_COLORS[e.status] ?? "bg-slate-100 text-slate-600 border-slate-200"}>{STATUS_LABELS[e.status] ?? e.status}</Badge></td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
        {!loading && !error && total > PAGE_SIZE && (
          <div className="flex items-center justify-between px-4 py-3 border-t border-slate-200">
            <p className="text-xs text-slate-500">Showing {from + 1}–{Math.min(from + filteredEvents.length, total)} of {total}</p>
            <div className="flex items-center gap-2">
              <button onClick={() => setPage(Math.max(0, page - 1))} disabled={page === 0} className="p-1.5 rounded-lg border border-slate-200 disabled:opacity-40 hover:bg-slate-50 transition-colors"><ChevronLeft className="w-4 h-4" /></button>
              <span className="text-sm text-slate-600">Page {page + 1} / {totalPages}</span>
              <button onClick={() => setPage(Math.min(totalPages - 1, page + 1))} disabled={page >= totalPages - 1} className="p-1.5 rounded-lg border border-slate-200 disabled:opacity-40 hover:bg-slate-50 transition-colors"><ChevronRight className="w-4 h-4" /></button>
            </div>
          </div>
        )}
      </Card>
    </div>
  );
}

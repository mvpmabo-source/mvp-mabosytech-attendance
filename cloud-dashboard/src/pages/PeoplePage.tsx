import { useEffect, useState, useMemo, useCallback, type FormEvent } from "react";
import { Users, Search, LogIn, LogOut, Trash2, RefreshCw, UserPlus } from "lucide-react";
import { supabase, type AttendanceEvent } from "@/lib/supabase";
import { Card, LoadingSpinner, EmptyState, ErrorState, Badge } from "@/components/ui";

type Person = {
  fingerprint_id: number; name: string; college_id: string; role: string; department: string;
  check_ins: number; check_outs: number; last_seen: string | null; last_type: string | null;
  enrollment_status: string;
};

export function PeoplePage() {
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [notice, setNotice] = useState<string | null>(null);
  const [people, setPeople] = useState<Person[]>([]);
  const [search, setSearch] = useState("");
  const [deletingId, setDeletingId] = useState<number | null>(null);
  const [showEnrollForm, setShowEnrollForm] = useState(false);
  const [enrollBusy, setEnrollBusy] = useState(false);
  const [newTeacher, setNewTeacher] = useState({ name: "", college_id: "", role: "Teacher", department: "", note: "" });

  const load = useCallback(async () => {
    try {
      const [teacherRes, eventRes] = await Promise.all([
        supabase.from("teachers").select("*").order("fingerprint_id", { ascending: true }),
        supabase.from("attendance_events").select("fingerprint_id, name, college_id, role, department, event_type, event_date, event_time").order("event_date", { ascending: false }).order("event_time", { ascending: false }),
      ]);
      if (teacherRes.error) throw new Error(teacherRes.error.message);
      if (eventRes.error) throw new Error(eventRes.error.message);
      const events = (eventRes.data as AttendanceEvent[]) ?? [];
      const stats = new Map<number, Person>();
      for (const e of events) {
        let p = stats.get(e.fingerprint_id);
        if (!p) {
          p = { fingerprint_id: e.fingerprint_id, name: e.name || `ID ${e.fingerprint_id}`, college_id: e.college_id || "", role: e.role || "", department: e.department || "", check_ins: 0, check_outs: 0, last_seen: null, last_type: null, enrollment_status: "enrolled" };
          stats.set(e.fingerprint_id, p);
        }
        if (!p.last_seen) { p.last_seen = `${e.event_date} ${e.event_time}`; p.last_type = e.event_type; }
        if (e.event_type === "CHECK_IN") p.check_ins++; else if (e.event_type === "CHECK_OUT") p.check_outs++;
      }
      const registry = (teacherRes.data ?? []) as Array<{ fingerprint_id: number; name?: string; college_id?: string; role?: string; department?: string; enrollment_status?: string }>;
      setPeople(registry.map((t) => {
        const s = stats.get(t.fingerprint_id);
        return {
          fingerprint_id: t.fingerprint_id,
          name: t.name || s?.name || `ID ${t.fingerprint_id}`,
          college_id: t.college_id || s?.college_id || "",
          role: t.role || s?.role || "Teacher",
          department: t.department || s?.department || "",
          check_ins: s?.check_ins ?? 0, check_outs: s?.check_outs ?? 0,
          last_seen: s?.last_seen ?? null, last_type: s?.last_type ?? null,
          enrollment_status: t.enrollment_status || "enrolled",
        };
      }));
      setError(null);
    } catch (err) {
      setError(err instanceof Error ? err.message : "Failed to load teacher registry");
    } finally { setLoading(false); }
  }, []);

  useEffect(() => {
    void load();
    const interval = setInterval(() => { void load(); }, 5000);
    return () => clearInterval(interval);
  }, [load]);

  async function queueEnrollment(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!newTeacher.name.trim()) { setError("Teacher name is required."); return; }
    setEnrollBusy(true); setError(null); setNotice(null);
    try {
      const { data: devices, error: deviceError } = await supabase.from("devices").select("device_id").order("created_at", { ascending: false }).limit(1);
      if (deviceError) throw new Error(deviceError.message);
      const deviceId = devices?.[0]?.device_id;
      if (!deviceId) throw new Error("No registered ESP32 device found.");
      const { error: commandError } = await supabase.from("device_commands").insert({
        device_id: deviceId,
        command: "enroll_fingerprint",
        payload: {
          name: newTeacher.name.trim(),
          college_id: newTeacher.college_id.trim(),
          role: newTeacher.role,
          department: newTeacher.department.trim(),
          note: newTeacher.note.trim(),
          fingerprint_id: 0,
        },
        status: "pending",
      });
      if (commandError) throw new Error(commandError.message);
      setNotice(`Enrollment for ${newTeacher.name.trim()} queued. The ESP32 will prompt for the finger; the teacher is added to the registry only after the sensor confirms enrollment.`);
      setNewTeacher({ name: "", college_id: "", role: "Teacher", department: "", note: "" });
      setShowEnrollForm(false);
    } catch (err) {
      setError(err instanceof Error ? err.message : "Could not queue enrollment");
    } finally { setEnrollBusy(false); }
  }

  async function deleteFingerprint(person: Person) {
    if (!window.confirm(`Delete fingerprint ID ${person.fingerprint_id} (${person.name}) from the ESP32 sensor? The teacher will be removed from the active registry only after the device confirms deletion. Attendance history is preserved unless you run Clean Start.`)) return;
    setDeletingId(person.fingerprint_id);
    setNotice(null);
    try {
      const { data: devices, error: deviceError } = await supabase.from("devices").select("device_id").order("created_at", { ascending: false }).limit(1);
      if (deviceError) throw new Error(deviceError.message);
      const deviceId = devices?.[0]?.device_id;
      if (!deviceId) throw new Error("No registered ESP32 device found.");
      const { error: commandError } = await supabase.from("device_commands").insert({
        device_id: deviceId, command: "delete_fingerprint",
        payload: { fingerprint_id: person.fingerprint_id },
        status: "pending",
      });
      if (commandError) throw new Error(commandError.message);
      setNotice(`Delete request for fingerprint ID ${person.fingerprint_id} queued. The registry updates after the ESP32 confirms sensor deletion.`);
      await load();
    } catch (err) {
      setError(err instanceof Error ? err.message : "Could not queue fingerprint deletion");
    } finally { setDeletingId(null); }
  }

  const filteredPeople = useMemo(() => {
    if (!search.trim()) return people;
    const q = search.toLowerCase();
    return people.filter((p) => p.name.toLowerCase().includes(q) || p.college_id.toLowerCase().includes(q) || p.department.toLowerCase().includes(q) || String(p.fingerprint_id).includes(q) || p.role.toLowerCase().includes(q));
  }, [people, search]);

  return (
    <div className="space-y-4">
      {notice && <Card className="p-3 border-sky-200 bg-sky-50"><p className="text-sm text-sky-800">{notice}</p></Card>}
      <div className="flex items-center justify-between gap-3 flex-wrap">
        <p className="text-sm text-slate-500">Manage active sensor fingerprints and teacher metadata.</p>
        <button onClick={() => setShowEnrollForm((v) => !v)} className="inline-flex items-center gap-2 px-3 py-2 text-sm font-medium rounded-lg bg-teal-700 text-white hover:bg-teal-800"><UserPlus className="w-4 h-4" />Enroll new teacher</button>
      </div>
      {showEnrollForm && <Card className="p-4">
        <h3 className="font-semibold text-slate-800 mb-3">New teacher enrollment</h3>
        <form onSubmit={queueEnrollment} className="grid grid-cols-1 md:grid-cols-2 gap-3">
          <label className="text-sm text-slate-600">Full name *<input required value={newTeacher.name} onChange={(e) => setNewTeacher((v) => ({ ...v, name: e.target.value }))} className="mt-1 w-full px-3 py-2 border border-slate-200 rounded-lg" /></label>
          <label className="text-sm text-slate-600">College ID<input value={newTeacher.college_id} onChange={(e) => setNewTeacher((v) => ({ ...v, college_id: e.target.value }))} className="mt-1 w-full px-3 py-2 border border-slate-200 rounded-lg" /></label>
          <label className="text-sm text-slate-600">Role<select value={newTeacher.role} onChange={(e) => setNewTeacher((v) => ({ ...v, role: e.target.value }))} className="mt-1 w-full px-3 py-2 border border-slate-200 rounded-lg"><option>Teacher</option><option>Administrator</option><option>Staff</option></select></label>
          <label className="text-sm text-slate-600">Department<input value={newTeacher.department} onChange={(e) => setNewTeacher((v) => ({ ...v, department: e.target.value }))} className="mt-1 w-full px-3 py-2 border border-slate-200 rounded-lg" /></label>
          <label className="text-sm text-slate-600 md:col-span-2">Note<input value={newTeacher.note} onChange={(e) => setNewTeacher((v) => ({ ...v, note: e.target.value }))} className="mt-1 w-full px-3 py-2 border border-slate-200 rounded-lg" /></label>
          <div className="md:col-span-2 flex gap-2"><button type="submit" disabled={enrollBusy} className="px-4 py-2 rounded-lg bg-teal-700 text-white text-sm disabled:opacity-50">{enrollBusy ? "Sending…" : "Start enrollment on ESP32"}</button><button type="button" onClick={() => setShowEnrollForm(false)} className="px-4 py-2 rounded-lg border border-slate-200 text-sm">Cancel</button></div>
        </form>
      </Card>}
      <div className="flex items-center gap-3">
        <div className="relative flex-1 max-w-xs">
          <Search className="absolute left-3 top-1/2 -translate-y-1/2 w-4 h-4 text-slate-400" />
          <input type="text" placeholder="Search teachers..." value={search} onChange={(e) => setSearch(e.target.value)} className="w-full pl-10 pr-4 py-2 text-sm border border-slate-200 rounded-lg focus:outline-none focus:border-teal-500 focus:ring-1 focus:ring-teal-500" />
        </div>
        <span className="text-sm text-slate-500">{filteredPeople.length} {filteredPeople.length === 1 ? "teacher" : "teachers"}</span>
        <button onClick={() => void load()} className="inline-flex items-center gap-2 px-3 py-2 text-sm rounded-lg border border-slate-200 text-slate-600 hover:bg-slate-50"><RefreshCw className="w-4 h-4" />Refresh</button>
      </div>
      <Card className="overflow-hidden">
        {loading ? <LoadingSpinner label="Loading teachers..." /> : error ? <ErrorState message={error} /> : filteredPeople.length === 0 ? (
          <EmptyState icon={<Users className="w-7 h-7" />} title={search ? "No matching teachers" : "No active teacher fingerprints"} description={search ? "Try a different search term." : "Only teachers in the active registry appear here. Legacy attendance history is not treated as an active fingerprint."} />
        ) : (
          <div className="overflow-x-auto">
            <table className="w-full text-sm">
              <thead><tr className="border-b border-slate-200 bg-slate-50">
                <th className="text-left px-4 py-3 font-medium text-slate-600">FP ID</th>
                <th className="text-left px-4 py-3 font-medium text-slate-600">Name</th>
                <th className="text-left px-4 py-3 font-medium text-slate-600 hidden md:table-cell">College ID</th>
                <th className="text-left px-4 py-3 font-medium text-slate-600">Role</th>
                <th className="text-left px-4 py-3 font-medium text-slate-600 hidden lg:table-cell">Department</th>
                <th className="text-center px-4 py-3 font-medium text-slate-600">Check-ins</th>
                <th className="text-center px-4 py-3 font-medium text-slate-600">Check-outs</th>
                <th className="text-left px-4 py-3 font-medium text-slate-600 hidden sm:table-cell">Last Activity</th>
                <th className="text-right px-4 py-3 font-medium text-slate-600">Action</th>
              </tr></thead>
              <tbody className="divide-y divide-slate-100">
                {filteredPeople.map((p) => (
                  <tr key={p.fingerprint_id} className="hover:bg-slate-50">
                    <td className="px-4 py-3 text-slate-700 font-mono">{p.fingerprint_id}</td>
                    <td className="px-4 py-3 text-slate-700 font-medium whitespace-nowrap">{p.name}</td>
                    <td className="px-4 py-3 text-slate-500 hidden md:table-cell whitespace-nowrap">{p.college_id || "—"}</td>
                    <td className="px-4 py-3 whitespace-nowrap"><Badge className={p.enrollment_status === "failed" ? "bg-rose-50 text-rose-700 border-rose-200" : "bg-slate-100 text-slate-600 border-slate-200"}>{p.role || "Teacher"} · {p.enrollment_status}</Badge></td>
                    <td className="px-4 py-3 text-slate-500 hidden lg:table-cell whitespace-nowrap">{p.department || "—"}</td>
                    <td className="px-4 py-3 text-center"><span className="inline-flex items-center gap-1 text-emerald-600 font-medium"><LogIn className="w-3.5 h-3.5" />{p.check_ins}</span></td>
                    <td className="px-4 py-3 text-center"><span className="inline-flex items-center gap-1 text-blue-600 font-medium"><LogOut className="w-3.5 h-3.5" />{p.check_outs}</span></td>
                    <td className="px-4 py-3 text-slate-500 text-xs hidden sm:table-cell whitespace-nowrap">{p.last_seen || "—"}</td>
                    <td className="px-4 py-3 text-right"><button onClick={() => void deleteFingerprint(p)} disabled={deletingId === p.fingerprint_id} className="inline-flex items-center gap-1 px-2 py-1.5 text-xs rounded border border-rose-200 text-rose-700 hover:bg-rose-50 disabled:opacity-50"><Trash2 className="w-3.5 h-3.5" />{deletingId === p.fingerprint_id ? "Queueing…" : "Delete FP"}</button></td>
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

import { useEffect, useState, useMemo } from "react";
import { Users, Search, LogIn, LogOut } from "lucide-react";
import { supabase, type AttendanceEvent } from "@/lib/supabase";
import { Card, LoadingSpinner, EmptyState, ErrorState, Badge } from "@/components/ui";

type Person = { fingerprint_id: number; name: string; college_id: string; role: string; department: string; check_ins: number; check_outs: number; last_seen: string | null; last_type: string | null };

export function PeoplePage() {
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [people, setPeople] = useState<Person[]>([]);
  const [search, setSearch] = useState("");

  useEffect(() => {
    async function load() {
      try {
        const { data, error: qErr } = await supabase.from("attendance_events").select("fingerprint_id, name, college_id, role, department, event_type, event_date, event_time").order("event_date", { ascending: false }).order("event_time", { ascending: false });
        if (qErr) throw new Error(qErr.message);
        const events = (data as AttendanceEvent[]) ?? [];
        const personMap = new Map<number, Person>();
        for (const e of events) {
          let p = personMap.get(e.fingerprint_id);
          if (!p) { p = { fingerprint_id: e.fingerprint_id, name: e.name || `ID ${e.fingerprint_id}`, college_id: e.college_id, role: e.role, department: e.department, check_ins: 0, check_outs: 0, last_seen: null, last_type: null }; personMap.set(e.fingerprint_id, p); }
          if (!p.last_seen) { p.last_seen = `${e.event_date} ${e.event_time}`; p.last_type = e.event_type; }
          if (e.event_type === "CHECK_IN") p.check_ins++; else if (e.event_type === "CHECK_OUT") p.check_outs++;
        }
        setPeople(Array.from(personMap.values()).sort((a, b) => a.fingerprint_id - b.fingerprint_id));
      } catch (err) { setError(err instanceof Error ? err.message : "Failed to load people"); } finally { setLoading(false); }
    }
    load();
  }, []);

  const filteredPeople = useMemo(() => {
    if (!search.trim()) return people;
    const q = search.toLowerCase();
    return people.filter((p) => p.name.toLowerCase().includes(q) || p.college_id.toLowerCase().includes(q) || p.department.toLowerCase().includes(q) || String(p.fingerprint_id).includes(q) || p.role.toLowerCase().includes(q));
  }, [people, search]);

  return (
    <div className="space-y-4">
      <div className="flex items-center gap-3">
        <div className="relative flex-1 max-w-xs">
          <Search className="absolute left-3 top-1/2 -translate-y-1/2 w-4 h-4 text-slate-400" />
          <input type="text" placeholder="Search people..." value={search} onChange={(e) => setSearch(e.target.value)} className="w-full pl-10 pr-4 py-2 text-sm border border-slate-200 rounded-lg focus:outline-none focus:border-teal-500 focus:ring-1 focus:ring-teal-500" />
        </div>
        <span className="text-sm text-slate-500">{filteredPeople.length} {filteredPeople.length === 1 ? "person" : "people"}</span>
      </div>
      <Card className="overflow-hidden">
        {loading ? <LoadingSpinner label="Loading people..." /> : error ? <ErrorState message={error} /> : filteredPeople.length === 0 ? (
          <EmptyState icon={<Users className="w-7 h-7" />} title={search ? "No matching people" : "No people records yet"} description={search ? "Try a different search term." : "Registered fingerprint profiles will appear here once attendance events are synced from the ESP32."} />
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
              </tr></thead>
              <tbody className="divide-y divide-slate-100">
                {filteredPeople.map((p) => (
                  <tr key={p.fingerprint_id} className="hover:bg-slate-50 transition-colors">
                    <td className="px-4 py-3 text-slate-700 font-mono">{p.fingerprint_id}</td>
                    <td className="px-4 py-3 text-slate-700 font-medium whitespace-nowrap">{p.name}</td>
                    <td className="px-4 py-3 text-slate-500 hidden md:table-cell whitespace-nowrap">{p.college_id || "—"}</td>
                    <td className="px-4 py-3 whitespace-nowrap">{p.role && <Badge className={p.role === "Administrator" ? "bg-teal-50 text-teal-700 border-teal-200" : "bg-slate-100 text-slate-600 border-slate-200"}>{p.role}</Badge>}</td>
                    <td className="px-4 py-3 text-slate-500 hidden lg:table-cell whitespace-nowrap">{p.department || "—"}</td>
                    <td className="px-4 py-3 text-center"><span className="inline-flex items-center gap-1 text-emerald-600 font-medium"><LogIn className="w-3.5 h-3.5" />{p.check_ins}</span></td>
                    <td className="px-4 py-3 text-center"><span className="inline-flex items-center gap-1 text-blue-600 font-medium"><LogOut className="w-3.5 h-3.5" />{p.check_outs}</span></td>
                    <td className="px-4 py-3 text-slate-500 text-xs hidden sm:table-cell whitespace-nowrap">{p.last_seen ? <span>{p.last_seen}</span> : "—"}</td>
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

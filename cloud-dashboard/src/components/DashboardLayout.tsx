import { useState, type ReactNode } from "react";
import {
  LayoutDashboard,
  CalendarCheck,
  Users,
  FileBarChart,
  Cpu,
  Settings,
  LogOut,
  Menu,
  X,
  Fingerprint,
} from "lucide-react";
import { useAuth } from "@/context/AuthContext";

type Page = "overview" | "attendance" | "people" | "reports" | "device" | "settings";

const NAV_ITEMS: { id: Page; label: string; icon: typeof LayoutDashboard }[] = [
  { id: "overview", label: "Overview", icon: LayoutDashboard },
  { id: "attendance", label: "Attendance", icon: CalendarCheck },
  { id: "people", label: "People", icon: Users },
  { id: "reports", label: "Reports", icon: FileBarChart },
  { id: "device", label: "Device", icon: Cpu },
  { id: "settings", label: "Settings", icon: Settings },
];

export function DashboardLayout({
  currentPage,
  onPageChange,
  children,
}: {
  currentPage: Page;
  onPageChange: (p: Page) => void;
  children: ReactNode;
}) {
  const { user, signOut } = useAuth();
  const [sidebarOpen, setSidebarOpen] = useState(false);

  const handleNav = (p: Page) => {
    onPageChange(p);
    setSidebarOpen(false);
  };

  return (
    <div className="min-h-screen bg-slate-50 flex">
      <aside
        className={`fixed lg:sticky top-0 left-0 z-40 h-screen w-64 bg-slate-900 text-slate-100 flex flex-col transition-transform duration-200 ${
          sidebarOpen ? "translate-x-0" : "-translate-x-full lg:translate-x-0"
        }`}
      >
        <div className="flex items-center gap-3 px-5 py-5 border-b border-slate-700">
          <div className="w-10 h-10 rounded-lg bg-teal-500/20 flex items-center justify-center">
            <Fingerprint className="w-6 h-6 text-teal-400" />
          </div>
          <div className="min-w-0">
            <h1 className="text-sm font-semibold truncate">MABOSYTECH</h1>
            <p className="text-xs text-slate-400">Attendance Cloud</p>
          </div>
        </div>

        <nav className="flex-1 px-3 py-4 space-y-1">
          {NAV_ITEMS.map((item) => {
            const Icon = item.icon;
            const active = currentPage === item.id;
            return (
              <button
                key={item.id}
                onClick={() => handleNav(item.id)}
                className={`w-full flex items-center gap-3 px-3 py-2.5 rounded-lg text-sm font-medium transition-colors ${
                  active ? "bg-teal-500/20 text-teal-300" : "text-slate-300 hover:bg-slate-800 hover:text-white"
                }`}
              >
                <Icon className="w-5 h-5 flex-shrink-0" />
                {item.label}
              </button>
            );
          })}
        </nav>

        <div className="px-3 py-4 border-t border-slate-700">
          <div className="px-3 mb-2 text-xs text-slate-400 truncate">{user?.email}</div>
          <button
            onClick={signOut}
            className="w-full flex items-center gap-3 px-3 py-2.5 rounded-lg text-sm font-medium text-slate-300 hover:bg-slate-800 hover:text-white transition-colors"
          >
            <LogOut className="w-5 h-5" />
            Sign Out
          </button>
        </div>
      </aside>

      {sidebarOpen && (
        <div className="fixed inset-0 z-30 bg-black/40 lg:hidden" onClick={() => setSidebarOpen(false)} />
      )}

      <div className="flex-1 min-w-0 flex flex-col">
        <header className="sticky top-0 z-20 bg-white border-b border-slate-200 px-4 lg:px-8 py-4 flex items-center justify-between">
          <div className="flex items-center gap-3">
            <button
              onClick={() => setSidebarOpen(!sidebarOpen)}
              className="lg:hidden p-2 -ml-2 rounded-lg hover:bg-slate-100"
            >
              {sidebarOpen ? <X className="w-5 h-5" /> : <Menu className="w-5 h-5" />}
            </button>
            <h2 className="text-lg font-semibold text-slate-800 capitalize">
              {NAV_ITEMS.find((n) => n.id === currentPage)?.label}
            </h2>
          </div>
        </header>
        <main className="flex-1 p-4 lg:p-8 overflow-auto">{children}</main>
      </div>
    </div>
  );
}

export type { Page };

import { useState } from "react";
import { AuthProvider, useAuth } from "@/context/AuthContext";
import { AuthScreen } from "@/components/AuthScreen";
import { DashboardLayout, type Page } from "@/components/DashboardLayout";
import { OverviewPage } from "@/pages/OverviewPage";
import { AttendancePage } from "@/pages/AttendancePage";
import { PeoplePage } from "@/pages/PeoplePage";
import { ReportsPage } from "@/pages/ReportsPage";
import { DevicePage } from "@/pages/DevicePage";
import { SettingsPage } from "@/pages/SettingsPage";
import { LoadingSpinner } from "@/components/ui";

function DashboardApp() {
  const { session, loading } = useAuth();
  const [page, setPage] = useState<Page>("overview");

  if (loading) {
    return (
      <div className="min-h-screen bg-slate-50 flex items-center justify-center">
        <LoadingSpinner label="Loading..." />
      </div>
    );
  }

  if (!session) {
    return <AuthScreen />;
  }

  return (
    <DashboardLayout currentPage={page} onPageChange={setPage}>
      {page === "overview" && <OverviewPage />}
      {page === "attendance" && <AttendancePage />}
      {page === "people" && <PeoplePage />}
      {page === "reports" && <ReportsPage />}
      {page === "device" && <DevicePage />}
      {page === "settings" && <SettingsPage />}
    </DashboardLayout>
  );
}

export default function App() {
  return (
    <AuthProvider>
      <DashboardApp />
    </AuthProvider>
  );
}

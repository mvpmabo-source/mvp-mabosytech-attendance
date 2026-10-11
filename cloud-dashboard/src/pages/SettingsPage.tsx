import { useEffect, useState } from "react";
import { Settings, Key, Plus, Trash2, Shield, FileClock, Copy, Check, RotateCw } from "lucide-react";
import { supabase, type Device, type AuditLogEntry } from "@/lib/supabase";
import { Card, LoadingSpinner, EmptyState, ErrorState, Badge, timeAgo } from "@/components/ui";
import { useAuth } from "@/context/AuthContext";

async function sha256Hex(input: string): Promise<string> {
  const data = new TextEncoder().encode(input);
  const hashBuffer = await crypto.subtle.digest("SHA-256", data);
  const hashArray = Array.from(new Uint8Array(hashBuffer));
  return hashArray.map((b) => b.toString(16).padStart(2, "0")).join("");
}

function generateApiKey(): string {
  const bytes = new Uint8Array(32);
  crypto.getRandomValues(bytes);
  return Array.from(bytes).map((b) => b.toString(16).padStart(2, "0")).join("");
}

export function SettingsPage() {
  const { user } = useAuth();
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [devices, setDevices] = useState<Device[]>([]);
  const [auditLog, setAuditLog] = useState<AuditLogEntry[]>([]);
  const [showAddDevice, setShowAddDevice] = useState(false);
  const [newDeviceId, setNewDeviceId] = useState("");
  const [newDeviceName, setNewDeviceName] = useState("");
  const [generatedKey, setGeneratedKey] = useState<string | null>(null);
  const [generatedDeviceId, setGeneratedDeviceId] = useState<string | null>(null);
  const [copied, setCopied] = useState(false);
  const [busy, setBusy] = useState(false);

  async function load() {
    try {
      const [devRes, auditRes] = await Promise.all([
        supabase.from("devices").select("*").order("created_at", { ascending: false }),
        supabase.from("audit_log").select("*").order("created_at", { ascending: false }).limit(50),
      ]);
      if (devRes.error) throw new Error(devRes.error.message);
      if (auditRes.error) throw new Error(auditRes.error.message);
      setDevices((devRes.data as Device[]) ?? []);
      setAuditLog((auditRes.data as AuditLogEntry[]) ?? []);
    } catch (err) { setError(err instanceof Error ? err.message : "Failed to load settings"); } finally { setLoading(false); }
  }

  useEffect(() => { load(); }, []);

  const handleAddDevice = async (e: React.FormEvent) => {
    e.preventDefault(); setBusy(true); setError(null);
    try {
      const deviceId = newDeviceId.trim() || newDeviceName.trim().replace(/\s+/g, "-").toLowerCase();
      if (!deviceId) { setError("Device ID or name is required"); return; }
      const apiKey = generateApiKey();
      const apiKeyHash = await sha256Hex(apiKey);
      const { error: insertErr } = await supabase.from("devices").insert({ device_id: deviceId, api_key_hash: apiKeyHash, name: newDeviceName.trim() || deviceId });
      if (insertErr) throw new Error(insertErr.message);
      await supabase.from("audit_log").insert({ user_id: user?.id, action: "device_registered", target: deviceId, details: { name: newDeviceName.trim() } });
      setGeneratedDeviceId(deviceId); setGeneratedKey(apiKey); setNewDeviceId(""); setNewDeviceName(""); setShowAddDevice(false); await load();
    } catch (err) { setError(err instanceof Error ? err.message : "Failed to add device"); } finally { setBusy(false); }
  };

  // Rotate the stored hash and show the new raw key once. This repairs a
  // lost/mismatched device key without deleting the device or its attendance data.
  const handleRotateKey = async (deviceId: string) => {
    if (!confirm(`Rotate the API key for "${deviceId}"? The ESP32 will need the new key flashed before cloud sync resumes.`)) return;
    setBusy(true); setError(null); setGeneratedKey(null); setGeneratedDeviceId(null);
    try {
      const apiKey = generateApiKey();
      const apiKeyHash = await sha256Hex(apiKey);
      const { error: updateErr } = await supabase
        .from("devices")
        .update({ api_key_hash: apiKeyHash, updated_at: new Date().toISOString() })
        .eq("device_id", deviceId);
      if (updateErr) throw new Error(updateErr.message);
      const { error: auditErr } = await supabase.from("audit_log").insert({
        user_id: user?.id,
        action: "device_api_key_rotated",
        target: deviceId,
        details: { reason: "manual_rotation" },
      });
      if (auditErr) throw new Error(auditErr.message);
      setGeneratedDeviceId(deviceId);
      setGeneratedKey(apiKey);
      await load();
    } catch (err) {
      setError(err instanceof Error ? err.message : "Failed to rotate device API key");
    } finally {
      setBusy(false);
    }
  };

  const handleDeleteDevice = async (deviceId: string) => {
    if (!confirm(`Remove device "${deviceId}"? This will not delete synced attendance data.`)) return;
    try {
      const { error: delErr } = await supabase.from("devices").delete().eq("device_id", deviceId);
      if (delErr) throw new Error(delErr.message);
      await supabase.from("audit_log").insert({ user_id: user?.id, action: "device_removed", target: deviceId });
      await load();
    } catch (err) { setError(err instanceof Error ? err.message : "Failed to remove device"); }
  };

  const copyKey = () => { if (generatedKey) { navigator.clipboard.writeText(generatedKey); setCopied(true); setTimeout(() => setCopied(false), 2000); } };

  if (loading) return <LoadingSpinner label="Loading settings..." />;

  return (
    <div className="space-y-6">
      {error && <Card className="p-4 border-rose-200 bg-rose-50"><p className="text-sm text-rose-700">{error}</p></Card>}
      {generatedKey && (
        <Card className="p-5 border-teal-200 bg-teal-50">
          <div className="flex items-start gap-3">
            <Shield className="w-5 h-5 text-teal-600 flex-shrink-0 mt-0.5" />
            <div className="flex-1 min-w-0">
              <h3 className="text-sm font-semibold text-teal-800">Device API Key Generated</h3>
              {generatedDeviceId && <p className="text-xs text-teal-800 mt-1">Device ID: <code className="font-mono">{generatedDeviceId}</code></p>}
              <p className="text-xs text-teal-700 mt-1 mb-3">Copy this key now. It cannot be recovered later — only its hash is stored. Put it in your firmware's <code className="font-mono bg-teal-100 px-1 rounded">secrets_local.h</code> as <code className="font-mono bg-teal-100 px-1 rounded">CLOUD_API_KEY</code>.</p>
              <div className="flex items-center gap-2"><code className="flex-1 min-w-0 px-3 py-2 bg-white rounded-lg text-xs font-mono text-slate-700 overflow-x-auto border border-teal-200">{generatedKey}</code><button onClick={copyKey} className="flex items-center gap-1 px-3 py-2 text-sm font-medium rounded-lg bg-teal-600 text-white hover:bg-teal-700 transition-colors flex-shrink-0">{copied ? <Check className="w-4 h-4" /> : <Copy className="w-4 h-4" />}{copied ? "Copied" : "Copy"}</button></div>
              <button onClick={() => { setGeneratedKey(null); setGeneratedDeviceId(null); }} className="mt-3 text-xs text-teal-700 hover:text-teal-900 font-medium">Dismiss</button>
            </div>
          </div>
        </Card>
      )}
      <Card className="p-5">
        <div className="flex items-center justify-between mb-4"><div className="flex items-center gap-2"><Key className="w-5 h-5 text-slate-400" /><h3 className="text-sm font-semibold text-slate-700">Device Credentials</h3></div><button onClick={() => setShowAddDevice(!showAddDevice)} className="flex items-center gap-2 px-3 py-2 text-sm font-medium rounded-lg bg-teal-500 text-white hover:bg-teal-600 transition-colors"><Plus className="w-4 h-4" />Add Device</button></div>
        {showAddDevice && (
          <form onSubmit={handleAddDevice} className="mb-4 p-4 bg-slate-50 rounded-lg space-y-3">
            <div><label className="block text-xs font-medium text-slate-500 mb-1">Device Name</label><input type="text" value={newDeviceName} onChange={(e) => setNewDeviceName(e.target.value)} placeholder="e.g. MBS BA MVP" className="w-full px-3 py-2 text-sm border border-slate-200 rounded-lg focus:outline-none focus:border-teal-500" /></div>
            <div><label className="block text-xs font-medium text-slate-500 mb-1">Device ID (optional — auto-generated from name)</label><input type="text" value={newDeviceId} onChange={(e) => setNewDeviceId(e.target.value)} placeholder="e.g. mbs-ba-mvp" className="w-full px-3 py-2 text-sm border border-slate-200 rounded-lg focus:outline-none focus:border-teal-500" /></div>
            <button type="submit" disabled={busy} className="px-4 py-2 text-sm font-medium rounded-lg bg-teal-500 text-white hover:bg-teal-600 disabled:opacity-50 transition-colors">{busy ? "Creating..." : "Generate API Key"}</button>
          </form>
        )}
        {devices.length === 0 ? <EmptyState icon={<Settings className="w-7 h-7" />} title="No devices registered" description="Add a device to generate its cloud API key. The ESP32 uses this key to authenticate when uploading attendance and heartbeats." /> : (
          <div className="space-y-2">{devices.map((d) => (
            <div key={d.id} className="flex items-center justify-between p-3 rounded-lg border border-slate-200 hover:bg-slate-50 transition-colors">
              <div className="min-w-0"><div className="flex items-center gap-2"><p className="font-medium text-slate-700 text-sm">{d.name}</p>{d.last_seen_at && <Badge className="bg-emerald-50 text-emerald-700 border-emerald-200">Active {timeAgo(d.last_seen_at)}</Badge>}</div><p className="text-xs text-slate-400 font-mono mt-0.5">{d.device_id}</p>{d.firmware_version && <p className="text-xs text-slate-400 mt-0.5">{d.firmware_version}</p>}</div>
              <div className="flex items-center gap-1 flex-shrink-0">
                <button onClick={() => handleRotateKey(d.device_id)} disabled={busy} title="Rotate device API key" aria-label={`Rotate API key for ${d.device_id}`} className="p-2 text-slate-400 hover:text-teal-600 hover:bg-teal-50 rounded-lg transition-colors disabled:opacity-50"><RotateCw className="w-4 h-4" /></button>
                <button onClick={() => handleDeleteDevice(d.device_id)} disabled={busy} title="Remove device" aria-label={`Remove device ${d.device_id}`} className="p-2 text-slate-400 hover:text-rose-500 hover:bg-rose-50 rounded-lg transition-colors disabled:opacity-50"><Trash2 className="w-4 h-4" /></button>
              </div>
            </div>
          ))}</div>
        )}
      </Card>
      <Card className="overflow-hidden">
        <div className="px-5 py-3 border-b border-slate-200 flex items-center gap-2"><FileClock className="w-4 h-4 text-slate-400" /><h3 className="text-sm font-semibold text-slate-700">Audit Log</h3></div>
        {auditLog.length === 0 ? <EmptyState icon={<FileClock className="w-7 h-7" />} title="No audit entries yet" description="Administrative actions like adding devices and sending commands will be logged here." /> : (
          <div className="divide-y divide-slate-100 max-h-96 overflow-y-auto">{auditLog.map((entry) => (
            <div key={entry.id} className="px-5 py-3 flex items-center justify-between"><div className="min-w-0"><p className="text-sm text-slate-700 font-medium">{entry.action}</p>{entry.target && <p className="text-xs text-slate-400">{entry.target}</p>}</div><p className="text-xs text-slate-400 whitespace-nowrap ml-3">{timeAgo(entry.created_at)}</p></div>
          ))}</div>
        )}
      </Card>
    </div>
  );
}

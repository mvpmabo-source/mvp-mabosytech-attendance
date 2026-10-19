import { useEffect, useState } from "react";
import { Cpu, Wifi, HardDrive, Clock, Fingerprint, RotateCw, CheckCircle2, XCircle, AlertTriangle, RefreshCw, Upload, Rocket, ShieldCheck, FileCheck2 } from "lucide-react";
import { supabase, type DeviceHeartbeat, type DeviceCommand, type Device, type FirmwareRelease } from "@/lib/supabase";
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
  const [releases, setReleases] = useState<FirmwareRelease[]>([]);
  const [firmwareFile, setFirmwareFile] = useState<File | null>(null);
  const [firmwareVersion, setFirmwareVersion] = useState("");
  const [releaseNotes, setReleaseNotes] = useState("");
  const [uploadingFirmware, setUploadingFirmware] = useState(false);
  const [deployingVersion, setDeployingVersion] = useState<string | null>(null);
  const [firmwareMessage, setFirmwareMessage] = useState<string | null>(null);

  async function load() {
    try {
      const [hbRes, devRes, cmdRes, releaseRes] = await Promise.all([
        supabase.from("device_heartbeats").select("*").order("received_at", { ascending: false }).limit(1).maybeSingle(),
        supabase.from("devices").select("*").order("created_at", { ascending: false }).limit(1).maybeSingle(),
        supabase.from("device_commands").select("*").order("created_at", { ascending: false }).limit(20),
        supabase.from("firmware_releases").select("*").order("created_at", { ascending: false }).limit(8),
      ]);
      if (hbRes.error) throw new Error(hbRes.error.message);
      if (devRes.error) throw new Error(devRes.error.message);
      setHeartbeat((hbRes.data as DeviceHeartbeat) ?? null);
      setDevice((devRes.data as Device) ?? null);
      setCommands((cmdRes.data as DeviceCommand[]) ?? []);
      if (releaseRes.error) {
        setFirmwareMessage(`Cloud OTA is not configured yet: ${releaseRes.error.message}`);
      } else {
        setReleases((releaseRes.data as FirmwareRelease[]) ?? []);
      }
    } catch (err) { setError(err instanceof Error ? err.message : "Failed to load device data"); } finally { setLoading(false); }
  }

  useEffect(() => { load(); const interval = setInterval(load, 15000); return () => clearInterval(interval); }, []);

  const sendCommand = async (command: "restart" | "sync_time") => {
    setSendingCmd(true);
    try {
      // The ESP32 sync_time handler requires a Unix epoch in UTC seconds.
      // Date.now() returns milliseconds since Unix epoch; timezone conversion
      // belongs to the device's configured Africa/Kigali timezone.
      const payload = command === "sync_time"
        ? { epoch: Math.floor(Date.now() / 1000) }
        : {};
      const { error: cmdErr } = await supabase.from("device_commands").insert({
        device_id: device?.device_id ?? heartbeat?.device_id ?? "default",
        command,
        payload,
        status: "pending",
        created_by: user?.id,
      });
      if (cmdErr) throw new Error(cmdErr.message);
      await load();
    } catch (err) { setError(err instanceof Error ? err.message : "Failed to send command"); } finally { setSendingCmd(false); }
  };

  async function uploadFirmwareRelease() {
    if (!firmwareFile) {
      setError("Choose an ESP32 .bin firmware file first.");
      return;
    }
    const version = firmwareVersion.trim();
    if (!/^V?[0-9]+\\.[0-9]+\\.[0-9]+(?:[-+][A-Za-z0-9.-]+)?$/.test(version)) {
      setError("Enter the firmware version exactly as compiled, for example V2.9.46.");
      return;
    }
    if (!firmwareFile.name.toLowerCase().endsWith(".bin")) {
      setError("Only compiled .bin firmware files are accepted.");
      return;
    }
    if (firmwareFile.size <= 0 || firmwareFile.size > 2 * 1024 * 1024) {
      setError("Firmware file must be between 1 byte and 2 MiB; the device will also check its OTA partition size.");
      return;
    }

    setUploadingFirmware(true);
    setError(null);
    setFirmwareMessage(null);
    try {
      const digestBuffer = await crypto.subtle.digest("SHA-256", await firmwareFile.arrayBuffer());
      const sha256 = Array.from(new Uint8Array(digestBuffer))
        .map((value) => value.toString(16).padStart(2, "0")).join("");
      const safeFileName = firmwareFile.name.replace(/[^A-Za-z0-9._-]/g, "_");
      const storagePath = `${version}/${crypto.randomUUID()}-${safeFileName}`;

      const { error: uploadError } = await supabase.storage.from("firmware-releases").upload(storagePath, firmwareFile, {
        contentType: "application/octet-stream",
        upsert: false,
      });
      if (uploadError) throw new Error(`Firmware upload failed: ${uploadError.message}`);

      const { error: metadataError } = await supabase.from("firmware_releases").insert({
        version,
        storage_path: storagePath,
        sha256,
        size_bytes: firmwareFile.size,
        release_notes: releaseNotes.trim() || null,
        uploaded_by: user?.id ?? null,
      });
      if (metadataError) throw new Error(`Firmware uploaded, but release metadata could not be saved: ${metadataError.message}`);

      setFirmwareMessage(`Firmware ${version} uploaded. SHA-256: ${sha256}. Uploading alone does not flash the device.`);
      setFirmwareFile(null);
      setFirmwareVersion("");
      setReleaseNotes("");
      await load();
    } catch (err) {
      setError(err instanceof Error ? err.message : "Firmware upload failed.");
    } finally {
      setUploadingFirmware(false);
    }
  }

  async function deployFirmwareRelease(release: FirmwareRelease) {
    if (!device?.device_id && !heartbeat?.device_id) {
      setError("No registered device ID is available.");
      return;
    }
    const confirmed = window.confirm(
      `Send firmware ${release.version} to the ESP32? Attendance scanning pauses during download and verification, then the device restarts.`
    );
    if (!confirmed) return;

    setDeployingVersion(release.version);
    setError(null);
    setFirmwareMessage(null);
    try {
      const { error: commandError } = await supabase.from("device_commands").insert({
        device_id: device?.device_id ?? heartbeat?.device_id,
        command: "cloud_ota",
        payload: {
          version: release.version,
          storage_path: release.storage_path,
          sha256: release.sha256,
          size_bytes: release.size_bytes,
        },
        status: "pending",
        created_by: user?.id ?? null,
      });
      if (commandError) throw new Error(commandError.message);
      setFirmwareMessage(`OTA command for ${release.version} queued. The ESP32 will receive a short-lived signed HTTPS download link at its next cloud heartbeat. Check Serial Monitor/OLED and command history for progress.`);
      await load();
    } catch (err) {
      setError(err instanceof Error ? err.message : "Could not queue the cloud OTA command.");
    } finally {
      setDeployingVersion(null);
    }
  }

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
      <Card className="p-6">
        <div className="flex flex-wrap items-start justify-between gap-3 mb-4">
          <div>
            <h3 className="text-base font-semibold text-slate-800 flex items-center gap-2"><Upload className="w-5 h-5 text-slate-500" /> Cloud Firmware OTA</h3>
            <p className="text-sm text-slate-500 mt-1">Upload a compiled ESP32 .bin, verify its SHA-256, then queue a cloud update.</p>
          </div>
          <span className="inline-flex items-center gap-1 rounded-full border border-sky-200 bg-sky-50 px-2.5 py-1 text-xs font-medium text-sky-700"><ShieldCheck className="w-3.5 h-3.5" /> HTTPS + SHA-256</span>
        </div>
        {firmwareMessage && <div className="rounded-lg border border-sky-200 bg-sky-50 p-3 text-sm text-sky-800 mb-4 break-words">{firmwareMessage}</div>}
        <div className="grid grid-cols-1 md:grid-cols-2 gap-4">
          <label className="block text-sm font-medium text-slate-700">Firmware version
            <input value={firmwareVersion} onChange={(event) => setFirmwareVersion(event.target.value)} placeholder="V2.9.46" className="mt-1 block w-full rounded-lg border border-slate-200 px-3 py-2 text-sm outline-none focus:border-sky-400 focus:ring-2 focus:ring-sky-100" />
            <span className="mt-1 block text-xs font-normal text-slate-400">Must match FIRMWARE_VERSION compiled into the uploaded binary.</span>
          </label>
          <label className="block text-sm font-medium text-slate-700">Firmware .bin file
            <input type="file" accept=".bin,application/octet-stream" onChange={(event) => setFirmwareFile(event.target.files?.[0] ?? null)} className="mt-1 block w-full rounded-lg border border-slate-200 px-3 py-2 text-sm file:mr-3 file:rounded file:border-0 file:bg-slate-100 file:px-3 file:py-1.5 file:text-xs" />
            <span className="mt-1 block text-xs font-normal text-slate-400">{firmwareFile ? `${firmwareFile.name} · ${(firmwareFile.size / 1024).toFixed(0)} KB` : "Maximum 2 MiB. Build with your private secrets_local.h."}</span>
          </label>
        </div>
        <label className="block text-sm font-medium text-slate-700 mt-3">Release notes (optional)
          <textarea value={releaseNotes} onChange={(event) => setReleaseNotes(event.target.value)} rows={2} maxLength={500} placeholder="What changed in this firmware?" className="mt-1 block w-full rounded-lg border border-slate-200 px-3 py-2 text-sm outline-none focus:border-sky-400 focus:ring-2 focus:ring-sky-100" />
        </label>
        <div className="flex flex-wrap items-center gap-3 mt-4">
          <button onClick={uploadFirmwareRelease} disabled={uploadingFirmware || !firmwareFile || !firmwareVersion.trim()} className="inline-flex items-center gap-2 rounded-lg bg-slate-800 px-4 py-2 text-sm font-medium text-white hover:bg-slate-700 disabled:opacity-50">
            <Upload className="w-4 h-4" />{uploadingFirmware ? "Uploading…" : "Upload release"}
          </button>
          <p className="text-xs text-slate-500">Uploading stores a private release; it does not flash the device.</p>
        </div>
      </Card>

      <Card className="overflow-hidden">
        <div className="px-5 py-3 border-b border-slate-200 flex items-center justify-between gap-3">
          <div><h3 className="text-sm font-semibold text-slate-800">Available Firmware Releases</h3><p className="text-xs text-slate-500 mt-1">Select a release to queue it for the ESP32.</p></div>
          <FileCheck2 className="w-5 h-5 text-slate-400" />
        </div>
        {releases.length === 0 ? (
          <div className="p-5 text-sm text-slate-500">No firmware releases uploaded yet, or the cloud OTA migration still needs to be applied.</div>
        ) : (
          <div className="divide-y divide-slate-100">
            {releases.map((release) => (
              <div key={release.id} className="p-4 flex flex-col md:flex-row md:items-center justify-between gap-3">
                <div className="min-w-0">
                  <p className="font-medium text-slate-800">{release.version} <span className="ml-2 text-xs text-slate-500 font-normal">{(release.size_bytes / 1024).toFixed(0)} KB · {timeAgo(release.created_at)}</span></p>
                  {release.release_notes && <p className="text-sm text-slate-600 mt-1">{release.release_notes}</p>}
                  <p className="text-xs font-mono text-slate-400 mt-1 break-all">SHA-256: {release.sha256}</p>
                </div>
                <button onClick={() => deployFirmwareRelease(release)} disabled={deployingVersion !== null || release.version === heartbeat?.firmware_version} className="shrink-0 inline-flex items-center justify-center gap-2 rounded-lg border border-sky-200 px-3 py-2 text-sm font-medium text-sky-700 hover:bg-sky-50 disabled:opacity-50">
                  <Rocket className="w-4 h-4" />{deployingVersion === release.version ? "Queueing…" : release.version === heartbeat?.firmware_version ? "Already installed" : "Send to ESP32"}
                </button>
              </div>
            ))}
          </div>
        )}
      </Card>

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

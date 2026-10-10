let state={},
    logs=[],
    registeredFingerprints=[],
    lastSeenKey="",
    firstLoad=true,
    soundEnabled=false,
    currentPage="dashboard",
    reportRowsCache=[];

let lastKnownLogCount=-1,
    lastKnownTemplates=-1,
    lastTickCursor=-1,
    lastSdGen=-1;

let pollFailStreak=0;

/* ROOT CAUSE of "Request timed out" / "Failed to fetch" during History,
   Report, Reason Report and Delete Attendance:

   The SD-backed request stream is serialized so one large operation cannot
   handler function is running, the synchronous HTTP loop could not accept or answer
   ANY other connection, even a tiny /api/tick. History/Report/Export and
   Delete Attendance legitimately wait on sdTask for real SD I/O and can
   take several seconds. During that whole window the fast dashboard poll
   was still firing every 800ms-2s in the background, and every one of
   those attempts genuinely could not be served -> "Request timed out" or
   "Failed to fetch", followed by the attendance events appearing all at
   once the moment the heavy request finally finished.

   This is a real hardware/library limitation, not a bug that a bigger
   timeout can hide. The correct fix is for the browser to simply not
   attempt the fast poll while it already knows the ESP32 is busy with a
   heavy request THAT THIS SAME BROWSER STARTED. That is what this flag
   does. It does not make the heavy request faster - it stops the
   dashboard from generating errors for an outcome that was never a
   fault. */
let heavyRequestActive=false;
let refreshInFlight=false,
    systemInFlight=false,
    pollStarted=false;

/* History date state.
 *   Every selected date is authoritative SD data. Today's selection is
 *   refreshed after a new attendance event so the view stays current.
 */
let historyFetchedLogs = null;
let historyFetchedDate = "";

/* Reason page state */
let lessonTeachers = [];
let lessonSelectedId = -1;
let lessonReportRowsCache = [];


/* ================= PAGE NAVIGATION ================= */

function showPage(name,btn){

  document.querySelectorAll(".page")
    .forEach(p=>p.classList.remove("active"));

  document.getElementById("page-"+name)
    .classList.add("active");

  document.querySelectorAll(".nav button")
    .forEach(b=>b.classList.remove("active"));

  if(btn)btn.classList.add("active");

  currentPage=name;

  if(name==="dashboard"){

    pageTitle.textContent="Attendance Dashboard";

    pageSubtitle.textContent=
      "Automatic fingerprint CHECK IN, CHECK OUT, registration, and records.";

  }else if(name==="register"){

    pageTitle.textContent="Register Fingerprint";

    pageSubtitle.textContent=
      "Register an Administrator or Staff member and enroll their fingerprint.";

  }else if(name==="history"){

    pageTitle.textContent="Attendance History";

    pageSubtitle.textContent=
      "View automatic CHECK IN and CHECK OUT attendance records.";

    const hd=document.getElementById("historyDate");
    loadHistoryForDate((hd&&hd.value)||todayString()||"");

  }else if(name==="records"){

    pageTitle.textContent="Administrator / Staff Records";

    pageSubtitle.textContent=
      "Registered people and their assigned fingerprint IDs.";

  }else if(name==="reports"){

    pageTitle.textContent="Reports";

    pageSubtitle.textContent=
      "Build, preview, export, and print professional attendance reports.";

  }else if(name==="lesson"){

    pageTitle.textContent="Reason / Impamvu Management";

    pageSubtitle.textContent=
      "Record reasons (impamvu) explaining attendance non-compliance and generate reason reports.";

    /* Initialise dates and load teachers whenever the page opens */
    const today=todayString();
    const d=document.getElementById("lessonDate");
    const h=document.getElementById("lessonHistoryDate");
    const f=document.getElementById("lessonReportFrom");
    const t=document.getElementById("lessonReportTo");
    if(d && !d.value) d.value=today||"";
    if(h && !h.value) h.value=today||"";
    if(f && !f.value) f.value=today||"";
    if(t && !t.value) t.value=today||"";

    /* Seed the time input from the current RTC time */
    const lt=document.getElementById("lessonTime");
    if(lt && !lt.value){
      const nowStr = state && state.currentTime
        ? String(state.currentTime).slice(11,19) : "";
      if(nowStr) lt.value = nowStr;
    }

    loadTeachers(true);
    if(h && h.value) loadLessonsForDate(h.value);

  }else if(name==="setup"){

    pageTitle.textContent="Setup";

    pageSubtitle.textContent=
      "Configure schedule, device name, timezone, and fingerprint sensitivity.";

  }else if(name==="system"){

    pageTitle.textContent="System";

    pageSubtitle.textContent=
      "Live hardware diagnostics, storage, logs, restart, and factory reset.";

    loadSystemInfo();
    loadSdManager();
    const sl=document.getElementById("systemLogDate");
    if(sl && !sl.value) sl.value=todayString()||"";
    loadSystemLogsForDate(sl&&sl.value?sl.value:(todayString()||""));

  }

}


/* ================= API ================= */

/* Per-endpoint timeout.

   NOTE: this is NOT "hide the error by making the timeout bigger".
   Fast RAM-only endpoints (/api/tick, /api/status, /api/registered)
   keep the short 7 s budget precisely SO THAT a real stall still shows
   up as an error. Only the endpoints that are documented as
   asynchronous SD reads get a longer budget, because on those the
   firmware itself may legitimately spend up to 12 s walking
   /attendance.csv or /lessons.csv on sdTask. Giving them 7 s would
   abort a request that was working correctly. */
function apiBudgetFor(path){
  if(/^\/api\/(history|report|lesson-report|lessons|export|system-logs)\b/.test(path)) return 30000;
  if(/^\/api\/logs\?.*limit=0/.test(path))                                 return 30000;
  return 7000;
}

async function api(path,options={},timeoutMs=null){

  if(timeoutMs===null||timeoutMs===undefined)
    timeoutMs=apiBudgetFor(path);

  const controller=new AbortController();

  const timer=setTimeout(()=>controller.abort(),timeoutMs);

  try{

    const res=await fetch(path,{...options,signal:controller.signal});

    if(res.status===401){
      location.href="/";
      throw new Error("Session expired");
    }

    if(!res.ok){

      const txt=await res.text();

      let message=txt||("HTTP "+res.status);

      try{
        const parsed=JSON.parse(txt);
        if(parsed&&parsed.message)message=parsed.message;
      }catch(e){}

      throw new Error(message);

    }

    return await res.json();

  }catch(e){

    if(e.name==="AbortError")
      throw new Error("Request timed out");

    throw e;

  }finally{

    clearTimeout(timer);

  }

}

async function apiWithRetry(path,options={},attempts=3){

  let lastError=null;
  for(let attempt=0;attempt<attempts;attempt++){
    try{
      return await api(path,options);
    }catch(e){
      lastError=e;
      const msg=String(e&&e.message||e);
      if(attempt>=attempts-1 ||
         !/Unexpected end of JSON input|SD task busy|Failed to fetch|Request timed out|HTTP 503/i.test(msg)) break;
      await new Promise(r=>setTimeout(r,[250,750][attempt]||1000));
    }
  }
  throw lastError||new Error("Request failed");
}


/* ================= SOUND ================= */

function toggleSound(){

  soundEnabled=!soundEnabled;

  const btn=document.getElementById("soundToggleBtn");

  if(soundEnabled){

    if(btn)
      btn.textContent="🔊 Disable Sound";

    beep("info");

    showBanner(
      "info",
      "Sound Enabled",
      "Dashboard sounds are now active."
    );

  }else{

    if(btn)
      btn.textContent="🔇 Enable Sound";

    showBanner(
      "info",
      "Sound Disabled",
      "Dashboard sounds are now off."
    );

  }

}

function beep(type="info"){

  if(!soundEnabled)return;

  try{

    const C=window.AudioContext||window.webkitAudioContext;

    const ctx=new C();

    const osc=ctx.createOscillator();

    const gain=ctx.createGain();

    osc.connect(gain);

    gain.connect(ctx.destination);

    osc.frequency.value=
      type==="success"
      ?880
      :type==="error"
      ?180
      :520;

    gain.gain.setValueAtTime(
      .0001,
      ctx.currentTime
    );

    gain.gain.exponentialRampToValueAtTime(
      .15,
      ctx.currentTime+.02
    );

    gain.gain.exponentialRampToValueAtTime(
      .0001,
      ctx.currentTime+.18
    );

    osc.start();

    osc.stop(ctx.currentTime+.2);

  }catch(e){}

}


/* ================= TOAST / BANNER / MODAL ================= */

function showBanner(type,title,text){

  bannerTitle.textContent=title;

  bannerText.textContent=text||"";

  bannerIcon.textContent=
    type==="success"
    ?"✓"
    :type==="error"
    ?"!"
    :"i";

  banner.className=
    "banner show "+
    (
      type==="success"
      ?"success"
      :type==="error"
      ?"error"
      :""
    );

  clearTimeout(showBanner._t);

  showBanner._t=setTimeout(
    ()=>banner.className="banner",
    4500
  );

}

function showModal(type,title,message,data={}){

  liveCard.className=
    "live-card "+
    (
      type==="error"
      ?"error"
      :type==="info"
      ?"info"
      :""
    );

  modalIcon.textContent=
    type==="error"
    ?"!"
    :type==="info"
    ?"i"
    :"✓";

  modalTitle.textContent=title;

  modalMessage.textContent=message||"";

  modalName.textContent=data.name||"-";

  modalId.textContent=
    data.id>=0
    ?data.id
    :"-";

  modalCollege.textContent=
    data.collegeId||"-";

  modalDept.textContent=
    data.department||"-";

  createConfetti(type);

  liveModal.classList.add("show");

  setTimeout(
    ()=>closeModal(),
    6000
  );

}

function closeModal(){

  liveModal.classList.remove("show");

}

function createConfetti(type){

  liveCard
    .querySelectorAll(".confetti")
    .forEach(e=>e.remove());

  if(type==="error")return;

  const colors=[
    "#16A34A",
    "#F97316",
    "#EAB308",
    "#166534",
    "#FFEDD5",
    "#C2410C"
  ];

  for(let i=0;i<24;i++){

    const c=document.createElement("div");

    c.className="confetti";

    c.style.left=
      (15+Math.random()*80)+"%";

    c.style.top=
      (-10-Math.random()*40)+"px";

    c.style.background=
      colors[i%colors.length];

    c.style.animationDelay=
      (Math.random()*0.35)+"s";

    liveCard.appendChild(c);

  }

}


/* ================= EVENT TYPE ================= */

function eventType(event){

  event=(event||"").toUpperCase();

  if(
    event.includes("ENTRY") ||
    event.includes("MATCH") ||
    event.includes("CHECK IN") ||
    event.includes("CHECKIN")
  )
    return"entry";

  if(
    event.includes("EXIT") ||
    event.includes("CHECK OUT") ||
    event.includes("CHECKOUT")
  )
    return"exit";

  if(
    event.includes("ENROLLED") ||
    event.includes("SAVED")
  )
    return"enrolled";

  if(
    event.includes("ERROR") ||
    event.includes("NOT") ||
    event.includes("DUPLICATE")
  )
    return"error";

  return"info";

}

function displayEvent(event){

  const type=eventType(event);
  const raw=event||"";

  if(raw==="WEEKEND" || raw==="Muri weekend ntabwo attendance Ikorwa:")
    return"Muri weekend ntabwo attendance Ikorwa:";

  if(type==="entry")
    return attendanceStatusFromEvent(raw) || "CHECK IN";

  if(type==="exit")
    return attendanceStatusFromEvent(raw) || "CHECK OUT";

  if(type==="enrolled")
    return"ENROLLED";

  return raw||"INFO";

}

function eventBadge(event){

  const t=eventType(event);

  if(t==="entry")
    return"b-entry";

  if(t==="exit")
    return"b-exit";

  if(t==="enrolled")
    return"b-enrolled";

  if(t==="error")
    return"b-error";

  return"b-info";

}

function eventKey(l){

  if(!l)return"";

  return[
    l.time,
    l.event,
    l.id,
    l.message
  ].join("|");

}

function personName(l){

  return l.name ||
    (
      l.id>=0
      ?("ID "+l.id)
      :"System"
    );

}

function displayLogMessage(l){

  if(!l) return "";

  /* Attendance status is a user-facing value. Keep the persisted
     English event/message fields intact for compatibility, but never
     expose those raw status phrases in the dashboard/reports. */
  return attendanceStatusFromEvent(l.event) || l.message || "";

}


/* ================= NEW EVENT ================= */

function handleNewEvent(l){

  if(!l)return;

  const type=eventType(l.event);

  const name=personName(l);

  const msg=l.message||"";

  lastEventPill.textContent=
    displayEvent(l.event);

  lastPersonPill.textContent=name;

  lastTimePill.textContent=
    l.time||"--:--:--";


  if(type==="enrolled"){

    setRegisterStep(3);

    if(typeof registerStatusText!=="undefined")
      registerStatusText.textContent=
        "Saved successfully";

    heroTitle.textContent=
      "Registration Successful";

    heroMessage.textContent=
      name+" has been enrolled successfully.";

    showBanner(
      "success",
      "Registration Successful",
      name+" has been enrolled successfully."
    );

    showModal(
      "success",
      "Registration Successful",
      name+" has been enrolled successfully.",
      l
    );

    beep("success");


  }else if(type==="entry"){

    const attStatus=attendanceStatusFromEvent(l.event);
    heroTitle.textContent=
      (attStatus?attStatus+": ":"CHECK IN: ")+name;

    heroMessage.textContent=
      attStatus ||
      msg ||
      "Kwinjira byabitswe neza.";

    showBanner(
      "success",
      attStatus || "CHECK IN Successful",
      name+" yinjiye."+(attStatus?" "+attStatus+".":"")
    );

    showModal(
      "success",
      attStatus || "CHECK IN",
      (attStatus?name+" — "+attStatus: (msg||name+" yinjiye neza.")),
      l
    );

    beep("success");


  }else if(type==="exit"){

    const attStatus=attendanceStatusFromEvent(l.event);
    heroTitle.textContent=
      (attStatus?attStatus+": ":"CHECK OUT: ")+name;

    heroMessage.textContent=
      attStatus ||
      msg ||
      "Gusohoka byabitswe neza.";

    showBanner(
      "info",
      attStatus || "CHECK OUT Successful",
      name+" yasohotse."+(attStatus?" "+attStatus+".":"")
    );

    showModal(
      "info",
      attStatus || "CHECK OUT",
      (attStatus?name+" — "+attStatus: (msg||name+" yasohotse neza.")),
      l
    );

    beep("info");


  }else if(
    (l.event||"") === "WEEKEND" ||
    (l.event||"") === "Muri weekend ntabwo attendance Ikorwa:"
  ){

    heroTitle.textContent="Muri weekend ntabwo attendance Ikorwa:";
    heroMessage.textContent="Muri weekend ntabwo attendance Ikorwa:";

    showBanner(
      "info",
      "Muri weekend ntabwo attendance Ikorwa:",
      "Muri weekend ntabwo attendance Ikorwa:"
    );

  }else if(type==="error"){

    if(typeof registerStatusText!=="undefined")
      registerStatusText.textContent=
        "Needs attention";

    heroTitle.textContent=
      "Attention Required";

    heroMessage.textContent=
      msg||
      l.event||
      "Something went wrong.";

    showBanner(
      "error",
      l.event||"Error",
      msg||"Please try again."
    );

    showModal(
      "error",
      l.event||"Error",
      msg||"Please try again.",
      l
    );

    beep("error");


  }else if(
    (l.event||"")
      .toUpperCase()
      .includes("ENROLL")
  ){

    setRegisterStep(2);

    if(typeof registerStatusText!=="undefined")
      registerStatusText.textContent=
        "Scanning finger";

    heroTitle.textContent=
      "Enrollment In Progress";

    heroMessage.textContent=
      "Place finger on the sensor and follow the OLED instructions.";

    showBanner(
      "info",
      "Enrollment Started",
      "Place finger on the JM-101/JM-101B sensor."
    );

    beep("info");


  }else{

    heroTitle.textContent=
      l.event||"System Update";

    heroMessage.textContent=
      msg||
      "Dashboard received a new event.";

    showBanner(
      "info",
      l.event||"System Update",
      msg||"New dashboard event."
    );

  }

}


/* ================= HTML ESCAPE ================= */

function escapeHtml(v){

  return String(v??"")
    .replace(
      /[&<>"']/g,
      ch=>({
        "&":"&amp;",
        "<":"&lt;",
        ">":"&gt;",
        "\"":"&quot;",
        "'":"&#39;"
      }[ch])
    );

}


/* ================= FORMAT HELPERS ================= */

function formatBytes(bytes){

  bytes=Number(bytes);

  if(!isFinite(bytes)||bytes<0)
    return"--";

  if(bytes<1024)
    return bytes+" B";

  if(bytes<1024*1024)
    return(bytes/1024).toFixed(1)+" KB";

  if(bytes<1024*1024*1024)
    return(bytes/1024/1024).toFixed(2)+" MB";

  return(bytes/1024/1024/1024).toFixed(2)+" GB";

}


/* ================= DATE HELPERS ================= */

function todayString(){

  const s=
    (state&&state.currentTime)
    ?String(state.currentTime)
    :"";

  if(s.length>=10 && /^\d{4}-\d{2}-\d{2}/.test(s))
    return s.slice(0,10);

  try{
    return new Date().toISOString().slice(0,10);
  }catch(e){
    return "";
  }

}


/* ================= RECORDS ================= */

function buildPeopleRecords(){

  const people={};

  registeredFingerprints.forEach(fp=>{

    const id=Number(fp.id);

    if(id<0)return;

    people[id]={

      id:id,

      name:fp.name||"",

      collegeId:fp.collegeId||"",

      role:fp.role||"",

      department:fp.department||"",

      lastSeen:"",

      status:
        fp.name||fp.collegeId
        ?"Registered"
        :"Unassigned"

    };

  });


  logs.forEach(l=>{

    const id=Number(l.id);

    if(id<0)return;

    if(!people[id])return;

    if(l.name)
      people[id].name=l.name;

    if(l.collegeId)
      people[id].collegeId=l.collegeId;

    if(l.role)
      people[id].role=l.role;

    if(l.department)
      people[id].department=l.department;


    const ev=(l.event||"").toUpperCase();


    if(
      ev.includes("ENROLLED") ||
      ev.includes("SAVED")
    ){

      people[id].status=
        "Registered";

      people[id].lastSeen=
        l.time||
        people[id].lastSeen;

    }


    if(
      ev.includes("ENTRY") ||
      ev.includes("MATCH") ||
      ev.includes("CHECK IN") ||
      ev.includes("CHECKIN")
    ){

      people[id].status=
        "Kwinjiye";

      people[id].lastSeen=
        l.time||
        people[id].lastSeen;

    }


    if(
      ev.includes("EXIT") ||
      ev.includes("CHECK OUT") ||
      ev.includes("CHECKOUT")
    ){

      people[id].status=
        "Wavuye";

      people[id].lastSeen=
        l.time||
        people[id].lastSeen;

    }

  });


  return Object.values(people)
    .sort((a,b)=>a.id-b.id);

}


/* ================= ATTENDANCE HISTORY ================= */

function attendanceStatusFromEvent(event){
  const ev=(event||"").toUpperCase();

  /* The exact on-time label depends on whether this is check-in or
     check-out: 07:55 => Uziye k' ugihe, 17:00 => Utahiye k' ugihe. */
  if(ev.includes("CHECK OUT")){
    if(ev.includes("LEFT EARLY"))
      return "Utashe kare";
    if(ev.includes("OVERTIME"))
      return "Utashe nyuma y' igihe";
    if(ev.includes("ON TIME"))
      return "Utahiye k' ugihe";
    return "Wavuye";
  }

  if(ev.includes("CHECK IN")){
    if(ev.includes("EARLY"))
      return "Wazindutse";
    if(ev.includes("ON TIME"))
      return "Uziye k' ugihe";
    if(ev.includes("LATE"))
      return "Wakerewe";
    return "Kwinjiye";
  }

  if(ev.includes("TIME UNKNOWN"))
    return "Igihe ntikizwi";

  return "";
}

function attendanceStatusBadge(status){
  const s=(status||"").toLowerCase();

  if(s==="wazindutse" || s==="uziye k' ugihe" || s==="kwinjiye")
    return "b-entry";

  if(s==="wakerewe" || s==="utashe kare" ||
     s==="utashe nyuma y' igihe" || s==="wavuye")
    return "b-info";

  return "b-exit";
}

function buildAttendanceHistory(source){

  const src =
    Array.isArray(source)
      ? source
      : logs;

  const openEntries={};
  const rows=[];

  const attendanceLogs=
    src
      .filter(l=>{
        const t=eventType(l.event);
        return t==="entry"||t==="exit";
      })
      .slice()
      .sort((a,b)=>String(a.time||"").localeCompare(String(b.time||"")));

  attendanceLogs.forEach(l=>{

    const id=Number(l.id);

    if(id<0)return;

    const t=eventType(l.event);

    const base={

      id:id,

      name:
        l.name||
        ("ID "+id),

      collegeId:
        l.collegeId||
        "-",

      role:
        l.role||
        "-",

      department:
        l.department||
        "-",

      inTime:"",

      outTime:"",

      status:""

    };


    if(t==="entry"){

      if(openEntries[id])
        rows.push(openEntries[id]);

      openEntries[id]={
        ...base,
        inTime:l.time||"",
        outTime:"",
        status:
          attendanceStatusFromEvent(l.event)||
          "Kwinjiye"
      };


    }else{

      if(openEntries[id]){

        openEntries[id].outTime=
          l.time||"";

        openEntries[id].status=
          attendanceStatusFromEvent(l.event)||
          "Wavuye";

        rows.push(
          openEntries[id]
        );

        delete openEntries[id];

      }else{

        rows.push({
          ...base,
          inTime:"-",
          outTime:l.time||"",
          status:
            attendanceStatusFromEvent(l.event)||
            "Wavuye"
        });

      }

    }

  });


  Object.keys(openEntries)
    .forEach(id=>{
      rows.push(openEntries[id]);
    });


  return rows.reverse();

}


/* ================= HISTORY DATE LOADER ================= */

function setHistoryStrip(msg){
  const el=document.getElementById("historyStrip");
  if(el) el.innerHTML=msg;
}

function setHistoryDateInput(dateStr){
  const el=document.getElementById("historyDate");
  if(el) el.value=dateStr||"";
}

let historyRequestToken=0;
let historyLoadInFlight=false;
let historyPendingDate="";

async function loadHistoryForDate(dateStr){

  const requested=(dateStr||todayString()||"").trim();
  if(!requested) return;

  // Keep only the newest requested date. This prevents two SD history
  // streams from fighting for the firmware's single SD reader.
  historyPendingDate=requested;
  if(historyLoadInFlight) return;
  historyLoadInFlight=true;

  try{

    while(historyPendingDate){

      const target=historyPendingDate;
      historyPendingDate="";

      // Wait for another heavy SD operation already started by this page.
      while(heavyRequestActive)
        await new Promise(r=>setTimeout(r,150));

      heavyRequestActive=true;
      historyRequestToken++;
      const myToken=historyRequestToken;

      try{

        setHistoryDateInput(target);
        historyFetchedLogs=[];
        historyFetchedDate=target;
        renderTables();
        setHistoryStrip("Loading <b>"+escapeHtml(target)+"</b> from SD card...");

        let data=null;
        let lastError=null;

        // A transient truncated/busy SD response is retried internally rather
        // than surfacing a noisy notification to the user.
        for(let attempt=0;attempt<3;attempt++){

          try{
            data=await api("/api/history?date="+encodeURIComponent(target));
            lastError=null;
            break;
          }catch(e){

            lastError=e;
            const msg=String(e&&e.message||e);

            if(attempt<2 &&
              /Unexpected end of JSON input|SD task busy|Failed to fetch|Request timed out/i.test(msg)){

              await new Promise(r=>setTimeout(r,[300,700][attempt]));
              continue;

            }

            break;
          }

        }

        if(lastError) throw lastError;

        if(myToken!==historyRequestToken) continue;

        if(data.date && data.date!==target){
          setHistoryStrip("Ignored a stale response for <b>"+escapeHtml(data.date)+"</b>.");
          continue;
        }

        historyFetchedLogs=Array.isArray(data.logs)?data.logs:[];
        historyFetchedDate=data.date||target;
        renderTables();

        setHistoryStrip(
          "Showing <b>"+escapeHtml(historyFetchedDate)+"</b> from SD card: "+
          historyFetchedLogs.length+" record(s)."
        );

        if(!historyFetchedLogs.length)
          showBanner("info","No Records","No attendance records were found for "+historyFetchedDate+".");

      }catch(e){

        // If a newer date is already queued, do not flash an error for the
        // older request; the newer request is authoritative.
        if(!historyPendingDate && myToken===historyRequestToken){
          historyFetchedLogs=[];
          historyFetchedDate=target;
          setHistoryStrip("Could not load <b>"+escapeHtml(target)+"</b>: "+escapeHtml(e.message));
          showBanner("error","History Load Failed",e.message);
          renderTables();
        }

      }finally{

        heavyRequestActive=false;

      }

    }

  }finally{

    historyLoadInFlight=false;

    // If another date arrived during the final synchronous turn, let it run
    // on the next task rather than recursively growing the call stack.
    if(historyPendingDate)
      setTimeout(()=>loadHistoryForDate(historyPendingDate),0);

  }

}

function resetHistoryDate(){
  loadHistoryForDate(todayString()||"");
}

function reloadHistoryDate(){

  const el=document.getElementById("historyDate");

  const v=el?el.value:"";

  loadHistoryForDate(v);

}


/* ================= TABLES ================= */

function renderTables(){

  const liveRows=
    logs
      .slice()
      .reverse()
      .map(l=>`

      <tr>

        <td>
          ${escapeHtml(l.time||"")}
        </td>

        <td>
          <span class="badge ${eventBadge(l.event)}">
            ${escapeHtml(displayEvent(l.event))}
          </span>
        </td>

        <td>
          ${l.id>=0?l.id:"-"}
        </td>

        <td>
          ${escapeHtml(l.name||"-")}
        </td>

        <td>
          ${escapeHtml(l.collegeId||"-")}
        </td>

        <td>
          ${escapeHtml(attendanceStatusFromEvent(l.event)||l.message||"")}
        </td>

      </tr>

      `)
      .join("");


  liveBody.innerHTML=
    liveRows||
    `<tr>
      <td colspan="6">
        No logs received yet.
      </td>
    </tr>`;


  const historySource=
    historyFetchedLogs || logs;

  const historyRows=
    buildAttendanceHistory(historySource)
      .map(r=>`

      <tr>

        <td>${escapeHtml(r.name)}</td>

        <td>${escapeHtml(r.collegeId)}</td>

        <td>${escapeHtml(r.role)}</td>

        <td>${escapeHtml(r.department)}</td>

        <td>${r.id}</td>

        <td>${escapeHtml(r.inTime||"-")}</td>

        <td>${escapeHtml(r.outTime||"-")}</td>

        <td>

          <span class="badge ${attendanceStatusBadge(r.status)}">

            ${escapeHtml(r.status)}

          </span>

        </td>

      </tr>

      `)
      .join("");


  if(typeof historyBody!=="undefined"){

    historyBody.innerHTML=
      historyRows||
      `<tr>
        <td colspan="8">
          ${
            historyFetchedLogs
            ? "No CHECK IN / CHECK OUT records for this date."
            : "No CHECK IN / CHECK OUT attendance history yet."
          }
        </td>
      </tr>`;

  }


  const peopleRows=
    buildPeopleRecords()
      .map(p=>`

      <tr>

        <td>${p.id}</td>

        <td>${escapeHtml(p.name||"-")}</td>

        <td>${escapeHtml(p.collegeId||"-")}</td>

        <td>${escapeHtml(p.role||"-")}</td>

        <td>${escapeHtml(p.department||"-")}</td>

        <td>
          <button
            class="btn-mini-red"
            onclick="deleteFingerprint(${p.id})">
            Delete
          </button>
        </td>

      </tr>

      `)
      .join("");


  recordsBody.innerHTML=
    peopleRows||
    `<tr>
      <td colspan="6">
        No registered fingerprints found in the sensor yet.
      </td>
    </tr>`;


  if(typeof registeredCount!=="undefined")
    registeredCount.textContent=
      registeredFingerprints.length;

}


/* ================= REPORTS ================= */

function resetReportFilters(){

  reportStart.value="";
  reportEnd.value="";
  reportTeacher.value="";
  reportCollege.value="";
  reportRole.value="";
  reportDept.value="";
  reportEvent.value="ALL";

  const sd=document.getElementById("reportSdDate");
  if(sd) sd.value="";

  reportRowsCache=[];

  reportBody.innerHTML=
    `<tr><td colspan="8">No report generated yet.</td></tr>`;

  reportTotalCount.textContent="0";
  reportEntryCount.textContent="0";
  reportExitCount.textContent="0";
  reportPeopleCount.textContent="0";

  printTotal.textContent="0";
  printEntries.textContent="0";
  printExits.textContent="0";
  printPeople.textContent="0";

}

function reportFilteredRows(){

  const start=reportStart.value;
  const end=reportEnd.value;

  const teacherQ=(reportTeacher.value||"").toLowerCase().trim();
  const collegeQ=(reportCollege.value||"").toLowerCase().trim();
  const role=reportRole.value;
  const deptQ=(reportDept.value||"").toLowerCase().trim();
  const evFilter=reportEvent.value;

  return logs.filter(l=>{

    const t=eventType(l.event);

    if(t!=="entry"&&t!=="exit")return false;

    if(evFilter==="ENTRY"&&t!=="entry")return false;
    if(evFilter==="EXIT"&&t!=="exit")return false;

    const dateStr=(l.time||"").slice(0,10);

    if(start&&dateStr&&dateStr<start)return false;
    if(end&&dateStr&&dateStr>end)return false;

    if(teacherQ&&!(l.name||"").toLowerCase().includes(teacherQ))return false;
    if(collegeQ&&!(l.collegeId||"").toLowerCase().includes(collegeQ))return false;
    if(role&&l.role!==role)return false;
    if(deptQ&&!(l.department||"").toLowerCase().includes(deptQ))return false;

    return true;

  }).slice().reverse();

}

function generateReport(){

  const rows=reportFilteredRows();

  reportRowsCache=rows;

  const entries=rows.filter(r=>eventType(r.event)==="entry").length;
  const exits=rows.filter(r=>eventType(r.event)==="exit").length;
  const uniquePeople=new Set(rows.map(r=>r.id)).size;

  reportTotalCount.textContent=rows.length;
  reportEntryCount.textContent=entries;
  reportExitCount.textContent=exits;
  reportPeopleCount.textContent=uniquePeople;

  const rowsHtml=rows.map(r=>`

    <tr>
      <td>${escapeHtml(r.time||"")}</td>
      <td>
        <span class="badge ${eventBadge(r.event)}">
          ${escapeHtml(displayEvent(r.event))}
        </span>
      </td>
      <td>${escapeHtml(r.name||"-")}</td>
      <td>${escapeHtml(r.collegeId||"-")}</td>
      <td>${escapeHtml(r.role||"-")}</td>
      <td>${escapeHtml(r.department||"-")}</td>
      <td>${r.id>=0?r.id:"-"}</td>
      <td>${escapeHtml(displayLogMessage(r))}</td>
    </tr>

  `).join("");

  reportBody.innerHTML=
    rowsHtml||
    `<tr><td colspan="8">No records match these filters.</td></tr>`;

  const deviceName=(state.deviceName||"MABOSYTECH");

  printDeviceName.textContent=
    deviceName+" - Attendance Report";

  const rangeText=
    (reportStart.value||"Any")+" to "+
    (reportEnd.value||"Any");

  printRangeText.textContent="Date range: "+rangeText;

  printGeneratedText.textContent=
    "Generated "+new Date().toLocaleString();

  printTotal.textContent=rows.length;
  printEntries.textContent=entries;
  printExits.textContent=exits;
  printPeople.textContent=uniquePeople;

  if(!rows.length){

    showBanner(
      "info",
      "No Matching Records",
      "Try widening the date range or clearing a filter."
    );

  }else{

    showBanner(
      "success",
      "Report Ready",
      rows.length+" record(s) found."
    );

  }

}

async function loadReportFromSd(){

  if(reportSdLoadInFlight) return;
  reportSdLoadInFlight=true;

  const el=document.getElementById("reportSdDate");
  const dateStr=el?el.value:"";

  if(!dateStr){

    showBanner(
      "error",
      "Date Required",
      "Pick a date before loading from the SD card."
    );

    reportSdLoadInFlight=false;
    return;

  }

heavyRequestActive=true;
  try{

    showBanner(
      "info",
      "Loading from SD",
      "Fetching "+dateStr+" records from attendance.csv..."
    );

    const data=
      await apiWithRetry(
        "/api/report?date="+
        encodeURIComponent(dateStr)
      );

    const incoming=
      Array.isArray(data.logs)
        ?data.logs
        :[];

    logs=mergeLogs(logs, incoming);

    reportStart.value=data.date||dateStr;
    reportEnd.value=data.date||dateStr;

    generateReport();

    showBanner(
      "success",
      "Loaded from SD",
      incoming.length+" record(s) for "+(data.date||dateStr)+"."
    );

  }catch(e){

    showBanner(
      "error",
      "Load Failed",
      e.message
    );

  }finally{

    heavyRequestActive=false;
    reportSdLoadInFlight=false;

  }

}

function clearReportSdDate(){

  const el=document.getElementById("reportSdDate");
  if(el) el.value="";

}

function csvEscape(value){

  value=String(value??"");

  value=value.replace(/"/g,'""');

  return'"'+value+'"';

}

function exportReportCsv(){

  const rows=
    reportRowsCache.length
    ?reportRowsCache
    :reportFilteredRows();

  if(!rows.length){

    showBanner(
      "error",
      "No Report Data",
      "Preview a report with at least one matching record before exporting."
    );

    return;

  }

  let csv=
    "\uFEFF"+
    "time,event,name,college_id,role,department,fingerprint_id,message\r\n";

  rows.forEach(r=>{

    csv += [

      csvEscape(r.time||""),

      csvEscape(displayEvent(r.event)),

      csvEscape(r.name||""),

      csvEscape(r.collegeId||""),

      csvEscape(r.role||""),

      csvEscape(r.department||""),

      csvEscape(r.id>=0?r.id:""),

      csvEscape(displayLogMessage(r))

    ].join(",")+"\r\n";

  });

  const blob=
    new Blob(
      [csv],
      {type:"text/csv;charset=utf-8;"}
    );

  const url=
    URL.createObjectURL(blob);

  const a=
    document.createElement("a");

  a.href=url;

  a.download="mabosytech_report.csv";

  document.body.appendChild(a);

  a.click();

  document.body.removeChild(a);

  URL.revokeObjectURL(url);

  showBanner(
    "success",
    "Export Complete",
    "Report exported as an Excel-compatible CSV file."
  );

}

function printReport(){

  if(!reportRowsCache.length){
    generateReport();
  }

  setTimeout(()=>window.print(),150);

}


/* ================= CSV (LEGACY ATTENDANCE EXPORT) ================= */

function exportAttendanceCsv(){

  const rows=
    buildAttendanceHistory(
      historyFetchedLogs||logs
    );


  if(!rows.length){

    showBanner(
      "error",
      "No Attendance Data",
      "There is no CHECK IN / CHECK OUT attendance history to export yet."
    );

    return;

  }


  let csv=
    "\uFEFF"+
    "name,college_id,role,department,fingerprint_id,check_in_time,check_out_time,status\r\n";


  rows.slice().reverse()
    .forEach(r=>{

      csv += [

        csvEscape(r.name),

        csvEscape(r.collegeId),

        csvEscape(r.role),

        csvEscape(r.department),

        csvEscape(r.id),

        csvEscape(r.inTime||"-"),

        csvEscape(r.outTime||"-"),

        csvEscape(r.status)

      ].join(",")+"\r\n";

    });


  const blob=
    new Blob(
      [csv],
      {type:"text/csv;charset=utf-8;"}
    );


  const url=
    URL.createObjectURL(blob);


  const a=
    document.createElement("a");

  a.href=url;

  a.download=
    "mabosytech_attendance.csv";

  document.body.appendChild(a);

  a.click();

  document.body.removeChild(a);

  URL.revokeObjectURL(url);


  showBanner(
    "success",
    "Export Complete",
    "MABOSYTECH CHECK IN / CHECK OUT history was exported."
  );

}


/* ================= STATUS ================= */

function updateStatusUi(){

  const wifi=!!state.wifi;

  wifiText.textContent=
    wifi
    ?"Connected"
    :"Offline";

  wifiDot.className=
    "dot "+
    (wifi?"ok":"");

  ipText.textContent=
    state.ip||"--";

  if(typeof deviceTimeText!=="undefined")
    deviceTimeText.textContent=
      state.currentTime||"--";

  if(typeof sensorText!=="undefined")
    sensorText.textContent=
      (state.sensor||"--")+
      (state.templates!=null
        ?(" ("+state.templates+"/"+(state.capacity??200)+")")
        :"");

  if(typeof sdText!=="undefined")
    sdText.textContent=
      state.sdAvailable
      ?"SD mounted"
      :"RAM only";

  apiTime.textContent=
    new Date().toLocaleTimeString();

  totalEnrolled.textContent=
    state.templates??"--";

  nextId.textContent=
    state.databaseFull
    ?"FULL"
    :(state.nextId??"--");

  entryCount.textContent=
    state.entryCount??
    logs.filter(
      l=>eventType(l.event)==="entry"
    ).length;

  exitCount.textContent=
    state.exitCount??
    logs.filter(
      l=>eventType(l.event)==="exit"
    ).length;


  const fwEl=document.getElementById("firmwareVersionText");
  if(fwEl) fwEl.textContent=state.firmwareVersion||"--";

  if(state.deviceName){
    brandName.textContent=state.deviceName;
    deviceLabel.textContent=state.deviceName;
    document.title=state.deviceName+" - Attendance Dashboard";
  }


  const pending   = state.pendingCount ?? 0;
  const persisted = state.sdRecords ?? 0;
  const lessons   = state.sdLessons ?? 0;
  const dropped   = (state.pendingDropped ?? 0) + (state.ramDropped ?? 0);

  if(typeof pendingText!=="undefined")
    pendingText.textContent=
      pending + (state.sdAvailable ? "" : " (SD offline)");

  if(typeof recordsText!=="undefined")
    recordsText.textContent=
      persisted + " stored" +
      (lessons > 0 ? " + " + lessons + " reasons" : "") +
      (pending > 0 ? " + " + pending + " pending" : "");

  debugStrip.innerHTML=
    "Persisted: <b>"+persisted+"</b>"+
    " | Reasons: <b>"+lessons+"</b>"+
    " | Pending: <b>"+pending+"</b>"+
    (dropped>0
      ? " | Dropped: <b style='color:#DC2626'>"+dropped+"</b>"
      : "")+
    " | SD: <b>"+(state.sdAvailable?"Mounted":"Unavailable")+"</b>"+
    " | Last: <b>"+
    (state.last&&state.last.event
      ?displayEvent(state.last.event)
      :"None")+
    "</b>";


  updateRegisterPreview();


  if(state.isEnrolling){

    setRegisterStep(2);

    if(typeof registerStatusText!=="undefined")
      registerStatusText.textContent=
        "Scanning finger";

  }

}


/* ================= REGISTER ================= */

function getInitials(name){

  const parts=
    String(name||"New User")
      .trim()
      .split(/\s+/)
      .filter(Boolean);

  return parts
    .slice(0,2)
    .map(p=>p[0].toUpperCase())
    .join("")
    ||"NU";

}


function setRegisterStep(step){

  const info=
    document.getElementById("regStepInfo");

  const scan=
    document.getElementById("regStepScan");

  const save=
    document.getElementById("regStepSave");


  if(!info||!scan||!save)return;


  [info,scan,save].forEach(el=>{

    el.classList.remove("active");

    el.classList.remove("done");

  });


  if(step===1){

    info.classList.add("active");

  }else if(step===2){

    info.classList.add("done");

    scan.classList.add("active");

  }else if(step===3){

    info.classList.add("done");

    scan.classList.add("done");

    save.classList.add("active");

  }

}


function updateRegisterPreview(){

  /*
   * Kept intentionally empty.
   * Existing dashboard flow remains unchanged.
   */

}


function clearRegisterForm(){

  enrollName.value="";

  enrollCollegeId.value="";

  enrollRole.value="Staff";

  enrollDept.value="";

  enrollNote.value="";


  const warn=
    document.getElementById("registerWarning");

  if(warn)
    warn.classList.remove("show");


  setRegisterStep(1);


  if(typeof registerStatusText!=="undefined")
    registerStatusText.textContent="Ready";


  updateRegisterPreview();

}


function validateRegisterForm(){

  const hasName=
    (enrollName?.value||"")
      .trim()
      .length>0;

  const hasId=
    (enrollCollegeId?.value||"")
      .trim()
      .length>0;


  const role=
    enrollRole?.value||"";


  const warn=
    document.getElementById("registerWarning");


  if(!hasName&&!hasId){

    if(warn)
      warn.classList.add("show");

    showBanner(
      "error",
      "Missing Details",
      "Enter at least a name or college ID before registration."
    );

    return false;

  }


  if(
    role!=="Administrator" &&
    role!=="Staff"
  ){

    showBanner(
      "error",
      "Invalid Role",
      "Only Administrator and Staff can be registered."
    );

    return false;

  }


  if(warn)
    warn.classList.remove("show");


  return true;

}


/* ================= BOOT PAYLOAD ================= */

function applyBootPayload(d){

  if(!d)return false;

  state=d.status||{};
  logs=d.logs||[];
  registeredFingerprints=d.registered||[];

  lastKnownLogCount=state.logCount;
  lastKnownTemplates=state.templates;

  if(d.status && d.status.sdGen !== undefined){
    lastSdGen = d.status.sdGen;
  }

  updateStatusUi();
  renderTables();

  if(d.system)applySystemInfo(d.system,false);
  if(d.setup)applyDeviceSetup(d.setup,false);

  const latest=
    state.last||
    logs[logs.length-1]||
    null;

  firstLoad=false;
  lastSeenKey=eventKey(latest);

  if(latest){

    lastEventPill.textContent=
      displayEvent(latest.event);

    lastPersonPill.textContent=
      personName(latest);

    lastTimePill.textContent=
      latest.time||"--:--:--";

  }

  return true;

}

async function bootstrapDashboard(){

  try{

    window.__bootPromise=window.__bootPromise||(async()=>{const deadline=Date.now()+45000;for(;;){try{const r=await fetch("/api/bootstrap",{cache:"no-store"});const j=await r.json();if(j&&j.booting===false)return j;}catch(e){}if(Date.now()>=deadline)throw new Error("Dashboard boot timed out. SD initialization may still be in progress.");await new Promise(r=>setTimeout(r,300));}})();
    const d=await window.__bootPromise;
    if(!window.__bootApplied){window.__bootApplied=true;applyBootPayload(d);}

  }catch(e){

    debugStrip.innerHTML=
      "Dashboard API error: <b>"+
      escapeHtml(e.message)+
      "</b>";

    showBanner(
      "error",
      "Dashboard API Error",
      e.message
    );

  }

}


/* ================= LOG MERGE ================= */

function logKey(l){
  return [
    l.time || "",
    l.event || "",
    l.id != null ? l.id : "",
    l.message || ""
  ].join("|");
}

function mergeLogs(existing,incoming){

  const out = Array.isArray(existing) ? existing.slice() : [];

  if(!Array.isArray(incoming) || !incoming.length){
    return out;
  }

  const seen = new Set(out.map(logKey));

  incoming.forEach(l=>{
    if(!l) return;
    const k = logKey(l);
    if(!seen.has(k)){
      seen.add(k);
      out.push(l);
    }
  });

  out.sort((a,b)=>
    String(a.time||"").localeCompare(String(b.time||""))
  );

  if(out.length > 600){
    return out.slice(out.length - 600);
  }

  return out;
}


/* ================= REFRESH (DASHBOARD / REGISTER) ================= */

async function refreshNow(forcePopup=false){

  if(refreshInFlight)return;

  refreshInFlight=true;
  let pollOk=true;

  try{

    if(forcePopup){

      const newState=await api("/api/status");
      state=newState||{};

      const logData=await api("/api/logs?limit=400");
      logs=mergeLogs(logs, logData.logs||[]);
      lastKnownLogCount=state.logCount;
      lastTickCursor=state.logCount ?? -1;
      if(state.sdGen !== undefined) lastSdGen = state.sdGen;

      const templatesChanged=
        state.templates!==undefined && state.templates!==lastKnownTemplates;

      if(templatesChanged || !registeredFingerprints.length){
        try{
          const regData=await api("/api/registered");
          registeredFingerprints=regData.fingerprints||[];
          lastKnownTemplates=state.templates;
        }catch(e){}
      }

      updateStatusUi();
      renderTables();

      const latest=state.last||logs[logs.length-1]||null;
      const key=eventKey(latest);

      firstLoad=false;
      lastSeenKey=key;

      if(latest){
        lastEventPill.textContent=displayEvent(latest.event);
        lastPersonPill.textContent=personName(latest);
        lastTimePill.textContent=latest.time||"--:--:--";
        handleNewEvent(latest);
      }

      return;
    }

    const url=lastTickCursor<0
      ? "/api/tick?since=-1&limit=100"
      : "/api/tick?since="+lastTickCursor;

    const d=await api(url);
    state=d.status||state;

    let forceFullNext = false;

    if(d.sdGen !== undefined && d.sdGen !== lastSdGen){
      lastSdGen = d.sdGen;
      forceFullNext = true;
    }

    if(d.sdReadOk === false){
      forceFullNext = true;
    }

    let logsChanged=false;

    if(d.logsIncluded && Array.isArray(d.logs)){
      logs=mergeLogs(logs, d.logs);
      logsChanged=true;
    }

    if(forceFullNext){
      lastTickCursor = -1;
    } else if(logsChanged){
      lastTickCursor = state.logCount ?? lastTickCursor;
    }

    const templatesChanged=
      state.templates!==undefined && state.templates!==lastKnownTemplates;

    if(templatesChanged || !registeredFingerprints.length){
      try{
        const regData=await api("/api/registered");
        registeredFingerprints=regData.fingerprints||[];
        lastKnownTemplates=state.templates;
      }catch(e){}
    }

    updateStatusUi();
    if(logsChanged)renderTables();
    if(logsChanged && currentPage==="history" && !heavyRequestActive){
      const hd=document.getElementById("historyDate");
      const selected=hd?hd.value:"";
      const today=todayString();
      if(selected && today && selected===today) loadHistoryForDate(selected);
    }

    const latest=state.last||logs[logs.length-1]||null;
    const key=eventKey(latest);

    if(firstLoad){
      firstLoad=false;
      lastSeenKey=key;
      if(latest){
        lastEventPill.textContent=displayEvent(latest.event);
        lastPersonPill.textContent=personName(latest);
        lastTimePill.textContent=latest.time||"--:--:--";
      }
      return;
    }

    if(key && key!==lastSeenKey){
      lastSeenKey=key;
      handleNewEvent(latest);
    }

    if(state.isEnrolling){
      heroTitle.textContent="Enrollment In Progress";
      heroMessage.textContent=
        "Place finger on the sensor and follow the OLED instructions.";
      debugStrip.innerHTML=
        "Enrollment running. Waiting for fingerprint sensor...";
    }

  }catch(e){

    if(heavyRequestActive){
      /* A heavy request started in the instant between this poll firing
         and now. This is expected, not a fault - stay silent. */
      pollOk=true;
    }else{

    pollOk=false;
    pollFailStreak++;

    debugStrip.innerHTML=
      "Dashboard API error: <b>"+
      escapeHtml(e.message)+
      "</b>";

    /* Show the real error, but do not spam a banner on every retry of
       the same outage - and never treat a network failure as a logout.
       Only a real HTTP 401 (handled inside api()) ends the session. */
    if(pollFailStreak===1)
      showBanner(
        "error",
        "Dashboard API Error",
        e.message
      );

    }

  }finally{

    /* finally also runs on the early `return`s in the success paths
       above, so a healthy poll always clears the backoff. */
    if(pollOk) pollFailStreak=0;
    refreshInFlight=false;

  }

}


/* ================= START ENROLLMENT ================= */

async function startEnroll(mode){

  if(!validateRegisterForm())
    return;


  let name=
    enrollName.value||"";

  let collegeId=
    enrollCollegeId.value||"";

  let role=
    enrollRole.value||"";

  let department=
    enrollDept.value||"";

  let note=
    enrollNote.value||"";


  const form=
    new URLSearchParams();

  form.append(
    "name",
    name
  );

  form.append(
    "collegeId",
    collegeId
  );

  form.append(
    "role",
    role
  );

  form.append(
    "department",
    department
  );

  form.append(
    "note",
    note
  );


  try{

    setRegisterStep(2);

    if(typeof registerStatusText!=="undefined")
      registerStatusText.textContent=
        "Scanning finger";


    heroTitle.textContent=
      "Enrollment In Progress";

    heroMessage.textContent=
      "Place finger on the JM-101/JM-101B sensor now.";


    showBanner(
      "info",
      "Enrollment Started",
      "Place finger on the sensor."
    );


    showModal(
      "info",
      "Enrollment Started",
      "Place finger on the JM-101/JM-101B sensor now.",
      {
        name:name||"New User",
        id:-1,
        collegeId:collegeId,
        department:department
      }
    );


    beep("info");


    await api(
      "/api/enroll",
      {
        method:"POST",
        headers:{
          "Content-Type":
            "application/x-www-form-urlencoded"
        },
        body:form
      }
    );

    let fastTicks=0;

    const fastPoll=setInterval(()=>{
      refreshNow(false);
      if(++fastTicks>=12)clearInterval(fastPoll);
    },400);


  }catch(e){

    setRegisterStep(1);

    if(typeof registerStatusText!=="undefined")
      registerStatusText.textContent=
        "Failed";


    showBanner(
      "error",
      "Enrollment Failed",
      e.message
    );


    showModal(
      "error",
      "Enrollment Failed",
      e.message,
      {
        name:name,
        id:-1,
        collegeId:collegeId,
        department:department
      }
    );


    beep("error");

  }

}


/* ================= CANCEL ================= */

async function cancelEnroll(){

  try{

    await api(
      "/api/cancel",
      {
        method:"POST"
      }
    );


    showBanner(
      "info",
      "Enrollment Cancelled",
      "System returned to fingerprint scan mode."
    );


    beep("info");

    refreshNow(true);


  }catch(e){

    showBanner(
      "error",
      "Cancel Failed",
      e.message
    );

  }

}


/* ================= DELETE ================= */

async function deleteFingerprint(id){

  if(!id){

    showBanner(
      "error",
      "Missing ID",
      "No fingerprint ID was selected."
    );

    return;

  }


  if(
    !confirm(
      "Delete fingerprint ID "+
      id+
      " and remove this Administrator/Staff record?"
    )
  ){

    return;

  }


  try{

    await api(
      "/api/fingerprint?id="+
      encodeURIComponent(id),
      {
        method:"DELETE"
      }
    );


    showBanner(
      "success",
      "Record Deleted",
      "Fingerprint ID "+
      id+
      " and assigned details were removed."
    );


    beep("success");

    refreshNow(true);


  }catch(e){

    showBanner(
      "error",
      "Delete Failed",
      e.message
    );

    beep("error");

  }

}


/* ================= SETUP PAGE ================= */

function applyDeviceSetup(data,announce){

  if(!data)return;

  setupCheckin.value=data.checkinTime||"";
  setupCheckout.value=data.checkoutTime||"";
  setupDeviceName.value=data.deviceName||"";
  setupTimezone.value=data.timezone||"";
  setupSecurity.value=String(data.securityLevel??2);

  if(data.deviceName){
    brandName.textContent=data.deviceName;
    deviceLabel.textContent=data.deviceName;
  }

  if(announce){

    showBanner(
      "success",
      "Setup Loaded",
      "Live values from the device: "+
      data.checkinTime+" / "+data.checkoutTime+
      ", TZ "+data.timezone+", security "+data.securityLevel+"."
    );

  }

}

async function loadDeviceSetup(){

  try{

    const data=await api("/api/device-setup");

    applyDeviceSetup(data,true);

  }catch(e){

    showBanner(
      "error",
      "Could Not Load Setup",
      e.message
    );

  }

}

async function saveDeviceSetup(){

  const form=new URLSearchParams();

  form.append("checkinTime",setupCheckin.value||"");
  form.append("checkoutTime",setupCheckout.value||"");
  form.append("deviceName",setupDeviceName.value||"");
  form.append("timezone",setupTimezone.value||"");
  form.append("securityLevel",setupSecurity.value||"2");

  try{

    await api("/api/device-setup",{
      method:"POST",
      headers:{
        "Content-Type":"application/x-www-form-urlencoded"
      },
      body:form
    });

    beep("success");

    const data=await api("/api/device-setup");
    applyDeviceSetup(data,false);

    showBanner(
      "success",
      "Setup Saved",
      "Saved to Preferences and applied immediately."
    );

    refreshNow(false);

  }catch(e){

    showBanner(
      "error",
      "Save Failed",
      e.message
    );

    beep("error");

  }

}


/* ================= SYSTEM PAGE ================= */

async function applySystemInfo(d,allowStorageFallback=true){

  if(!d)return;

  sysWifi.innerHTML=
    d.wifi
    ?'<span class="badge b-ok">Connected</span>'
    :'<span class="badge b-bad">Offline</span>';

  sysIp.textContent=d.ip||(d.wifi?"(no IP)":"Offline");

  sysFingerprint.innerHTML=
    d.fingerprintOk
    ?'<span class="badge b-ok">Ready</span>'
    :'<span class="badge b-bad">Not Found</span>';

  sysSd.innerHTML=
    d.sdAvailable
    ?'<span class="badge b-ok">Mounted</span>'
    :'<span class="badge b-error">Not Available (RAM logs only)</span>';

  sysRtc.innerHTML=
    d.rtcAvailable
    ?'<span class="badge b-ok">Detected</span>'
    :'<span class="badge b-error">Not Detected (NTP fallback)</span>';

  sysTemplates.textContent=
    (d.templates??0)+" / "+(d.capacity??200);

  sysNextId.textContent=
    d.databaseFull
    ?"FULL ("+(d.capacity??200)+"/"+(d.capacity??200)+")"
    :(d.nextId??"--");

  sysTime.textContent=d.currentTime||"(no time source)";

  if(d.deviceName){
    brandName.textContent=d.deviceName;
    deviceLabel.textContent=d.deviceName;
  }

  let usedBytes=d.sdUsedBytes,
      freeBytes=d.sdFreeBytes,
      totalBytes=d.sdTotalBytes??
        (usedBytes!=null&&freeBytes!=null?usedBytes+freeBytes:null),
      csvBytes=d.csvSizeBytes,
      recordCount=d.logRecords;

  if((usedBytes==null||!totalBytes)&&allowStorageFallback){

    try{

      const s=await api("/api/storage");

      if(s.mounted){

        sysStorageText.textContent=s.percent+"% used";

        sysStorageBar.style.width=s.percent+"%";

        sysStorageBar.className=
          "progress-fill "+
          (s.percent>90?"danger":s.percent>70?"warn":"ok");

        sysSdUsed.textContent=s.used||"--";
        sysSdFree.textContent=s.free||"--";

        sysCsvSize.textContent=
          (s.csvSize||"--")+
          (s.logs!=null?"  ("+s.logs+" records)":"");

        return;

      }

    }catch(e){}

  }

  if(usedBytes!=null&&totalBytes){

    const pct=
      Math.max(0,Math.min(100,(usedBytes/totalBytes)*100));

    sysStorageBar.style.width=pct.toFixed(1)+"%";

    sysStorageBar.className=
      "progress-fill "+
      (pct>90?"danger":pct>70?"warn":"ok");

    sysStorageText.textContent=pct.toFixed(1)+"% used";

    sysSdUsed.textContent=formatBytes(usedBytes);

    sysSdFree.textContent=
      formatBytes(freeBytes??(totalBytes-usedBytes));

  }else{

    sysStorageBar.style.width="0%";
    sysStorageBar.className="progress-fill ok";
    sysStorageText.textContent=
      d.sdAvailable?"Reading...":"No SD card";
    sysSdUsed.textContent="--";
    sysSdFree.textContent="--";

  }

  sysCsvSize.textContent=
    (csvBytes!=null?formatBytes(csvBytes):"--")+
    (recordCount!=null?"  ("+recordCount+" records)":"");

}

async function loadSystemInfo(){

  if(systemInFlight)return;

  systemInFlight=true;

  try{

    const d=await api("/api/system");

    await applySystemInfo(d,true);

  }catch(e){

    showBanner(
      "error",
      "Could Not Load System Info",
      e.message
    );

  }finally{

    systemInFlight=false;

  }

}


/* ============================================================
 * SD CARD MANAGER (System page)
 * ============================================================ */

async function loadSdManager(){

  try{

    const d=await api("/api/sd/status");

    updateSdManagerUi(d);

  }catch(e){

    showBanner(
      "error",
      "SD Status Failed",
      e.message
    );

  }

}

function updateSdManagerUi(d){

  if(!d)return;

  const st=document.getElementById("sdMgrStatus");
  if(!st)return;

  if(d.mounted){
    st.textContent="Mounted";
    st.className="badge b-ok";
  }else{
    st.textContent="Not mounted";
    st.className="badge b-bad";
  }

  const mounted=!!d.mounted;
  const total=mounted ? Number(d.totalBytes)||0 : 0;
  const used =mounted ? Number(d.usedBytes)||0 : 0;
  const free =mounted ? Number(d.freeBytes)||0 : 0;
  const csv  =mounted ? Number(d.csvBytes)||0 : 0;

  const pct=total>0?(used/total)*100:0;

  const elTotal=document.getElementById("sdMgrTotal");
  const elUsed =document.getElementById("sdMgrUsed");
  const elFree =document.getElementById("sdMgrFree");
  const elCsv  =document.getElementById("sdMgrCsv");
  const elText =document.getElementById("sdMgrUsageText");
  const elBar  =document.getElementById("sdMgrBar");

  if(elTotal) elTotal.textContent=mounted?formatBytes(total):"--";
  if(elUsed)  elUsed.textContent =mounted?formatBytes(used):"--";
  if(elFree)  elFree.textContent =mounted?formatBytes(free):"--";
  if(elCsv)   elCsv.textContent  =mounted
                                  ? formatBytes(csv)+(d.recordCount!=null?"  ("+d.recordCount+" records)":"")
                                  : "--";
  if(elText)  elText.textContent =mounted?pct.toFixed(1)+"% used":"SD offline";

  if(elBar){
    elBar.style.width=mounted?pct.toFixed(1)+"%":"0%";
    elBar.className="progress-fill "+(mounted?(pct>90?"danger":pct>70?"warn":"ok"):"ok");
  }

}

let systemLogsInFlight=false;
let lessonLoadInFlight=false;
let lessonPendingDate="";
let reportSdLoadInFlight=false;

async function loadSystemLogsForDate(dateStr){
  const body=document.getElementById("systemLogsBody");
  if(!body || !dateStr || systemLogsInFlight) return;
  systemLogsInFlight=true;
  body.innerHTML='<tr><td colspan="4">Loading...</td></tr>';
  try{
    // Share the same heavy-request gate as History/Reasons/Reports so the
    // single SD HTTP stream slot is never contested by this page.
    while(heavyRequestActive)
      await new Promise(r=>setTimeout(r,100));
    heavyRequestActive=true;
    const data=await apiWithRetry("/api/system-logs?date="+encodeURIComponent(dateStr));
    const rows=Array.isArray(data.logs)?data.logs:[];
    if(!rows.length){
      body.innerHTML='<tr><td colspan="4">No system logs for '+escapeHtml(dateStr)+'.</td></tr>';
      return;
    }
    body.innerHTML=rows.map(r=>`<tr><td>${escapeHtml(r.time||"-")}</td><td><span class="badge b-info">${escapeHtml(r.level||"INFO")}</span></td><td>${escapeHtml(r.event||"-")}</td><td>${escapeHtml(displayLogMessage(r))}</td></tr>`).join("");
  }catch(e){
    body.innerHTML='<tr><td colspan="4">Could not load system logs: '+escapeHtml(e.message)+'</td></tr>';
  }finally{
    heavyRequestActive=false;
    systemLogsInFlight=false;
  }
}

function reloadSystemLogs(){
  const el=document.getElementById("systemLogDate");
  loadSystemLogsForDate(el&&el.value?(el.value):todayString()||"");
}


async function deleteSdAttendance(){

  if(!confirm(
    "Delete ALL attendance records from the SD card?\n\n"+
    "This will permanently erase /attendance.csv and clear the in-memory history.\n"+
    "Fingerprints, teacher profiles, WiFi settings, and Preferences are NOT affected.\n"+
    "Reason records in /lessons.csv are also preserved.\n\n"+
    "This cannot be undone. Continue?"
  )) return;

heavyRequestActive=true;
  try{

    const r=await api(
      "/api/sd/delete-attendance",
      {method:"POST"}
    );

    if(r && r.success){

      logs=[];
      historyFetchedLogs=[];
      historyFetchedDate=todayString()||"";
      reportRowsCache=[];

      renderTables();

      showBanner(
        "success",
        "Attendance Deleted",
        r.message||"All attendance data has been deleted."
      );

      loadSdManager();
      loadSystemInfo();

    }else{

      showBanner(
        "error",
        "Delete Failed",
        (r&&r.message)||"Unknown error"
      );

    }

  }catch(e){

    showBanner(
      "error",
      "Delete Failed",
      e.message
    );

  }finally{

    heavyRequestActive=false;

  }

}


async function restartDevice(){

  if(!confirm("Restart the device now? The dashboard will be briefly unreachable."))
    return;

  try{

    await api("/api/system/restart",{method:"POST"});

    showBanner(
      "info",
      "Restarting",
      "The device is restarting. This page will reconnect automatically."
    );

    setTimeout(()=>location.reload(),8000);

  }catch(e){

    showBanner(
      "error",
      "Restart Failed",
      e.message
    );

  }

}

async function factoryReset(){

  const confirmText=(factoryConfirmText.value||"").trim();
  const password=factoryConfirmPassword.value||"";

  if(confirmText!=="FACTORY RESET"){

    showBanner(
      "error",
      "Confirmation Required",
      'Type "FACTORY RESET" exactly in the confirmation box.'
    );

    return;

  }

  if(!password){

    showBanner(
      "error",
      "Password Required",
      "Enter your current dashboard password to confirm."
    );

    return;

  }

  if(
    !confirm(
      "This will permanently erase the account, setup, and all fingerprints. "+
      "attendance.csv on the SD card will be kept. This cannot be undone. Continue?"
    )
  ){
    return;
  }

  const form=new URLSearchParams();
  form.append("confirm",confirmText);
  form.append("password",password);

  try{

    await api("/api/system/factory-reset",{
      method:"POST",
      headers:{
        "Content-Type":"application/x-www-form-urlencoded"
      },
      body:form
    });

    showBanner(
      "info",
      "Factory Reset Complete",
      "The device is restarting. You will need to create a new account."
    );

    factoryConfirmText.value="";
    factoryConfirmPassword.value="";

    setTimeout(()=>location.reload(),8000);

  }catch(e){

    showBanner(
      "error",
      "Factory Reset Failed",
      e.message
    );

  }

}


/* ============================================================
 * REASON / IMPAMVU MANAGEMENT
 * ============================================================ */

async function loadTeachers(showMessage){

  try{

    const list = await api("/api/teachers");

    lessonTeachers = Array.isArray(list) ? list : [];

    renderTeacherList();

    if(showMessage){

      showBanner(
        "success",
        "Teachers Loaded",
        lessonTeachers.length + " teacher(s) available."
      );

    }

  }catch(e){

    showBanner("error","Could not load teachers",e.message);

  }

}

function renderTeacherList(){

  const box = document.getElementById("lessonTeacherList");

  if(!box) return;

  const q = (document.getElementById("lessonTeacherSearch").value||"")
              .toLowerCase().trim();

  const filtered = lessonTeachers.filter(t=>{

    if(!q) return true;

    return String(t.name||"").toLowerCase().includes(q)
        || String(t.collegeId||"").toLowerCase().includes(q)
        || String(t.department||"").toLowerCase().includes(q);

  });

  if(!filtered.length){

    box.innerHTML =
      '<div style="padding:12px;color:var(--muted);font-size:13px">No teachers found.</div>';

    return;

  }

  box.innerHTML = filtered.map(t=>{

    const act = (Number(t.id)===Number(lessonSelectedId))?"active":"";

    const label = escapeHtml(t.name||("ID "+t.id));

    const sub = escapeHtml(
      (t.collegeId||"") + (t.department?(" · "+t.department):"")
    );

    return '<button class="'+act+'" onclick="selectTeacher('+t.id+')">'
      +'<div style="display:flex;flex-direction:column;gap:2px">'
      +'<span>'+label+'</span>'
      +'<span style="font-size:11px;color:var(--muted);font-weight:600">'+sub+'</span>'
      +'</div></button>';

  }).join("");

}

function selectTeacher(id){

  const t = lessonTeachers.find(x=>Number(x.id)===Number(id));

  if(!t) return;

  lessonSelectedId = Number(id);

  renderTeacherList();

  document.getElementById("lessonFingerId").value = id;

  const info = document.getElementById("lessonTeacherInfo");

  info.style.display = "block";

  info.innerHTML =
    "<strong>"+escapeHtml(t.name||("ID "+t.id))+"</strong><br>"+
    "College ID: <b>"+escapeHtml(t.collegeId||"-")+"</b><br>"+
    "Role: <b>"+escapeHtml(t.role||"-")+"</b><br>"+
    "Department: <b>"+escapeHtml(t.department||"-")+"</b><br>"+
    "Fingerprint ID: <b>"+t.id+"</b>";

  /* Seed the time input from the current RTC time if empty */
  const lt = document.getElementById("lessonTime");
  if(lt && !lt.value){
    const nowStr = state && state.currentTime
      ? String(state.currentTime).slice(11,19) : "";
    if(nowStr) lt.value = nowStr;
  }

}

async function saveLesson(){

  if(lessonSelectedId < 1){
    showBanner("error","Select a Teacher","Please pick a registered teacher first.");
    return;
  }

  const date = (document.getElementById("lessonDate").value||"").trim();
  const time = (document.getElementById("lessonTime").value||"").trim();
  const type = document.getElementById("lessonType").value||"";
  const text = (document.getElementById("lessonText").value||"").trim();

  if(!date){ showBanner("error","Date Required","Pick a date."); return; }
  if(!time){ showBanner("error","Time Required","Pick a time."); return; }
  if(!text){ showBanner("error","Reason Required","Please write the reason / impamvu."); return; }
  if(text.length>250){ showBanner("error","Too Long","Max 250 characters."); return; }

  const form = new URLSearchParams();
  form.append("date", date);
  form.append("time", time);
  form.append("fingerprintId", String(lessonSelectedId));
  form.append("type", type);
  form.append("lesson", text);

  try{

    const r = await api("/api/lessons", {
      method:"POST",
      headers:{"Content-Type":"application/x-www-form-urlencoded"},
      body:form
    });

    if(r && r.ok){

      showBanner(
        "success",
        r.durable ? "Reason Secured" : "Reason Saved",
        r.message || "Reason accepted for durable save."
      );

      beep("success");

      /* Reload the date list after sdTask has had a chance to commit the
         journaled row. The NVS journal remains the safety copy if SD is
         offline; the next normal refresh will show it after remount. */
      const h = document.getElementById("lessonHistoryDate");
      if(h) h.value = date;
      setTimeout(()=>loadLessonsForDate(date), 800);

      /* Clear the form for the next entry (keeps teacher selected) */
      document.getElementById("lessonText").value = "";
      document.getElementById("lessonType").value = "Late Arrival";

    }else{

      showBanner(
        "error",
        "Save Failed",
        (r && r.error) || "Unknown error"
      );

      beep("error");

    }

  }catch(e){

    showBanner("error","Save Failed",e.message);

    beep("error");

  }

}

function clearLessonForm(){

  document.getElementById("lessonText").value = "";
  document.getElementById("lessonType").value = "Late Arrival";

  lessonSelectedId = -1;

  document.getElementById("lessonFingerId").value = "";

  document.getElementById("lessonTeacherInfo").style.display = "none";

  renderTeacherList();

}

async function loadLessonsForDate(dateStr){

  const requested=(dateStr||"").trim();
  if(!requested) return;

  const body = document.getElementById("lessonHistoryBody");
  if(!body) return;

  lessonPendingDate=requested;
  if(lessonLoadInFlight) return;
  lessonLoadInFlight=true;

  try{
    while(lessonPendingDate){
      const target=lessonPendingDate;
      lessonPendingDate="";

      while(heavyRequestActive)
        await new Promise(r=>setTimeout(r,150));

      heavyRequestActive=true;
      body.innerHTML = '<tr><td colspan="5">Loading...</td></tr>';

      try{
        const data = await apiWithRetry(
          "/api/lessons?date=" + encodeURIComponent(target)
        );

        const rows = Array.isArray(data.lessons) ? data.lessons : [];
        if(!rows.length){
          body.innerHTML =
            '<tr><td colspan="5">No reasons recorded for this date.</td></tr>';
        }else{
          body.innerHTML = rows.map(r=>`
            <tr>
              <td>${escapeHtml(r.time||"-")}</td>
              <td>${escapeHtml(r.name||"-")}</td>
              <td>${escapeHtml(r.collegeId||"-")}</td>
              <td><span class="badge b-info">${escapeHtml(r.type||"-")}</span></td>
              <td>${escapeHtml(r.lesson||"")}</td>
            </tr>`).join("");
        }
      }catch(e){
        if(lessonPendingDate){
          // A newer date selection superseded this response; do not display
          // an error for the stale request.
        }else{
          body.innerHTML =
            '<tr><td colspan="5">Could not load reasons: '+
            escapeHtml(e.message)+'</td></tr>';
        }
      }finally{
        heavyRequestActive=false;
      }
    }
  }finally{
    lessonLoadInFlight=false;
  }

}

async function generateLessonReport(){

  while(heavyRequestActive)
    await new Promise(r=>setTimeout(r,100));

  const from = document.getElementById("lessonReportFrom").value||"";
  const to   = document.getElementById("lessonReportTo").value||"";

  const body = document.getElementById("lessonReportBody");

  if(!body) return;

  body.innerHTML = '<tr><td colspan="8">Loading...</td></tr>';

heavyRequestActive=true;
  try{

    const url =
      "/api/lesson-report?from="+encodeURIComponent(from)+
      "&to="+encodeURIComponent(to);

    const data = await apiWithRetry(url);

    const rows = Array.isArray(data.lessons) ? data.lessons : [];

    lessonReportRowsCache = rows;

    if(!rows.length){
      body.innerHTML =
        '<tr><td colspan="8">No reason records in this date range.</td></tr>';
      return;
    }

    body.innerHTML = rows.map(r=>`
      <tr>
        <td>${escapeHtml(r.date||"")}</td>
        <td>${escapeHtml(r.time||"")}</td>
        <td>${escapeHtml(r.name||"")}</td>
        <td>${escapeHtml(r.collegeId||"")}</td>
        <td>${escapeHtml(r.role||"")}</td>
        <td>${escapeHtml(r.department||"")}</td>
        <td><span class="badge b-info">${escapeHtml(r.type||"")}</span></td>
        <td>${escapeHtml(r.lesson||"")}</td>
      </tr>`).join("");

    showBanner("success","Reason Report Ready",rows.length+" record(s).");

  }catch(e){

    body.innerHTML =
      '<tr><td colspan="8">Report failed: '+escapeHtml(e.message)+'</td></tr>';

  }finally{

    heavyRequestActive=false;

  }

}

function exportLessonReportCsv(){

  const rows = lessonReportRowsCache;

  if(!rows.length){
    showBanner("error","Nothing To Export","Generate a report first.");
    return;
  }

  let csv = "\uFEFF"+
    "date,time,name,college_id,role,department,type,reason\r\n";

  rows.forEach(r=>{

    const fields=[
      r.date||"",r.time||"",r.name||"",r.collegeId||"",
      r.role||"",r.department||"",r.type||"",r.lesson||""
    ];

    csv += fields
      .map(v=>'"'+String(v).replace(/"/g,'""')+'"')
      .join(",") + "\r\n";

  });

  const blob = new Blob([csv],{type:"text/csv;charset=utf-8;"});

  const url = URL.createObjectURL(blob);

  const a = document.createElement("a");
  a.href = url;
  a.download = "mabosytech_reason_report.csv";

  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);

  URL.revokeObjectURL(url);

  showBanner("success","Export Complete","Reason report exported.");

}


/* ================= LOGOUT ================= */

async function logoutDashboard(){

  if(!confirm("Logout from MABOSYTECH dashboard?"))
    return;

  try{

    await api(
      "/api/logout",
      {
        method:"POST"
      }
    );

    window.location.href="/logout";

  }catch(e){

    /* Even if the API call fails (e.g. network hiccup), still send
       the user to the branded logout page. Do NOT treat a network
       timeout as a session failure here. */
    window.location.href="/logout";

  }

}


/* ================= INIT / ADAPTIVE POLLING ================= */

setRegisterStep(1);

(function(){const b=window.__BOOT;const ok=!!(b&&b.status&&b.system&&b.setup);if(ok){window.__bootApplied=true;applyBootPayload(b);return;}window.addEventListener("bootReady",function h(e){window.removeEventListener("bootReady",h);if(!window.__bootApplied){window.__bootApplied=true;applyBootPayload(e.detail||window.__BOOT);}});bootstrapDashboard();})();

(function initialiseDateInputs(){

  const today=todayString();

  const h=document.getElementById("historyDate");
  if(h) h.value=today||"";

  const r=document.getElementById("reportSdDate");
  if(r) r.value=today||"";

  const ld=document.getElementById("lessonDate");
  if(ld) ld.value=today||"";

  const lh=document.getElementById("lessonHistoryDate");
  if(lh) lh.value=today||"";

  const lf=document.getElementById("lessonReportFrom");
  if(lf) lf.value=today||"";

  const lt=document.getElementById("lessonReportTo");
  if(lt) lt.value=today||"";

})();

/* Adaptive polling: keep the live screen responsive without flooding the AP. */
function pollTick(){

  if(heavyRequestActive){
    /* Heavy SD requests temporarily own the single SD stream. */
    setTimeout(pollTick,400);
    return;
  }

  if(currentPage==="dashboard"||currentPage==="register"){

    refreshNow(false);

  }else if(currentPage==="system"){

    loadSystemInfo();

  }else if(currentPage==="history"||currentPage==="records"||currentPage==="reports"){

    refreshNow(false);

  }else if(currentPage==="lesson"){

    /* On the Reason/Impamvu page we do not poll the attendance feed to
       avoid overloading the HTTP interface. The user can
       refresh the teacher list or reload a date on demand. */

  }

  /* Live pages poll at 1 Hz; background tabs and failures back off. */

  let delay =
    (currentPage==="dashboard"||currentPage==="register")
      ? 1000
      : (currentPage==="lesson" ? 5000 : 5000);

  if(document.hidden) delay=10000;

  if(pollFailStreak>0)
    delay=Math.min(delay*Math.pow(2,Math.min(pollFailStreak,4)),15000);

  setTimeout(pollTick,delay);

}

/* Come back immediately when the user returns to the tab. */
document.addEventListener("visibilitychange",()=>{
  if(!document.hidden){
    pollFailStreak=0;
    if(currentPage==="dashboard"||currentPage==="register")
      refreshNow(false);
  }
});

if(!pollStarted){
  pollStarted=true;
  setTimeout(pollTick,600);
}
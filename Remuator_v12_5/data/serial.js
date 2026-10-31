import { $, request, showError, clearError, status } from "./app.js";
async function refresh() {try {$("serialOutput").textContent=await request("/api/serial");clearError();}catch(e){showError(e);}}
$("refreshSerial").onclick=refresh;
refresh();

async function loadSettings() {
  const data=await request("/api/serial/settings");
  $("serialEnabled").checked=Boolean(data.serialOn);
  $("serialSettings").querySelector("fieldset").disabled=false;
}
$("serialSettings").onsubmit=async(event)=>{
  event.preventDefault();
  const fieldset=$("serialSettings").querySelector("fieldset");
  fieldset.disabled=true;
  try {
    clearError();await request("/api/serial/settings",{enabled:$("serialEnabled").checked?"1":"0"});
    await loadSettings();status("Настройка Serial сохранена");
  } catch(e) {showError(e);} finally {fieldset.disabled=false;}
};
loadSettings().catch(showError);

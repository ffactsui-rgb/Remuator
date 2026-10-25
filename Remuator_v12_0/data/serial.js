import { $, request, showError, clearError } from "./app.js";
async function refresh() {try {$("serialOutput").textContent=await request("/api/serial");clearError();}catch(e){showError(e);}}
$("refreshSerial").onclick=refresh;
refresh();

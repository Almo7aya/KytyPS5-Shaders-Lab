#pragma once
#include <string_view>
namespace sl {
inline constexpr std::string_view report_head = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src data:; base-uri 'none'; form-action 'none'">
<title>PS5 Shader Lab · Evidence report</title><style>
:root{color-scheme:dark;--bg:#0b1018;--panel:#131c28;--raised:#1a2635;--border:#2b3b4f;--ink:#edf3fc;--muted:#a4b6cb;--blue:#8fbaff;--cyan:#64dfd4;--amber:#f6c978;--red:#ff9d9d}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:14px/1.55 system-ui,-apple-system,Segoe UI,sans-serif}main{max-width:1640px;margin:auto;padding:32px}a{color:var(--blue)}button,input,select{font:inherit}button,select,input{background:var(--raised);color:var(--ink);border:1px solid var(--border);border-radius:8px;padding:9px 12px}button{cursor:pointer}button:hover{border-color:var(--blue);background:#24354a}button:disabled{opacity:.45;cursor:default}:focus-visible{outline:2px solid var(--cyan);outline-offset:3px}h1{font-size:32px;letter-spacing:-1px;margin:6px 0}h2{font-size:19px;margin:0 0 12px}h3{font-size:15px;margin:0 0 8px}p{margin:6px 0 12px}small,.muted{color:var(--muted)}code,pre,.mono{font-family:ui-monospace,Consolas,monospace}code{overflow-wrap:anywhere;font-size:12px}pre{white-space:pre-wrap;overflow-wrap:anywhere;background:#0c131e;border:1px solid var(--border);border-radius:8px;padding:14px;font-size:12px;max-height:350px;overflow:auto;margin:10px 0}header{display:flex;align-items:center;justify-content:space-between;gap:20px}.eyebrow{font-size:11px;font-weight:700;letter-spacing:2px;color:var(--cyan)}.pill,.badge{display:inline-block;border:1px solid var(--border);border-radius:6px;padding:3px 8px;font-size:11px;font-weight:650}.pill{border-radius:99px;color:var(--muted);white-space:nowrap}.valid{color:var(--cyan)}.blocked{color:var(--amber)}.failed{color:var(--red)}.untested{color:var(--muted)}.notice{border:1px solid #456386;background:#142237;border-radius:12px;padding:18px 22px;margin:24px 0}.notice strong{color:#bdd6ff}.notice p{margin:5px 0 0}.metrics{display:grid;grid-template-columns:repeat(5,1fr);gap:12px;margin-bottom:20px}.metric,.panel{border:1px solid var(--border);background:var(--panel);border-radius:12px}.metric{padding:18px}.metric .number{display:block;font-size:30px;font-weight:650;letter-spacing:-1px}.metric .label{font-size:12px;color:var(--muted)}.panel{padding:20px;margin-bottom:18px;min-width:0}.two{display:grid;grid-template-columns:1.25fr 1fr;gap:18px}.bar{display:flex;gap:3px;height:12px;border-radius:4px;overflow:hidden;background:var(--raised);margin:12px 0 16px}.bar span{min-width:2px}.bar .valid{background:var(--cyan)}.bar .blocked{background:var(--amber)}.bar .failed{background:var(--red)}.bar .untested{background:#65778c}.outcomes{display:flex;gap:8px;flex-wrap:wrap}.outcomes button{text-align:left;font-size:12px;padding:7px 10px}.outcomes b{padding-left:8px}.legend{font-size:12px;margin-top:12px}.meta{display:grid;grid-template-columns:120px minmax(0,1fr);gap:8px 12px}.meta dt{color:var(--muted)}.meta dd{margin:0;overflow-wrap:anywhere}.toolbar{display:grid;grid-template-columns:2fr repeat(3,1fr);gap:12px}.toolbar label,.sorter label{font-size:11px;font-weight:600;color:var(--muted)}.toolbar input,.toolbar select{display:block;width:100%;margin-top:5px;min-width:0}.sorter{display:flex;justify-content:space-between;gap:12px;align-items:center;margin:14px 0 0;flex-wrap:wrap}.sorter select{margin-left:8px;padding:6px}.workspace{display:grid;grid-template-columns:minmax(320px,.85fr) minmax(0,1.6fr);gap:18px;align-items:start}.case-list{display:grid;gap:7px}.case-button{display:block;width:100%;text-align:left;padding:13px 15px;background:#101a27}.case-button[aria-selected=true]{border-color:var(--blue);box-shadow:inset 3px 0 var(--blue);background:#1b2c42}.case-top{display:flex;justify-content:space-between;gap:8px;align-items:center}.case-file{display:block;color:var(--muted);font-size:12px;margin-top:6px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.case-bottom{display:flex;align-items:center;justify-content:space-between;gap:8px;margin-top:8px}.empty{text-align:center;color:var(--muted);padding:36px 16px}.inspector{position:sticky;top:18px;max-height:calc(100vh - 36px);overflow:auto;scrollbar-gutter:stable}.inspector:focus{outline:none}.inspector-header{display:flex;justify-content:space-between;align-items:start;gap:15px}.inspector h2{overflow-wrap:anywhere;margin-bottom:3px}.verdict{border:1px solid var(--border);border-left:4px solid currentColor;background:#101b29;border-radius:8px;padding:16px;margin:18px 0}.verdict p{color:var(--ink)}.verdict .answer{font-size:19px;font-weight:650;margin:4px 0}.semantic{color:var(--amber);font-weight:650}.next{background:#1a2839;border-radius:7px;padding:12px;margin-top:12px;color:var(--ink)}.pipeline{display:grid;grid-template-columns:repeat(4,1fr);gap:6px;margin:12px 0}.step{border:1px solid var(--border);border-radius:6px;padding:9px;font-size:11px;color:var(--muted)}.step.reached{border-color:#367d78;color:var(--cyan);background:#122d30}.step.stopped{border-color:#a9864d;color:var(--amber);background:#32291d}.step span{display:block;font-size:10px;margin-top:4px}.facts{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;margin:16px 0}.fact{padding:10px;background:#0c1521;border-radius:7px}.fact strong{display:block}.fact small{font-size:11px}.source{border:1px solid var(--border);border-radius:8px;padding:12px;margin-bottom:9px}.source p{margin:4px 0}.source .meta{grid-template-columns:100px minmax(0,1fr);font-size:12px}.diagnostic{border-left:3px solid var(--red);padding-left:12px;margin:12px 0;overflow-wrap:anywhere}ul{padding-left:20px}li{margin:5px 0}.warning{color:var(--amber)}.footer{font-size:12px;color:var(--muted);padding:12px 0 25px}.table-wrap{overflow:auto}table{border-collapse:collapse;width:100%;font-size:12px}th,td{text-align:left;vertical-align:top;border-bottom:1px solid var(--border);padding:9px}th{color:var(--muted)}.spaced{margin-top:18px}.flash{font-size:12px;color:var(--cyan)}#coverage-list{max-height:450px;overflow:auto}#source-root{overflow-wrap:anywhere}.sr-only{position:absolute;width:1px;height:1px;padding:0;overflow:hidden;clip:rect(0,0,0,0);white-space:nowrap}.no-js{padding:20px;border:1px solid var(--amber);color:var(--amber)}
.case-list{max-height:760px;overflow:auto;scrollbar-gutter:stable}.step.rejected{border-color:#a45353;color:var(--red);background:#321d22}.stage-guide{margin:10px 0;color:var(--muted);font-size:12px}
@media(max-width:1100px){main{padding:22px}.metrics{grid-template-columns:repeat(3,1fr)}.workspace{grid-template-columns:minmax(280px,.8fr) minmax(0,1.2fr)}.two{grid-template-columns:1fr}.toolbar{grid-template-columns:repeat(2,1fr)}}
@media(max-width:760px){main{padding:14px}header{align-items:start}h1{font-size:27px}.metrics{grid-template-columns:repeat(2,1fr)}.workspace{grid-template-columns:1fr}.inspector{position:static;max-height:none}.toolbar{grid-template-columns:1fr 1fr}.toolbar label:first-child{grid-column:1/-1}.pipeline{grid-template-columns:repeat(2,1fr)}.facts{grid-template-columns:repeat(2,1fr)}.panel{padding:15px}.case-list{max-height:480px;overflow:auto}.meta{grid-template-columns:95px minmax(0,1fr)}}
@media(max-width:760px){header{flex-direction:column;gap:8px}.notice{margin-top:18px}}

/* Continuous browser and direct-access evidence navigation. */
[hidden]{display:none!important}
.workspace{grid-template-columns:minmax(310px,.72fr) minmax(0,1.6fr)}
.case-panel{position:sticky;top:18px}
.list-heading{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:14px}
.list-heading h2{margin:0}.list-heading button{font-size:11px;padding:7px}
.case-list{display:block;position:relative;height:min(70vh,700px);max-height:none;overflow:auto;overflow-anchor:none;scrollbar-gutter:stable}
#case-canvas{position:relative;width:100%}
.case-button{position:absolute;left:0;right:0;height:104px;cursor:pointer;overflow:hidden;border:1px solid var(--border);border-radius:8px;padding:12px;background:#101a27}
.case-button:hover{border-color:#6685ad;background:#19293b}.case-top code{font-size:12px}
.case-bottom .badge{max-width:72%;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.case-bottom small{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.list-help{font-size:11px;margin:12px 0 0}
.inspector{position:static;max-height:none;overflow:visible;scrollbar-gutter:auto;padding:0}
.inspector-nav{position:sticky;top:0;z-index:3;background:var(--panel);border-radius:12px 12px 0 0;border-bottom:1px solid var(--border);padding:18px 20px 0}
.inspector-header h2{font-size:20px}.inspector-header small{display:block;max-width:42ch;overflow:hidden;white-space:nowrap;text-overflow:ellipsis}
.inspector-header button{white-space:nowrap;font-size:12px}
.tabs{display:flex;gap:4px;margin-top:14px;overflow:auto;scrollbar-width:thin}
.tab{flex:1;border:0;border-bottom:3px solid transparent;border-radius:5px 5px 0 0;background:transparent;color:var(--muted);white-space:nowrap;padding:11px 9px;font-size:12px;font-weight:650}
.tab[aria-selected=true]{color:var(--blue);border-bottom-color:var(--blue);background:#1a2a3e}
.tab:hover{background:var(--raised)}.tab .tab-count{display:inline-block;font-size:10px;margin-left:4px;opacity:.8}
#inspector-content{padding:20px;min-width:0}#inspector-content:focus{outline:none}
#inspector-content .verdict{margin-top:0}
.evidence-section{margin-top:24px;padding-top:20px;border-top:1px solid var(--border)}
.evidence-section:first-child{margin-top:0;padding-top:0;border-top:0}
.evidence-section h3{font-size:16px}.section-description{font-size:12px;color:var(--muted)}
.section-title{display:flex;align-items:center;justify-content:space-between;gap:12px}
.key-evidence{background:#211c24;border:1px solid #764d5b;border-radius:8px;padding:14px;margin:16px 0}
.key-evidence h3{color:var(--red)}.key-evidence p{overflow-wrap:anywhere;font-size:13px;margin-bottom:0}
.shortcut{color:var(--blue);font-size:12px;background:transparent}
.log-view{max-height:520px;line-height:1.7;tab-size:4}
.source{margin-top:14px;padding:16px}.source:first-child{margin-top:0}
.source pre{max-height:260px}.source-number{font-size:11px;letter-spacing:1px;color:var(--cyan);margin-bottom:6px}
.artifact-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px}
.artifact-card{display:block;border:1px solid var(--border);border-radius:8px;background:#101a27;padding:14px;text-decoration:none}
.artifact-card:hover{border-color:var(--blue);background:#19293b}.artifact-card strong{display:block;font-size:13px;color:var(--blue)}
.artifact-card span{display:block;font-size:12px;color:var(--muted);margin-top:6px}
.raw-view{max-height:600px}.fingerprints{font-size:11px}.fingerprints dd{word-break:break-all}
.coverage-label{display:block;font-size:12px;color:var(--muted);margin-bottom:6px}
#coverage-search{width:100%;margin-bottom:14px}.coverage-grid{display:grid;grid-template-columns:minmax(220px,.8fr) minmax(0,1.2fr);gap:16px}
#coverage-list{max-height:420px;overflow:auto}
.finding-button{display:block;width:100%;text-align:left;margin-bottom:7px}
.finding-button[aria-pressed=true]{border-color:var(--blue);background:#1b2c42}.finding-button code{display:block}
.finding-button small{display:block;margin-top:4px}
.coverage-grid pre{margin:0;max-height:420px}.coverage-grid h3{overflow-wrap:anywhere}
.navigation-hint{margin-top:12px;font-size:12px;color:var(--muted)}
@media(max-width:1100px){.workspace{grid-template-columns:minmax(285px,.75fr) minmax(0,1.35fr)}.tab{padding:10px 6px}}
@media(max-width:760px){.workspace{grid-template-columns:1fr}.case-panel{position:static}.case-list{height:360px;max-height:none}.inspector-nav{position:static;padding:16px 14px 0}#inspector-content{padding:15px}.tabs{flex-wrap:wrap;overflow:visible;gap:2px}.tab{flex:1 0 30%}.coverage-grid,.artifact-grid{grid-template-columns:1fr}.inspector-header small{max-width:24ch}.fingerprints{font-size:10px}}

</style></head><body><main>
<header><div><div class="eyebrow">KYTYPS5 / OFFLINE COMPILER EVIDENCE</div><h1>Shader investigation lab</h1><p class="muted">From recovered bytes to compiler output. Understand what passed, what stopped, and what remains unknown.</p></div><span id="report-mode" class="pill">OFFLINE REPORT</span></header>
<noscript><p class="no-js">Enable JavaScript to explore the embedded report. No network access is required. The source manifest and results.json remain the machine-readable records.</p></noscript>
<div class="notice"><strong>Is the shader correct?</strong><p>This report can show whether Kyty emitted structurally valid SPIR-V under a recorded profile. It cannot prove equivalent pixels, memory writes, numerical results or synchronization. <b>Rendering / semantic correctness is unverified for every case.</b> Failures under assumed state are investigation leads, not automatically emulator bugs.</p></div>
<div id="metrics" class="metrics"></div>
<div class="two"><section class="panel"><h2>Compiler outcomes</h2><p class="muted" id="run-summary"></p><div id="outcome-bar" class="bar" aria-hidden="true"></div><div id="outcomes" class="outcomes"></div><p class="legend muted">Counts describe unique header + code cases, not all shaders a game may use. Click an outcome to filter the case explorer.</p></section>
<section class="panel"><h2>Run identity &amp; coverage</h2><dl id="identity" class="meta"></dl><h3 class="spaced">Compiler and profile fingerprints</h3><dl id="fingerprints" class="meta fingerprints"></dl><p id="coverage-warning" class="warning"></p></section></div>
<section class="panel" id="explorer"><h2>Explore shader cases</h2><div class="toolbar">
<label>Search evidence<input id="search" type="search" placeholder="Hash, source, opcode, error or phase…"></label>
<label>Outcome<select id="status-filter"><option value="">All outcomes</option></select></label>
<label>Header stage<select id="stage-filter"><option value="">All stages</option></select></label>
<label>Game / source group<select id="game-filter"><option value="">All games</option></select></label></div>
<div class="sorter"><span id="match-count" role="status" aria-live="polite"></span><div><label>Sort<select id="sort"><option value="attention">Needs attention first</option><option value="hash">Kyty hash</option><option value="time">Worker duration ↓</option><option value="size">Code size ↓</option></select></label> <button id="reset" type="button">Reset filters</button></div></div></section>
<div class="workspace"><section class="panel case-panel" aria-label="Shader cases"><div class="list-heading"><div><h2>Shader cases</h2><small id="list-count"></small></div><button id="reveal-selected" type="button">Find selected</button></div><div id="case-list" class="case-list" role="listbox" tabindex="0" aria-label="Shader cases" aria-describedby="list-help"><div id="case-canvas"></div></div><p id="list-help" class="muted list-help">Scroll through all matches · ↑ ↓ to select · Home / End to jump</p></section>
<section id="inspector" class="panel inspector" tabindex="-1" aria-label="Selected shader evidence"></section></div>
<section class="panel"><h2>Extraction coverage &amp; limitations</h2><p>Scanning a file is not proof that every shader inside it was recovered. Encrypted or unsupported archives, runtime-generated code, ambiguous matches and missing draw state can leave gaps. Extraction findings are separate from compiler failures.</p><p id="coverage-summary" class="muted"></p><div id="traversal-errors"></div><label class="coverage-label" for="coverage-search">Find a source finding</label><input id="coverage-search" type="search" placeholder="Filter by file, status or finding text…"><div class="coverage-grid"><div id="coverage-list" aria-label="Source findings"></div><div id="coverage-evidence" aria-label="Selected source finding"></div></div></section>
<section class="panel"><h2>How to read this report</h2><div class="table-wrap"><table><thead><tr><th>Evidence layer</th><th>What it establishes</th><th>What it does not establish</th></tr></thead><tbody>
<tr><td>Extraction</td><td>A candidate matched a supported binary layout or documented pairing heuristic. Source offsets and hashes identify it.</td><td>Universal archive coverage, a real runtime invocation, or correct shader state.</td></tr>
<tr><td>Decode / CFG / translation</td><td>The worker reached recorded compiler phases; available disassembly and IR help localize failures.</td><td>That every instruction was translated with equivalent behavior.</td></tr>
<tr><td>SPIR-V validation</td><td>The emitted module passed the recorded validator environment under this profile.</td><td>Correct rendering, correct resource contents, or compatibility with every GPU/driver.</td></tr>
<tr><td>Reference execution</td><td>Not performed by this tool.</td><td>Semantic correctness requires controlled inputs and comparison with an independent trusted reference.</td></tr>
</tbody></table></div><p class="spaced">A header probe uses derived/default state; a context snapshot is supplied state, not independently verified capture provenance. Capture real user-data, descriptors, memory, stage and draw/dispatch state before classifying profile-dependent failures as emulator bugs.</p></section>
<footer class="footer">Self-contained offline report · No external assets, telemetry or network requests · Artifact links require the original run layout. Reports contain source paths and diagnostic logs; review before sharing.</footer>
</main>
)HTML";

inline constexpr std::string_view report_script = R"JS(
<script>
'use strict';
const DATA = JSON.parse(document.getElementById('report-data').textContent);
const $ = id => document.getElementById(id);
const esc = value => String(value ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const pretty = value => JSON.stringify(value, null, 2);
const num = value => Number(value || 0).toLocaleString();
const defined = value => value === undefined || value === null ? 'Not recorded' : String(value);
const pre = value => '<pre>' + esc(typeof value === 'string' ? value : pretty(value)) + '</pre>';
const pair = (label, value, mono=false) => '<dt>'+esc(label)+'</dt><dd'+(mono?' class="mono"':'')+'>'+esc(defined(value))+'</dd>';
const OUTCOMES = {
 pass_checkpoint_reached: ['untested','Diagnostic stop','Requested compiler prefix completed','Compilation stopped intentionally at a pass checkpoint. Intermediate IR and pass return are not validation or semantic correctness.','Inspect pass-trace.json and pass-stop.ir. Remove stop_after_pass to run the complete compiler; use bisect-passes on a reproducible assertion bundle.'],
 spirv_valid_under_profile: ['valid','SPIR-V valid','Structurally valid; semantics unverified','Kyty emitted a module that passed SPIRV-Tools under the recorded profile. No GPU/reference execution was performed.','Preserve this profile as a regression baseline. To test correctness, replay controlled resources and compare outputs against an independent trusted reference.'],
 spirv_invalid_under_profile: ['failed','SPIR-V invalid','Generated module failed validation','The validator rejected the emitted module in the recorded environment. It is not structurally valid under this profile.','Inspect validator messages and shader.spvasm, then trace the offending operation through final.ir and the SPIR-V emitter. Confirm the profile matches the intended stage/state.'],
 resource_context_unresolved: ['blocked','Needs resource context','Compilation blocked by unresolved resources','The worker could not materialize the required resources from the supplied or assumed user-data and memory.','Inspect memory-reads.json and capture the missing descriptor/user-data memory. Retry with an explicit context profile; do not substitute fabricated descriptors.'],
 missing_stage_context: ['blocked','Needs stage context','Compilation blocked by missing stage state','The adapter lacks enough stage, partner, fetch or draw state to compile this case responsibly. Decode evidence may still be available.','Capture stage/partner and draw state. Extend the adapter for the required stage preparation; changing only the stage label does not recreate missing state.'],
 unsupported_instruction: ['blocked','Unsupported instruction','Instruction support is incomplete','The decoder reported an unknown or unsupported instruction. This is a support gap, not a successful translation.','Use the recorded instruction PC, family, opcode and guest.asm to verify encoding and implement/test the missing decoding or translation semantics.'],
 unsupported_ray_tracing: ['blocked','Ray tracing unsupported','Ray-tracing path is not supported','A BVH/ray-tracing skip path was detected. Decode may have stopped early; no successful RT compilation is established.','Inspect the BVH instruction and required resource/state semantics. Add supported RT translation and dedicated reference tests before treating this shader as covered.'],
 worker_crash_or_error: ['failed','Worker failed','Compiler attempt failed','The isolated compiler worker exited with an error. The last phase and log identify where to investigate, not necessarily the root cause.','Read the fatal message and worker.log. Reproduce with matching source, shader and profile; verify resource/state assumptions before changing the compiler.'],
 timeout: ['failed','Timed out','Compiler attempt exceeded its deadline','The worker was terminated at the configured deadline. Slow compilation and a hang are both possible.','Inspect the last recorded phase and log. Re-run this case with a deliberate larger timeout and compare progress before diagnosing a compiler hang.'],
 invalid_input: ['failed','Invalid input','Input validation failed','The worker rejected the header/code extent or input format.','Inspect the reason, source offsets and header/code hashes. Check extraction and format assumptions before interpreting compiler behavior.'],
 adapter_error: ['failed','Adapter error','Adapter could not complete the request','The worker adapter rejected a profile or raised an exception. This is not evidence of incorrect shader output.','Inspect the reason and selected profile. Correct unsupported fields, invalid ranges or missing adapter context and retry.'],
 runner_error: ['failed','Runner error','No reliable compiler verdict','The orchestration layer could not prepare or complete this case.','Inspect the recorded error, object hashes, file permissions and worker setup before retrying.'],
 worker_protocol_error: ['failed','Worker protocol error','No reliable compiler response','The worker did not return the expected response schema and case identity.','Inspect request.json, response.json and worker.log; verify the worker matches this tool protocol.'],
 not_tested: ['untested','Not tested','No compiler result available','This extracted case has no matching result in the supplied run. Extraction alone says nothing about compilation or correctness.','Run the corpus without a case limit, or supply the matching results.json when generating this report.'],
 unknown: ['untested','Unknown outcome','No interpretable verdict','The recorded status is not recognized by this report version. It is not treated as a pass.','Inspect the raw result and worker version. Update the status mapping only when its evidence contract is understood.']
};
const statusOf = c => c.result.status || 'not_tested';
const outcome = c => OUTCOMES[statusOf(c)] || OUTCOMES.unknown;
const sourceName = o => o.game && o.game !== '.' ? o.game : (DATA.root.replaceAll('\\','/').split('/').filter(Boolean).pop() || 'Input root');
const cases = DATA.cases;
const counts = {};
const revisions = new Set();
cases.forEach((c,i) => {
 c.index=i; c.details=c.result.details || {}; c.origins=c.shader.origins || [];
 c.games=[...new Set(c.origins.map(sourceName))];
 c.phase=c.details.last_phase || c.result.last_phase?.phase || 'Not recorded';
 c.search=JSON.stringify([c.id,c.shader,c.result.status,c.phase,c.details,c.result.error,c.result.log_tail]).toLowerCase();
 const s=statusOf(c); counts[s]=(counts[s]||0)+1;
 if(c.details.kyty_revision) revisions.add(c.details.kyty_revision);
});
const tested=cases.filter(c=>Object.keys(c.result).length).length;
const valid=counts.spirv_valid_under_profile||0;
const blocked=cases.filter(c=>outcome(c)[0]==='blocked').length;
const failed=cases.filter(c=>outcome(c)[0]==='failed').length;
const card=(n,label,sub,kind='')=>'<div class="metric"><span class="label">'+esc(label)+'</span><span class="number '+kind+'">'+num(n)+'</span><small>'+esc(sub)+'</small></div>';
$('metrics').innerHTML=card(cases.length,'Extracted cases',num(DATA.file_count)+' files in manifest')+card(valid,'Structurally valid','Under the recorded profile','valid')+card(blocked,'Blocked / unsupported','Missing context or support','blocked')+card(failed,'Failed attempts','Compiler, input or runner errors','failed')+card(0,'Semantically verified','No reference execution performed','blocked');
$('run-summary').textContent=num(tested)+' / '+num(cases.length)+' cases have recorded results. '+(tested ? (100*valid/tested).toFixed(1)+'% yielded valid SPIR-V (not a correctness score).' : 'No compilation evidence loaded.');
$('identity').innerHTML=pair('Input root',DATA.root)+pair('Scan',DATA.scan_in_progress?'Checkpoint / still in progress':DATA.scan_limited?'Limited scan':'No explicit scan limit recorded')+pair('Results',DATA.has_run?(DATA.run_limited?'Limited run; inspect untested cases':'Run results loaded'):'Extraction only')+pair('Kyty revision',revisions.size?[...revisions].join(', '):'Not recorded in available responses',true);
$('fingerprints').innerHTML=pair('Worker SHA-256',DATA.worker_sha256||'Not recorded',true)+pair('Profile SHA-256',DATA.profile_sha256||'Not recorded',true)+pair('Extractor',DATA.extractor_id||'Not recorded',true);
const warnings=[];
if(DATA.scan_in_progress) warnings.push('This manifest is an in-progress checkpoint.');
if(DATA.scan_limited) warnings.push('The scan was limited; extraction coverage is incomplete.');
if(tested<cases.length) warnings.push(num(cases.length-tested)+' cases have no recorded result.');
if(DATA.orphan_results) warnings.push(num(DATA.orphan_results)+' results do not match this manifest and were excluded. Check dataset/run pairing.');
if(DATA.gaps.length || DATA.traversal_errors.length) warnings.push('File findings or traversal errors are recorded below.');
$('coverage-warning').textContent=warnings.join(' ');
for(const [s,n] of Object.entries(counts).sort((a,b)=>b[1]-a[1])) {
 const info=OUTCOMES[s]||OUTCOMES.unknown;
 const segment=document.createElement('span');segment.className=info[0];segment.style.flex=String(n);$('outcome-bar').append(segment);
 const button=document.createElement('button');button.type='button';button.className=info[0];button.innerHTML=esc(info[1])+' <b>'+num(n)+'</b>';
 button.addEventListener('click',()=>{$('status-filter').value=s;applyFilters();$('explorer').scrollIntoView({block:'start',behavior:'smooth'});});$('outcomes').append(button);
 $('status-filter').add(new Option(info[1]+' ('+n+')',s));
}
for(const stage of [...new Set(cases.map(c=>c.shader.type||'Unknown'))].sort()) $('stage-filter').add(new Option(stage,stage));
for(const game of [...new Set(cases.flatMap(c=>c.games))].sort()) $('game-filter').add(new Option(game,game));
$('coverage-summary').textContent=num(DATA.gaps.length)+' files with findings or non-scanned status; '+num(DATA.traversal_errors.length)+' traversal errors. '+(DATA.coverage||'');

// Source findings use a searchable list and a visible evidence pane, not nested disclosures.
function renderCoverage(){
 const query=$('coverage-search').value.trim().toLowerCase();
 const visible=DATA.gaps.map((g,index)=>({g,index})).filter(({g})=>JSON.stringify(g).toLowerCase().includes(query));
 $('traversal-errors').innerHTML=DATA.traversal_errors.length?'<h3>Traversal errors</h3>'+pre(DATA.traversal_errors):'';
 $('coverage-list').innerHTML=visible.map(({g,index})=>'<button type="button" class="finding-button" data-finding="'+index+'" aria-pressed="false"><code>'+esc(g.file)+'</code><small>'+esc(g.record.status)+' · '+num((g.record.findings||[]).length)+' findings</small></button>').join('')||'<p class="muted">'+(DATA.gaps.length?'No findings match this search.':'No file findings recorded. This does not prove universal extraction coverage.')+'</p>';
 function selectFinding(index){
  const g=DATA.gaps[index];
  $('coverage-list').querySelectorAll('button').forEach(b=>b.setAttribute('aria-pressed',String(Number(b.dataset.finding)===index)));
  $('coverage-evidence').innerHTML=g?'<h3>'+esc(g.file)+'</h3>'+pre(g.record):'<p class="muted">Select a source finding to inspect its full record.</p>';
 }
 $('coverage-list').querySelectorAll('button').forEach(b=>b.addEventListener('click',()=>selectFinding(Number(b.dataset.finding))));
 selectFinding(visible[0]?.index);
}
$('coverage-search').addEventListener('input',renderCoverage);
renderCoverage();

let filtered=[],selected=null,activeTab='overview';
const rowStride=112,overscan=4;
const TAB_NAMES=[['overview','Overview'],['diagnostics','Diagnostics'],['context','Context'],['sources','Sources'],['artifacts','Artifacts'],['raw','Raw data']];
const fact=(label,value)=>'<div class="fact"><small>'+esc(label)+'</small><strong>'+esc(defined(value))+'</strong></div>';
const section=(title,body,description='')=>'<section class="evidence-section"><h3>'+title+'</h3>'+(description?'<p class="section-description">'+description+'</p>':'')+body+'</section>';
function selectedProfile(c){const p=DATA.profile||{};return p.cases?(p.cases[c.id]||p.default||{}):p;}
function fatalMessage(c){
 const log=c.result.log_tail||'',marker=log.lastIndexOf('--- Error ---');
 return marker>=0?log.slice(marker+13).trim().split(/\r?\n/)[0]:'';
}
function diagnosticHtml(c){
 const d=c.details,r=c.result,fatal=fatalMessage(c);
 return (d.reason?'<div class="diagnostic"><b>Recorded reason</b><p>'+esc(d.reason)+'</p></div>':'')+
 (r.error?'<div class="diagnostic"><b>Runner error</b><p>'+esc(r.error)+'</p></div>':'')+
 (fatal?'<div class="diagnostic"><b>Fatal message from worker log</b><p>'+esc(fatal)+'</p><small>This is the reported failure site, not proof of root cause.</small></div>':'')+
 (d.validator_messages||[]).map(m=>'<div class="diagnostic"><b>Validator · instruction index '+esc(defined(m.index))+'</b><p>'+esc(m.message)+'</p></div>').join('');
}
function unsupportedHtml(c){
 const ops=c.details.unsupported||[];
 return ops.length?'<div class="table-wrap"><table><thead><tr><th>Guest PC</th><th>Family / opcode</th><th>Instruction</th></tr></thead><tbody>'+ops.map(o=>'<tr><td><code>0x'+esc(Number(o.pc).toString(16))+'</code></td><td>'+esc(o.family)+' / '+esc(o.opcode_id)+'</td><td><code>'+esc(o.text||o.opcode)+'</code></td></tr>').join('')+'</tbody></table></div>':'';
}
function overviewHtml(c){
 const d=c.details,r=c.result,info=outcome(c),status=statusOf(c);
 const stages=[['input_validation','Input'],['initialize','Initialize'],['decode','Decode'],['cfg','CFG'],...(d.context_mode==='captured_compute'||c.profile?.mode==='captured_compute'||c.phase==='prepare'?[['prepare','Prepare state']]:[]),['translate','Translate'],['materialize','Resources'],['emit','SPIR-V'],['validate','Validate']];
 const at=stages.findIndex(p=>p[0]===c.phase);
 const pipeline=stages.map(([id,label],i)=>{
  const finished=i===at&&(status==='spirv_valid_under_profile'||status==='spirv_invalid_under_profile');
  const state=i<at?'reached':i===at?(finished?(status==='spirv_valid_under_profile'?'reached':'rejected'):'stopped'):'';
  const text=i<at?'Advanced beyond':i===at?(finished?(status==='spirv_valid_under_profile'?'Validation passed':'Validation failed'):'Last phase reached'):'Not recorded';
  return '<div class="step '+state+'">'+label+'<span>'+text+'</span></div>';
 }).join('');
 const reason=fatalMessage(c)||d.reason||r.error||(d.validator_messages||[])[0]?.message;
 const evidence=reason?'<div class="key-evidence"><div class="section-title"><h3>Reported failure</h3><button type="button" class="shortcut" data-open-tab="diagnostics">Full diagnostics →</button></div><p>'+esc(reason)+'</p></div>':(d.unsupported||[]).length?'<div class="key-evidence"><h3>'+num(d.unsupported.length)+' unsupported instructions</h3>'+unsupportedHtml(c)+'<button type="button" class="shortcut" data-open-tab="diagnostics">Inspect diagnostics →</button></div>':'';
 return '<div class="verdict '+info[0]+'"><span class="badge '+info[0]+'">'+esc(info[1])+'</span><p class="answer">'+esc(info[2])+'</p><p>'+esc(info[3])+'</p><div class="semantic">Correct shader behavior? Not verified.</div><div class="next"><b>Next investigation step</b><p>'+esc(info[4])+'</p></div></div>'+evidence+
 section('Compiler journey','<div class="pipeline">'+pipeline+'</div>','Phase markers show execution progress, not semantic correctness. GPU execution and reference comparison were not performed.')+
 section('Recorded measurements','<div class="facts">'+fact('Header stage',c.shader.type)+fact('Effective compiler stage',d.stage)+fact('Code bytes',c.shader.code_bytes)+fact('Decoded instructions',d.decoded_instruction_count)+fact('IR blocks',d.ir_blocks)+fact('SPIR-V words',d.spirv_words)+fact('Worker duration',r.elapsed_ms===undefined?undefined:num(r.elapsed_ms)+' ms')+fact('Process exit code',r.exit_code)+fact('Result reused',r.cache_hit===undefined?undefined:r.cache_hit?'Yes — prior attempt':'No')+'</div><p class="muted">Duration is process wall time, not GPU time. Reused results retain the original duration. Exit code 0 means a response was returned, not that the shader passed.</p>')+
 '<p class="navigation-hint">Use Context for profile assumptions, Sources for extraction provenance, or Artifacts for generated compiler files.</p>';
}
function contextHtml(c){
 const d=c.details,r=c.result,profile=selectedProfile(c),assumptions=d.assumptions||[];
 return section('Context & confidence','<dl class="meta">'+pair('Context mode',d.context_mode||profile.mode||'Not recorded')+pair('Validation target',d.validation_environment)+pair('Kyty revision',d.kyty_revision,true)+pair('Dispatcher fallback',d.dispatcher_fallback)+'</dl><p class="warning">Header probes use inferred/default state. Supplied snapshots are not independently verified captures. Neither mode proves semantic correctness.</p>')+
 section('Recorded assumptions',assumptions.length?'<ul>'+assumptions.map(a=>'<li>'+esc(a)+'</li>').join(''):'<p class="muted">No assumptions were returned. This does not certify that runtime state was captured.</p>')+
 (d.effective_compute?section('Effective compute configuration',pre(d.effective_compute)):'')+
 (d.effective_pixel?section('Effective pixel configuration and defaults',pre(d.effective_pixel)):'')+
 section('Selected input profile',pre(profile))+
 section('Profile identity','<dl class="meta">'+pair('Selected profile SHA-256',r.profile_sha256,true)+'</dl>')+
 '<p class="stage-guide">CS = compute; PS = pixel/fragment; VS = vertex; GS = geometry; HS = hull/tessellation control. Header variants may require additional partner or draw state.</p>';
}
const ARTIFACT_GROUPS=[
 ['Disassembly & control flow',[['guest.asm','Decoded guest instructions'],['instructions.json','Instruction inventory and opcode histogram'],['native-cfg.txt','Native control-flow graph'],['cfg.json','Control-flow blocks and edges'],['cfg.dot','Graphviz control-flow graph'],['cfg.txt','Translated control-flow dump']]],
 ['Translation & resources',[['translated.ir','Intermediate representation after translation'],['final.ir','Final intermediate representation'],['header-registers.json','Header register values'],['memory-reads.json','Resource materialization read trace']]],
 ['Compiler passes',[['pass-trace.json','Ordered upstream pass entry/return checkpoints'],['pass-stop.ir','IR at an intentional prefix stop; not a correctness verdict']]],
 ['Captured state',[['captured-state.json','Ordered register/PM4 provenance and upstream preparation inputs']]],
 ['SPIR-V output',[['shader.spv','Emitted binary module'],['shader.spvasm','Human-readable SPIR-V assembly']]],
 ['Reproduction & worker evidence',[['request.json','Exact worker request and selected profile'],['response.json','Worker response and diagnostics'],['result.json','Runner outcome and timing'],['phase.json','Last compiler phase checkpoint'],['worker.log','Complete worker log']]]
];
function artifactsHtml(c){
 let body='<p class="muted">Open an artifact directly. Availability is not proof of success: files may remain from an earlier retry. Links require the original run layout.</p>';
 for(const [title,items] of ARTIFACT_GROUPS){
  const found=items.map(([name,description])=>({a:c.artifacts.find(a=>a.name===name),description})).filter(x=>x.a);
  if(found.length)body+=section(title,'<div class="artifact-grid">'+found.map(({a,description})=>'<a class="artifact-card" href="'+esc(a.href)+'" target="_blank" rel="noopener"><strong>'+esc(a.name)+' ↗</strong><span>'+esc(description)+'</span></a>').join('')+'</div>');
 }
 if(!c.artifacts.length)body+='<div class="empty">No artifacts found. Restore the run directory and regenerate the report to recover links.</div>';
 if(c.artifact_warning)body+='<p class="warning">'+esc(c.artifact_warning)+'</p>';
 return body;
}
function tabHtml(c){
 const d=c.details,r=c.result;
 if(activeTab==='overview')return overviewHtml(c);
 if(activeTab==='diagnostics'){
  const diagnostic=diagnosticHtml(c);
  return section('Diagnostics & failure evidence',diagnostic||'<p class="muted">No structured diagnostic was recorded. Check the worker log for interrupted attempts.</p>')+
  ((d.unsupported||[]).length?section('Unsupported instructions',unsupportedHtml(c)):'')+
  (d.missing_memory?'<p class="warning">The resource materializer reported missing memory. Inspect memory-reads.json for attempted ranges.</p>':'')+
  section('Worker log tail','<pre class="log-view">'+esc(r.log_tail||'No worker log tail recorded.')+'</pre>','Up to the final 8 KiB. The complete worker.log is available in Artifacts.')+
  '<dl class="meta">'+pair('Raw status',statusOf(c))+pair('Last phase',c.phase)+'</dl>';
 }
 if(activeTab==='context')return contextHtml(c);
 if(activeTab==='sources')return '<h3>'+num(c.origins.length)+' recorded source origins</h3><p class="muted">Offsets are in the stated coordinate space, not necessarily the original file. Every recorded origin is shown below.</p>'+
  (c.origins.map((o,i)=>'<article class="source"><div class="source-number">ORIGIN '+(i+1)+'</div><b>'+esc(sourceName(o))+'</b><p><code>'+esc(o.file)+'</code></p><dl class="meta">'+pair('Method',o.method)+pair('Offset space',o.offset_space||'file')+pair('Header offset',o.header_offset,true)+pair('Code offset',o.code_offset,true)+'</dl><h3>Extraction evidence</h3>'+pre(o.evidence||[])+'</article>').join('')||'<p class="muted">No source origins recorded.</p>');
 if(activeTab==='artifacts')return artifactsHtml(c);
 return section('Complete case identity','<dl class="meta">'+pair('Case ID',c.id,true)+pair('Header SHA-256',c.shader.header_sha256,true)+pair('Code SHA-256',c.shader.code_sha256,true)+pair('SPIR-V SHA-256',d.spirv_sha256,true)+'</dl><p class="muted">Kyty hashes identify code. The complete case ID also distinguishes header variants.</p>')+
 section('Raw worker result','<pre class="raw-view">'+esc(pretty(r))+'</pre>');
}
function renderTab(){
 const c=selected===null?null:cases[selected];if(!c)return;
 $('inspector').querySelectorAll('[role=tab]').forEach(b=>{const active=b.dataset.tab===activeTab;b.setAttribute('aria-selected',String(active));b.tabIndex=active?0:-1;});
 const panel=$('inspector-content');panel.setAttribute('aria-labelledby','tab-'+activeTab);panel.innerHTML=tabHtml(c);
 panel.querySelectorAll('[data-open-tab]').forEach(b=>b.addEventListener('click',()=>{activeTab=b.dataset.openTab;renderTab();$('tab-'+activeTab).focus({preventScroll:true});}));
}
function renderInspector(c){
 const box=$('inspector');if(!c){box.innerHTML='<div class="empty"><h2>No shader selected</h2><p>Adjust the filters or load a dataset containing extracted cases.</p></div>';return;}
 box.innerHTML='<div class="inspector-nav"><div class="inspector-header"><div><div class="eyebrow">SELECTED SHADER / '+esc(c.shader.type||'UNKNOWN')+'</div><h2 class="mono">'+esc(c.shader.kyty_hash)+'</h2><small>'+esc(c.games.join(' · '))+'</small></div><button id="copy-case" type="button">Copy case ID</button></div><span id="copy-state" class="flash" role="status"></span>'+
 '<nav class="tabs" role="tablist" aria-label="Shader evidence">'+TAB_NAMES.map(([key,label])=>'<button id="tab-'+key+'" class="tab" type="button" role="tab" data-tab="'+key+'" aria-selected="'+(activeTab===key)+'" aria-controls="inspector-content" tabindex="'+(activeTab===key?0:-1)+'">'+label+((key==='sources'||key==='artifacts')?'<span class="tab-count">'+num(key==='sources'?c.origins.length:c.artifacts.length)+'</span>':'')+'</button>').join('')+'</nav></div><div id="inspector-content" role="tabpanel" tabindex="0"></div>';
 box.querySelectorAll('[role=tab]').forEach(b=>{
  b.addEventListener('click',()=>{activeTab=b.dataset.tab;renderTab();});
  b.addEventListener('keydown',e=>{
   const i=TAB_NAMES.findIndex(([key])=>key===activeTab);let n;
   if(e.key==='ArrowRight')n=(i+1)%TAB_NAMES.length;else if(e.key==='ArrowLeft')n=(i+TAB_NAMES.length-1)%TAB_NAMES.length;else if(e.key==='Home')n=0;else if(e.key==='End')n=TAB_NAMES.length-1;else return;
   e.preventDefault();activeTab=TAB_NAMES[n][0];renderTab();$('tab-'+activeTab).focus({preventScroll:true});
  });
 });
 $('copy-case').addEventListener('click',async()=>{try{await navigator.clipboard.writeText(c.id);$('copy-state').textContent='Case ID copied.';}catch{$('copy-state').textContent='Clipboard unavailable. Use Raw data to select and copy the case ID.';}});
 renderTab();
}
function renderList(){
 const viewport=$('case-list'),canvas=$('case-canvas');
 canvas.style.height=(filtered.length*rowStride)+'px';
 const start=Math.max(0,Math.floor(viewport.scrollTop/rowStride)-overscan),end=Math.min(filtered.length,Math.ceil((viewport.scrollTop+viewport.clientHeight)/rowStride)+overscan);
 canvas.innerHTML=filtered.slice(start,end).map((c,i)=>'<div id="case-'+c.index+'" class="case-button" role="option" data-index="'+c.index+'" aria-selected="'+(selected===c.index)+'" aria-posinset="'+(start+i+1)+'" aria-setsize="'+filtered.length+'" style="top:'+((start+i)*rowStride)+'px"><span class="case-top"><code>'+esc(c.shader.kyty_hash)+'</code><span class="pill">'+esc(c.shader.type||'Unknown')+'</span></span><span class="case-file">'+esc(c.games.join(' · '))+' / '+esc(c.origins[0]?.file||'No source recorded')+'</span><span class="case-bottom"><span class="badge '+outcome(c)[0]+'">'+esc(outcome(c)[1])+'</span><small>'+esc(c.phase)+'</small></span></div>').join('');
 if(!filtered.length){canvas.style.height='auto';canvas.innerHTML='<div class="empty">No cases match these filters.</div>';}
 if(selected!==null&&$('case-'+selected))viewport.setAttribute('aria-activedescendant','case-'+selected);else viewport.removeAttribute('aria-activedescendant');
 $('list-count').textContent=num(filtered.length)+' matches · continuous scrolling';
 $('reveal-selected').disabled=selected===null;
}
function revealSelected(){
 const pos=filtered.findIndex(c=>c.index===selected);if(pos<0)return;
 const viewport=$('case-list'),top=pos*rowStride,bottom=top+rowStride;
 if(top<viewport.scrollTop)viewport.scrollTop=top;else if(bottom>viewport.scrollTop+viewport.clientHeight)viewport.scrollTop=bottom-viewport.clientHeight;
 renderList();
}
function selectCase(index,keyboard=false){
 if(!filtered.some(c=>c.index===index))return;
 selected=index;if(keyboard)revealSelected();else renderList();renderInspector(cases[selected]);
 if(!keyboard&&matchMedia('(max-width:760px)').matches){$('inspector').scrollIntoView({block:'start'});$('inspector').focus({preventScroll:true});}
}
let scrollFrame=0;
$('case-list').addEventListener('scroll',()=>{if(!scrollFrame)scrollFrame=requestAnimationFrame(()=>{scrollFrame=0;renderList();});});
new ResizeObserver(()=>renderList()).observe($('case-list'));
$('case-list').addEventListener('click',e=>{const row=e.target.closest('[data-index]');if(row){$('case-list').focus({preventScroll:true});selectCase(Number(row.dataset.index));}});
$('case-list').addEventListener('keydown',e=>{
 if(!filtered.length)return;
 const pos=Math.max(0,filtered.findIndex(c=>c.index===selected));let next;
 if(e.key==='ArrowDown')next=Math.min(pos+1,filtered.length-1);else if(e.key==='ArrowUp')next=Math.max(pos-1,0);else if(e.key==='Home')next=0;else if(e.key==='End')next=filtered.length-1;else return;
 e.preventDefault();selectCase(filtered[next].index,true);
});
$('reveal-selected').addEventListener('click',()=>{revealSelected();$('case-list').focus({preventScroll:true});});
function applyFilters(){
 const q=$('search').value.trim().toLowerCase(),s=$('status-filter').value,stage=$('stage-filter').value,game=$('game-filter').value;
 filtered=cases.filter(c=>(!q||c.search.includes(q))&&(!s||statusOf(c)===s)&&(!stage||(c.shader.type||'Unknown')===stage)&&(!game||c.games.includes(game)));
 const rank={failed:0,blocked:1,untested:2,valid:3},order=$('sort').value;
 filtered.sort((a,b)=>{let diff=0;if(order==='attention')diff=rank[outcome(a)[0]]-rank[outcome(b)[0]];if(order==='time')diff=(b.result.elapsed_ms||0)-(a.result.elapsed_ms||0);if(order==='size')diff=(b.shader.code_bytes||0)-(a.shader.code_bytes||0);return diff||String(a.shader.kyty_hash).localeCompare(String(b.shader.kyty_hash))||a.id.localeCompare(b.id);});
 if(!filtered.some(c=>c.index===selected))selected=filtered[0]?.index??null;
 $('match-count').textContent=num(filtered.length)+' of '+num(cases.length)+' cases match · Summary counts remain corpus-wide';
 $('case-list').scrollTop=0;renderList();revealSelected();renderInspector(selected===null?null:cases[selected]);
}
['search','status-filter','stage-filter','game-filter','sort'].forEach(id=>$(id).addEventListener(id==='search'?'input':'change',applyFilters));
$('reset').addEventListener('click',()=>{['search','status-filter','stage-filter','game-filter'].forEach(id=>$(id).value='');$('sort').value='attention';applyFilters();});
applyFilters();
</script></body></html>
)JS";
} // namespace sl

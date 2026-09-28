#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>

namespace ankurafathom::ir::explanation {
inline std::string escape_html(const std::string& value) {
    std::string result;
    for(char c:value) {
        if(c=='&') result+="&amp;";else if(c=='<') result+="&lt;";else if(c=='>') result+="&gt;";
        else if(c=='\"') result+="&quot;";else if(c=='\'') result+="&#39;";else result+=c;
    }
    return result;
}
inline std::string explanation_html(const nlohmann::json& report) {
    const auto e=[](const auto& x){return escape_html(x.template get<std::string>());};
    const auto number=[](const auto& x) {std::ostringstream s;s<<std::setprecision(10)<<x.template get<double>();return s.str();};
    std::ostringstream out;
    out<<"<!doctype html><html lang=\"en\"><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
       <<"<title>AnkuraFathom — "<<e(report.at("output"))<<"</title><style>"
       <<"*{box-sizing:border-box}body{margin:0;background:#f3f5f7;color:#172b3a;font:16px/1.55 system-ui,sans-serif}"
       <<"main{max-width:1120px;margin:auto;padding:40px 28px}h1{font-size:32px;line-height:1.2;margin:8px 0 16px}h2{font-size:22px}"
       <<".eyebrow{color:#526677;letter-spacing:.08em;font-size:13px}.card{background:white;border:1px solid #d6e0e7;border-radius:10px;padding:24px;margin:22px 0}"
       <<".metrics{display:flex;gap:36px;flex-wrap:wrap}.metric strong{font-size:28px;display:block}.muted{color:#526677}"
       <<"table{border-collapse:collapse;width:100%;font-size:14px}td,th{text-align:left;padding:10px 12px;border-bottom:1px solid #d6e0e7;vertical-align:top}"
       <<"th{background:#edf2f5}code,pre{font:13px/1.6 ui-monospace,monospace;overflow-wrap:anywhere}pre{white-space:pre-wrap}"
       <<".scroll{overflow:auto}svg{min-width:720px;width:100%;height:auto}summary{cursor:pointer;font-weight:600}li{margin:8px 0}"
       <<"@media(max-width:600px){main{padding:24px 14px}.card{padding:16px}h1{font-size:26px}}"
       <<"</style><main><div class=\"eyebrow\">ANKURAFATHOM / VERIFIED EXPLANATION</div><h1>What changed "<<e(report.at("output"))<<"?</h1>"
       <<"<p class=\"muted\">"<<e(report.at("model_name"))<<"</p><p>Scenario "<<report.at("scenario")<<" · Replication "<<report.at("replication")<<" · "
       <<number(report.at("window").at("from"))<<" → "<<number(report.at("window").at("at"))<<" "<<e(report.at("window").at("time_unit"))<<"</p>"
       <<"<div class=\"card\"><div class=\"metrics\"><div class=\"metric\">Starting value<strong>"<<number(report.at("start_value"))
       <<"</strong></div><div class=\"metric\">Ending value<strong>"<<number(report.at("value"))<<"</strong></div>";
    const auto& accounting=report.at("accounting");
    if(!accounting.is_null()) out<<"<div class=\"metric\">Change ("<<e(accounting.at("unit"))<<")<strong>"<<number(accounting.at("change"))<<"</strong></div>";
    out<<"</div><p class=\"muted\">Verified re-execution matches the captured inputs and complete numeric result. Passive flow tracking preserves the original observations bit for bit.</p></div>";
    if(!accounting.is_null()) {
        out<<"<section class=\"card\"><h2>Flows contributing to the change</h2><p>"<<e(report.at("method"))<<" · "<<e(accounting.at("unit"))<<"</p>";
        const auto& flows=accounting.at("flows");double largest=0;
        for(const auto& f:flows) largest=std::max(largest,std::abs(f.at("contribution").get<double>()));
        out<<"<div class=\"scroll\"><svg role=\"img\" aria-labelledby=\"flow-title\" viewBox=\"0 0 960 "<<48+flows.size()*34<<"\">"
           <<"<title id=\"flow-title\">Signed flow contributions to "<<e(report.at("output"))<<"</title>"
           <<"<line x1=\"500\" x2=\"500\" y1=\"12\" y2=\""<<30+flows.size()*34<<"\" stroke=\"#9aacb9\"/>";
        std::size_t i=0;
        for(const auto& f:flows) {
            const double value=f.at("contribution").get<double>(),width=largest?200*(std::abs(value)/largest):0;const auto y=24+34*i++;
            out<<"<text x=\"0\" y=\""<<y+15<<"\" font-size=\"13\">"<<e(f.at("flow"))<<"</text>"
               <<"<rect x=\""<<(value<0?500-width:500)<<"\" y=\""<<y<<"\" width=\""<<width<<"\" height=\"20\" fill=\""<<(value<0?"#b35c35":"#17766b")<<"\"/>"
               <<"<text x=\"740\" y=\""<<y+15<<"\" font-size=\"13\">"<<number(f.at("contribution"))<<"</text>";
        }
        out<<"</svg></div><p>Flow total: <strong>"<<number(accounting.at("flow_total"))<<"</strong>. Arithmetic residual: <code>"<<number(accounting.at("rounding_residual"))
           <<"</code>.</p><p class=\"muted\">The residual is retained explicitly because stock updates and integrated-flow subtraction can round differently.</p></section>";
    }else out<<"<section class=\"card\"><h2>Dependency explanation</h2><p>This expression or delay output has no additive flow decomposition. Its dependencies appear below.</p></section>";
    out<<"<section class=\"card\"><h2>Inputs and dependencies</h2><p class=\"muted\">State values are snapshots at the window boundaries. Expression references include inactive branches; they are not estimated causal effects.</p>"
       <<"<div class=\"scroll\"><table><thead><tr><th>Name</th><th>Kind</th><th>Start / parameter value</th><th>End</th></tr></thead><tbody>";
    for(const auto& node:report.at("dependencies").at("nodes")) {
        out<<"<tr><td><code>"<<e(node.at("id"))<<"</code>";
        if(node.contains("declaration") && node.at("declaration").contains("expr")) out<<"<br><code class=\"muted\">"<<e(node.at("declaration").at("expr"))<<"</code>";
        if(node.value("scenario_override",false)) out<<"<br><span class=\"muted\">Scenario override</span>";
        if(node.contains("data_sources")) for(const auto& source:node.at("data_sources")) out<<"<br><span class=\"muted\">"<<e(source.at("binding_id"))<<" / "<<e(source.at("column"))<<"</span>";
        out<<"</td><td>"<<e(node.at("kind"))<<"</td><td>";
        if(node.contains("start_value")) out<<number(node.at("start_value"));else if(node.contains("value")) out<<number(node.at("value"));else out<<"—";
        out<<"</td><td>"<<(node.contains("end_value")?number(node.at("end_value")):"—")<<"</td></tr>";
    }
    out<<"</tbody></table></div></section><section class=\"card\"><h2>Scope and provenance</h2><ul>";
    for(const auto& note:report.at("limitations")) out<<"<li>"<<e(note)<<"</li>";
    out<<"</ul><p>Model SHA-256<br><code>"<<e(report.at("model_sha256"))<<"</code></p><p>Run manifest<br><code>"<<e(report.at("manifest_id"))
       <<"</code></p><p>Artifact verification: "<<e(report.at("verification").at("artifact"))<<"</p>"
       <<"<details><summary>Complete explanation, declarations and source receipts</summary><pre>"<<escape_html(report.dump(2))<<"</pre></details></section></main></html>";
    return out.str();
}
}

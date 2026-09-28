#include "explanation.hpp"
#include "explanation_html.hpp"
#include "provenance.hpp"
#include "ankurafathom/runtime/atomic_file.hpp"
#include <bit>
#include <charconv>
#include <cmath>
#include <iostream>
#include <limits>
#include <set>

namespace ankurafathom::ir::explanation {
namespace {
using Json=nlohmann::json;
struct Captured {
    Json manifest;
    Model model;
    std::optional<runtime::Experiment> experiment;
    std::optional<InputIdentity> experiment_input;
    bool artifact_verified=false;
};
Captured capture(const std::string& path) {
    if(std::filesystem::is_directory(path)) {
        auto bundle=provenance::load_bundle(path);
        return {std::move(bundle.manifest),std::move(bundle.model),std::move(bundle.experiment),std::move(bundle.experiment_input),false};
    }
    Json manifest;bool verified=false;
    const auto extension=std::filesystem::path(path).extension().string();
    try {
        if(extension==".parquet" || extension==".arrow" || extension==".ipc") {
            const auto format=extension==".parquet"?runtime::OutputFormat::parquet:runtime::OutputFormat::arrow_ipc;
            manifest=provenance::validate_manifest(Json::parse(runtime::verify_embedded_observations(path,format).manifest_json));verified=true;
        }else if(extension==".csv") {
            manifest=provenance::read_manifest(path+".manifest.json");
            runtime::verify_observations(path,runtime::OutputFormat::csv,{manifest.dump()});verified=true;
        }else manifest=provenance::read_manifest(path);
    }catch(const Error&) {throw;}catch(const std::exception& e) {throw Error("IR_EXPLAIN_RESULT","/results",e.what());}
    if(manifest.at("manifest_version")=="0.3") throw Error("IR_EXPLAIN_SCOPE","/manifest/manifest_version","memory manifests cannot be re-executed for explanation");
    const auto& input=manifest.at("inputs");
    auto model=load_file(input.at("model").at("path").get<std::string>());
    std::optional<runtime::Experiment> experiment;std::optional<InputIdentity> identity;
    const auto seed=input.at("execution").at("seed").get<std::uint64_t>();
    if(!input.at("experiment").is_null()) {
        identity.emplace();const auto doc=provenance::read_input(input.at("experiment").at("path").get<std::string>(),*identity,"/experiment");
        experiment=load_experiment_json(doc.dump(),model,seed);
    }
    const auto actual=provenance::inputs(model,experiment,identity,seed,input.at("execution").at("threads").get<std::size_t>());
    provenance::require_equal(actual,input,"/inputs");
    return {std::move(manifest),std::move(model),std::move(experiment),std::move(identity),verified};
}
std::vector<runtime::Observation> observations(const std::vector<Row>& rows) {
    std::vector<runtime::Observation> result;result.reserve(rows.size());
    for(const auto& row:rows) result.push_back({row.time,row.output_id,row.value});
    return result;
}
std::size_t grid_index(double value,const Model& model) {
    const double offset=(value-model.start)/model.dt;
    const double nearest=std::round(offset);
    const auto steps=static_cast<std::size_t>(std::llround(model.horizon/model.dt));
    if(!std::isfinite(offset) || nearest<0 || nearest>static_cast<double>(steps) ||
        std::abs(offset-nearest)>32*std::numeric_limits<double>::epsilon()*std::max(1.,std::abs(offset)))
        throw Error("IR_EXPLAIN_TIME","/time","time must select an existing output-grid sample; interpolation is not supported");
    return static_cast<std::size_t>(nearest);
}
double finite(double v) {
    if(!std::isfinite(v)) throw Error("IR_EXPLAIN_NUMERIC","","explanation arithmetic is not representable as finite binary64");
    return v;
}
Json explain(const Captured& captured,const std::string& output,double at,std::optional<double> from,
             std::uint32_t scenario_id,std::uint32_t replication,std::size_t threads) {
    const auto& model=captured.model;
    if(model.kind!=Model::Kind::sd) throw Error("IR_EXPLAIN_SCOPE","/mode","explanation currently requires standalone SD");
    if(std::any_of(model.stocks.begin(),model.stocks.end(),[](const auto& s){return s.clip_outflows;}) ||
       std::any_of(model.flows.begin(),model.flows.end(),[](const auto& f){return f.clip_negative;}))
        throw Error("IR_EXPLAIN_SCOPE","/components","clipped flows require applied-flow instrumentation; unsupported by passive integral reconstruction");
    const auto selected=std::find_if(model.outputs.begin(),model.outputs.end(),[&](const auto& o){return o.id==output;});
    if(selected==model.outputs.end()) throw Error("IR_EXPLAIN_OUTPUT","/output","unknown output id");
    const auto end_index=grid_index(at,model);
    const auto start_index=from?grid_index(*from,model):(end_index?end_index-1:0);
    if(start_index>end_index) throw Error("IR_EXPLAIN_TIME","/from","window start exceeds end");
    const double start=model.start+start_index*model.dt,end=model.start+end_index*model.dt;
    const auto seed=captured.manifest.at("inputs").at("execution").at("seed").get<std::uint64_t>();
    const auto experiment=captured.experiment.value_or(runtime::Experiment{seed,{{0,{}}},1});
    const auto scenario=std::find_if(experiment.scenarios.begin(),experiment.scenarios.end(),[&](const auto& s){return s.id==scenario_id;});
    if(scenario==experiment.scenarios.end() || replication>=experiment.replications)
        throw Error("IR_EXPLAIN_ADDRESS","/scenario","scenario/replication is not in the captured experiment");
    const auto trajectories=runtime::run_experiment(experiment,[&](const auto& s,std::uint32_t r,std::uint64_t seed) {
        return observations(run(model,s.parameters,seed,s.id,r));
    },runtime::ExecutionOptions{threads});
    provenance::require_equal(provenance::result(trajectories),captured.manifest.at("result"),"/result");
    const auto reference=std::find_if(trajectories.begin(),trajectories.end(),[&](const auto& t){return t.scenario==scenario_id && t.replication==replication;});
    // Passive accounting stocks use the same stage states and integration method.
    // They are never read by original expressions; original rows must remain bit-identical.
    Model traced=model;std::set<std::string> ids,original_outputs;
    for(const auto& [id,value]:model.parameters) { (void)value;ids.insert(id); }
    for(const auto& s:model.stocks) ids.insert(s.id);
    for(const auto& a:model.auxiliaries) ids.insert(a.id);
    for(const auto& d:model.delays) ids.insert(d.id);
    for(const auto& f:model.flows) ids.insert(f.id);
    for(const auto& t:model.tables) ids.insert(t.id);
    for(const auto& s:model.series_data) ids.insert(s.id);
    for(const auto& o:model.outputs) {ids.insert(o.id);original_outputs.insert(o.id);}
    const auto unique=[&](std::string name) {while(ids.contains(name)) name+="_";ids.insert(name);return name;};
    std::map<std::string,std::string> values,integrals;
    for(const auto& s:model.stocks) {const auto id=unique("__explain_stock_"+s.id);values[s.id]=id;traced.outputs.push_back({id,s.id,false,""});}
    for(const auto& a:model.auxiliaries) {const auto id=unique("__explain_aux_"+a.id);values[a.id]=id;traced.outputs.push_back({id,"",false,"",Expression(a.id)});}
    for(const auto& d:model.delays) {const auto id=unique("__explain_delay_"+d.id);values[d.id]=id;traced.outputs.push_back({id,d.id,true,""});}
    for(const auto& f:model.flows) {
        const auto stock=unique("__explain_integral_"+f.id),flow=unique("__explain_rate_"+f.id),out=unique("__explain_amount_"+f.id);
        traced.stocks.push_back({stock,0,false,f.unit+"*"+model.time_unit});
        traced.flows.push_back({flow,f.component_index,std::nullopt,stock,f.expression,f.unit,false,false});
        traced.outputs.push_back({out,stock,false,""});integrals[f.id]=out;
    }
    const auto rows=run(traced,scenario->parameters,seed,scenario_id,replication);
    std::map<double,std::map<std::string,double>> grid;
    std::size_t index=0;
    for(const auto& row:rows) {
        if(original_outputs.contains(row.output_id)) {
            if(index>=reference->observations.size()) throw Error("IR_EXPLAIN_PARITY","/result","instrumented result length differs");
            const auto& r=reference->observations[index++];
            if(r.output_id!=row.output_id || std::bit_cast<std::uint64_t>(r.time)!=std::bit_cast<std::uint64_t>(row.time) ||
               std::bit_cast<std::uint64_t>(r.value)!=std::bit_cast<std::uint64_t>(row.value))
                throw Error("IR_EXPLAIN_PARITY","/result","passive instrumentation changed original observations");
        }
        if(row.time==start || row.time==end) grid[row.time].emplace(row.output_id,row.value);
    }
    if(index!=reference->observations.size()) throw Error("IR_EXPLAIN_PARITY","/result","instrumented result length differs");
    const auto& before=grid.at(start);const auto& after=grid.at(end);
    auto parameters=model.parameters;for(const auto& [name,value]:scenario->parameters) parameters[name]=value;
    const auto document=Json::parse(model.input.canonical_json);
    std::map<std::string,Json> nodes;
    std::map<std::string,std::vector<std::pair<std::string,std::string>>> dependencies;
    const auto expr_links=[&](const std::string& id,const Expression& expr) {
        for(const auto& symbol:expr.symbols()) dependencies[id].push_back({symbol,"expression-symbol"});
        for(const auto& f:expr.functions()) if(!Expression::builtin(f)) dependencies[id].push_back({f,"function"});
    };
    nodes["t"]={{"id","t"},{"kind","time"},{"unit",model.time_unit},{"start_value",start},{"end_value",end}};
    nodes["dt"]={{"id","dt"},{"kind","time-step"},{"unit",model.time_unit},{"value",model.dt}};
    for(const auto& [id,value]:parameters) {
        Json sources=Json::array();
        for(const auto& binding:model.parameter_data) if(binding.columns.contains(id)) {
            Json key;std::visit([&](const auto& v){key=v;},binding.source_key);
            sources.push_back({{"binding_id",binding.id},{"path",binding.source},{"column",binding.columns.at(id)},
                {"source_key",key},{"file_sha256",binding.file_hash},{"canonical_sha256",binding.canonical_hash}});
        }
        nodes[id]={{"id",id},{"kind","parameter"},{"value",value},{"scenario_override",scenario->parameters.contains(id)},
            {"dimension",model.parameter_units.at(id).powers},{"data_sources",sources}};
    }
    const auto state_node=[&](const std::string& id,const char* kind,const std::string& unit) {
        nodes[id]={{"id",id},{"kind",kind},{"unit",unit},{"start_value",before.at(values.at(id))},{"end_value",after.at(values.at(id))}};
    };
    for(const auto& s:model.stocks) state_node(s.id,"stock",s.unit);
    for(const auto& a:model.auxiliaries) {
        state_node(a.id,"auxiliary",a.unit);nodes[a.id]["declaration"]=document.at("components").at(a.component_index);expr_links(a.id,a.expression);
    }
    for(const auto& d:model.delays) {
        state_node(d.id,"delay",d.unit);nodes[d.id]["declaration"]=document.at("components").at(d.component_index);
        nodes[d.id]["history_attribution"]="not_assessed";expr_links(d.id,d.input);
        if(const auto* expr=std::get_if<Expression>(&d.duration)) expr_links(d.id,*expr);
    }
    for(const auto& f:model.flows) {
        nodes[f.id]={{"id",f.id},{"kind","flow"},{"unit",f.unit},
            {"integrated_amount",finite(after.at(integrals.at(f.id))-before.at(integrals.at(f.id)))},
            {"amount_dimension",Dimension::parse(f.unit).multiplied(Dimension::parse(model.time_unit)).powers},
            {"declaration",document.at("components").at(f.component_index)}};
        expr_links(f.id,f.expression);
        if(f.source) dependencies[*f.source].push_back({f.id,"outflow"});
        if(f.destination) dependencies[*f.destination].push_back({f.id,"inflow"});
    }
    for(const auto& table:model.tables) {
        const auto decl=std::find_if(document.at("components").begin(),document.at("components").end(),[&](const auto& c){return c.at("id")==table.id;});
        nodes[table.id]={{"id",table.id},{"kind","table"},{"declaration",*decl}};
    }
    for(const auto& binding:model.series_data) {
        const auto& data=captured.manifest.at("inputs").at("data");
        const auto receipt=std::find_if(data.begin(),data.end(),[&](const auto& d){return d.at("binding").at("id")==binding.id;});
        nodes[binding.id]={{"id",binding.id},{"kind","exogenous-series"},{"receipt",*receipt},
            {"value_at_window_start",binding.series.value_at(start)},{"value_at_window_end",binding.series.value_at(end)}};
    }
    // Output IDs occupy a distinct IR namespace; never alias a same-named stock.
    const std::string root="output:"+output;
    nodes[root]={{"id",root},{"kind","output"},{"output_id",output},{"start_value",before.at(output)},{"end_value",after.at(output)}};
    nodes[root]["declaration"]=document.at("outputs").at(static_cast<std::size_t>(std::distance(model.outputs.begin(),selected)));
    if(selected->expression) expr_links(root,*selected->expression);else dependencies[root].push_back({selected->source,"observes"});
    std::set<std::string> visited;std::vector<std::string> pending{root};Json graph_nodes=Json::array(),edges=Json::array();
    for(std::size_t i=0;i<pending.size();++i) {
        const auto id=pending[i];if(!visited.insert(id).second) continue;
        graph_nodes.push_back(nodes.at(id));
        for(const auto& [dependency,role]:dependencies[id]) {
            edges.push_back({{"from",dependency},{"to",id},{"role",role}});pending.push_back(dependency);
        }
    }
    Json accounting=nullptr;
    if(!selected->expression && !selected->delay) {
        Json contributions=Json::array();double sum=0;
        for(const auto& f:model.flows) {
            const int sign=static_cast<int>(f.destination && *f.destination==selected->source)-static_cast<int>(f.source && *f.source==selected->source);
            if((f.source && *f.source==selected->source) || (f.destination && *f.destination==selected->source)) {
                const double amount=nodes.at(f.id).at("integrated_amount").get<double>();
                const double contribution=finite(sign*amount);sum=finite(sum+contribution);
                contributions.push_back({{"flow",f.id},{"endpoint_sign",sign},{"integrated_amount",amount},{"contribution",contribution}});
            }
        }
        const double delta=finite(after.at(output)-before.at(output));
        accounting={{"stock",selected->source},{"unit",nodes.at(selected->source).at("unit")},{"change",delta},{"flows",contributions},{"flow_total",sum},{"rounding_residual",finite(delta-sum)}};
    }
    return {{"explanation_version","0.1"},{"scope","standalone-sd-unclipped-integrator-accounting"},{"model_name",model.name},
        {"manifest_id",captured.manifest.at("id")},{"model_sha256",model.input.canonical_hash},
        {"result_sha256",captured.manifest.at("result").at("sha256")},
        {"scenario",scenario_id},{"replication",replication},{"output",output},
        {"window",{{"from",start},{"at",end},{"steps",end_index-start_index},{"time_unit",model.time_unit}}},
        {"verification",{{"inputs","exact-match"},{"result","exact-match"},{"passive_observations","bit-identical"},
            {"artifact",captured.artifact_verified?"verified":"not-supplied; manifest numeric identity re-executed"}}},
        {"method",model.integrator==sd::Integrator::rk4?"rk4-passive-flow-integrals":"euler-passive-flow-integrals"},
        {"start_value",before.at(output)},{"value",after.at(output)},{"accounting",accounting},
        {"dependencies",{{"nodes",graph_nodes},{"edges",edges}}},
        {"limitations",Json::array({"Reconstructed by verified re-execution; not a stored event trace.",
            "Dependency values are boundary snapshots, not individual RK4 stage values.",
            "Syntactic dependencies include inactive conditional branches; edges are not causal effect estimates.",
            "Passive integral subtraction and stock arithmetic may differ by the reported rounding residual.",
            "Expression/delay outputs have dependency values but no additive change attribution.",
            "Delay history internals, clipped flows and hybrid bridges are not attributed."})}};
}
}
int cli(int argc,char** argv) {
    std::string output,destination,format="json";std::optional<double> at,from;
    std::uint32_t scenario=0,replication=0;std::size_t threads=1;
    std::set<std::string> seen;
    for(int i=3;i<argc;i+=2) {
        const std::string option=argv[i];
        if(!seen.insert(option).second || i+1>=argc || argv[i+1][0]=='\0' || std::string_view(argv[i+1]).starts_with("--"))
            throw Error("IR_USAGE","","explain option requires one value and cannot repeat");
        const std::string value=argv[i+1];
        if(option=="--output") output=value;
        else if(option=="--out") destination=value;
        else if(option=="--format") {if(value!="json" && value!="html") throw Error("IR_USAGE","","explain format must be json or html");format=value;}
        else if(option=="--at" || option=="--from") {
            double v=0;const auto [end,error]=std::from_chars(value.data(),value.data()+value.size(),v);
            if(error!=std::errc{} || end!=value.data()+value.size() || !std::isfinite(v)) throw Error("IR_USAGE","","time must be finite numeric text");
            if(option=="--at") at=v;else from=v;
        }else if(option=="--scenario" || option=="--replication" || option=="--threads") {
            std::uint64_t v=0;const auto [end,error]=std::from_chars(value.data(),value.data()+value.size(),v);
            if(error!=std::errc{} || end!=value.data()+value.size() || v>65535 || (option=="--threads" && (v<1 || v>256)))
                throw Error("IR_USAGE","","invalid unsigned address or thread count");
            if(option=="--scenario") scenario=v;else if(option=="--replication") replication=v;else threads=v;
        }else throw Error("IR_USAGE","","unknown explain option: "+option);
    }
    if(output.empty() || !at) throw Error("IR_USAGE","","explain requires --output ID and --at TIME");
    if(!destination.empty() && (std::filesystem::exists(std::filesystem::symlink_status(destination)) || provenance::same_destination(destination,argv[2])))
        throw Error("IR_USAGE","","explanation destination must be new and cannot overwrite its input");
    const auto captured=capture(argv[2]);
    if(std::filesystem::is_directory(argv[2]) && !destination.empty() && provenance::within_bundle(destination,std::filesystem::canonical(argv[2])))
        throw Error("IR_USAGE","","explanation cannot modify its input bundle");
    const auto result=explain(captured,output,*at,from,scenario,replication,threads);
    const auto rendered=format=="html"?explanation_html(result):result.dump(2)+"\n";
    if(!destination.empty()) runtime::write_text_atomically(destination,rendered);
    std::cout<<(format=="html" && destination.empty()?rendered:result.dump())<<'\n';if(!std::cout) throw Error("IR_IO","","cannot write explanation");
    return 0;
}
}

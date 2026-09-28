#include "validation.hpp"
#include "provenance.hpp"
#include "ankurafathom/runtime/atomic_file.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <iostream>
#include <set>

namespace ankurafathom::ir::validation {
using Json=nlohmann::json;
namespace {
std::string text(const Json& obj,const char* key,const std::string& p) {
    if(!obj.contains(key)) throw Error("IR_MISSING",p+"/"+key,"required field is missing");
    if(!obj.at(key).is_string() || obj.at(key).get<std::string>().empty())
        throw Error("IR_TYPE",p+"/"+key,"expected a nonempty string");
    return obj.at(key).get<std::string>();
}
double number(const Json& obj,const char* key,const std::string& p) {
    if(!obj.at(key).is_number() || !std::isfinite(obj.at(key).get<double>()))
        throw Error("IR_TYPE",p+"/"+key,"expected a finite number");
    return obj.at(key).get<double>();
}
std::string token(const std::string& s) {
    std::string r;for(char c:s) r+=c=='~'?"~0":c=='/'?"~1":std::string(1,c);return r;
}
struct RunFailure : std::runtime_error {
    std::string code,pointer;std::uint32_t scenario,replication;
    RunFailure(std::string code,std::string pointer,std::string message,std::uint32_t s,std::uint32_t r)
        :std::runtime_error(message),code(std::move(code)),pointer(std::move(pointer)),scenario(s),replication(r) {}
};
bool close(double a,double b,const ModelCheck& c) {
    // Scale before arithmetic: on macOS/arm64 long double has f64 range too.
    const double magnitude=std::max(std::abs(a),std::abs(b));
    const double scale=std::max(magnitude,c.absolute_tolerance);
    if(scale==0) return true;
    return std::abs(a/scale-b/scale)<=c.absolute_tolerance/scale+
        c.relative_tolerance*(magnitude/scale);
}
bool less_equal(double a,double b,const ModelCheck& c) { return a<=b || close(a,b,c); }
}

void parse_checks(const Json& doc,Model& model) {
    if(!doc.contains("checks")) return;
    if(model.kind!=Model::Kind::sd) throw Error("IR_CHECK_SCOPE","/checks","declared checks currently require standalone SD");
    const auto& checks=doc.at("checks");
    if(!checks.is_array() || checks.empty() || checks.size()>1024)
        throw Error("IR_TYPE","/checks","checks must contain 1..1024 declarations");
    auto units=model.parameter_units;
    units.emplace("t",Dimension::parse(model.time_unit));units.emplace("dt",Dimension::parse(model.time_unit));
    std::map<std::string,Dimension> stock_units;
    for(const auto& s:model.stocks) { units.emplace(s.id,Dimension::parse(s.unit));stock_units.emplace(s.id,Dimension::parse(s.unit)); }
    for(const auto& a:model.auxiliaries) units.emplace(a.id,Dimension::parse(a.unit));
    for(const auto& d:model.delays) units.emplace(d.id,Dimension::parse(d.unit));
    std::map<std::string,std::pair<Dimension,Dimension>> functions;
    for(const auto& t:model.tables) functions.emplace(t.id,std::make_pair(t.input_unit,t.output_unit));
    for(const auto& s:model.series_data) functions.emplace(s.id,std::make_pair(s.input_unit,s.output_unit));
    const auto expression=[&](const std::string& source,const std::string& p) {
        std::optional<Expression> expr;
        try { expr.emplace(source); }catch(const std::exception& e) { throw Error("IR_EXPR",p,e.what()); }
        for(const auto& symbol:expr->symbols()) if(!units.contains(symbol)) throw Error("IR_SYMBOL",p,"unknown check symbol: "+symbol);
        for(const auto& f:expr->functions()) {
            if(!Expression::builtin(f) && !functions.contains(f)) throw Error("IR_SYMBOL",p,"unknown check function: "+f);
            if(Expression::tick_builtin(f) && model.integrator!=sd::Integrator::euler)
                throw Error("IR_INTEGRATOR",p,"tick input functions require Euler");
        }
        try { (void)expr->infer_unit(units,functions); }catch(const std::exception& e) { throw Error("IR_UNIT",p,e.what()); }
        return *expr;
    };
    std::set<std::string> ids;
    for(std::size_t i=0;i<checks.size();++i) {
        const auto& item=checks[i];const auto p="/checks/"+std::to_string(i);
        if(!item.is_object()) throw Error("IR_TYPE",p,"expected a check object");
        ModelCheck c;c.index=i;c.kind=text(item,"kind",p);
        if(c.kind!="assert" && c.kind!="bounds" && c.kind!="monotone" && c.kind!="conserved")
            throw Error("IR_CHECK",p+"/kind","expected assert, bounds, monotone or conserved");
        std::set<std::string> allowed{"kind","id","absolute_tolerance","relative_tolerance"};
        if(c.kind=="assert") { allowed.insert("expr");allowed.insert("when"); }
        if(c.kind=="bounds") { allowed.insert("output");allowed.insert("min");allowed.insert("max"); }
        if(c.kind=="monotone") { allowed.insert("output");allowed.insert("direction"); }
        if(c.kind=="conserved") allowed.insert("stocks");
        for(auto it=item.begin();it!=item.end();++it) if(!allowed.contains(it.key()))
            throw Error("IR_FIELD",p+"/"+token(it.key()),"unknown check field");
        c.id=item.contains("id")?text(item,"id",p):"check_"+std::to_string(i);
        if(!ids.insert(c.id).second) throw Error("IR_ID",p+"/id","duplicate check id");
        for(const auto* key:{"absolute_tolerance","relative_tolerance"}) if(item.contains(key)) {
            const auto v=number(item,key,p);
            if(v<0) throw Error("IR_CHECK",p+"/"+key,"tolerance must be nonnegative");
            if(std::string_view(key)=="absolute_tolerance") c.absolute_tolerance=v;else c.relative_tolerance=v;
        }
        if(c.kind=="assert") {
            if(item.contains("when")) c.when=text(item,"when",p);
            if(c.when!="always" && c.when!="end") throw Error("IR_CHECK",p+"/when","expected always or end");
            const auto source=text(item,"expr",p);
            const auto pos=source.find_first_of("<>=!");
            if(pos==std::string::npos || pos+1>=source.size() || source[pos+1]!='=' || source[pos]=='!')
                throw Error("IR_CHECK",p+"/expr","assertion requires one <=, >= or == comparison");
            c.comparison=source.substr(pos,2);
            c.left=expression(source.substr(0,pos),p+"/expr");c.right=expression(source.substr(pos+2),p+"/expr");
            if(!(c.left->infer_unit(units,functions)==c.right->infer_unit(units,functions)))
                throw Error("IR_UNIT",p+"/expr","assertion sides must have equal units; normalize dimensional values explicitly");
        } else if(c.kind=="conserved") {
            if(!item.contains("stocks")) throw Error("IR_MISSING",p+"/stocks","stocks are required");
            const auto& stocks=item.at("stocks");
            if(!stocks.is_array() || stocks.empty() || stocks.size()>1024) throw Error("IR_TYPE",p+"/stocks","expected 1..1024 stocks");
            std::set<std::string> seen;
            for(std::size_t j=0;j<stocks.size();++j) {
                const auto location=p+"/stocks/"+std::to_string(j);
                if(!stocks[j].is_string()) throw Error("IR_TYPE",location,"expected stock id");
                const auto name=stocks[j].get<std::string>();
                if(!stock_units.contains(name) || !seen.insert(name).second) throw Error("IR_REF",location,"stock must exist and occur once");
                if(!c.stocks.empty() && !(stock_units.at(name)==stock_units.at(c.stocks.front())))
                    throw Error("IR_UNIT",location,"conserved stocks must share units");
                c.stocks.push_back(name);
            }
        } else {
            c.output=text(item,"output",p);
            if(std::none_of(model.outputs.begin(),model.outputs.end(),[&](const Output& o){return o.id==c.output;}))
                throw Error("IR_REF",p+"/output","unknown output");
            if(c.kind=="bounds") {
                if(!item.contains("min") && !item.contains("max")) throw Error("IR_CHECK",p,"bounds needs min or max");
                if(item.contains("min")) c.minimum=number(item,"min",p);
                if(item.contains("max")) c.maximum=number(item,"max",p);
                if(c.minimum && c.maximum && *c.minimum>*c.maximum) throw Error("IR_CHECK",p,"min exceeds max");
            } else {
                c.direction=text(item,"direction",p);
                if(c.direction!="increasing" && c.direction!="decreasing") throw Error("IR_CHECK",p+"/direction","expected increasing or decreasing (nonstrict)");
            }
        }
        model.checks.push_back(std::move(c));
    }
}

Json check(const Model& model,const runtime::Experiment& experiment,std::size_t threads) {
    if(model.kind!=Model::Kind::sd) throw Error("IR_CHECK_SCOPE","/mode","behavioral checking currently requires standalone SD");
    Model observed=model;
    std::set<std::string> original_ids,ids;
    for(const auto& o:model.outputs) original_ids.insert(o.id);
    ids=original_ids;
    std::vector<std::vector<std::string>> signals, symbols;
    const auto add=[&](Output o) {
        while(ids.contains(o.id)) o.id+="_";
        ids.insert(o.id);const auto id=o.id;observed.outputs.push_back(std::move(o));return id;
    };
    for(const auto& c:model.checks) {
        const auto prefix="__fathom_check_"+std::to_string(c.index);
        std::vector<std::string> names, references;
        if(c.kind=="assert") {
            auto needed=c.left->symbols();needed.insert(c.right->symbols().begin(),c.right->symbols().end());
            for(const auto& symbol:needed) {
                if(model.parameters.contains(symbol) || symbol=="t" || symbol=="dt") continue;
                references.push_back(symbol);
                const bool stock=std::any_of(model.stocks.begin(),model.stocks.end(),[&](const auto& v){return v.id==symbol;});
                const bool delay=std::any_of(model.delays.begin(),model.delays.end(),[&](const auto& v){return v.id==symbol;});
                names.push_back(add(Output{prefix+"_"+symbol,stock||delay?symbol:"",delay,"",
                    stock||delay?std::nullopt:std::optional{Expression(symbol)}}));
            }
        } else if(c.kind=="conserved") {
            for(const auto& s:c.stocks) names.push_back(add(Output{prefix+"_"+s,s,false,""}));
        } else names.push_back(c.output);
        signals.push_back(std::move(names));symbols.push_back(std::move(references));
    }
    Json report={{"validation_version","0.1"},{"scope","standalone-sd-declared-checks"},
        {"verdict",model.checks.empty()?"warn":"pass"},
        {"sampling",{{"policy","output-grid-including-initial-and-final"},{"start",model.start},{"dt",model.dt},{"horizon",model.horizon}}},
        {"coverage",{{"structural","loader-symbol-unit-and-reference-validation"},
          {"not_assessed",Json::array({"parameter-extremes","step-halving","monte-carlo-standard-errors","fit","sensitivity","hybrid-behavior","reachability-and-feedback-inventory"})}}},
        {"checks",Json::array()}, {"result",nullptr}};
    report["checks"].push_back({{"id","structural"},{"code","CHECK_STRUCTURE"},{"pointer",""},{"verdict","pass"}});
    std::vector<runtime::Trajectory> trajectories;
    try {
        trajectories=runtime::run_experiment(experiment,[&](const runtime::Scenario& scenario,std::uint32_t replication,std::uint64_t seed) {
            try {
                const auto rows=run(observed,scenario.parameters,seed,scenario.id,replication);
                std::vector<runtime::Observation> observations;observations.reserve(rows.size());
                for(const auto& row:rows) observations.push_back({row.time,row.output_id,row.value});
                return observations;
            }catch(const Error& e) {
                auto pointer=e.pointer;auto code=e.code;
                if(code=="IR_OUTPUT_RUNTIME") {
                    for(std::size_t i=model.outputs.size();i<observed.outputs.size();++i) if(pointer=="/outputs/"+std::to_string(i)+"/expr") {
                        for(std::size_t j=0;j<signals.size();++j)
                            if(std::find(signals[j].begin(),signals[j].end(),observed.outputs[i].id)!=signals[j].end())
                                pointer="/checks/"+std::to_string(j)+"/expr";
                        code="CHECK_EVALUATION";break;
                    }
                }
                throw RunFailure(code,pointer,e.what(),scenario.id,replication);
            }catch(const std::exception& e) {throw RunFailure("IR_RUNTIME","",e.what(),scenario.id,replication);}
        },runtime::ExecutionOptions{threads});
    }catch(const RunFailure& e) {
        report["verdict"]="fail";
        report["checks"].push_back({{"id","execution"},{"code",e.code},{"pointer",e.pointer},{"verdict","fail"},
            {"message",e.what()},{"scenario",e.scenario},{"replication",e.replication}});
        report["coverage"]["behavioral"]="incomplete-runtime-failure";
        return report;
    }
    report["checks"].push_back({{"id","execution"},{"code","CHECK_EXECUTION"},{"pointer",""},{"verdict","pass"},
        {"trajectories",trajectories.size()}});
    std::vector<Json> outcomes;
    for(const auto& c:model.checks) outcomes.push_back({{"id",c.id},{"kind",c.kind},
        {"code",c.kind=="assert"?"CHECK_ASSERT":c.kind=="bounds"?"CHECK_BOUNDS":c.kind=="monotone"?"CHECK_MONOTONE":"CHECK_CONSERVED"},
        {"pointer","/checks/"+std::to_string(c.index)},{"verdict","pass"},{"samples",0},{"failures",0},
        {"absolute_tolerance",c.absolute_tolerance},{"relative_tolerance",c.relative_tolerance}});
    std::map<std::string,std::function<double(double)>> functions;
    for(const auto& table:model.tables) {
        const auto* lookup=&table.lookup;functions.emplace(table.id,[lookup](double x){return lookup->evaluate(x);});
    }
    for(const auto& binding:model.series_data) {
        const auto* series=&binding.series;functions.emplace(binding.id,[series](double t){return series->value_at(t);});
    }
    for(auto& trajectory:trajectories) {
        auto parameters=model.parameters;
        const auto scenario=std::find_if(experiment.scenarios.begin(),experiment.scenarios.end(),[&](const auto& s){return s.id==trajectory.scenario;});
        for(const auto& [name,value]:scenario->parameters) parameters[name]=value;
        std::map<double,std::map<std::string,double>> grid;
        for(const auto& row:trajectory.observations) grid[row.time].emplace(row.output_id,row.value);
        for(std::size_t i=0;i<model.checks.size();++i) {
            const auto& c=model.checks[i];auto& outcome=outcomes[i];std::optional<double> previous,initial;
            for(const auto& [time,values]:grid) {
                if(c.when=="end" && time!=grid.rbegin()->first) continue;
                double actual=c.kind=="assert"?0:values.at(signals[i][0]),expected=actual;
                bool pass=true;
                const auto evaluation_error=[&](const std::string& message) {
                    outcome["samples"]=outcome["samples"].get<std::size_t>()+1;
                    outcome["failures"]=outcome["failures"].get<std::size_t>()+1;
                    outcome["verdict"]="fail";outcome["code"]="CHECK_EVALUATION";report["verdict"]="fail";
                    if(!outcome.contains("first_error")) outcome["first_error"]={{"scenario",trajectory.scenario},
                        {"replication",trajectory.replication},{"time",time},{"message",message}};
                };
                if(c.kind=="conserved") {
                    long double sum=0;for(const auto& name:signals[i]) sum+=values.at(name);
                    actual=static_cast<double>(sum);
                    if(!std::isfinite(actual)) {evaluation_error("conserved sum is not finite");continue;}
                    if(!initial) initial=actual;
                    expected=*initial;pass=close(actual,expected,c);
                } else if(c.kind=="assert") {
                    auto environment=parameters;environment["t"]=time;environment["dt"]=model.dt;
                    for(std::size_t j=0;j<symbols[i].size();++j) environment[symbols[i][j]]=values.at(signals[i][j]);
                    try {
                        actual=c.left->evaluate(environment,functions);expected=c.right->evaluate(environment,functions);
                    }catch(const std::exception& e) {
                        evaluation_error(e.what());
                        continue;
                    }
                    pass=c.comparison=="=="?close(actual,expected,c):c.comparison=="<="?less_equal(actual,expected,c):less_equal(expected,actual,c);
                } else if(c.kind=="bounds") {
                    if(c.minimum && !less_equal(*c.minimum,actual,c)) {pass=false;expected=*c.minimum;}
                    if(c.maximum && !less_equal(actual,*c.maximum,c)) {pass=false;expected=*c.maximum;}
                } else {
                    if(!previous) {previous=actual;continue;}
                    expected=*previous;pass=c.direction=="increasing"?less_equal(expected,actual,c):less_equal(actual,expected,c);previous=actual;
                }
                outcome["samples"]=outcome["samples"].get<std::size_t>()+1;
                if(!pass) {
                    outcome["failures"]=outcome["failures"].get<std::size_t>()+1;
                    outcome["verdict"]="fail";report["verdict"]="fail";
                    if(!outcome.contains("first_failure")) outcome["first_failure"]={{"scenario",trajectory.scenario},
                        {"replication",trajectory.replication},{"time",time},{"actual",actual},{"reference",expected}};
                }
            }
        }
        std::erase_if(trajectory.observations,[&](const auto& row){return !original_ids.contains(row.output_id);});
    }
    for(auto& outcome:outcomes) report["checks"].push_back(std::move(outcome));
    if(model.checks.empty()) report["checks"].push_back({{"id","declarations"},{"code","CHECK_NONE"},{"pointer","/checks"},{"verdict","warn"}});
    report["result"]=provenance::result(trajectories);
    report["coverage"]["behavioral"]="declared-checks-at-output-grid";
    return report;
}

int cli(int argc,char** argv) {
    std::string output,experiment_path;std::size_t threads=1;std::uint64_t seed=0;
    bool threads_set=false,seed_set=false;
    for(int i=3;i<argc;i+=2) {
        const std::string option=argv[i];
        if(i+1>=argc || argv[i+1][0]=='\0' || std::string_view(argv[i+1]).starts_with("--")) throw Error("IR_USAGE","","check option needs a value");
        const std::string value=argv[i+1];
        if(option=="--out" && output.empty()) output=value;
        else if(option=="--experiment" && experiment_path.empty()) experiment_path=value;
        else if((option=="--threads" && !threads_set) || (option=="--seed" && !seed_set)) {
            std::uint64_t n=0;const auto [end,error]=std::from_chars(value.data(),value.data()+value.size(),n);
            if(error!=std::errc{} || end!=value.data()+value.size()) throw Error("IR_USAGE","","option needs an unsigned decimal integer");
            if(option=="--threads") {if(n<1 || n>256) throw Error("IR_USAGE","","threads must be in [1, 256]");threads=n;threads_set=true;}
            else {seed=n;seed_set=true;}
        }else throw Error("IR_USAGE","","unknown or duplicate check option: "+option);
    }
    if(!output.empty() && std::filesystem::exists(std::filesystem::symlink_status(output)))
        throw Error("IR_USAGE","","validation report destination must be new");
    for(const auto& input:{std::string(argv[2]),experiment_path}) if(!output.empty() && !input.empty() && provenance::same_destination(output,input))
        throw Error("IR_USAGE","","validation report cannot overwrite an input");
    Json report;
    try {
        const auto model=load_file(argv[2]);
        std::optional<runtime::Experiment> experiment;std::optional<InputIdentity> identity;
        if(!experiment_path.empty()) {
            identity.emplace();const auto doc=provenance::read_input(experiment_path,*identity,"/experiment");
            experiment=load_experiment_json(doc.dump(),model,seed_set?std::optional{seed}:std::nullopt);
        }
        const runtime::Experiment effective=experiment.value_or(runtime::Experiment{seed,{{0,{}}},1});
        const auto inputs=provenance::inputs(model,experiment,identity,effective.seed,threads);
        report=check(model,effective,threads);report["inputs"]=inputs;
    }catch(const Error& e) {
        report={{"validation_version","0.1"},{"scope","standalone-sd-declared-checks"},{"verdict","fail"},
            {"checks",Json::array({{{"id","load"},{"code",e.code},{"pointer",e.pointer},{"verdict","fail"},{"message",e.what()}}})},
            {"coverage",{{"behavioral","not-run"}}},{"result",nullptr}};
    }
    if(!output.empty()) runtime::write_text_atomically(output,report.dump(2)+"\n");
    std::cout<<report.dump()<<'\n';
    if(!std::cout) throw Error("IR_IO","","cannot write validation report");
    return report.at("verdict")=="fail"?1:0;
}
}

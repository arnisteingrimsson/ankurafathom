#include "ankurafathom/ir/model.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>

using Json=nlohmann::json;
namespace ir=ankurafathom::ir;
void require(bool ok,const char* text) { if(!ok) throw std::runtime_error(text); }
double value(const std::vector<ir::Row>& rows,double time,const std::string& id) {
    for(const auto& row:rows) if(row.time==time && row.output_id==id) return row.value;
    throw std::runtime_error("missing output "+id);
}
void close(double actual,double expected) { require(std::abs(actual-expected)<2e-12*std::max(1.,std::abs(expected)),"declarative agent-stock numeric mismatch"); }
class Fixture {
public:
    explicit Fixture(std::string path):path_(std::move(path)) {}
    ~Fixture() { std::error_code ignored;std::filesystem::remove(path_,ignored); }
    ir::Model load(const Json& document)const {
        { std::ofstream out(path_);out.exceptions(std::ios::badbit|std::ios::failbit);out<<document.dump(); }
        return ir::load_file(path_);
    }
private: std::string path_;
};
template<class F> void rejects(F fn,const std::string& code={}) {
    bool caught=false;
    try { fn(); } catch(const ir::Error& error) {
        caught=code.empty() || error.code==code;
        if(!caught) throw std::runtime_error("expected "+code+", got "+error.code+" at "+error.pointer+": "+error.what());
    }
    require(caught,"invalid declarative continuous-agent model accepted or unstructured error");
}
void analytic(const ir::Model& model) {
    for(double productivity:{1.,2.}) {
        const auto rows=ir::run(model,{{"productivity",productivity}});
        require(rows.size()==50,"unexpected continuous-agent output count");
        for(int tick=0;tick<=4;++tick) {
            const double time=tick*.5,first=2*std::pow(1-.125*productivity,2*tick),second=4*std::pow(1-.0625*productivity,2*tick);
            close(value(rows,time,"pending_total"),first+second);
            close(value(rows,time,"completed_total"),6-first-second);
            close(value(rows,time,"delivered_total"),6-first-second);
            close(value(rows,time,"balance"),6);
            close(value(rows,time,"selected_average"),first);
            close(value(rows,time,"people_count"),2);
            close(value(rows,time,"minimum_work"),first);
            close(value(rows,time,"maximum_work"),second);
            close(value(rows,time,"completion_rate"),productivity*(first*.5+second*.25));
            close(value(rows,time,"weighted_total"),first+2*second);
        }
    }
    const auto first=ir::run(model),replay=ir::run(model,{},19,7,9);
    for(std::size_t i=0;i<first.size();++i)
        require(first[i].time==replay[i].time && first[i].output_id==replay[i].output_id && first[i].value==replay[i].value,"deterministic model did not replay");
    rejects([&] { (void)ir::run(model,{{"productivity",100}}); },"IR_AGENT_STOCK_SD_RUNTIME");
    rejects([&] { (void)ir::run(model,{{"unknown",1}}); },"IR_OVERRIDE");
    rejects([&] { (void)ir::run(model,{{"productivity",NAN}}); },"IR_OVERRIDE");
}
void domains(Json document,const Fixture& fixture) {
    auto changed=document;
    std::reverse(changed["components"].begin(),changed["components"].end());
    analytic(fixture.load(changed));
    changed=document;changed["components"][0]["agents"]=Json::array();
    for(const auto& row:ir::run(fixture.load(changed))) require(row.value==0,"empty population did not honor fallbacks");
    changed["components"][6].erase("empty_value");
    rejects([&] { (void)ir::run(fixture.load(changed)); },"IR_AGENT_STOCK_SD_RUNTIME");
    changed=document;changed["components"][6]["filter"]="2*enabled";
    rejects([&] { (void)ir::run(fixture.load(changed)); },"IR_AGENT_STOCK_SD_RUNTIME");
    changed=document;
    changed["parameters"].push_back({{"id","drain"},{"value",10},{"unit","hour/day"}});
    changed["components"][1]["outflow_expr"]="drain";changed["components"][1]["non_negative"]=false;
    changed["components"][2]["inflow_expr"]="drain";
    changed["components"][5]["expr"]="drain";
    const auto signed_rows=ir::run(fixture.load(changed));
    close(value(signed_rows,2,"pending_total"),-34);close(value(signed_rows,2,"delivered_total"),40);
    close(value(signed_rows,2,"balance"),6);
    // SD can observe more often than the population commits; values are held.
    changed=document;changed["components"][0]["dt"]=1.;
    const auto held=ir::run(fixture.load(changed));
    close(value(held,.5,"pending_total"),6);close(value(held,.5,"delivered_total"),1);
    close(value(held,1,"pending_total"),4);close(value(held,1,"delivered_total"),2);
    // Agent and scalar expressions bind different nominal steps deliberately.
    changed=document;changed["components"][1]["outflow_expr"]="todo/dt";changed["components"][2]["inflow_expr"]="todo/dt";
    const auto step=ir::run(fixture.load(changed));close(value(step,.5,"pending_total"),0);
}
void diagnostics(const Json& document,const Fixture& fixture) {
    std::size_t index=0;
    const auto check=[&](const std::function<void(Json&)>& mutate,const std::string& code={}) {
        ++index;auto bad=document;mutate(bad);
        try { rejects([&] { (void)fixture.load(bad); },code); }
        catch(const std::exception& e) { throw std::runtime_error("diagnostic case "+std::to_string(index)+": "+e.what()); }
    };
    check([](auto& j) { j["components"][1]["field"]="level"; });
    check([](auto& j) { j["components"][2]["field"]="todo"; });
    check([](auto& j) { j["components"][1]["population"]="missing"; });
    check([](auto& j) { j["components"][3]["population"]="missing"; });
    check([](auto& j) { j["components"][1].erase("outflow_expr"); });
    check([](auto& j) { j["components"][0]["execution"]="sync"; });
    check([](auto& j) { j["components"][0]["dt"]=0; },"IR_TIME");
    check([](auto& j) { j["components"][0]["dt"]=1e-12; },"IR_TIME");
    check([](auto& j) { j["components"].push_back(j["components"][0]); });
    check([](auto& j) { j["components"][0]["fields"][0]["name"]="productivity"; },"IR_ID");
    check([](auto& j) { j["components"][0]["agents"][0]["todo"]=true; },"IR_TYPE");
    check([](auto& j) { j["components"][0]["agents"][0]["level"]=9007199254740992.; },"IR_TYPE");
    check([](auto& j) { j["components"][0]["agents"][0]["extra"]=1; },"IR_TYPE");
    check([](auto& j) { j["components"][0]["agents"][0]["todo"]=-1; });
    check([](auto& j) { j["components"][0]["fields"][3]["unit"]="hour"; },"IR_UNIT");
    check([](auto& j) { j["components"][1]["outflow_expr"]="todo"; },"IR_UNIT");
    check([](auto& j) { j["components"][1]["outflow_expr"]="throughput"; },"IR_SYMBOL");
    check([](auto& j) { j["components"][3]["expr"]="role"; },"IR_SYMBOL");
    check([](auto& j) { j["components"][3]["expr"]="t"; },"IR_SYMBOL");
    check([](auto& j) { j["components"][3]["expr"]="STEP(todo,speed)"; },"IR_FUNCTION");
    check([](auto& j) { j["components"][3]["unit"]="dollar"; },"IR_UNIT");
    check([](auto& j) { j["components"][7]["unit"]="person"; },"IR_UNIT");
    check([](auto& j) { j["components"][7]["expr"]="todo"; },"IR_FIELD");
    check([](auto& j) { j["components"][3]["empty_value"]=0; },"IR_FIELD");
    check([](auto& j) { j["components"][6]["filter"]="todo"; },"IR_UNIT");
    check([](auto& j) { j["components"][12]["destination"]="pending"; });
    check([](auto& j) { j["components"][12]["expr"]="todo"; },"IR_SYMBOL");
    check([](auto& j) { j["components"][12]["unit"]="hour"; },"IR_UNIT");
    check([](auto& j) { j["components"][12].erase("destination"); });
    check([](auto& j) { j["components"][11]["clip_outflows"]=true; },"IR_FIELD");
    check([](auto& j) { j["outputs"][0]["unit"]="day"; },"IR_UNIT");
    check([](auto& j) { j["outputs"][0]["id"]="pending"; },"IR_ID");
    check([](auto& j) { j["links"]=Json::array(); },"IR_FIELD");
    check([](auto& j) { j["integrator"]="rk4"; },"IR_FIELD");
}
int main(int argc,char** argv) {
    try {
        if(argc!=3) throw std::invalid_argument("expected fixture and scratch paths");
        std::ifstream input(argv[1]);const auto document=Json::parse(input);Fixture fixture(argv[2]);
        auto model=fixture.load(document);
        require(model.kind==ir::Model::Kind::agent_stock_sd,"wrong model mode");
        analytic(model);domains(document,fixture);diagnostics(document,fixture);
        std::cout<<"Declarative agent stocks: recurrence, conservation, units, signed fields, filters, held inputs, overrides and diagnostics passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}

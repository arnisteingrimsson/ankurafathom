#include "ankurafathom/ir/model.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <cmath>
#include <map>
#include <string>

using Json=nlohmann::json;
namespace ir=ankurafathom::ir;

int main(int argc,char** argv) {
    try {
        if(argc!=2) throw std::runtime_error("usage: tr-live MODEL.json");
        const auto model=ir::load_file(argv[1]);
        if(model.kind!=ir::Model::Kind::sd || model.dt!=1 || model.start!=0 || model.horizon!=60 || model.integrator!=ankurafathom::sd::Integrator::euler)
            throw std::runtime_error("T&R live runner requires the 60-month Euler model");
        for(const auto& c:model.checks) if(c.kind!="assert" || c.when!="always")
            throw std::runtime_error("T&R live checks require always assertions");
        std::string line;
        if(!std::getline(std::cin,line)) return 0;
        const auto config=Json::parse(line);
        if(!config.is_object() || config.size()!=1 || !config.contains("parameters"))
            throw std::runtime_error("expected parameters configuration");
        const auto parameters=config.at("parameters").get<std::map<std::string,double>>();
        std::map<std::string,std::function<double(double)>> functions;
        for(const auto& t:model.tables) functions.emplace(t.id,[&lookup=t.lookup](double x){return lookup.evaluate(x);});
        for(const auto& s:model.series_data) functions.emplace(s.id,[&series=s.series](double t){return series.value_at(t);});
        (void)ir::run_observed_sd(model,parameters,[&](double time,const auto& values,std::span<const ir::Row> rows) {
            Json outputs=Json::object(),checks=Json::array();bool passed=true;
            for(const auto& row:rows) outputs[row.output_id]=row.value;
            for(const auto& check:model.checks) {
                const double left=check.left->evaluate(values,functions),right=check.right->evaluate(values,functions);
                const double magnitude=std::max(std::abs(left),std::abs(right));
                const double scale=std::max(magnitude,check.absolute_tolerance);
                const bool close=scale==0 || std::abs(left/scale-right/scale)<=check.absolute_tolerance/scale+check.relative_tolerance*(magnitude/scale);
                const bool okay=check.comparison=="=="?close:check.comparison=="<="?(left<=right||close):(left>=right||close);
                passed=passed&&okay;
                checks.push_back({{"id",check.id},{"left",left},{"right",right},{"comparison",check.comparison},
                    {"absolute_gap",std::abs(left-right)},{"absolute_tolerance",check.absolute_tolerance},
                    {"relative_tolerance",check.relative_tolerance},{"passed",okay}});
            }
            const bool complete=time>=model.horizon;
            std::cout<<Json({{"month",time},{"outputs",outputs},{"checks",checks},{"checks_passed",passed},{"complete",complete}}).dump()<<'\n'<<std::flush;
            if(!passed || complete) return false;
            // No future derivative or state transition is computed while this read blocks.
            if(!std::getline(std::cin,line)) return false;
            const auto command=Json::parse(line);
            if(command!=Json{{"command","step"}}) throw std::runtime_error("expected step command");
            return true;
        });
        return 0;
    } catch(const std::exception& e) {
        std::cout<<Json({{"error",e.what()}}).dump()<<'\n'<<std::flush;
        return 1;
    }
}

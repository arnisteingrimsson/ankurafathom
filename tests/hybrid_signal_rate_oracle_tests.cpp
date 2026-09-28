#include "ankurafathom/hybrid/signal_rate_source.hpp"
#include "ankurafathom/hybrid/signal_select.hpp"
#include "ankurafathom/hybrid/publishing_signal_sd.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <fstream>
#include <iostream>

struct Job {};
using Token=ankurafathom::des::EntityToken<Job>;
using Scalar=ankurafathom::hybrid::ScalarPublication;
using Pulse=ankurafathom::hybrid::StockPulse;
using M=std::variant<Token,Scalar,Pulse>;
using Source=ankurafathom::hybrid::SignalRateSource<Job,M>;
using Select=ankurafathom::hybrid::SignalSelect<Job,M>;
using SD=ankurafathom::hybrid::SignalSD<M>;
using Publisher=ankurafathom::hybrid::PublishingSignalSD<M>;
using json=nlohmann::json;
int main(int argc,char** argv) {
    try {
        if(argc!=3) throw std::invalid_argument("expected plan/output paths");
        std::ifstream in(argv[1]);const auto plan=json::parse(in);
        std::ofstream out(argv[2]);out.exceptions(std::ios::badbit|std::ios::failbit);std::size_t paths=0;
        for(const auto& c:plan.at("cases")) {
            const auto first=c.at("replication_start").get<std::uint32_t>();
            for(std::uint32_t replication=first;replication<first+c.at("replications").get<std::uint32_t>();++replication)
            for(const auto& order:plan.at("orders")) {
                ankurafathom::devs::Simulator<M> sim;std::array<std::size_t,3> ids{};
                for(int role:order) {
                    if(role==0) {
                        SD core({{"rate",c.at("initial")}},{{SD::boundary,0,[slope=c.at("slope").get<double>()](const auto&,const auto&,double) { return slope; }}},
                            {0},c.at("dt"),{"events"});
                        ids[role]=sim.add(std::make_unique<Publisher>(std::move(core),[](const auto& state,const auto&,double) {
                            return std::vector<double>{state[0],state[0]/(state[0]+1)};
                        }));
                    }
                    if(role==1) {
                        Source::Store store(17,{{"arrival",ankurafathom::des::FieldKind::real},{"rate",ankurafathom::des::FieldKind::real},
                            {"probability",ankurafathom::des::FieldKind::real},{"identity",ankurafathom::des::FieldKind::integer},
                            {"group",ankurafathom::des::FieldKind::boolean},{"label",ankurafathom::des::FieldKind::string}},c.at("first_id"));
                        Source::Draws draws{c.at("seed"),c.at("scenario"),replication,c.at("source_stream")};
                        ids[role]=sim.add(std::make_unique<Source>(store,std::vector<double>{0,0},[](const auto& values,double) { return values[0]; },
                            [label=c.at("id").get<std::string>()](auto ref,const auto& v,double time) {
                                return Source::Record{time,v[0],v[1],static_cast<std::int64_t>(ref.id),bool(ref.id%2),label};
                            },draws,c.at("limit").get<std::size_t>()));
                    }
                    if(role==2) {
                        Select::Draws draws{c.at("seed"),c.at("scenario"),replication,c.at("routing_stream")};
                        ids[role]=sim.add(std::make_unique<Select>(17,std::vector<double>{0,0},[](const auto& v,double) {
                            return std::vector<double>{v[1],(1-v[1])/4,(1-v[1])*3/4};
                        },draws));
                    }
                }
                sim.connect(ids[0],1,ids[1],0);sim.connect(ids[0],1,ids[2],Select::signal_port);sim.connect(ids[1],1,ids[2],0);
                std::uint64_t revision=0;
                for(const auto& p:c.at("pulses")) sim.inject(p[0],ids[0],SD::pulse_port,M{Pulse{p[0],"events",revision++,{p[1]}}});
                json arrivals=json::array();
                for(const auto& step:sim.run_until_transactional(plan.at("horizon"))) for(const auto& emission:step.emissions)
                    if(emission.source==ids[2]) {
                        const auto& token=std::get<Token>(emission.value);const auto& r=token.snapshot->record;
                        if(token.snapshot->time!=step.time || std::get<double>(r[0])!=step.time || std::get<std::int64_t>(r[3])!=static_cast<std::int64_t>(token.entity.id))
                            throw std::logic_error("typed source snapshot/identity mismatch");
                        arrivals.push_back(json{{"id",token.entity.id},{"time",step.time},{"branch",emission.port},
                            {"rate",std::get<double>(r[1])},{"probability",std::get<double>(r[2])},
                            {"group",std::get<bool>(r[4])},{"label",std::get<std::string>(r[5])}});
                    }
                out<<json{{"case",c.at("id")},{"replication",replication},{"order",order},{"arrivals",arrivals},
                    {"generated",dynamic_cast<const Source&>(sim.model(ids[1])).generated_count()},
                    {"routed",dynamic_cast<const Select&>(sim.model(ids[2])).received_count()}}.dump()<<'\n';++paths;
            }
        }
        std::cout<<paths<<" scalar-driven arrival/routing trajectories generated\n";
    }catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}

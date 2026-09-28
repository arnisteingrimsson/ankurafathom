#include "ankurafathom/des/reference_resource.hpp"
#include "ankurafathom/des/reference_delay.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iomanip>
#include <iostream>

namespace {
using namespace ankurafathom;
using Json=nlohmann::json;
struct Work {};
using Token=des::EntityToken<Work>;
using Message=des::ResourceProcessMessage<Work>;
using Seize=des::ReferenceSeize<Work>;
using Release=des::ReferenceRelease<Work>;
using Pool=des::TypedResourcePool<Message>;
using Delay=des::ReferenceDelay<Work,Message>;
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
void close(double a,double b,const char* message) { require(std::abs(a-b)<=2e-9*std::max({1.,std::abs(a),std::abs(b)}),message); }

void run(const char* input_path,const char* output_path) {
    std::ifstream input(input_path); Json plan; input>>plan;
    require(plan.at("version")==1,"unknown resource trace plan");
    Json report={{"version",1},{"cases",Json::array()}};
    for(const auto& spec:plan.at("cases")) {
        if(!spec.at("queue_capacity").is_null()) continue; // Resource pool has no finite waiting-room policy.
        const auto discipline=spec.at("discipline").get<std::string>();
        const auto capacity=spec.at("capacity").get<std::size_t>();
        devs::Simulator<Message> sim;
        auto seize=std::make_unique<Seize>(7,11,1,1);
        auto* s=seize.get();
        const auto sid=sim.add(std::move(seize));
        auto pool=std::make_unique<Pool>(capacity,1,discipline=="fifo" ? des::QueueDiscipline::fifo :
            discipline=="lifo" ? des::QueueDiscipline::lifo : des::QueueDiscipline::priority);
        auto* p=pool.get();
        const auto pid=sim.add(std::move(pool));
        auto delay=std::make_unique<Delay>(7,[&spec](const Token& item) {
            require(item.leases.size()==1 && item.leases[0].pool==11 && item.leases[0].units==1,
                    "delivery began without its resource lease");
            return spec.at("jobs").at(item.entity.id).at("service").get<double>();
        });
        auto* d=delay.get();
        const auto did=sim.add(std::move(delay));
        const auto rid=sim.add(std::make_unique<Release>(7,11));
        sim.connect(sid,3,pid,0); sim.connect(pid,2,sid,2); sim.connect(sid,1,did,0);
        sim.connect(did,1,rid,0); sim.connect(rid,3,pid,0);
        double end=1;
        for(const auto& job:spec.at("jobs")) {
            sim.inject(job.at("arrival").get<double>(),sid,0,Token{{7,job.at("id").get<std::uint64_t>()},
                job.at("priority").get<std::int32_t>()});
            end+=job.at("service").get<double>();
        }
        end+=spec.at("jobs").back().at("arrival").get<double>();
        std::map<std::uint64_t,double> starts;
        std::map<std::uint64_t,Json> records;
        for(const auto& step:sim.run_until(end)) for(const auto& out:step.emissions) {
            if(out.source==sid && out.port==1) {
                require(starts.emplace(std::get<Token>(out.value).entity.id,step.time).second,"duplicate resource start");
            } else if(out.source==rid && out.port==1) {
                const auto& item=std::get<Token>(out.value);
                const auto id=item.entity.id;
                require(item.leases.empty(),"completion retains resource lease");
                require(records.emplace(id,Json::array({id,0,spec.at("jobs").at(id).at("arrival"),starts.at(id),step.time})).second,
                        "duplicate resource completion");
            }
        }
        require(records.size()==spec.at("jobs").size() && s->waiting()==0 && s->granted_count()==records.size() &&
                p->pool().available()==capacity && p->pool().allocated_units()==0 && p->pool().waiting()==0 &&
                d->active_count()==0 && d->completed_count()==records.size(),"resource graph failed to drain/return ownership");
        double wait=0,busy=0;
        Json completed=Json::array();
        for(const auto& [id,record]:records) {
            (void)id;
            wait+=record.at(3).get<double>()-record.at(2).get<double>();
            busy+=record.at(4).get<double>()-record.at(3).get<double>();
            completed.push_back(record);
        }
        close(p->mean_waiting(end)*end,wait,"resource queue area differs from individual waits");
        close(p->mean_allocated(end)*end,busy,"allocated-unit area differs from held leases");
        close(p->utilization(end),busy/(capacity*end),"resource utilization differs");
        close(d->completed_residence_time(),busy,"delivery residence differs from resource holding time");
        report["cases"].push_back({{"spec",spec},{"records",completed},{"rejected",Json::array()}});
    }
    require(report["cases"].size()==24,"expected all 24 unbounded reference cases");
    std::ofstream output(output_path); output<<std::setw(2)<<report<<'\n';
    require(output.good(),"cannot write reference resource report");
}
}

int main(int argc,char** argv) {
    try { require(argc==3,"expected trace plan and output paths"); run(argv[1],argv[2]);
        std::cout<<"Reference seize/delay/release: 24 traces with resource ownership and exact time integrals passed\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}

#include "ankurafathom/des/reference_queue.hpp"
#include "ankurafathom/des/reference_delay.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iomanip>
#include <iostream>

namespace {
using namespace ankurafathom;
using Json=nlohmann::json;
struct Work {};
using Store=des::EntityStore<Work,double,std::int64_t>;
using Token=des::EntityToken<Work>;
using Message=des::ProcessMessage<Work>;
using Queue=des::ReferenceQueue<Work>;
using Delay=des::ReferenceDelay<Work>;
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
void close(double a,double b,const char* message) { require(std::abs(a-b)<=2e-9*std::max({1.,std::abs(a),std::abs(b)}),message); }

void run(const char* input_path,const char* output_path) {
    std::ifstream input(input_path); Json plan; input>>plan;
    require(plan.at("version")==1,"unknown trace plan");
    Json report={{"version",1},{"cases",Json::array()}};
    for(const auto& spec:plan.at("cases")) {
        Store entities(7);
        for(const auto& job:spec.at("jobs")) {
            auto ref=entities.spawn({job.at("service").get<double>(),job.at("priority").get<std::int64_t>()});
            require(ref.id==job.at("id"),"non-sequential oracle identity");
        }
        const Store& store=entities; // Read-only for the complete trajectory.
        devs::Simulator<Message> sim;
        std::optional<std::size_t> capacity;
        if(!spec.at("queue_capacity").is_null()) capacity=spec.at("queue_capacity").get<std::size_t>();
        const auto discipline=spec.at("discipline").get<std::string>();
        auto queue=std::make_unique<Queue>(7,capacity,discipline=="fifo" ? des::QueueDiscipline::fifo :
            discipline=="lifo" ? des::QueueDiscipline::lifo : des::QueueDiscipline::priority);
        auto* q=queue.get();
        const auto qid=sim.add(std::move(queue));
        auto delay=std::make_unique<Delay>(7,[&store](const Token& item) { return store.field<0>(item.entity); },
                                         spec.at("capacity").get<std::size_t>());
        auto* d=delay.get();
        const auto did=sim.add(std::move(delay));
        sim.connect(qid,1,did,0); sim.connect(did,3,qid,2);
        double end=1;
        for(const auto& job:spec.at("jobs")) {
            const des::EntityRef<Work> ref{7,job.at("id").get<std::uint64_t>()};
            sim.inject(job.at("arrival").get<double>(),qid,0,Token{ref,static_cast<std::int32_t>(store.field<1>(ref))});
            end+=job.at("service").get<double>();
        }
        end+=spec.at("jobs").back().at("arrival").get<double>();
        std::map<std::uint64_t,Json> records,losses;
        std::map<std::uint64_t,double> starts;
        for(const auto& step:sim.run_until(end)) for(const auto& out:step.emissions) {
            if(out.source==qid && out.port==1) {
                const auto id=std::get<Token>(out.value).entity.id;
                require(starts.emplace(id,step.time).second,"duplicate service start");
            } else if(out.source==qid && out.port==3) {
                const auto id=std::get<Token>(out.value).entity.id;
                require(losses.emplace(id,Json::array({id,step.time})).second,"duplicate reference loss");
            } else if(out.source==did && out.port==1) {
                const auto id=std::get<Token>(out.value).entity.id;
                const double born=spec.at("jobs").at(id).at("arrival");
                require(records.emplace(id,Json::array({id,0,born,starts.at(id),step.time})).second,"duplicate reference completion");
            } else if(out.source==did && out.port==2) throw std::runtime_error("credit-controlled delay rejected an entity");
        }
        require(records.size()+losses.size()==entities.size() && q->waiting()==0 && d->active_count()==0 &&
                q->accepted_count()==q->released_count() && q->released_count()==d->completed_count() &&
                q->demand()==spec.at("capacity").get<std::size_t>(),"reference graph failed conservation or credit return");
        double queue_area=0,busy_area=0;
        Json completed=Json::array(),rejected=Json::array();
        for(const auto& [id,record]:records) {
            (void)id;
            queue_area+=record.at(3).get<double>()-record.at(2).get<double>();
            busy_area+=record.at(4).get<double>()-record.at(3).get<double>();
            completed.push_back(record);
        }
        for(const auto& [id,record]:losses) { (void)id; rejected.push_back(record); }
        close(q->mean_queue_length(end)*end,queue_area,"reference queue integral differs from individual waits");
        close(q->total_waiting_time(),queue_area,"reference queue wait total differs");
        close(d->mean_in_process(end)*end,busy_area,"reference delay integral differs from service history");
        close(d->completed_residence_time(),busy_area,"reference residence total differs");
        report["cases"].push_back({{"spec",spec},{"records",completed},{"rejected",rejected}});
    }
    std::ofstream output(output_path); output<<std::setw(2)<<report<<'\n';
    require(output.good(),"cannot write reference process trace");
}
}

int main(int argc,char** argv) {
    try { require(argc==3,"expected trace plan and output paths"); run(argv[1],argv[2]);
        std::cout<<"Reference queue/delay: 96 traces, exact conservation and individual-area identities passed\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}

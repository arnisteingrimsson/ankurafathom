#include "ankurafathom/des/multi_server.hpp"
#include "ankurafathom/des/process.hpp"
#include "ankurafathom/des/resource_pool.hpp"
#include <algorithm>
#include <iostream>
#include <numeric>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>

namespace {
using namespace ankurafathom;
using namespace des;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
using Input = devs::Input<ResourceMessage>;
Input seize(std::uint64_t id, std::size_t units, std::int32_t priority=0, bool preempt=false) {
    return {0,0,Seize{id,units,priority,preempt}};
}
Input release(std::uint64_t id) { return {0,0,Release{id}}; }
Input resize(std::size_t units) { return {0,2,SetCapacity{units}}; }
std::vector<std::uint64_t> grants(const ResourcePool& pool) {
    std::vector<std::uint64_t> result;
    for (const auto& event:pool.output()) result.push_back(std::get<Grant>(event.value).request_id);
    return result;
}

void lifo_station() {
    devs::Simulator<Entity> sim;
    auto station=std::make_unique<MultiServer<>>(1,1,QueueOptions{2,QueueDiscipline::lifo});
    auto* observed=station.get();
    const auto id=sim.add(std::move(station));
    // Admission is 1,2,3; 4 is lost. Dispatch is 3, then new 5, then 2, then 1.
    for (std::uint64_t i=1;i<=4;++i) sim.inject(0,id,0,Entity{i,0,1});
    sim.inject(1,id,0,Entity{5,1,1});
    sim.inject(1.5,id,0,Entity{6,1.5,1}); // full and cannot interrupt 5
    std::vector<std::uint64_t> order;
    for (const auto& step:sim.run_until(4)) for (const auto& out:step.emissions) order.push_back(out.value.id);
    require(order==std::vector<std::uint64_t>{3,5,2,1},"LIFO completion order differs");
    require(observed->accepted_count()==4 && observed->completed_count()==4 && observed->rejected_count()==2,
            "LIFO admission/conservation differs");
    require(observed->rejected()[0].entity.id==4 && observed->rejected()[1].entity.id==6,"LIFO overflow evicted accepted work");
    require(observed->mean_queue_length(4)==1.25 && observed->total_waiting_time()==5 && observed->utilization(4)==1,
            "LIFO time statistics differ");
    MultiServer<> atomic(1,1,{1,QueueDiscipline::lifo});
    atomic.external_transition(0,{{0,0,Entity{1,0,2}},{0,0,Entity{2,0,1}}});
    bool caught=false;
    try { atomic.confluent_transition({{0,0,Entity{3,1,1}},{0,0,Entity{2,1,1}}}); }
    catch (const std::invalid_argument&) { caught=true; }
    require(caught && atomic.completed_count()==0 && atomic.waiting()==1 && atomic.output()[0].value.id==2,
            "LIFO invalid confluence failed rollback");
    atomic.confluent_transition({{0,0,Entity{3,1,1}}});
    require(atomic.output()[0].value.id==3,"LIFO new confluent arrival did not overtake waiter");
}

void pool_priority_and_permutations() {
    std::vector<int> permutation{0,1,2,3};
    do {
        ResourcePool pool(2,2,QueueDiscipline::priority);
        const std::vector<Input> requests{seize(4,1,0),seize(2,1,0),seize(3,1,-1),seize(1,1,1)};
        std::vector<Input> bag;
        for (auto index:permutation) bag.push_back(requests[index]);
        pool.external_transition(0,bag);
        require(grants(pool)==std::vector<std::uint64_t>{3,2},"priority simultaneous arbitration differs");
        pool.internal_transition();
        // A new urgent request outranks an older low-priority waiter at release.
        pool.external_transition(1,{release(3),seize(5,1,-2)});
        require(grants(pool)==std::vector<std::uint64_t>{5} && pool.waiting()==2,"priority release/new-request ordering differs");
        pool.internal_transition();
        pool.external_transition(1,{release(2),release(5)});
        require(grants(pool)==std::vector<std::uint64_t>{4,1},"priority queue remainder differs");
    } while(std::next_permutation(permutation.begin(),permutation.end()));
    ResourcePool ties(1,1,QueueDiscipline::priority);
    ties.external_transition(0,{seize(90,1)}); ties.internal_transition();
    ties.external_transition(1,{seize(8,1)});
    ties.external_transition(1,{seize(1,1)});
    ties.external_transition(1,{release(90)});
    require(grants(ties)==std::vector<std::uint64_t>{8},"priority ties lost earlier admission");
}

void pool_lifo_blocking_and_errors() {
    ResourcePool pool(1,3,QueueDiscipline::lifo);
    pool.external_transition(0,{seize(1,1)}); pool.internal_transition();
    pool.external_transition(1,{seize(2,1),seize(3,3)});
    pool.external_transition(1,{release(1)});
    require(pool.waiting()==2 && pool.available()==1 && pool.output().empty(),"LIFO bypassed blocked newest request");
    pool.external_transition(1,{resize(3)});
    require(grants(pool)==std::vector<std::uint64_t>{3},"LIFO expansion grant differs");
    bool caught=false;
    try { pool.confluent_transition({release(3),seize(4,1,0,true)}); }
    catch(const std::invalid_argument&) { caught=true; }
    require(caught && grants(pool)==std::vector<std::uint64_t>{3} && pool.allocated_units()==3,
            "preemption rejection changed a pending grant/allocation");
    auto cloned=pool.clone();
    pool.confluent_transition({release(3),seize(4,1)});
    require(grants(pool)==std::vector<std::uint64_t>{4,2} && pool.available()==1,"LIFO confluence order differs");
    require(std::get<Grant>(cloned->output()[0].value).request_id==3,"clone shares queue state");
    caught=false;
    try { pool.external_transition(0,{resize(0)}); } catch(const std::invalid_argument&) { caught=true; }
    require(caught && pool.capacity()==3 && pool.allocated_units()==2,"resize revoked active LIFO allocations");
    ResourcePool priority(1,3,QueueDiscipline::priority);
    priority.external_transition(0,{seize(1,3,-1),seize(2,1,0)});
    require(priority.waiting()==2 && priority.available()==1,"priority bypassed an oversized head");
    caught=false;
    try { ResourcePool invalid(1,1,static_cast<QueueDiscipline>(99)); } catch(const std::invalid_argument&) { caught=true; }
    require(caught,"unknown resource discipline accepted");
}

// Independent integer-time scheduler. Ranking is recomputed from immutable
// arrival timestamps and bag ranks instead of mutating a native-style queue.
void pool_reference_traces() {
    for (auto discipline:{QueueDiscipline::fifo,QueueDiscipline::lifo,QueueDiscipline::priority})
    for (std::uint64_t variant=0;variant<32;++variant) {
        struct Request { std::uint64_t id; std::size_t units; std::int32_t priority; int arrived; int rank; };
        struct Active { Request request; int finish; };
        std::vector<Request> waiting;
        std::vector<Active> active;
        devs::Simulator<ResourceMessage> sim;
        auto pool=std::make_unique<ResourcePool>(0,4,discipline);
        auto* observed=pool.get();
        const auto node=sim.add(std::move(pool));
        std::size_t cumulative=0;
        for (int time=0;time<=100;++time) {
            for (const auto& job:active) if (job.finish==time) sim.inject(time,node,0,Release{job.request.id});
            std::erase_if(active,[&](const auto& job) { return job.finish==time; });
            std::size_t allocated=0;
            for (const auto& job:active) allocated+=job.request.units;
            const auto capacity=allocated+(time<20 ? (time*7+variant)%5 : 16);
            sim.inject(time,node,2,SetCapacity{capacity});
            std::vector<Request> incoming;
            if (time<16) for(int j=0;j<2;++j) {
                const auto id=static_cast<std::uint64_t>(31-2*time-j);
                Request request{id,1+(id*7+variant)%4,static_cast<std::int32_t>((id+variant)%5)-2,time,j};
                incoming.push_back(request);
                sim.inject(time,node,0,Seize{id,request.units,request.priority});
            }
            if (discipline==QueueDiscipline::priority) {
                std::sort(incoming.begin(),incoming.end(),[](auto a,auto b) {
                    return std::pair{a.priority,a.id}<std::pair{b.priority,b.id};
                });
                for(std::size_t i=0;i<incoming.size();++i) incoming[i].rank=static_cast<int>(i);
            }
            waiting.insert(waiting.end(),incoming.begin(),incoming.end());
            std::vector<std::uint64_t> expected;
            for (;;) {
                if (waiting.empty()) break;
                const auto next=std::min_element(waiting.begin(),waiting.end(),[&](auto a,auto b) {
                    if (discipline==QueueDiscipline::priority && a.priority!=b.priority) return a.priority<b.priority;
                    return discipline==QueueDiscipline::lifo ? std::pair{a.arrived,a.rank}>std::pair{b.arrived,b.rank} :
                        std::pair{a.arrived,a.rank}<std::pair{b.arrived,b.rank};
                });
                if (next->units>capacity-allocated) break;
                const auto request=*next;
                waiting.erase(next);
                expected.push_back(request.id);
                active.push_back({request,time+1+static_cast<int>((request.id+variant)%4)});
                allocated+=request.units;
            }
            cumulative+=expected.size();
            std::vector<std::uint64_t> actual;
            for(const auto& step:sim.run_until(time)) for(const auto& event:step.emissions)
                actual.push_back(std::get<Grant>(event.value).request_id);
            require(actual==expected,"resource reference grant order differs");
            require(observed->capacity()==capacity && observed->allocated_units()==allocated &&
                    observed->available()==capacity-allocated && observed->waiting()==waiting.size() &&
                    observed->allocated()==active.size(),"resource reference conservation differs");
        }
        require(cumulative==32 && waiting.empty() && active.empty(),"resource reference did not drain exactly once");
    }
}

void write_oracle_trace(const char* plan_path, const char* report_path) {
    using Json=nlohmann::json;
    std::ifstream input(plan_path); Json plan; input>>plan;
    require(plan.at("version")==1 && plan.at("numerical_tolerance")==2e-9,"unknown discipline plan");
    Json report={{"version",1},{"cases",Json::array()}};
    for(const auto& spec:plan.at("cases")) {
        std::vector<Entity> jobs;
        for(const auto& job:spec.at("jobs")) {
            Entity e{job.at("id"),job.at("arrival"),job.at("service")};
            e.priority=job.at("priority"); jobs.push_back(e);
        }
        QueueOptions options;
        if(!spec.at("queue_capacity").is_null()) options.capacity=spec.at("queue_capacity").get<std::size_t>();
        const auto discipline=spec.at("discipline").get<std::string>();
        require(discipline=="fifo" || discipline=="lifo" || discipline=="priority","unknown oracle discipline");
        options.discipline=discipline=="fifo" ? QueueDiscipline::fifo : discipline=="lifo" ? QueueDiscipline::lifo : QueueDiscipline::priority;
        devs::Simulator<Entity> sim;
        const auto source=sim.add(std::make_unique<ScheduledSource<>>(jobs));
        auto model=std::make_unique<MultiServer<>>(spec.at("capacity").get<std::size_t>(),1,options);
        auto* station=model.get();
        const auto node=sim.add(std::move(model));
        sim.connect(source,0,node,0);
        std::map<std::uint64_t,Json> records;
        const double drain_horizon=jobs.back().arrived_at+1+std::accumulate(jobs.begin(),jobs.end(),0.0,
            [](double sum,const Entity& e) { return sum+e.service_duration; });
        for(const auto& step:sim.run_until(drain_horizon))
            for(const auto& event:step.emissions) if(event.source==node) {
                const auto& e=event.value;
                require(records.emplace(e.id,Json::array({e.id,0,e.entered_at,e.completed_at-e.service_duration,e.completed_at})).second,
                        "duplicate oracle completion");
            }
        require(station->waiting()==0 && station->busy_servers()==0 &&
                station->completed_count()+station->rejected_count()==jobs.size(),"oracle trace failed to drain");
        Json completed=Json::array(), rejected=Json::array();
        for(const auto& [id,record]:records) { (void)id; completed.push_back(record); }
        for(const auto& lost:station->rejected()) rejected.push_back(Json::array({lost.entity.id,lost.rejected_at}));
        report["cases"].push_back({{"spec",spec},{"records",completed},{"rejected",rejected}});
    }
    std::ofstream output(report_path); output<<std::setw(2)<<report<<'\n';
    require(output.good(),"cannot write discipline trace report");
}
}

int main(int argc,char** argv) {
    try { lifo_station(); pool_priority_and_permutations(); pool_lifo_blocking_and_errors(); pool_reference_traces();
        require(argc==1 || argc==3,"expected discipline plan and report paths");
        if(argc==3) write_oracle_trace(argv[1],argv[2]);
        std::cout<<"Queue/resource discipline hand traces, 24 priority permutations, and 96 independent resource traces passed\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}

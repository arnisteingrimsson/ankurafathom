#include "ankurafathom/des/multi_server.hpp"
#include "ankurafathom/des/process.hpp"
#include "ankurafathom/ir/model.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
using namespace ankurafathom;
using des::Entity;
using des::MultiServer;
using des::QueueDiscipline;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
Entity entity(std::uint64_t id, double arrival, double duration, std::int32_t priority = 0) {
    Entity e{id, arrival, duration};
    e.priority = priority;
    return e;
}

void hand_schedule_and_permutations() {
    std::vector<Entity> initial{entity(1,0,3), entity(2,0,2,1), entity(3,0,1,2), entity(4,0,1,-1)};
    do {
        devs::Simulator<Entity> sim;
        auto server = std::make_unique<MultiServer<>>(1, 1, des::QueueOptions{2, QueueDiscipline::priority});
        auto* station = server.get();
        const auto id = sim.add(std::move(server));
        for (auto e : initial) sim.inject(0,id,0,e);
        sim.inject(0.5,id,0,entity(7,0.5,1,-10)); // Cannot evict accepted work.
        sim.inject(1,id,0,entity(5,1,1,-2));
        sim.inject(2,id,0,entity(6,2,1,-3));
        const auto trace = sim.run_until(8);
        std::vector<std::uint64_t> ids;
        std::vector<double> times;
        for (const auto& step : trace) for (const auto& output : step.emissions) {
            ids.push_back(output.value.id);
            times.push_back(output.value.completed_at);
        }
        require(ids == std::vector<std::uint64_t>{4,5,6,1,2} &&
                times == std::vector<double>{1,2,3,6,8}, "priority hand trace or permutation differs");
        require(station->accepted_count() == 5 && station->completed_count() == 5 &&
                station->rejected_count() == 2 && station->rejected()[0].entity.id == 3 &&
                station->rejected()[1].entity.id == 7 && station->rejected()[1].rejected_at == 0.5,
                "admission, rejection identity, or confluence differs");
        require(station->total_waiting_time() == 9 && station->mean_queue_length(8) == 9.0/8 &&
                station->utilization(8) == 1, "priority hand statistics differ");
    } while (std::next_permutation(initial.begin(),initial.end(),
                                  [](auto a, auto b) { return a.id < b.id; }));
}

void ties_nonpreemption_and_rollback() {
    MultiServer<> station(1,1,{2,QueueDiscipline::priority});
    station.external_transition(0,{{0,0,entity(9,0,3,10)}});
    station.external_transition(1,{{0,0,entity(8,1,1,0)}});
    station.external_transition(0,{{0,0,entity(1,1,1,0)}});
    require(station.output()[0].value.id == 9 && station.time_advance() == 2,
            "new higher priority work preempted service");
    bool caught = false;
    try { station.confluent_transition({{0,0,entity(2,3,1,-1)}, {0,0,entity(3,3,-1)}}); }
    catch (const std::invalid_argument&) { caught = true; }
    require(caught && station.completed_count() == 0 && station.rejected_count() == 0 &&
            station.time_advance() == 2 && station.waiting() == 2,
            "invalid confluence changed queue or completion state");
    station.internal_transition();
    require(station.output()[0].value.id == 8, "equal priority must preserve earlier admission");
    station.external_transition(0,{{0,0,entity(4,3,1)}});
    station.external_transition(0,{{0,0,entity(5,3,1)}}); // Full: record loss.
    require(station.rejected_count() == 1, "full queue did not reject");
    caught = false;
    try { station.external_transition(0,{{0,0,entity(6,3,1)}, {0,0,entity(5,3,1)}}); }
    catch (const std::invalid_argument&) { caught = true; }
    require(caught && station.rejected_count() == 1 && station.accepted_count() == 4,
            "rejected ID replay or partially invalid bag changed state");
    station.internal_transition();
    require(station.output()[0].value.id == 1, "stable tie order was lost after rejection");

    MultiServer<> zero(2,1,{0,QueueDiscipline::fifo});
    zero.external_transition(0,{{0,0,entity(1,0,1)}, {0,0,entity(2,0,1)}, {0,0,entity(3,0,1)}});
    zero.confluent_transition({{0,0,entity(4,1,1)}, {0,0,entity(5,1,1)}, {0,0,entity(6,1,1)}});
    require(zero.completed_count() == 2 && zero.accepted_count() == 4 &&
            zero.rejected_count() == 2 && zero.waiting() == 0 && zero.busy_servers() == 2,
            "zero-buffer simultaneous completions did not free both service places");
    MultiServer<> huge(1,1,{std::numeric_limits<std::size_t>::max(),QueueDiscipline::fifo});
    huge.external_transition(0,{{0,0,entity(1,0,1)}, {0,0,entity(2,0,1)}});
    require(huge.waiting() == 1 && huge.rejected_count() == 0, "queue capacity arithmetic overflowed");
}

// Independent integer-clock reference: finish jobs with absolute deadlines,
// admit an entire tick's arrivals, dispatch by explicit rank, then integrate
// one time unit. It does not use the event kernel or station transition code.
void integer_clock_reference() {
    for (int variant=0; variant<96; ++variant) {
        const std::size_t capacity = 1 + variant%2;
        const std::size_t limit = (variant/2)%4;
        const auto discipline = variant<64 ? ((variant/8)%2 ? QueueDiscipline::priority : QueueDiscipline::fifo) :
            QueueDiscipline::lifo;
        const bool priority = discipline==QueueDiscipline::priority;
        const bool bounded = variant<32 || (variant>=64 && variant<80);
        des::QueueOptions options{bounded ? std::optional<std::size_t>(limit) : std::nullopt,
                                 discipline};
        devs::Simulator<Entity> sim;
        auto server = std::make_unique<MultiServer<>>(capacity,1,options);
        auto* station = server.get();
        const auto id = sim.add(std::move(server));
        std::vector<Entity> arrivals;
        for (std::uint64_t j=0; j<16; ++j) {
            auto e = entity(100-j, static_cast<double>(j/3), 1+(j*7+variant)%3,
                            static_cast<std::int32_t>((j*5+variant)%5)-2);
            arrivals.push_back(e);
            sim.inject(e.arrived_at,id,0,e);
        }
        struct Job { Entity e; int end; };
        std::vector<Job> active;
        std::vector<Entity> queue;
        std::vector<std::uint64_t> rejected;
        std::size_t accepted=0, completed=0;
        double queue_area=0, busy_area=0, wait=0;
        for (int t=0; t<=60; ++t) {
            std::vector<std::uint64_t> expected;
            for (auto job : active) if (job.end == t) expected.push_back(job.e.id);
            completed += expected.size();
            std::erase_if(active,[t](auto job) { return job.end == t; });
            std::vector<Entity> incoming;
            for (auto e : arrivals) if (e.arrived_at == t) incoming.push_back(e);
            if (priority) std::sort(incoming.begin(),incoming.end(),[](auto a,auto b) {
                return a.priority != b.priority ? a.priority < b.priority : a.id < b.id;
            });
            for (auto e : incoming) {
                if (bounded && active.size()+queue.size() == capacity+limit) rejected.push_back(e.id);
                else { queue.push_back(e); ++accepted; }
            }
            while (active.size()<capacity && !queue.empty()) {
                auto next=queue.begin();
                if (discipline==QueueDiscipline::lifo) next=std::prev(queue.end());
                if (priority) next=std::min_element(queue.begin(),queue.end(),[](auto a,auto b) {
                    return a.priority < b.priority;
                });
                auto e=*next;
                queue.erase(next);
                wait += t-e.arrived_at;
                active.push_back({e,t+static_cast<int>(e.service_duration)});
            }
            std::vector<std::uint64_t> actual;
            for (const auto& step : sim.run_until(t))
                for (const auto& event : step.emissions) actual.push_back(event.value.id);
            std::sort(expected.begin(),expected.end());
            std::sort(actual.begin(),actual.end());
            require(actual == expected, "integer-clock completion identities differ");
            require(station->accepted_count()==accepted && station->completed_count()==completed &&
                    station->rejected_count()==rejected.size() && station->waiting()==queue.size() &&
                    station->busy_servers()==active.size() && station->total_waiting_time()==wait &&
                    accepted==completed+queue.size()+active.size(), "integer-clock accounting differs");
            for (std::size_t i=0;i<rejected.size();++i)
                require(station->rejected()[i].entity.id==rejected[i], "integer-clock rejected IDs differ");
            if (t>0) require(std::abs(station->mean_queue_length(t)-queue_area/t)<1e-12 &&
                             std::abs(station->utilization(t)-busy_area/(capacity*t))<1e-12,
                             "integer-clock time integrals differ");
            queue_area += queue.size();
            busy_area += active.size();
        }
        require(accepted+rejected.size()==arrivals.size() && accepted==completed,
                "integer-clock drained conservation differs");
    }
}

void tandem_loss_and_overflow() {
    devs::Simulator<Entity> sim;
    const auto source = sim.add(std::make_unique<des::ScheduledSource<>>(std::vector<Entity>{
        entity(2,0,1,5), entity(1,0,1,-5), entity(3,1,1,-10)}));
    const auto first = sim.add(std::make_unique<MultiServer<>>(2));
    auto server = std::make_unique<MultiServer<>>(1,1,des::QueueOptions{0,QueueDiscipline::priority});
    auto* station = server.get();
    const auto second = sim.add(std::move(server));
    auto sink = std::make_unique<des::CompletionSink<>>();
    auto* completed = sink.get();
    const auto last = sim.add(std::move(sink));
    sim.connect(source,0,first,0);
    sim.connect(first,1,second,0);
    sim.connect(second,1,last,0);
    (void)sim.run_until_transactional(3);
    require(completed->completed_count()==2 && completed->completed()[0].id==1 &&
            completed->completed()[1].id==3 && completed->total_cycle_time()==4 &&
            station->rejected_count()==1 && station->rejected()[0].entity.id==2 &&
            station->rejected()[0].entity.completed_at==1 && station->rejected()[0].rejected_at==1,
            "tandem loss, priority propagation, or completion-boundary admission differs");

    MultiServer<> overflow(2,1,{0,QueueDiscipline::fifo});
    overflow.external_transition(0,{{0,0,entity(1,0,1e308)}, {0,0,entity(2,0,1e308)}});
    bool caught=false;
    try { overflow.internal_transition(); }
    catch (const std::overflow_error&) { caught=true; }
    require(caught && overflow.time_advance()==1e308 && overflow.completed_count()==0 &&
            overflow.busy_servers()==2 && overflow.rejected_count()==0,
            "statistics overflow did not roll back internal transition");
}

void declarative(const char* path, const char* hybrid_path) {
    auto model=ir::load_file(path);
    const auto rows=ir::run(model);
    for (const auto& row : rows) if (row.time==8) {
        if (row.output_id=="accepted" || row.output_id=="completed") require(row.value==5,"IR admitted/completed differs");
        if (row.output_id=="rejected") require(row.value==2,"IR lost count differs");
        if (row.output_id=="queue_mean") require(row.value==9.0/8,"IR queue area differs");
        if (row.output_id=="cycle_total") require(row.value==17,"IR cycle time differs");
    }
    model.dt=0.25;
    const auto dense=ir::run(model);
    for (const auto& row : rows) {
        auto found=std::find_if(dense.begin(),dense.end(),[&](const auto& r) {
            return r.time==row.time && r.output_id==row.output_id;
        });
        require(found!=dense.end() && found->value==row.value,"sampling density changed process");
    }
    auto hybrid=ir::load_file(hybrid_path);
    hybrid.dt=0.5;
    hybrid.horizon=8;
    hybrid.flows.clear();
    hybrid.process=model.process;
    const auto stock_output=hybrid.outputs.front();
    hybrid.outputs=model.outputs;
    hybrid.outputs.push_back(stock_output);
    const auto coupled=ir::run(hybrid);
    for (const auto& row : rows) {
        auto found=std::find_if(coupled.begin(),coupled.end(),[&](const auto& r) {
            return r.time==row.time && r.output_id==row.output_id;
        });
        require(found!=coupled.end() && found->value==row.value,"hybrid bounded station differs from DES");
        if (row.output_id=="completed") {
            auto stock=std::find_if(coupled.begin(),coupled.end(),[&](const auto& r) {
                return r.time==row.time && r.output_id==stock_output.id;
            });
            require(stock!=coupled.end() && stock->value==row.value,
                    "rejected entity produced a hybrid completion pulse");
        }
    }
}
}
int main(int argc,char** argv) {
    try {
        require(argc==3,"expected bounded process and hybrid fixtures");
        hand_schedule_and_permutations();
        ties_nonpreemption_and_rollback();
        integer_clock_reference();
        tandem_loss_and_overflow();
        declarative(argv[1],argv[2]);
        std::cout << "Bounded queues: hand trace, 24 permutations, 96 integer-clock references, IR passed\n";
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}

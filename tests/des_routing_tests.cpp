#include "ankurafathom/des/multi_server.hpp"
#include "ankurafathom/des/routing.hpp"
#include "ankurafathom/ir/model.hpp"

#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <variant>

namespace {
using namespace ankurafathom;
using des::Entity;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
Entity entity(std::uint64_t id, double arrival, std::int32_t priority = 0) {
    Entity e{id,arrival,2}; e.priority=priority; return e;
}
void router_contract() {
    using Message=std::variant<Entity,double>;
    des::PriorityRouter<Message> router(-1);
    router.external_transition(0,{{0,0,entity(1,0,-2)}, {0,0,entity(2,0,-1)}, {0,0,entity(3,0,0)}});
    require(router.time_advance()==0 && router.output().size()==3 && router.matched_count()==2 &&
            router.otherwise_count()==1 && router.output()[0].port==1 &&
            router.output()[1].port==1 && router.output()[2].port==2,
            "priority threshold boundary or partition is wrong");
    bool caught=false;
    try { router.confluent_transition({{0,0,entity(4,0)}, {0,0,entity(4,0)}}); }
    catch (const std::invalid_argument&) { caught=true; }
    require(caught && router.received_count()==3 && router.output().size()==3,
            "failed router confluence lost publication or committed arrivals");
    router.confluent_transition({{0,0,entity(4,0)}});
    require(router.output().size()==1 && std::get<Entity>(router.output()[0].value).id==4 &&
            router.received_count()==4, "router confluence re-emitted previous bag");
    router.internal_transition();
    require(std::isinf(router.time_advance()),"router failed to passivate");
    caught=false;
    try { router.external_transition(1,{{0,0,3.0}}); }
    catch (const std::bad_variant_access&) { caught=true; }
    require(caught && router.received_count()==4, "wrong message variant committed router state");
    router.external_transition(1,{{0,0,entity(5,1,std::numeric_limits<std::int32_t>::min())},
                                  {0,0,entity(6,1,std::numeric_limits<std::int32_t>::max())}});
    require(router.output()[0].port==1 && router.output()[1].port==2,"priority extremes routed incorrectly");
}
void rejection_publication() {
    des::MultiServer<> station(1,1,{0,des::QueueDiscipline::fifo},true);
    station.external_transition(0,{{0,0,entity(1,0)}, {0,0,entity(2,0)}});
    require(station.time_advance()==0 && station.output().size()==1 &&
            station.output()[0].port==des::MultiServer<>::rejection_port &&
            station.output()[0].value.id==2 && std::isnan(station.output()[0].value.completed_at),
            "rejection was delayed or turned into completion");
    bool caught=false;
    try { station.external_transition(1e-10,{}); }
    catch (const std::invalid_argument&) { caught=true; }
    require(caught && station.time_advance()==0 && station.output()[0].value.id==2,
            "positive elapsed time postponed a pending rejection publication");
    caught=false;
    try { station.confluent_transition({{0,0,entity(3,0)}, {0,0,entity(3,0)}}); }
    catch (const std::invalid_argument&) { caught=true; }
    require(caught && station.rejected_count()==1 && station.output()[0].value.id==2,
            "failed publication confluence changed pending rejection");
    station.confluent_transition({{0,0,entity(3,0)}});
    require(station.rejected_count()==2 && station.output()[0].value.id==3 &&
            station.completed_count()==0,"publication confluence completed active service");
    station.internal_transition();
    require(station.time_advance()==2 && station.output()[0].value.id==1,
            "zero-time publication consumed service time");
    station.confluent_transition({{0,0,entity(4,2)}, {0,0,entity(5,2)}});
    require(station.completed_count()==1 && station.output()[0].value.id==5 &&
            station.time_advance()==0,"completion confluence failed to publish new overflow");
    station.internal_transition();
    require(station.time_advance()==2 && station.output()[0].value.id==4,
            "overflow publication changed new service deadline");
}
class FailableSink final : public devs::Atomic<Entity> {
public:
    explicit FailableSink(std::shared_ptr<bool> fail):fail_(std::move(fail)) {}
    std::unique_ptr<devs::Atomic<Entity>> clone() const override { return std::make_unique<FailableSink>(*this); }
    double time_advance() const override { return std::numeric_limits<double>::infinity(); }
    std::vector<devs::PortValue<Entity>> output() const override { return {}; }
    void internal_transition() override { throw std::logic_error("passive"); }
    void confluent_transition(const std::vector<devs::Input<Entity>>&) override { throw std::logic_error("passive"); }
    void external_transition(double elapsed,const std::vector<devs::Input<Entity>>& bag) override {
        sink.external_transition(elapsed,bag);
        if (*fail_) throw std::runtime_error("injected downstream failure");
    }
    des::DiscardSink<> sink;
private:
    std::shared_ptr<bool> fail_;
};
void downstream_rollback() {
    devs::Simulator<Entity> sim;
    const auto server=sim.add(std::make_unique<des::MultiServer<>>(1,1,
        des::QueueOptions{0,des::QueueDiscipline::fifo},true));
    auto fail=std::make_shared<bool>(true);
    const auto sink=sim.add(std::make_unique<FailableSink>(fail));
    sim.connect(server,2,sink,0);
    sim.inject(0,server,0,entity(1,0)); sim.inject(0,server,0,entity(2,0));
    (void)sim.step_transactional();
    bool caught=false;
    try { (void)sim.step_transactional(); } catch (const std::runtime_error&) { caught=true; }
    const auto& restored=dynamic_cast<const des::MultiServer<>&>(sim.model(server));
    require(caught && restored.output().size()==1 && restored.output()[0].value.id==2 &&
            restored.rejected_count()==1 && restored.completed_count()==0 &&
            dynamic_cast<const FailableSink&>(sim.model(sink)).sink.received_count()==0,
            "downstream failure did not restore publication and receiver");
    *fail=false;
    const auto trace=sim.run_until_transactional(2);
    std::size_t rejected=0,completed=0;
    for (const auto& step:trace) for (const auto& event:step.emissions) {
        if (event.port==2) ++rejected; else if (event.port==1) ++completed;
    }
    require(rejected==1 && completed==1 &&
            dynamic_cast<const FailableSink&>(sim.model(sink)).sink.received_count()==1,
            "retry duplicated or lost routed work");
}
void graph_hand_oracle(const char* path) {
    const auto model=ir::load_file(path);
    const auto rows=ir::run(model);
    std::map<double,std::map<std::string,double>> values;
    for (const auto& row:rows) values[row.time][row.output_id]=row.value;
    for (const auto& [time,v]:values) {
        const auto get=[&](const std::string& name) { return v.at(name); };
        const double completed=time<1 ? 0 : time<2 ? 1 : time<3 ? 4 : 6;
        const double cycle=time<1 ? 0 : time<2 ? 1 : time<3 ? 6 : 10;
        require(get("done_completed")==completed && get("done_cycle_total")==cycle &&
                get("lost_discarded")==2,"graph hand completion/loss/cycle oracle differs");
        double in_process=0;
        for (const auto& id:{"urgent","normal","backup"}) {
            const std::string name(id);
            in_process+=get(name+"_waiting")+get(name+"_busy");
            require(get(name+"_accepted")==get(name+"_completed")+get(name+"_waiting")+get(name+"_busy"),
                    "station conservation failed after graph quiescence");
        }
        require(get("arrivals_emitted")==completed+get("lost_discarded")+in_process,
                "global graph conservation failed");
        require(get("triage_received")==get("triage_matched")+get("triage_otherwise") &&
                get("audit_received")==completed,"branch partition or merge duplicated work");
        require(get("backup_rejected")==2 && get("urgent_rejected")==2 && get("normal_rejected")==2,
                "routed rejection attempts were counted as terminal losses");
        if (time>0) require(std::abs(get("backup_queue_mean")-std::min(time,2.0)/time)<1e-12 &&
                           std::abs(get("backup_utilization")-std::min(time,3.0)/time)<1e-12,
                           "zero-time routing changed queue or busy integrals");
    }
    auto dense=model; dense.dt=0.25;
    const auto dense_rows=ir::run(dense);
    for (const auto& row:dense_rows) if (values.contains(row.time))
        require(values.at(row.time).at(row.output_id)==row.value,"graph changed with observation density");
}
}
int main(int argc,char** argv) {
    try {
        require(argc==2,"expected graph fixture");
        router_contract(); rejection_publication(); downstream_rollback(); graph_hand_oracle(argv[1]);
        std::cout<<"DES routing: partition, rejection confluence, rollback/retry, graph conservation passed\n";
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}

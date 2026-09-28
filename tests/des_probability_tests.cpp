#include "ankurafathom/des/routing.hpp"
#include "ankurafathom/ir/model.hpp"
#include <algorithm>
#include <iostream>
#include <map>
#include <numeric>
#include <variant>

namespace {
using namespace ankurafathom;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F call) {
    bool caught=false;
    try { call(); } catch (const std::exception&) { caught=true; }
    require(caught,"invalid probability input was accepted");
}

void atomic_contract() {
    const des::BernoulliRouting policy{.35,987654321,17,19,301};
    const auto expected=[&](std::uint64_t id) {
        const auto word=rng::draw(987654321,{17,19,id,0,301,0})[0];
        return (static_cast<double>(word)+.5)/4294967296.0 < .35 ? 1U : 2U;
    };
    std::vector<devs::Input<des::Entity>> bag;
    for (std::uint64_t id=0;id<64;++id) bag.push_back({0,0,{id,0,.25}});
    for (int permutation=0;permutation<8;++permutation) {
        std::rotate(bag.begin(),bag.begin()+7,bag.end());
        if (permutation%2) std::reverse(bag.begin(),bag.end());
        des::ProbabilityRouter<> router(policy);
        router.external_transition_at(0,0,bag);
        require(router.output().size()==64 && *router.next_event_time()==0,"probability router did not buffer input");
        auto clone=router.clone();
        for (const auto& event:router.output()) {
            require(event.port==expected(event.value.id),"routing draw ignored its address");
            require(event.value.arrived_at==0 && event.value.service_duration==.25 && std::isnan(event.value.completed_at),
                    "router changed entity metadata");
        }
        const auto pending=router.output();
        for (std::size_t i=0;i<pending.size();++i)
            require(router.output()[i].port==pending[i].port && clone->output()[i].port==pending[i].port,
                    "output or clone changed a choice");
        rejects([&] { router.confluent_transition({{0,0,{64,0,1}}, {0,0,{0,0,1}}}); });
        require(router.received_count()==64 && router.output().size()==64,"failed confluence committed state");
        router.confluent_transition({{0,0,{64,0,1}}});
        require(router.received_count()==65 && router.output().size()==1 && router.output()[0].port==expected(64),
                "confluence lost or duplicated publication");
        router.internal_transition();
        rejects([&] { router.external_transition(0,{{0,0,{1ULL<<48,0,1}}}); });
        require(router.received_count()==65 && router.output().empty(),"address failure committed state");
        require(router.matched_count()+router.otherwise_count()==router.received_count(),"routing counts do not conserve");
    }
    for (double probability:{0.0,1.0}) {
        auto endpoint=policy; endpoint.match_probability=probability;
        des::ProbabilityRouter<> router(endpoint);
        router.external_transition(0,bag);
        for (const auto& event:router.output()) require(event.port==(probability==1 ? 1U : 2U),"probability endpoint is inexact");
        router.internal_transition();
        rejects([&] { router.external_transition(0,{{0,0,{1ULL<<48,0,1}}}); });
    }
    for (double probability:{-.1,1.1,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        auto invalid=policy; invalid.match_probability=probability;
        rejects([&] { (void)des::ProbabilityRouter<>(invalid); });
    }
    for (int field=0;field<3;++field) {
        auto invalid=policy;
        if (field==0) invalid.scenario=65536;
        if (field==1) invalid.replication=65536;
        if (field==2) invalid.stream=65536;
        rejects([&] { (void)des::ProbabilityRouter<>(invalid); });
    }
    using Message=std::variant<des::Entity,double>;
    des::ProbabilityRouter<Message> variant(policy);
    variant.external_transition(0,{{0,0,Message{des::Entity{0,0,1}}}});
    require(variant.output()[0].port==expected(0),"variant router differs");
    rejects([&] { variant.confluent_transition({{0,0,Message{3.0}}}); });
    require(variant.received_count()==1 && variant.output().size()==1,"variant failure lost pending output");
}

class FailingReceiver final : public devs::Atomic<des::Entity> {
public:
    explicit FailingReceiver(std::shared_ptr<bool> fail):fail_(std::move(fail)) {}
    std::unique_ptr<devs::Atomic<des::Entity>> clone() const override { return std::make_unique<FailingReceiver>(*this); }
    double time_advance() const override { return std::numeric_limits<double>::infinity(); }
    std::vector<devs::PortValue<des::Entity>> output() const override { return {}; }
    void internal_transition() override { throw std::logic_error("passive receiver"); }
    void external_transition(double, const std::vector<devs::Input<des::Entity>>& bag) override {
        for (const auto& input:bag) ids.push_back(input.value.id);
        if (*fail_) throw std::runtime_error("injected receiver failure");
    }
    void confluent_transition(const std::vector<devs::Input<des::Entity>>& bag) override { external_transition(0,bag); }
    std::vector<std::uint64_t> ids;
private:
    std::shared_ptr<bool> fail_;
};

void downstream_rollback() {
    devs::Simulator<des::Entity> sim;
    const auto router=sim.add(std::make_unique<des::ProbabilityRouter<>>(des::BernoulliRouting{.35,123,1,2,301}));
    auto fail=std::make_shared<bool>(true);
    const auto sink=sim.add(std::make_unique<FailingReceiver>(fail));
    sim.connect(router,1,sink,0); sim.connect(router,2,sink,0);
    for (std::uint64_t id=0;id<16;++id) sim.inject(0,router,0,{id,0,1});
    (void)sim.step_transactional();
    const auto pending=sim.model(router).output();
    rejects([&] { (void)sim.step_transactional(); });
    require(dynamic_cast<const FailingReceiver&>(sim.model(sink)).ids.empty(),"downstream mutation survived rollback");
    require(sim.model(router).output().size()==16,"failed downstream erased buffered routes");
    *fail=false;
    const auto retried=*sim.step_transactional();
    require(retried.emissions.size()==pending.size(),"retry changed emission count");
    for (std::size_t i=0;i<pending.size();++i)
        require(retried.emissions[i].port==pending[i].port && retried.emissions[i].value.id==pending[i].value.id,
                "checked retry redrew branch choice");
}

void declarative_reference(const std::string& path) {
    const auto model=ir::load_file(path);
    for (std::uint32_t replication:{0U,9U}) {
        const auto rows=ir::run(model,{},987654321,17,replication);
        std::vector<double> completions;
        std::vector<bool> choices;
        double initial=0, branches[2]={0,0};
        for (const auto& entity:model.process->schedule) {
            initial=std::max(initial,entity.arrived_at)+entity.service_duration;
            const bool choice=rng::uniform_open(rng::draw(987654321,{17,replication,entity.id,0,301,0})[0]) < .35;
            const auto index=choice ? 0 : 1;
            branches[index]=std::max(branches[index],initial)+entity.service_duration*(choice ? 1 : 2);
            completions.push_back(branches[index]); choices.push_back(choice);
        }
        for (const auto& row:rows) {
            double received=0, matched=0, completed=0, cycle=0;
            initial=0;
            for (std::size_t i=0;i<model.process->schedule.size();++i) {
                const auto& entity=model.process->schedule[i];
                initial=std::max(initial,entity.arrived_at)+entity.service_duration;
                if (initial<=row.time) { ++received; if (choices[i]) ++matched; }
                if (completions[i]<=row.time) { ++completed; cycle+=completions[i]-entity.arrived_at; }
            }
            const std::map<std::string,double> expected{{"received",received},{"matched",matched},
                {"otherwise",received-matched},{"completed",completed},{"cycle_total",cycle}};
            require(std::abs(row.value-expected.at(row.output_id))<1e-10,"declarative probability path differs from recurrence");
        }
    }
}
}
int main(int argc,char** argv) {
    try {
        require(argc==2,"expected probability fixture");
        atomic_contract(); downstream_rollback(); declarative_reference(argv[1]);
        std::cout<<"Probabilistic routing: addressing, endpoints, permutations, confluence, checked retry and IR recurrence passed\n";
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}

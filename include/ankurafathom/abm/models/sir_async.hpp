#pragma once

#include "ankurafathom/abm/async_population.hpp"
#include "ankurafathom/abm/models/sir.hpp"

namespace ankurafathom::abm::models {

// Direct Gillespie process using the native transactional asynchronous calendar.
// All hazards are recomputed after each event; this does not extend statecharts.
// Contracts: docs/SEMANTICS.md, continuous-time network and well-mixed SIR sections.
class AsyncSIR {
public:
    using Store=SIR::Store;
    using Population=AsyncPopulation<SIR::Agent>;
    struct Parameters { double infection_rate=.4,recovery_rate=.2; };
    struct Draws {
        std::uint64_t seed=0;
        std::uint32_t scenario=0,replication=0,waiting_stream=1401,selection_stream=1402;
    };
    using Variates=std::array<double,2>; // Open (0,1): waiting-time and event-selection draws.
    using Script=std::vector<Variates>;
    struct Event {
        double time;
        std::uint64_t agent;
        std::int64_t before,after;
        std::uint64_t generation;
        bool operator==(const Event&)const=default;
    };
    AsyncSIR(Parameters parameters,std::vector<std::int64_t> states,CsrNetwork network,Draws draws,
             std::uint64_t first_id=0,std::optional<Script> script=std::nullopt)
        :population_(initial(parameters,states,std::move(network),draws,first_id,std::move(script))) {}
    // Implicit complete mixing: susceptible hazard (beta/N)*I, including recovered in N.
    // Here beta is a mass-action coefficient; the graph constructor takes per-edge beta.
    // Individual records/selection are retained. No CSR graph is stored.
    static AsyncSIR well_mixed(Parameters parameters,std::vector<std::int64_t> states,Draws draws,
                              std::uint64_t first_id=0,std::optional<Script> script=std::nullopt) {
        return AsyncSIR(initial(parameters,states,std::nullopt,draws,first_id,std::move(script)));
    }
    bool is_well_mixed()const noexcept { return !store().network(); }
    const Store& store()const noexcept { return population_.store(); }
    double time()const noexcept { return population_.now(); }
    double next_time()const noexcept { return population_.next_time(); }
    std::uint64_t events()const noexcept { return events_; }
    std::size_t pending_count()const noexcept { return population_.pending_count(); }
    std::vector<std::int64_t> states()const {
        std::vector<std::int64_t> result;
        for(auto id=store().first_id();id<store().next_id();++id) result.push_back(state(store(),id));
        return result;
    }
    SIR::Counts counts()const {
        SIR::Counts c;
        for(auto value:states()) {
            if(value==0) ++c.susceptible;
            else if(value==1) ++c.infected;
            else ++c.recovered;
        }
        return c;
    }
    std::optional<Event> step() {
        if(!std::isfinite(next_time())) return std::nullopt;
        auto candidate=population_;
        const auto trace=candidate.step();
        if(trace.size()!=1) throw std::logic_error("SIR calendar must contain one strictly future event");
        const auto& timer=trace.front();
        const Event event{timer.time,timer.agent.id,state(store(),timer.agent.id),state(candidate.store(),timer.agent.id),timer.generation};
        if(event.generation!=events_ || event.after!=event.before+1) throw std::logic_error("SIR event history invariant");
        population_=std::move(candidate); ++events_;
        return event;
    }
    // Whole requested horizon is transactional, including pending event and RNG generation.
    std::vector<Event> run_until(double horizon) {
        if(!std::isfinite(horizon) || horizon<time()) throw std::invalid_argument("invalid SIR horizon");
        auto candidate=*this;
        std::vector<Event> trace;
        while(candidate.next_time()<=horizon) trace.push_back(*candidate.step());
        candidate.population_.run_until(horizon);
        *this=std::move(candidate);
        return trace;
    }
private:
    explicit AsyncSIR(Population population):population_(std::move(population)) {}
    static std::int64_t state(const Store& s,std::uint64_t id) {
        return std::get<std::int64_t>(s.field({0,id},0));
    }
    static void validate(const Store& s,std::size_t n) {
        if(s.size()!=n || s.active_count()!=n || (s.network() && s.network()->directed()))
            throw std::logic_error("async SIR membership/network invariant");
        for(auto id=s.first_id();id<s.next_id();++id)
            if(state(s,id)<0 || state(s,id)>2) throw std::invalid_argument("invalid async SIR state");
    }
    static double mixed_infection_rate(const Store& s,Parameters p) {
        if(s.size()==0) return 0;
        std::size_t infected=0;
        for(auto id=s.first_id();id<s.next_id();++id) if(state(s,id)==1) ++infected;
        return (p.infection_rate/static_cast<double>(s.size()))*static_cast<double>(infected);
    }
    static double rate(const Store& s,std::uint64_t id,Parameters p,std::optional<double> common=std::nullopt) {
        const auto value=state(s,id);
        if(value==2) return 0;
        if(value==1) return p.recovery_rate;
        if(!s.network()) return common ? *common : mixed_infection_rate(s,p);
        std::size_t infectious=0;
        for(auto other:s.network()->neighbors(id)) if(state(s,other)==1) ++infectious;
        return p.infection_rate*static_cast<double>(infectious);
    }
    static std::optional<Population::Schedule> next(const Store& s,Parameters p,Draws d,
                                                    const std::optional<Script>& script,double now,std::uint64_t generation) {
        std::vector<double> rates;
        double total=0;
        const auto common=s.network() ? std::optional<double>{} : mixed_infection_rate(s,p);
        for(auto id=s.first_id();id<s.next_id();++id) {
            const auto r=rate(s,id,p,common); rates.push_back(r); total+=r;
            if(!std::isfinite(total)) throw std::overflow_error("async SIR total hazard overflow");
        }
        if(total==0) return std::nullopt;
        if(generation>65535) throw std::out_of_range("async SIR event generation exceeds draw address");
        Variates u;
        if(script) {
            if(generation>=script->size()) throw std::out_of_range("async SIR script exhausted");
            u=script->at(generation);
        } else {
            u={rng::uniform_open(rng::draw(d.seed,{d.scenario,d.replication,s.first_id(),static_cast<std::uint32_t>(generation),d.waiting_stream,0})[0]),
               rng::uniform_open(rng::draw(d.seed,{d.scenario,d.replication,s.first_id(),static_cast<std::uint32_t>(generation),d.selection_stream,0})[0])};
        }
        for(auto value:u) if(!std::isfinite(value) || value<=0 || value>=1)
            throw std::invalid_argument("async SIR variate outside (0,1)");
        const auto deadline=now-std::log(u[0])/total;
        if(!std::isfinite(deadline) || deadline<=now) throw std::overflow_error("async SIR event clock cannot advance");
        // Rounding a product to total must not select beyond the final positive rate.
        const auto target=std::min(u[1]*total,std::nextafter(total,0.));
        double cumulative=0;
        for(std::size_t i=0;i<rates.size();++i) {
            cumulative+=rates[i];
            if(target<cumulative) return Population::Schedule{deadline,{0,s.first_id()+i},"sir",generation};
        }
        throw std::logic_error("async SIR event selection failed");
    }
    static Population initial(Parameters p,const std::vector<std::int64_t>& states,std::optional<CsrNetwork> network,
                              Draws d,std::uint64_t first,std::optional<Script> script) {
        if(!std::isfinite(p.infection_rate) || p.infection_rate<0 || !std::isfinite(p.recovery_rate) || p.recovery_rate<0)
            throw std::invalid_argument("async SIR requires finite nonnegative rates");
        if(states.size()>1000000 || (network && network->directed())) throw std::invalid_argument("async SIR requires bounded population and undirected graph");
        if(d.waiting_stream==d.selection_stream) throw std::invalid_argument("async SIR streams overlap");
        for(auto stream:{d.waiting_stream,d.selection_stream}) (void)rng::pack_counter({d.scenario,d.replication,first,0,stream,0});
        Store s(0,{{"state",des::FieldKind::integer}},first);
        std::vector<Store::Record> records;
        for(auto value:states) records.push_back({value});
        s.spawn_many(records);
        if(network) s.configure_network(std::move(*network));
        validate(s,states.size());
        const auto timer=next(s,p,d,script,0,0);
        Population result(std::move(s),[p,d,script=std::move(script)](const Population::Timer& timer,const Store& before) {
            const auto value=state(before,timer.agent.id);
            if(timer.kind!="sir" || value==2 || rate(before,timer.agent.id,p)<=0)
                throw std::logic_error("invalid async SIR pending event");
            auto after=before;
            after.update(timer.agent,{value+1});
            Population::Effects effects;
            effects.updates.push_back({timer.agent,{value+1}});
            if(auto successor=next(after,p,d,script,timer.time,timer.generation+1)) effects.schedules.push_back(*successor);
            return effects;
        },1,[n=states.size()](const Store& candidate) { validate(candidate,n); });
        if(timer) result.schedule(*timer);
        return result;
    }
    Population population_;
    std::uint64_t events_=0;
};

} // namespace ankurafathom::abm::models

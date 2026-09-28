#pragma once
#include "ankurafathom/abm/population_store.hpp"
#include "ankurafathom/abm/topic_broker.hpp"
#include "ankurafathom/des/resource_pool.hpp"
#include <functional>

namespace ankurafathom::hybrid {

// Contract: docs/SEMANTICS.md, "Typed agent-backed pool and broker writeback".
template<class Tag>
class TypedAgentPool {
public:
    using Store=abm::PopulationStore<Tag>;
    using Reference=typename Store::Reference;
    using Record=typename Store::Record;
    using Capacity=std::function<std::size_t(Reference,const Store&)>;
    using Phase=std::function<Record(Reference,const Store&)>;
    struct Notification {
        std::uint64_t request_id;
        Reference agent;
        std::size_t units;
        bool assigned;
        bool operator==(const Notification&)const=default;
    };
    using Handler=std::function<Record(const Notification&,const Store&)>;
    struct Share {
        Reference agent;
        std::size_t units;
        bool operator==(const Share&)const=default;
    };
    struct Change {
        double time=0;
        std::vector<des::Release> releases;
        std::vector<std::pair<Reference,Record>> updates;
        std::vector<Reference> retirements;
        std::vector<Record> births;
        bool run_phases=false;
        std::vector<des::Seize> requests;
    };
    struct Result {
        double time;
        Store snapshot;
        std::vector<Reference> born;
        std::vector<des::Grant> grants;
        std::vector<Notification> notifications;
    };
    struct Statistics {
        double capacity_time=0,allocated_time=0,waiting_request_time=0,live_agent_time=0;
        double utilization()const noexcept { return capacity_time ? allocated_time/capacity_time : 0; }
        bool operator==(const Statistics&)const=default;
    };
    TypedAgentPool(Store store,std::size_t allocated_field,Capacity capacity,std::size_t max_request_units,
                   des::QueueDiscipline discipline=des::QueueDiscipline::fifo,Handler handler={},
                   std::size_t topic_capacity=1000000)
        :field_(allocated_field),capacity_(std::move(capacity)),handler_(std::move(handler)) {
        if(!capacity_ || field_>=store.schema().size() || store.schema()[field_].kind!=des::FieldKind::integer)
            throw std::invalid_argument("invalid typed agent pool capacity or allocation field");
        for(const auto ref:references(store)) if(allocated(store,ref)!=0)
            throw std::invalid_argument("initial agent pool allocation must be zero");
        const auto total=total_capacity(capacities(store));
        state_=std::make_unique<State>(std::move(store),total,max_request_units,discipline,topic_capacity);
    }
    TypedAgentPool(const TypedAgentPool& other)
        :field_(other.field_),capacity_(other.capacity_),handler_(other.handler_),phases_(other.phases_),
         state_(std::make_unique<State>(*other.state_)) {}
    TypedAgentPool(TypedAgentPool&&)=default;
    TypedAgentPool& operator=(TypedAgentPool&&)=default;
    TypedAgentPool& operator=(const TypedAgentPool& other) {
        if(this!=&other) { TypedAgentPool copy(other);*this=std::move(copy); }return *this;
    }
    void add_phase(Phase phase) {
        if(busy_ || state_->started) throw std::logic_error("agent pool phases are frozen after execution");
        if(!phase) throw std::invalid_argument("empty agent pool phase");
        phases_.push_back(std::move(phase));
    }
    const Store& store()const noexcept { return state_->store; }
    const des::ResourcePool& pool()const noexcept { return state_->pool; }
    const auto& assignments()const noexcept { return state_->assignments; }
    double time()const noexcept { return state_->time; }
    Statistics statistics(double horizon)const {
        if(!std::isfinite(horizon) || horizon<time()) throw std::invalid_argument("invalid agent pool statistics horizon");
        auto value=state_->statistics;accumulate(value,*state_,horizon-time());return value;
    }
    Result transact(Change change) {
        if(busy_) throw std::logic_error("reentrant agent pool transaction");
        struct Guard { bool& busy;explicit Guard(bool& b):busy(b) { busy=true; }~Guard() { busy=false; } } guard(busy_);
        if(!std::isfinite(change.time) || change.time<time()) throw std::invalid_argument("invalid agent pool transaction time");
        auto next=std::make_unique<State>(*state_);
        accumulate(next->statistics,*state_,change.time-time());
        std::sort(change.releases.begin(),change.releases.end(),[](const auto& a,const auto& b) { return a.request_id<b.request_id; });
        std::sort(change.requests.begin(),change.requests.end(),[](const auto& a,const auto& b) { return a.request_id<b.request_id; });
        std::vector<Notification> released,assigned,notifications;
        for(const auto release:change.releases) {
            const auto found=next->assignments.find(release.request_id);
            if(found==next->assignments.end()) throw std::invalid_argument("agent pool release is inactive or duplicated");
            for(const auto& share:found->second) released.push_back({release.request_id,share.agent,share.units,false});
            next->assignments.erase(found);
        }
        write_allocations(*next);
        deliver(*next,released,"unassigned",notifications);
        std::set<std::uint64_t> changed;
        for(const auto& [ref,record]:change.updates) {
            preserve_allocation(next->store,ref,record);
            if(!changed.insert(ref.id).second) throw std::invalid_argument("duplicate agent pool update");
        }
        next->store.update_many(change.updates);
        for(const auto ref:change.retirements) {
            if(!changed.insert(ref.id).second || allocated(next->store,ref)!=0)
                throw std::invalid_argument("agent pool retirement overlaps update or active assignment");
            next->store.retire(ref);
        }
        for(const auto& record:change.births) if(record.size()!=next->store.schema().size() ||
            !std::holds_alternative<std::int64_t>(record[field_]) || std::get<std::int64_t>(record[field_])!=0)
            throw std::invalid_argument("newborn agent pool allocation must be zero");
        auto born=next->store.spawn_many(change.births);
        validate_allocations(*next);
        if(change.run_phases) for(const auto& phase:phases_) {
            std::vector<std::pair<Reference,Record>> updates;
            for(const auto ref:references(next->store)) {
                auto record=phase(ref,next->store);preserve_allocation(next->store,ref,record);
                updates.emplace_back(ref,std::move(record));
            }
            next->store.update_many(updates);validate_allocations(*next);
        }
        std::vector<devs::Input<des::ResourceMessage>> bag;
        for(const auto release:change.releases) bag.push_back({0,0,des::ResourceMessage{release}});
        bag.push_back({0,2,des::ResourceMessage{des::SetCapacity{total_capacity(capacities(next->store))}}});
        for(const auto request:change.requests) bag.push_back({0,0,des::ResourceMessage{request}});
        next->pool.external_transition(change.time-time(),bag);
        std::vector<des::Grant> grants;
        auto remaining=capacities(next->store);
        for(const auto ref:references(next->store)) remaining.at(ref.id)-=allocated(next->store,ref);
        for(const auto& output:next->pool.output()) {
            const auto grant=std::get<des::Grant>(output.value);grants.push_back(grant);
            auto units=grant.units;std::vector<Share> shares;
            for(auto& [id,available]:remaining) {
                const auto take=std::min(units,available);if(!take) continue;
                const Reference ref{next->store.store_id(),id};
                shares.push_back({ref,take});assigned.push_back({grant.request_id,ref,take,true});
                available-=take;units-=take;if(!units) break;
            }
            if(units || !next->assignments.emplace(grant.request_id,std::move(shares)).second)
                throw std::logic_error("agent pool grant cannot be mapped to workforce");
        }
        if(!grants.empty()) next->pool.internal_transition();
        write_allocations(*next);deliver(*next,assigned,"assigned",notifications);
        validate_allocations(*next);
        std::size_t units=0;
        for(const auto& [request,shares]:next->assignments) {
            (void)request;for(const auto& share:shares) units=checked_add(units,share.units);
        }
        if(next->pool.capacity()!=total_capacity(capacities(next->store)) || next->pool.allocated_units()!=units ||
           next->pool.allocated()!=next->assignments.size() || next->pool.available()!=next->pool.capacity()-units)
            throw std::logic_error("agent pool resource accounting mismatch");
        next->time=change.time;next->started=true;
        Result result{change.time,next->store,std::move(born),std::move(grants),std::move(notifications)};
        static_assert(std::is_nothrow_move_constructible_v<Result>);
        state_.swap(next);return result;
    }
private:
    using Broker=abm::TopicBroker<Notification>;
    using Capacities=std::map<std::uint64_t,std::size_t>;
    struct State {
        Store store;
        des::ResourcePool pool;
        std::map<std::uint64_t,std::vector<Share>> assignments;
        Broker broker;
        Statistics statistics;
        double time=0;
        bool started=false;
        State(Store value,std::size_t capacity,std::size_t max_request,des::QueueDiscipline discipline,std::size_t bound)
            :store(std::move(value)),pool(capacity,max_request,discipline) {
            broker.declare_topic("assigned",bound);broker.declare_topic("unassigned",bound);
        }
    };
    static std::vector<Reference> references(const Store& store) {
        std::vector<Reference> refs;
        for(auto id=store.first_id();id<store.next_id();++id) if(store.alive({store.store_id(),id})) refs.push_back({store.store_id(),id});
        return refs;
    }
    static std::size_t checked_add(std::size_t a,std::size_t b) {
        if(b>std::numeric_limits<std::size_t>::max()-a) throw std::overflow_error("agent pool integer capacity overflow");
        return a+b;
    }
    Capacities capacities(const Store& store)const {
        Capacities result;
        for(const auto ref:references(store)) {
            const auto n=capacity_(ref,store);
            if(n>static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                throw std::overflow_error("agent capacity exceeds allocation field range");
            result.emplace(ref.id,n);
        }
        (void)total_capacity(result);return result;
    }
    static std::size_t total_capacity(const Capacities& values) {
        std::size_t total=0;for(const auto& [id,n]:values) { (void)id;total=checked_add(total,n); }return total;
    }
    std::size_t allocated(const Store& store,Reference ref)const {
        const auto value=std::get<std::int64_t>(store.field(ref,field_));
        if(value<0) throw std::invalid_argument("negative agent allocation");
        return static_cast<std::size_t>(value);
    }
    void preserve_allocation(const Store& store,Reference ref,const Record& record)const {
        if(record.size()!=store.schema().size() || record[field_]!=store.field(ref,field_))
            throw std::invalid_argument("agent pool rule changed reserved allocation field");
    }
    Capacities ledger_allocations(const State& state)const {
        Capacities values;for(const auto ref:references(state.store)) values.emplace(ref.id,0);
        for(const auto& [request,shares]:state.assignments) {
            (void)request;
            for(const auto& share:shares) {
                if(!share.units || !state.store.alive(share.agent)) throw std::logic_error("invalid agent pool assignment reference");
                auto& n=values.at(share.agent.id);n=checked_add(n,share.units);
            }
        }
        return values;
    }
    void write_allocations(State& state)const {
        const auto before=capacities(state.store),values=ledger_allocations(state);
        std::vector<std::pair<Reference,Record>> updates;
        for(const auto& [id,n]:values) {
            if(n>before.at(id)) throw std::invalid_argument("agent capacity would revoke an assignment");
            const Reference ref{state.store.store_id(),id};auto record=state.store.record(ref);
            record[field_]=static_cast<std::int64_t>(n);updates.emplace_back(ref,std::move(record));
        }
        state.store.update_many(updates);
        if(capacities(state.store)!=before) throw std::invalid_argument("agent capacity depends on reserved allocation");
    }
    void validate_allocations(const State& state)const {
        const auto caps=capacities(state.store),values=ledger_allocations(state);
        for(const auto& [id,n]:values) if(n>caps.at(id) || n!=allocated(state.store,{state.store.store_id(),id}))
            throw std::invalid_argument("agent pool update invalidates active assignment");
    }
    void deliver(State& state,const std::vector<Notification>& values,const std::string& topic,
                 std::vector<Notification>& delivered)const {
        std::vector<typename Broker::Publication> publications;
        for(const auto& value:values) publications.push_back({topic,{value.request_id,value.agent.id,value.agent.id,value}});
        state.broker.publish_many(publications);state.broker.flush();
        const auto before=capacities(state.store);
        for(const auto& message:state.broker.visible(topic)) {
            const auto& value=message.payload;
            if(handler_) {
                auto record=handler_(value,state.store);preserve_allocation(state.store,value.agent,record);
                state.store.update(value.agent,record);
                if(capacities(state.store)!=before) throw std::invalid_argument("agent pool broker handler changed capacity");
            }
            delivered.push_back(value);
        }
    }
    static void accumulate(Statistics& value,const State& state,double elapsed) {
        const auto add=[&](double& area,std::size_t level) {
            const auto next=area+elapsed*static_cast<double>(level);
            if(!std::isfinite(next)) throw std::overflow_error("agent pool statistics overflow");
            area=next;
        };
        add(value.capacity_time,state.pool.capacity());add(value.allocated_time,state.pool.allocated_units());
        add(value.waiting_request_time,state.pool.waiting());add(value.live_agent_time,state.store.active_count());
    }
    std::size_t field_;
    Capacity capacity_;
    Handler handler_;
    std::vector<Phase> phases_;
    std::unique_ptr<State> state_;
    bool busy_=false;
};
} // namespace ankurafathom::hybrid

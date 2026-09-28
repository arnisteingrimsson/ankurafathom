#pragma once

#include "ankurafathom/abm/population_store.hpp"
#include "ankurafathom/sd/model.hpp"
#include <set>
#include <type_traits>

namespace ankurafathom::hybrid {

// Simultaneous Euler coupling with staged discrete changes. Records remain the sole
// authoritative agent state; the temporary SD model exists only during a step.
template<class Tag>
class AgentStocks {
public:
    using Store=abm::PopulationStore<Tag>;
    using Reference=typename Store::Reference;
    using State=sd::Model::State;
    struct Field { std::size_t index; bool nonnegative=true; };
    struct GlobalStock { std::string name; double initial; bool nonnegative=true; };
    struct Endpoint {
        enum class Kind { boundary, agent, global };
        Kind kind;
        std::size_t index=0;
        static Endpoint boundary() { return {Kind::boundary,0}; }
        // Field index in the population schema, not in the binding list.
        static Endpoint agent(std::size_t field) { return {Kind::agent,field}; }
        static Endpoint global(std::size_t stock) { return {Kind::global,stock}; }
    };
    using AgentRate=std::function<double(Reference,const Store&,const State&,double)>;
    using GlobalRate=std::function<double(const Store&,const State&,double)>;
    struct FieldUpdate { Reference agent;std::size_t field;typename Store::Value value; };
    struct Pulse { Endpoint endpoint;std::optional<Reference> agent;double amount; };
    struct Change {
        std::vector<FieldUpdate> updates;
        std::vector<Pulse> pulses;
        std::vector<Reference> retirements;
        std::vector<typename Store::Record> births;
    };
    struct Receipt {
        double time;
        std::vector<Reference> born,retired;
        // Agent vectors follow binding order; global vectors follow stock order.
        std::vector<double> birth_amounts,retired_amounts,agent_pulses,global_pulses;
    };

    AgentStocks(Store store,std::vector<Field> fields,std::vector<GlobalStock> globals={})
        :store_(std::move(store)),fields_(std::move(fields)),globals_(std::move(globals)) {
        std::set<std::size_t> seen;
        for(const auto& field:fields_) {
            if(field.index>=store_.schema().size() ||
               store_.schema()[field.index].kind!=des::FieldKind::real ||
               !seen.insert(field.index).second)
                throw std::invalid_argument("agent stock requires a unique real field");
            for(auto id=store_.first_id();id<store_.next_id();++id) {
                const Reference ref{store_.store_id(),id};
                if(store_.alive(ref) && field.nonnegative &&
                   std::get<double>(store_.field(ref,field.index))<0)
                    throw std::invalid_argument("negative initial agent stock");
            }
        }
        if(fields_.empty()) throw std::invalid_argument("agent stock bindings must not be empty");
        sd::Model validation;
        for(const auto& stock:globals_) {
            validation.add_stock(stock.name,stock.initial,stock.nonnegative);
            state_.push_back(stock.initial);
        }
    }

    const Store& store()const noexcept { return store_; }
    const State& global_state()const noexcept { return state_; }
    double time()const noexcept { return time_; }

    // This helper can also be used inside a pure rate callback on its snapshot.
    static double sum(const Store& store,std::size_t field) {
        if(field>=store.schema().size() || store.schema()[field].kind!=des::FieldKind::real)
            throw std::invalid_argument("agent sum requires a real field");
        double result=0;
        for(auto id=store.first_id();id<store.next_id();++id) {
            const Reference ref{store.store_id(),id};
            if(store.alive(ref)) {
                result+=std::get<double>(store.field(ref,field));
                if(!std::isfinite(result)) throw std::overflow_error("agent sum overflow");
            }
        }
        return result;
    }
    double sum(std::size_t field)const { return sum(store_,field); }

    void add_agent_flow(Endpoint source,Endpoint destination,AgentRate rate,bool nonnegative=true) {
        configurable(); validate(source,true); validate(destination,true);
        if(!rate || (source.kind!=Endpoint::Kind::agent && destination.kind!=Endpoint::Kind::agent))
            throw std::invalid_argument("per-agent flow requires an agent endpoint and rate");
        agent_flows_.push_back({source,destination,std::move(rate),nonnegative});
    }
    void add_global_flow(Endpoint source,Endpoint destination,GlobalRate rate,bool nonnegative=true) {
        configurable(); validate(source,false); validate(destination,false);
        if(!rate || (source.kind==Endpoint::Kind::boundary && destination.kind==Endpoint::Kind::boundary))
            throw std::invalid_argument("global flow requires a stock endpoint and rate");
        global_flows_.push_back({source,destination,std::move(rate),nonnegative});
    }

    void step(double dt) { integrate(dt,time_+dt); }
    void step_to(double target) {
        if(!std::isfinite(target) || target<=time_) throw std::invalid_argument("agent stock target must advance finitely");
        integrate(target-time_,target);
    }
    Receipt apply(const Change& change) {
        if(busy_) throw std::logic_error("recursive agent stock mutation");
        Busy guard(busy_);
        auto next_store=store_;auto next_state=state_;
        Receipt result{time_,{},{},std::vector<double>(fields_.size()),std::vector<double>(fields_.size()),
                       std::vector<double>(fields_.size()),std::vector<double>(globals_.size())};
        std::set<std::pair<std::uint64_t,std::size_t>> writes;
        std::map<std::uint64_t,typename Store::Record> records;
        for(const auto& update:change.updates) {
            if(update.field>=store_.schema().size() || bound(update.field) || !store_.alive(update.agent) ||
               !writes.emplace(update.agent.id,update.field).second)
                throw std::invalid_argument("invalid, duplicate or stock-owned agent field update");
            auto [record,inserted]=records.try_emplace(update.agent.id,store_.record(update.agent));(void)inserted;
            record->second[update.field]=update.value;
        }
        std::vector<std::pair<Reference,typename Store::Record>> updates;
        for(auto& [id,record]:records) updates.emplace_back(Reference{store_.store_id(),id},std::move(record));
        next_store.update_many(updates);updates.clear();records.clear();
        std::map<std::pair<std::uint64_t,std::size_t>,double> agent_net;
        for(const auto& pulse:change.pulses) {
            if(!std::isfinite(pulse.amount)) throw std::invalid_argument("nonfinite agent stock pulse");
            if(pulse.endpoint.kind==Endpoint::Kind::agent) {
                const auto index=bound(pulse.endpoint.index);
                if(!index || !pulse.agent || !store_.alive(*pulse.agent)) throw std::invalid_argument("invalid agent stock pulse reference or field");
                add_finite(agent_net[{pulse.agent->id,*index}],pulse.amount);
            } else if(pulse.endpoint.kind==Endpoint::Kind::global) {
                if(pulse.agent || pulse.endpoint.index>=globals_.size()) throw std::invalid_argument("invalid global stock pulse target");
                add_finite(result.global_pulses[pulse.endpoint.index],pulse.amount);
            } else throw std::invalid_argument("pulse requires an agent or global stock");
        }
        for(const auto& [key,amount]:agent_net) {
            const auto [id,index]=key;const Reference ref{store_.store_id(),id};
            auto [record,inserted]=records.try_emplace(id,next_store.record(ref));(void)inserted;
            auto value=std::get<double>(record->second[fields_[index].index]);add_finite(value,amount);
            if(fields_[index].nonnegative && value<0) throw std::invalid_argument("agent stock pulse violates domain");
            record->second[fields_[index].index]=value;add_finite(result.agent_pulses[index],amount);
        }
        for(std::size_t i=0;i<globals_.size();++i) {
            add_finite(next_state[i],result.global_pulses[i]);
            if(globals_[i].nonnegative && next_state[i]<0) throw std::invalid_argument("global stock pulse violates domain");
        }
        for(auto& [id,record]:records) updates.emplace_back(Reference{store_.store_id(),id},std::move(record));
        next_store.update_many(updates);
        std::set<std::uint64_t> retired;
        for(const auto ref:change.retirements) {
            if(!next_store.alive(ref) || !retired.insert(ref.id).second) throw std::invalid_argument("inactive or duplicate agent stock retirement");
            for(std::size_t i=0;i<fields_.size();++i) add_finite(result.retired_amounts[i],std::get<double>(next_store.field(ref,fields_[i].index)));
            next_store.retire(ref);result.retired.push_back(ref);
        }
        result.born=next_store.spawn_many(change.births);
        for(const auto ref:result.born) for(std::size_t i=0;i<fields_.size();++i) {
            const auto value=std::get<double>(next_store.field(ref,fields_[i].index));
            if(fields_[i].nonnegative && value<0) throw std::invalid_argument("negative newborn agent stock");
            add_finite(result.birth_amounts[i],value);
        }
        static_assert(std::is_nothrow_move_assignable_v<Store> && std::is_nothrow_move_assignable_v<State>);
        static_assert(std::is_nothrow_move_constructible_v<Receipt>);
        store_=std::move(next_store);state_=std::move(next_state);started_=true;
        return result;
    }

private:
    struct Busy {
        bool& value;
        explicit Busy(bool& busy):value(busy) { value=true; }
        ~Busy() { value=false; }
    };
    static void add_finite(double& value,double amount) {
        const auto next=value+amount;
        if(!std::isfinite(next)) throw std::overflow_error("agent stock discrete amount overflow");
        value=next;
    }
    std::optional<std::size_t> bound(std::size_t field)const {
        for(std::size_t i=0;i<fields_.size();++i) if(fields_[i].index==field) return i;
        return {};
    }
    void integrate(double dt,double target) {
        if(busy_) throw std::logic_error("recursive agent stock mutation");
        if(!std::isfinite(dt) || dt<=0 || !std::isfinite(target) || target<=time_)
            throw std::invalid_argument("agent stock time must advance finitely");
        Busy guard(busy_);

        sd::Model candidate;
        // Global indexes in the temporary model equal the public indexes.
        for(std::size_t i=0;i<globals_.size();++i)
            candidate.add_stock("global/"+globals_[i].name,state_[i],globals_[i].nonnegative);
        std::vector<Reference> agents;
        std::vector<std::vector<std::size_t>> indexes;
        for(auto id=store_.first_id();id<store_.next_id();++id) {
            const Reference ref{store_.store_id(),id};
            if(!store_.alive(ref)) continue;
            agents.push_back(ref);
            std::vector<std::size_t> row;
            for(const auto& field:fields_)
                row.push_back(candidate.add_stock("agent/"+std::to_string(id)+"/"+std::to_string(field.index),
                    std::get<double>(store_.field(ref,field.index)),field.nonnegative));
            indexes.push_back(std::move(row));
        }
        const auto endpoint=[&](Endpoint e,const std::vector<std::size_t>& row) {
            if(e.kind==Endpoint::Kind::boundary) return sd::Model::boundary;
            if(e.kind==Endpoint::Kind::global) return e.index;
            for(std::size_t i=0;i<fields_.size();++i) if(fields_[i].index==e.index) return row.at(i);
            throw std::logic_error("unbound agent stock");
        };
        const auto add=[&](Endpoint source,Endpoint destination,const auto& row,double rate,bool nonnegative) {
            if(!std::isfinite(rate) || (nonnegative && rate<0))
                throw std::domain_error("invalid agent stock flow rate");
            candidate.add_flow(endpoint(source,row),endpoint(destination,row),
                [rate](const State&,double) { return rate; },nonnegative);
        };
        for(std::size_t i=0;i<agents.size();++i)
            for(const auto& flow:agent_flows_)
                add(flow.source,flow.destination,indexes[i],flow.rate(agents[i],store_,state_,time_),flow.nonnegative);
        for(const auto& flow:global_flows_)
            add(flow.source,flow.destination,std::vector<std::size_t>{},flow.rate(store_,state_,time_),flow.nonnegative);
        candidate.step(time_,dt,sd::Integrator::euler);

        std::vector<std::pair<Reference,typename Store::Record>> updates;
        for(std::size_t i=0;i<agents.size();++i) {
            auto record=store_.record(agents[i]);
            for(std::size_t j=0;j<fields_.size();++j) record[fields_[j].index]=candidate.state()[indexes[i][j]];
            updates.emplace_back(agents[i],std::move(record));
        }
        auto next_store=store_;
        next_store.update_many(updates);
        State next_state(candidate.state().begin(),candidate.state().begin()+globals_.size());
        static_assert(std::is_nothrow_move_assignable_v<Store>);
        static_assert(std::is_nothrow_move_assignable_v<State>);
        store_=std::move(next_store);
        state_=std::move(next_state);
        time_=target;
        started_=true;
    }

private:
    template<class Rate> struct Flow { Endpoint source,destination; Rate rate; bool nonnegative; };
    void configurable()const {
        if(busy_ || started_) throw std::logic_error("agent stock flows are frozen during/after integration");
    }
    void validate(Endpoint e,bool per_agent)const {
        switch(e.kind) {
            case Endpoint::Kind::boundary:
                if(e.index!=0) throw std::invalid_argument("invalid boundary index");
                return;
            case Endpoint::Kind::global:
                if(e.index<globals_.size()) return;
                break;
            case Endpoint::Kind::agent:
                if(per_agent)
                    for(const auto& field:fields_) if(field.index==e.index) return;
                break;
            default: break;
        }
        throw std::invalid_argument("invalid agent stock flow endpoint");
    }
    Store store_;
    std::vector<Field> fields_;
    std::vector<GlobalStock> globals_;
    State state_;
    std::vector<Flow<AgentRate>> agent_flows_;
    std::vector<Flow<GlobalRate>> global_flows_;
    double time_=0;
    bool started_=false,busy_=false;
};
} // namespace ankurafathom::hybrid

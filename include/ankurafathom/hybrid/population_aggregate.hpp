#pragma once
#include "ankurafathom/abm/population_store.hpp"
#include "ankurafathom/devs/simulator.hpp"
#include "ankurafathom/hybrid/scalar_publication.hpp"
#include <functional>
#include <set>
#include <variant>

namespace ankurafathom::hybrid {

template<class Tag> struct PopulationSnapshot {
    double time;
    std::uint64_t revision;
    abm::PopulationStore<Tag> population;
};
template<class Tag>
using AggregateMessage=std::variant<PopulationSnapshot<Tag>,ScalarPublication>;

template<class Tag>
class PopulationReduction {
public:
    using Store=abm::PopulationStore<Tag>;
    using Reference=typename Store::Reference;
    using Projection=std::function<double(Reference,const Store&)>;
    using Predicate=std::function<bool(Reference,const Store&)>;
    enum class Kind { sum,mean,count,minimum,maximum };

    PopulationReduction(std::string name,Kind kind,Projection projection={},Predicate select={},
                        std::optional<double> empty={})
        :name_(std::move(name)),kind_(kind),projection_(std::move(projection)),
         select_(std::move(select)),empty_(empty) {
        if(name_.empty()) throw std::invalid_argument("aggregate name must not be empty");
        switch(kind_) {
            case Kind::sum: case Kind::mean: case Kind::count: case Kind::minimum: case Kind::maximum: break;
            default: throw std::invalid_argument("unknown aggregate reducer");
        }
        if((kind_==Kind::count)==static_cast<bool>(projection_))
            throw std::invalid_argument("count has no projection; other reducers require one");
        if(empty_ && (!std::isfinite(*empty_) || kind_==Kind::sum || kind_==Kind::count))
            throw std::invalid_argument("empty fallback only applies to mean/min/max and must be finite");
    }
    const std::string& name()const noexcept { return name_; }
    double evaluate(const Store& store)const {
        double value=0;
        std::size_t count=0;
        for(auto id=store.first_id();id<store.next_id();++id) {
            const Reference ref{store.store_id(),id};
            if(!store.alive(ref) || (select_ && !select_(ref,store))) continue;
            ++count; // Native stores have at most 2^48 records: count is exact in double.
            if(kind_==Kind::count) continue;
            const double x=projection_(ref,store);
            if(!std::isfinite(x)) throw std::domain_error("nonfinite aggregate projection");
            switch(kind_) {
                case Kind::sum: value+=x; break;
                case Kind::mean: value=count==1 ? x : std::lerp(value,x,1./static_cast<double>(count)); break;
                case Kind::minimum: value=count==1 ? x : std::min(value,x); break;
                case Kind::maximum: value=count==1 ? x : std::max(value,x); break;
                case Kind::count: break;
            }
            if(!std::isfinite(value)) throw std::overflow_error("aggregate arithmetic overflow");
        }
        if(kind_==Kind::count) return static_cast<double>(count);
        if(count || kind_==Kind::sum) return value;
        if(empty_) return *empty_;
        throw std::domain_error("empty selection has no aggregate value");
    }
private:
    std::string name_;
    Kind kind_;
    Projection projection_;
    Predicate select_;
    std::optional<double> empty_;
};

// Only immutable input snapshots are read. No external population pointer is
// retained, and clone() owns all committed/pending publication state.
template<class Tag,class Message=AggregateMessage<Tag>>
class PopulationAggregate final:public devs::Atomic<Message> {
public:
    using Store=abm::PopulationStore<Tag>;
    using Snapshot=PopulationSnapshot<Tag>;
    using Reduction=PopulationReduction<Tag>;
    static constexpr std::uint32_t input_port=0,output_port=1;
    PopulationAggregate(const Store& prototype,std::vector<Reduction> reductions)
        :store_id_(prototype.store_id()),first_id_(prototype.first_id()),schema_(prototype.schema()),
         reductions_(std::move(reductions)) {
        if(reductions_.empty()) throw std::invalid_argument("aggregate needs at least one reducer");
        std::set<std::string> names;
        for(const auto& reduction:reductions_)
            if(!names.insert(reduction.name()).second) throw std::invalid_argument("duplicate aggregate name");
    }
    std::unique_ptr<devs::Atomic<Message>> clone()const override { return std::make_unique<PopulationAggregate>(*this); }
    double time_advance()const override { return pending_ ? 0 : std::numeric_limits<double>::infinity(); }
    std::optional<double> next_event_time()const override {
        return pending_ ? clock_ : std::numeric_limits<double>::infinity();
    }
    std::vector<devs::PortValue<Message>> output()const override {
        if(!pending_) return {};
        return {{output_port,Message{*last_}}};
    }
    void internal_transition()override {
        if(!pending_) throw std::logic_error("aggregate has no pending publication");
        pending_=false;
    }
    void external_transition(double elapsed,const std::vector<devs::Input<Message>>& bag)override {
        if(bag.size()!=1 || !std::holds_alternative<Snapshot>(bag.front().value))
            throw std::invalid_argument("aggregate requires one population snapshot");
        external_transition_at(std::get<Snapshot>(bag.front().value).time,elapsed,bag);
    }
    void external_transition_at(double time,double elapsed,const std::vector<devs::Input<Message>>& bag)override {
        if(pending_ || !std::isfinite(elapsed) || elapsed<0 || !std::isfinite(time) ||
           time<clock_ || time-clock_!=elapsed)
            throw std::invalid_argument("invalid aggregate timestamp or pending publication");
        accept(time,bag);
    }
    void confluent_transition(const std::vector<devs::Input<Message>>& bag)override {
        if(!pending_) throw std::logic_error("aggregate is not imminent");
        // output() publishes the old committed batch first; accept stages the new one.
        accept(clock_,bag);
    }
    const std::optional<ScalarPublication>& last()const noexcept { return last_; }
    double time()const noexcept { return clock_; }
    std::size_t index(const std::string& name)const {
        for(std::size_t i=0;i<reductions_.size();++i) if(reductions_[i].name()==name) return i;
        throw std::out_of_range("unknown aggregate name");
    }
private:
    void accept(double time,const std::vector<devs::Input<Message>>& bag) {
        if(bag.size()!=1 || bag[0].port!=input_port)
            throw std::invalid_argument("aggregate requires one snapshot on its input port");
        const auto* snapshot=std::get_if<Snapshot>(&bag[0].value);
        if(!snapshot || snapshot->time!=time || (last_ && snapshot->revision<=last_->revision) ||
           (owner_ && *owner_!=bag[0].source))
            throw std::invalid_argument("invalid aggregate snapshot time, revision or owner");
        const auto& population=snapshot->population;
        if(population.store_id()!=store_id_ || population.first_id()!=first_id_ || population.schema()!=schema_)
            throw std::invalid_argument("aggregate population schema or namespace changed");
        ScalarPublication result{time,snapshot->revision,{}};
        for(const auto& reduction:reductions_) result.values.push_back(reduction.evaluate(population));
        last_=std::move(result);
        owner_=bag[0].source;
        clock_=time;
        pending_=true;
    }
    std::uint32_t store_id_;
    std::uint64_t first_id_;
    std::vector<des::EntityField> schema_;
    std::vector<Reduction> reductions_;
    std::optional<std::size_t> owner_;
    std::optional<ScalarPublication> last_;
    double clock_=0;
    bool pending_=false;
};
} // namespace ankurafathom::hybrid

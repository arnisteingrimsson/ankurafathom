#pragma once

#include "ankurafathom/des/process_reference.hpp"
#include "ankurafathom/des/reference_publication.hpp"
#include "ankurafathom/des/resource_pool.hpp"
#include <functional>
#include <optional>

namespace ankurafathom::des {

template<class Tag>
using ResourceProcessMessage=std::variant<EntityToken<Tag>,QueuePull,Seize,Release,Grant,SetCapacity>;

namespace detail {
template<class Tag>
void validate_resource_token(const EntityToken<Tag>& token,std::uint32_t store) {
    if(token.entity.store!=store || token.entity.id>0xFFFFFFFFFFFFULL)
        throw std::invalid_argument("invalid resource token reference");
    std::set<std::uint32_t> pools;
    for(const auto& lease:token.leases)
        if(lease.units==0 || (lease.request_id & 0xFFFFFFFFFFFFULL)!=token.entity.id || !pools.insert(lease.pool).second)
            throw std::invalid_argument("invalid or duplicate resource lease");
}
} // namespace detail

template<class Tag,class Message=ResourceProcessMessage<Tag>>
class ReferenceSeize final:public detail::PublicationAtomic<ReferenceSeize<Tag,Message>,Message> {
    using Token=EntityToken<Tag>;
    friend class detail::PublicationAtomic<ReferenceSeize<Tag,Message>,Message>;
public:
    using Units=std::function<std::size_t(const Token&)>;
    using Priority=std::function<std::int32_t(const Token&)>;
    static constexpr std::uint32_t input_port=0, output_port=1, grant_port=2, request_port=3;
    ReferenceSeize(std::uint32_t store,std::uint32_t pool,std::uint32_t block,std::size_t units,bool preempt=false,Priority priority={})
        : ReferenceSeize(store,pool,block,Units{[units](const Token&) { return units; }},preempt,std::move(priority)) {
        if(units==0) throw std::invalid_argument("seize units must be positive");
    }
    ReferenceSeize(std::uint32_t store,std::uint32_t pool,std::uint32_t block,Units units,bool preempt=false,Priority priority={})
        : store_(store),pool_(pool),block_(block),units_(std::move(units)),priority_(std::move(priority)) {
        if(block>65535 || !units_ || preempt) throw std::invalid_argument("invalid seize block or unsupported preemption");
    }
    std::size_t received_count()const noexcept { return seen_.size(); }
    std::size_t waiting()const noexcept { return waiting_.size(); }
    std::size_t granted_count()const noexcept { return granted_; }
private:
    struct Waiting { Token token; std::size_t units; };
    void accept(const std::vector<devs::Input<Message>>& bag) {
        // Grants cannot refer to a request first created in this same bag.
        for(const auto& input:bag) {
            if(input.port==grant_port && std::holds_alternative<Grant>(input.value)) {
                const auto& grant=std::get<Grant>(input.value);
                const auto found=waiting_.find(grant.request_id);
                if(found==waiting_.end() || found->second.units!=grant.units)
                    throw std::invalid_argument("unknown, duplicate, or mismatched resource grant");
                auto token=found->second.token;
                token.leases.push_back({pool_,grant.request_id,grant.units});
                this->pending_.push_back({output_port,token});
                waiting_.erase(found); ++granted_;
            } else if(input.port!=input_port || !std::holds_alternative<Token>(input.value))
                throw std::invalid_argument("invalid seize message or port");
        }
        for(const auto& input:bag) if(input.port==input_port) {
            auto token=std::get<Token>(input.value);
            detail::validate_resource_token(token,store_);
            if(!seen_.insert(token.entity.id).second || std::any_of(token.leases.begin(),token.leases.end(),
                [&](const auto& lease) { return lease.pool==pool_; }))
                throw std::invalid_argument("duplicate seize or already leased pool");
            const auto units=units_(token);
            if(units==0) throw std::invalid_argument("seize units must be positive");
            if(priority_) token.priority=priority_(token);
            const auto request=(static_cast<std::uint64_t>(block_)<<48)|token.entity.id;
            waiting_.emplace(request,Waiting{token,units});
            this->pending_.push_back({request_port,Seize{request,units,token.priority,false}});
        }
    }
    std::uint32_t store_,pool_,block_;
    Units units_;
    Priority priority_;
    std::set<std::uint64_t> seen_;
    std::map<std::uint64_t,Waiting> waiting_;
    std::size_t granted_=0;
};

template<class Tag,class Message=ResourceProcessMessage<Tag>>
class ReferenceRelease final:public detail::PublicationAtomic<ReferenceRelease<Tag,Message>,Message> {
    using Token=EntityToken<Tag>;
    friend class detail::PublicationAtomic<ReferenceRelease<Tag,Message>,Message>;
public:
    static constexpr std::uint32_t input_port=0, output_port=1, request_port=3;
    ReferenceRelease(std::uint32_t store,std::uint32_t pool):store_(store),pool_(pool) {}
    std::size_t released_count()const noexcept { return seen_.size(); }
private:
    void accept(const std::vector<devs::Input<Message>>& bag) {
        for(const auto& input:bag) {
            if(input.port!=input_port || !std::holds_alternative<Token>(input.value))
                throw std::invalid_argument("invalid release message or port");
            auto token=std::get<Token>(input.value);
            detail::validate_resource_token(token,store_);
            if(!seen_.insert(token.entity.id).second) throw std::invalid_argument("duplicate entity release");
            const auto lease=std::find_if(token.leases.begin(),token.leases.end(),[&](const auto& value) { return value.pool==pool_; });
            if(lease==token.leases.end()) throw std::invalid_argument("entity does not hold release pool");
            this->pending_.push_back({request_port,Release{lease->request_id}});
            token.leases.erase(lease);
            this->pending_.push_back({output_port,token});
        }
    }
    std::uint32_t store_,pool_;
    std::set<std::uint64_t> seen_;
};

template<class Message>
class TypedResourcePool final:public devs::Atomic<Message> {
public:
    TypedResourcePool(std::size_t capacity,std::size_t max_request_units,QueueDiscipline discipline=QueueDiscipline::fifo)
        : pool_(capacity,max_request_units,discipline) {}
    std::unique_ptr<devs::Atomic<Message>> clone()const override { return std::make_unique<TypedResourcePool>(*this); }
    double time_advance()const override { return pool_.time_advance(); }
    std::optional<double> next_event_time()const override {
        return pool_.time_advance()==0 ? clock_ : std::numeric_limits<double>::infinity();
    }
    std::vector<devs::PortValue<Message>> output()const override {
        std::vector<devs::PortValue<Message>> result;
        for(const auto& event:pool_.output()) {
            const auto& grant=std::get<Grant>(event.value);
            result.push_back({static_cast<std::uint32_t>(1+(grant.request_id>>48)),grant});
        }
        return result;
    }
    void internal_transition()override { pool_.internal_transition(); }
    void external_transition(double elapsed,const std::vector<devs::Input<Message>>& bag)override {
        if(!std::isfinite(elapsed) || elapsed<0 || (elapsed>0 && clock_+elapsed<=clock_))
            throw std::invalid_argument("invalid typed pool elapsed time");
        external_transition_at(clock_+elapsed,(clock_+elapsed)-clock_,bag);
    }
    void external_transition_at(double time,double elapsed,const std::vector<devs::Input<Message>>& bag)override {
        if(!std::isfinite(time) || !std::isfinite(elapsed) || elapsed<0 || time-clock_!=elapsed || time>*next_event_time())
            throw std::invalid_argument("invalid typed pool timestamp");
        TypedResourcePool candidate(*this);
        candidate.capacity_area_+=static_cast<double>(pool_.capacity())*elapsed;
        candidate.allocated_area_+=static_cast<double>(pool_.allocated_units())*elapsed;
        candidate.waiting_area_+=static_cast<double>(pool_.waiting())*elapsed;
        if(!std::isfinite(candidate.capacity_area_) || !std::isfinite(candidate.allocated_area_) || !std::isfinite(candidate.waiting_area_))
            throw std::overflow_error("resource time-weighted statistics overflow");
        candidate.clock_=time;
        candidate.pool_.external_transition(elapsed,convert(bag));
        *this=std::move(candidate);
    }
    void confluent_transition(const std::vector<devs::Input<Message>>& bag)override { pool_.confluent_transition(convert(bag)); }
    const ResourcePool& pool()const noexcept { return pool_; }
    double utilization(double horizon)const {
        const double capacity=project(capacity_area_,pool_.capacity(),horizon);
        const double allocated=project(allocated_area_,pool_.allocated_units(),horizon);
        return capacity>0 ? allocated/capacity : 0;
    }
    double mean_waiting(double horizon)const { return project(waiting_area_,pool_.waiting(),horizon)/horizon; }
    double mean_allocated(double horizon)const { return project(allocated_area_,pool_.allocated_units(),horizon)/horizon; }
    double mean_capacity(double horizon)const { return project(capacity_area_,pool_.capacity(),horizon)/horizon; }
private:
    double project(double area,std::size_t level,double horizon)const {
        if(!std::isfinite(horizon) || horizon<=0 || horizon<clock_) throw std::invalid_argument("invalid resource statistics horizon");
        const auto value=area+static_cast<double>(level)*(horizon-clock_);
        if(!std::isfinite(value)) throw std::overflow_error("resource statistics projection overflow");
        return value;
    }
    static std::vector<devs::Input<ResourceMessage>> convert(const std::vector<devs::Input<Message>>& bag) {
        std::vector<devs::Input<ResourceMessage>> result;
        for(const auto& input:bag) std::visit([&](const auto& value) {
            using T=std::decay_t<decltype(value)>;
            if constexpr(std::is_same_v<T,Seize> || std::is_same_v<T,Release> || std::is_same_v<T,SetCapacity>)
                result.push_back({input.source,input.port,ResourceMessage{value}});
            else throw std::invalid_argument("unsupported typed pool message");
        },input.value);
        return result;
    }
    ResourcePool pool_;
    double clock_=0,capacity_area_=0,allocated_area_=0,waiting_area_=0;
};

} // namespace ankurafathom::des

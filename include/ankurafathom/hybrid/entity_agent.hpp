#pragma once
#include "ankurafathom/abm/population_atomic.hpp"
#include "ankurafathom/des/reference_resource.hpp"

namespace ankurafathom::hybrid {

template<class Tag>
using EntityAgentMessage=std::variant<des::EntityToken<Tag>,des::QueuePull,des::Seize,des::Release,des::Grant,des::SetCapacity,
    abm::PopulationCommand<Tag>,abm::PopulationResult<Tag>,abm::PopulationTopicInput<Tag>,
    abm::PopulationLifecycleInput<Tag>,abm::PopulationNetworkInput<Tag>>;

template<class Tag,class Message=EntityAgentMessage<Tag>>
class EntityAgentAtomic final:public devs::Atomic<Message> {
public:
    using Population=abm::PopulationAtomic<Tag,Message>;
    using Store=typename Population::Store;
    using Reference=typename Store::Reference;
    using Token=des::EntityToken<Tag>;
    using Eligible=std::function<bool(Reference,const Store&,double)>;
    using Priority=std::function<std::int32_t(Reference,const Store&,double)>;
    enum class Status { waiting,in_flight,returned };
    static constexpr std::uint32_t process_port=5,return_port=6;
    static constexpr std::uint64_t completion_sequence=std::numeric_limits<std::uint64_t>::max();
    EntityAgentAtomic(Population population,Eligible eligible,std::string completion_message,Priority priority={})
        :population_(std::move(population)),eligible_(std::move(eligible)),priority_(std::move(priority)),
         completion_(std::move(completion_message)) {
        if(population_.now()!=0 || !population_.output().empty() || !eligible_ || completion_.empty())
            throw std::invalid_argument("invalid entity-agent owner configuration");
    }
    std::unique_ptr<devs::Atomic<Message>> clone()const override { return std::make_unique<EntityAgentAtomic>(*this); }
    double time_advance()const override { return *next_event_time()-clock_; }
    std::optional<double> next_event_time()const override {
        if(initial_ || !tokens_.empty()) return clock_;
        return population_.next_event_time();
    }
    std::vector<devs::PortValue<Message>> output()const override {
        if(initial_) return {};
        auto result=population_.output();
        for(const auto& token:tokens_) result.push_back({process_port,Message{token}});
        return result;
    }
    void internal_transition()override {
        auto candidate=*this;
        if(initial_) {
            candidate.initial_=false;
            if(candidate.population_.next_event_time()==clock_) candidate.population_.internal_transition();
            candidate.reconcile();
        } else if(!tokens_.empty() || !population_.output().empty()) {
            candidate.tokens_.clear();
            if(!population_.output().empty()) candidate.population_.internal_transition();
        } else {
            candidate.population_.internal_transition();
            candidate.clock_=candidate.population_.now();
            candidate.reconcile();
        }
        *this=std::move(candidate);
    }
    void external_transition(double elapsed,const std::vector<devs::Input<Message>>& bag)override {
        if(!std::isfinite(elapsed) || elapsed<0 || (elapsed>0 && clock_+elapsed<=clock_))
            throw std::invalid_argument("invalid entity-agent elapsed time");
        external_transition_at(clock_+elapsed,(clock_+elapsed)-clock_,bag);
    }
    void external_transition_at(double time,double elapsed,const std::vector<devs::Input<Message>>& bag)override {
        if(initial_ || !output().empty() || !std::isfinite(time) || !std::isfinite(elapsed) || elapsed<0 || time<clock_ || time-clock_!=elapsed)
            throw std::invalid_argument("invalid entity-agent timestamp or pending output");
        auto candidate=*this;
        candidate.accept(time,bag);
        *this=std::move(candidate);
    }
    void confluent_transition(const std::vector<devs::Input<Message>>& bag)override {
        const double time=*next_event_time();
        if(!std::isfinite(time)) throw std::logic_error("entity-agent owner is not imminent");
        auto candidate=*this;
        candidate.initial_=false;candidate.tokens_.clear();
        candidate.accept(time,bag);
        *this=std::move(candidate);
    }
    const Population& population()const noexcept { return population_; }
    const Store& store()const noexcept { return population_.store(); }
    double time()const noexcept { return clock_; }
    Status status(Reference ref)const {
        if(ref.store!=store().store_id()) throw std::invalid_argument("foreign entity-agent reference");
        return ledger_.at(ref.id).status;
    }
    const Token& issued(Reference ref)const {
        if(ref.store!=store().store_id()) throw std::invalid_argument("foreign entity-agent reference");
        return ledger_.at(ref.id).token.value();
    }
    std::size_t launched_count()const noexcept {
        return std::count_if(ledger_.begin(),ledger_.end(),[](const auto& e) { return e.second.status!=Status::waiting; });
    }
    std::size_t returned_count()const noexcept {
        return std::count_if(ledger_.begin(),ledger_.end(),[](const auto& e) { return e.second.status==Status::returned; });
    }
    std::size_t in_flight_count()const noexcept { return launched_count()-returned_count(); }
private:
    struct Entry { Status status=Status::waiting; std::optional<Token> token; };
    void reconcile() {
        for(const auto& [id,entry]:ledger_)
            if(entry.status==Status::in_flight && !store().alive({store().store_id(),id}))
                throw std::invalid_argument("cannot retire an in-flight entity-agent");
        for(auto id=store().first_id();id<store().next_id();++id) {
            const Reference ref{store().store_id(),id};
            if(!store().alive(ref)) continue;
            auto& entry=ledger_[id];
            if(entry.status!=Status::waiting || !eligible_(ref,store(),clock_)) continue;
            Token token{ref,priority_?priority_(ref,store(),clock_):0,{},
                des::EntitySnapshot<Tag>{clock_,store().schema(),store().record(ref)}};
            entry.status=Status::in_flight;entry.token=token;tokens_.push_back(std::move(token));
        }
    }
    void accept(double time,const std::vector<devs::Input<Message>>& bag) {
        if(bag.empty() || time<clock_ || time>*population_.next_event_time())
            throw std::invalid_argument("invalid entity-agent input or skipped population deadline");
        std::vector<devs::Input<Message>> commands;
        for(const auto& input:bag) {
            const auto* token=std::get_if<Token>(&input.value);
            if(!token) {
                if(input.port==return_port) throw std::invalid_argument("process return requires an entity token");
                if(const auto* command=std::get_if<typename Population::Command>(&input.value);
                   command && command->sequence==completion_sequence)
                    throw std::invalid_argument("entity-agent completion sequence is reserved");
                commands.push_back(input);continue;
            }
            if(input.port!=return_port || token->entity.store!=store().store_id() || !store().alive(token->entity) || !token->leases.empty())
                throw std::invalid_argument("invalid, retired or leased entity-agent return");
            const auto found=ledger_.find(token->entity.id);
            if(found==ledger_.end() || found->second.status!=Status::in_flight || !found->second.token ||
               token->priority!=found->second.token->priority || token->snapshot!=found->second.token->snapshot)
                throw std::invalid_argument("replayed or mutated entity-agent return");
            found->second.status=Status::returned;
            commands.push_back({input.source,0,Message{typename Population::Command{time,completion_sequence,token->entity,completion_}}});
        }
        if(population_.next_event_time()==time) population_.confluent_transition(commands);
        else population_.external_transition_at(time,time-population_.now(),commands);
        clock_=population_.now();reconcile();
    }
    Population population_;
    Eligible eligible_;
    Priority priority_;
    std::string completion_;
    std::map<std::uint64_t,Entry> ledger_;
    std::vector<Token> tokens_;
    double clock_=0;
    bool initial_=true;
};
} // namespace ankurafathom::hybrid

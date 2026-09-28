#include "ankurafathom/abm/topic_broker.hpp"
#include <algorithm>
#include <iostream>

namespace {
using Broker=ankurafathom::abm::TopicBroker<std::string>;
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
template<class Function> void rejects(Function function) {
    bool caught=false; try { function(); } catch(const std::exception&) { caught=true; }
    require(caught,"invalid broker operation succeeded");
}
void batches() {
    Broker broker;
    broker.declare_topic("work",3); broker.declare_topic("audit",1); broker.declare_topic("closed",0);
    const std::vector<Broker::Publication> batch{{"work",{9,2,0,"direct"}},
        {"work",{1,std::nullopt,3,"broadcast"}},{"work",{1,7,1,"other"}},{"audit",{0,2,0,"trace"}}};
    std::vector<int> order{0,1,2,3};
    do {
        auto candidate=broker; std::vector<Broker::Publication> permuted;
        for(auto index:order) permuted.push_back(batch[index]);
        candidate.publish_many(permuted);
        require(candidate.visible("work").empty() && candidate.pending_count("work")==3,"pending leaked into current batch");
        candidate.flush();
        const auto delivered=candidate.for_receiver("work",2);
        require(delivered.size()==2 && delivered[0].payload=="broadcast" && delivered[1].payload=="direct","canonical recipient delivery differs");
        require(candidate.visible("work")[0].payload=="other" && candidate.pending_count("work")==0,"flush ordering/count differs");
        auto snapshot=candidate;
        const auto* visible_buffer=candidate.visible("work").data();
        candidate.publish_many({{"work",{0,2,0,"next"}}});
        require(candidate.visible("work").data()==visible_buffer,"publication invalidated a consumer's visible batch");
        for(const auto& message:candidate.visible("work")) {
            candidate.publish_many({{"audit",{message.sender,2,message.sequence,"reply"}}});
            require(message.payload.size()>0,"consumer message invalidated during publication");
            break; // audit topic has capacity one
        }
        require(candidate.for_receiver("work",2)==delivered,"consumer publication leaked into visible batch");
        candidate.flush();
        require(candidate.for_receiver("work",2).size()==1 && candidate.visible("audit").size()==1 &&
                candidate.visible("audit")[0].payload=="reply","next flush did not replace visible batches");
        require(snapshot.for_receiver("work",2)==delivered,"broker snapshot changed");
        candidate.flush(); require(candidate.visible("work").empty(),"empty flush retained old messages");
    } while(std::next_permutation(order.begin(),order.end()));
    broker.publish_many({batch[0]});
    rejects([&] { broker.publish_many({batch[1],batch[2],{"work",{0,0,0,"overflow"}}}); });
    rejects([&] { broker.publish_many({batch[1],batch[0]}); });
    rejects([&] { broker.publish_many({batch[1],{"missing",{0,0,0,"unknown"}}}); });
    rejects([&] { broker.publish_many({batch[1],{"closed",{0,0,0,"overflow"}}}); });
    require(broker.pending_count("work")==1 && broker.pending_count("audit")==0,"failed batch partially published");
    rejects([&] { broker.declare_topic("work",20); });
    rejects([&] { broker.declare_topic("",1); });
    broker.flush(); require(broker.visible("work").size()==1,"failure lost prior pending message");
}
struct Payload {
    int value;
    static inline bool fail=false;
    explicit Payload(int value):value(value) {}
    Payload(const Payload& other):value(other.value) { if(fail && value==13) throw std::runtime_error("payload copy failure"); }
    Payload(Payload&&)=default;
    Payload& operator=(const Payload&)=default;
    Payload& operator=(Payload&&)=default;
};
void payload_failure() {
    using CopyBroker=ankurafathom::abm::TopicBroker<Payload>;
    CopyBroker broker; broker.declare_topic("topic",4);
    broker.publish_many({{"topic",{0,0,0,Payload{1}}}}); broker.flush();
    const std::vector<CopyBroker::Publication> bad{{"topic",{1,0,0,Payload{2}}},{"topic",{2,0,0,Payload{13}}}};
    Payload::fail=true;
    rejects([&] { broker.publish_many(bad); });
    require(broker.pending_count("topic")==0 && broker.visible("topic")[0].payload.value==1,"failed payload copy corrupted broker");
    Payload::fail=false; broker.publish_many(bad); Payload::fail=true;
    rejects([&] { broker.flush(); });
    require(broker.pending_count("topic")==2 && broker.visible("topic")[0].payload.value==1,"failed flush changed either batch");
    Payload::fail=false; broker.flush();
    require(broker.visible("topic").size()==2,"flush retry lost pending messages");
}
}
int main() {
    try { batches(); payload_failure(); std::cout<<"Bounded broker permutation, visibility, overflow and payload rollback passed\n"; }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}

#include "ankurafathom/abm/sync_population.hpp"
#include "ankurafathom/devs/simulator.hpp"
#include "ankurafathom/sd/model.hpp"
#include "ankurafathom/rng/philox.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <cmath>
#include <deque>
#include <iostream>
#include <queue>
#include <numeric>
#include <string>
using J=nlohmann::json;namespace af=ankurafathom;
// 0 qualify queue; 1 qualifying; 2 design queue; 3 designing; 4 capacity;
// 5 equipment; 6 install queue; 7 installing; 8 active; 9 lost.
struct Customer {int type=0,blocks=1,preferred_site=0,preferred_provider=0,site=-1,provider=-1,state=0,work=-1;bool site_lock=false,provider_lock=false,liquid=false,reserved=false;double born=0,deadline=0,budget=0,kw=0,activated=0,revenue=0;std::string reason="Waiting for qualification";};
struct Site {double power=0,density=0,reserved=0,active=0,pue=1;int racks=0,liquid=0,used=0,liquid_used=0;};
struct Event{double time;int kind,id;std::uint64_t order;};
struct Later{bool operator()(const Event&a,const Event&b)const{return std::tie(a.time,a.kind,a.order)>std::tie(b.time,b.kind,b.order);}};
class Market final:public af::devs::Atomic<int>{
public:
 J config,p;std::uint64_t seed,sequence=0;af::abm::SyncPopulation<Customer> customers;af::sd::Model sd;
 std::array<Site,3> sites;std::array<int,3> busy{0,0,0};std::priority_queue<Event,std::vector<Event>,Later> events;std::deque<J> log;
 double now=0,revenue_rate=0,cost_rate=0,energy_rate=0,ledger_revenue=0,ledger_cost=0,ledger_energy=0,ledger_capex=0;int qualified=0,accepted=0;
 double v(const char*k)const{return p.at(k).get<double>();}
 double u(std::uint64_t entity,int stream,int step=0)const{af::rng::DrawAddress a;a.entity=entity;a.stream=stream;a.step=step;return af::rng::uniform_open(af::rng::draw(seed,a)[0]);}
 void add(double t,int kind,int id){events.push({t,kind,id,sequence++});}
 void note(int id,const std::string&kind,const std::string&text){log.push_back(J{{"time",now},{"customer",id},{"kind",kind},{"text",text}});if(log.size()>50)log.pop_front();}
 explicit Market(J c):config(std::move(c)),p(config["parameters"]),seed(p["seed"]){
  for(int i=0;i<3;++i){auto&s=sites[i];const auto&d=config["sites"][i];s.power=d["power_kw"];s.density=d["max_rack_kw"];s.racks=d["racks"];s.liquid=d["liquid_racks"];s.pue=d["pue"];}
  for(const char*n:{"revenue","operating_cost","energy_kwh","investment"})sd.add_stock(n,0);
  sd.add_flow(af::sd::Model::boundary,0,[this](const auto&,double){return revenue_rate;});sd.add_flow(af::sd::Model::boundary,1,[this](const auto&,double){return cost_rate;});sd.add_flow(af::sd::Model::boundary,2,[this](const auto&,double){return energy_rate;});
  rates();if(v("arrivals")>0)add(-std::log(u(0,0))/v("arrivals"),5,0);if(v("upgrade")>0)add(v("upgrade_day"),0,-1);
 }
 double time_advance()const override{return events.empty()?INFINITY:std::max(0.,events.top().time-now);}
 std::optional<double> next_event_time()const override{return events.empty()?INFINITY:events.top().time;}
 std::vector<af::devs::PortValue<int>> output()const override{return {};}
 void advance(double t){double d=t-now;if(d<-1e-10)throw std::runtime_error("Clock moved backwards");if(d<=0)return;double at=now;while(at<t){double dt=std::min(v("sd_dt"),t-at);sd.step(at,dt);at+=dt;}ledger_revenue+=revenue_rate*d;ledger_cost+=cost_rate*d;ledger_energy+=energy_rate*d;for(const auto&r:customers.records())if(r.value.state==8){auto a=r.value;a.revenue+=a.kw*v("colo_price")/30*d;customers.replace(r.id,a);}now=t;}
 void rates(){revenue_rate=0;energy_rate=0;for(const auto&s:sites){revenue_rate+=s.active*v("colo_price")/30;energy_rate+=s.active*v("it_load")*s.pue*24;}cost_rate=v("overhead_day")+(v("qualifiers")+v("designers")+v("installers"))*v("team_cost")+energy_rate*v("electricity");}
 void release(Customer&a){if(!a.reserved)return;auto&s=sites[a.site];s.reserved-=a.kw;s.used-=a.blocks;if(a.liquid)s.liquid_used-=a.blocks;a.reserved=false;}
 void lose(int id,Customer&a,const std::string&reason){release(a);if(a.work>=0){--busy[a.work];a.work=-1;}a.state=9;a.reason=reason;customers.replace(id,a);note(id,"lost",reason);}
 void arrive(int id){Customer a;double mix=u(id,1);a.type=mix<v("enterprise_share")?0:mix<v("enterprise_share")+v("dense_share")?1:2;a.blocks=a.type==1?3+int(u(id,2)*4):1+int(u(id,2)*2);a.liquid=a.type==1;a.preferred_site=int(u(id,3)*3);a.preferred_provider=int(u(id,4)*5);a.site_lock=u(id,5)<v("site_lock");a.provider_lock=u(id,6)<v("provider_lock");a.born=now;a.deadline=now+v("patience")*(.75+.5*u(id,7));a.budget=a.blocks*40*(200+200*u(id,8));customers.spawn(a);add(a.deadline,4,id);note(id,"arrived","New AI opportunity");if(v("arrivals")>0)add(now-std::log(u(id+1,0))/v("arrivals"),5,id+1);}
 bool reserve(Customer&a){
  std::string reason="No compatible partner profile";bool enabled=false,power=false,cooling=false,space=false,budget=false,density=false;
  for(int si=0;si<3;++si){int site=(a.preferred_site+si)%3;if(si&&(a.site_lock||v("alternatives")==0))continue;
   for(int pi=0;pi<5;++pi){int provider=(a.preferred_provider+pi)%5;if(pi&&(a.provider_lock||v("alternatives")==0))continue;const auto&profile=config["providers"][provider];if(!profile["enabled"].get<bool>())continue;enabled=true;auto&s=sites[site];double kw=a.blocks*profile["power_kw"].get<double>();if(profile["power_kw"].get<double>()>s.density){density=true;continue;}if(kw*v("colo_price")>a.budget){budget=true;continue;}if(s.used+a.blocks>s.racks){space=true;continue;}if(a.liquid&&s.liquid_used+a.blocks>s.liquid){cooling=true;continue;}if(s.reserved+kw>s.power+1e-9){power=true;continue;}
    s.used+=a.blocks;s.reserved+=kw;if(a.liquid)s.liquid_used+=a.blocks;a.site=site;a.provider=provider;a.kw=kw;a.reserved=true;a.state=5;a.reason="Equipment procurement";return true;
   }
  }
  if(enabled){reason="No feasible allocation:";if(budget)reason+=" budget";if(space)reason+=" rack space";if(cooling)reason+=" liquid cooling";if(power)reason+=" IT power";if(density)reason+=" rack density";}a.reason=reason;return false;
 }
 void dispatch(){
  // Stable customer arrival order. Feasible later requests may use resources that
  // an earlier incompatible request cannot use; no forced head-of-line blocking.
  for(const auto&r:customers.records()){
   auto a=r.value;if(a.state==0&&busy[0]<v("qualifiers")){a.state=1;a.work=0;a.reason="Qualification in progress";++busy[0];add(now+v("qualification_days"),1,int(r.id));note(int(r.id),"qualification","Qualification started");}
   if(a.state==2&&busy[1]<v("designers")){a.state=3;a.work=1;a.reason="Solution design in progress";++busy[1];double duration=v("design_days")*(.75+.25*a.blocks)*(1-v("design_saving"));if(v("design_saving")>0&&u(r.id,11)<v("design_rework"))duration+=.5*v("design_days");add(now+duration,2,int(r.id));note(int(r.id),"design","Solution design started");}
   if(a.state==4&&reserve(a)){add(now+config["providers"][a.provider]["lead_days"].get<double>(),3,int(r.id));note(int(r.id),"reserved","IT power and racks reserved; awaiting equipment");}
   if(a.state==6&&busy[2]<v("installers")){a.state=7;a.work=2;a.reason="Installation and testing";++busy[2];add(now+v("installation_days")*a.blocks,6,int(r.id));note(int(r.id),"install","Installation started");}
   customers.replace(r.id,a);
  }rates();
 }
 void internal_transition()override{
  auto e=events.top();events.pop();advance(e.time);
  if(e.kind==0){sites[0].power+=800;sites[0].density=std::max(sites[0].density,80.);sites[0].liquid=std::min(sites[0].racks,sites[0].liquid+12);sd.add_to_stock(3,v("upgrade_cost"));ledger_capex+=v("upgrade_cost");note(-1,"upgrade","Site A upgrade commissioned");}
  else if(e.kind==5)arrive(e.id);
  else {auto a=customers.records().at(e.id).value;
   if(e.kind==4){if(a.state!=8&&a.state!=9)lose(e.id,a,"Customer deadline exceeded · "+a.reason);}
   else if(e.kind==3){if(a.state==5){a.state=6;a.reason="Waiting for installation lane";customers.replace(e.id,a);note(e.id,"equipment","Equipment arrived");}}
   else{int lane=e.kind==1?0:e.kind==2?1:2;if(a.work==lane){--busy[lane];a.work=-1;}if(a.state!=9){if(e.kind==1){++qualified;a.state=2;a.reason="Waiting for solution designer";}else if(e.kind==2){if(u(e.id,10)>v("win_rate")){lose(e.id,a,"Commercial decision lost");}else{++accepted;a.state=4;a.reason="Checking infrastructure availability";}}else if(e.kind==6){a.state=8;a.activated=now;a.reason="Operational";sites[a.site].active+=a.kw;note(e.id,"active","Customer activated; recurring revenue begins");}}customers.replace(e.id,a);}
  }dispatch();
 }
 void external_transition(double elapsed,const std::vector<af::devs::Input<int>>&)override{advance(now+elapsed);}
 void external_transition_at(double t,double,const std::vector<af::devs::Input<int>>&)override{advance(t);}
 void confluent_transition(const std::vector<af::devs::Input<int>>&)override{internal_transition();}
 J frame()const{
  J agents=J::array(),ss=J::array(),checks=J::array();std::array<int,10> counts{};double active_days=0,revenue=0,reserved=0,activekw=0;std::array<double,3> allocated{};std::array<int,3> racks{},liquid{},work{};
  for(const auto&r:customers.records()){const auto&a=r.value;++counts[a.state];if(a.work>=0)++work[a.work];if(a.reserved){allocated[a.site]+=a.kw;racks[a.site]+=a.blocks;if(a.liquid)liquid[a.site]+=a.blocks;}if(a.state==8)active_days+=a.activated-a.born;revenue+=a.revenue;agents.push_back(J{{"id",r.id},{"type",a.type},{"blocks",a.blocks},{"state",a.state},{"preferred_site",a.preferred_site},{"preferred_provider",a.preferred_provider},{"site",a.site},{"provider",a.provider},{"site_lock",a.site_lock},{"provider_lock",a.provider_lock},{"liquid",a.liquid},{"born",a.born},{"deadline",a.deadline},{"budget",a.budget},{"kw",a.kw},{"reason",a.reason},{"activated",a.activated},{"revenue",a.revenue}});}
  auto check=[&](std::string name,double actual,double expected,double tol=0){if(tol>0)tol+=std::abs(expected)*1e-10;checks.push_back(J{{"name",name},{"actual",actual},{"expected",expected},{"tolerance",tol},{"passed",std::abs(actual-expected)<=tol}});};
  for(int i=0;i<3;++i){const auto&s=sites[i];reserved+=s.reserved;activekw+=s.active;ss.push_back(J{{"name",config["sites"][i]["name"]},{"power_kw",s.power},{"max_rack_kw",s.density},{"reserved_kw",s.reserved},{"active_kw",s.active},{"it_draw_kw",s.active*v("it_load")},{"racks",s.racks},{"used_racks",s.used},{"liquid_racks",s.liquid},{"used_liquid",s.liquid_used},{"pue",s.pue}});check("Site "+std::to_string(i+1)+" reservations",s.reserved,allocated[i],1e-7);check("Site "+std::to_string(i+1)+" capacity limits",s.reserved<=s.power+1e-7&&s.used<=s.racks&&s.liquid_used<=s.liquid&&s.active<=s.reserved+1e-7&&s.reserved>=-1e-7?1:0,1);check("Site "+std::to_string(i+1)+" rack ledger",s.used,racks[i]);check("Site "+std::to_string(i+1)+" cooling ledger",s.liquid_used,liquid[i]);check("Team "+std::to_string(i+1)+" exclusivity",busy[i],work[i]);check("Team "+std::to_string(i+1)+" capacity",busy[i]>=0&&busy[i]<=v(i==0?"qualifiers":i==1?"designers":"installers")?1:0,1);}
  check("Customer conservation",customers.active_count(),std::accumulate(counts.begin(),counts.end(),0));check("Revenue vs customer ledger",sd.state()[0],revenue,1e-5);check("Operating-cost integration",sd.state()[1],ledger_cost,1e-5);check("Energy integration",sd.state()[2],ledger_energy,1e-5);check("Investment ledger",sd.state()[3],ledger_capex,1e-5);
  bool ok=true;for(const auto&c:checks)ok&=c["passed"].get<bool>();
  return J{{"time",now},{"complete",now>=v("days")},{"checks_passed",ok},{"checks",checks},{"customers",agents},{"sites",ss},{"counts",counts},{"busy",busy},{"queues",J{{"qualification",counts[0]},{"design",counts[2]},{"capacity",counts[4]},{"equipment",counts[5]},{"installation",counts[6]}}},{"events",log},{"metrics",J{{"opportunities",customers.active_count()},{"active",counts[8]},{"lost",counts[9]},{"pipeline",customers.active_count()-counts[8]-counts[9]},{"qualified",qualified},{"accepted",accepted},{"revenue",sd.state()[0]},{"cost",sd.state()[1]},{"energy_kwh",sd.state()[2]},{"capex",sd.state()[3]},{"contribution",sd.state()[0]-sd.state()[1]},{"cash_proxy",sd.state()[0]-sd.state()[1]-sd.state()[3]},{"reserved_kw",reserved},{"active_kw",activekw},{"it_draw_kw",activekw*v("it_load")},{"activation_days",counts[8]?J(active_days/counts[8]):J(nullptr)}}}};
 }
};
#ifndef EQUINIX_MODEL_TEST
int main(){try{std::string line;if(!std::getline(std::cin,line))return 0;auto config=J::parse(line);af::devs::Simulator<int> sim;auto model=std::make_unique<Market>(config);auto*m=model.get();auto id=sim.add(std::move(model));std::cout<<m->frame().dump()<<std::endl;while(std::getline(std::cin,line)){double target=J::parse(line).at("until").get<double>();if(!std::isfinite(target)||target<=m->now||target>m->v("days"))throw std::runtime_error("Invalid observation time");sim.inject(target,id,0,0);sim.run_until(target,1000000);auto f=m->frame();std::cout<<f.dump()<<std::endl;if(f["complete"].get<bool>()||!f["checks_passed"].get<bool>())break;}return 0;}catch(const std::exception&e){std::cout<<J{{"error",e.what()}}.dump()<<std::endl;return 1;}}
#endif

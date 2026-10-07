// Project-specific hybrid model. All clocks/integration/populations use native platform primitives.
#include "ankurafathom/abm/sync_population.hpp"
#include "ankurafathom/devs/simulator.hpp"
#include "ankurafathom/sd/model.hpp"
#include "ankurafathom/rng/philox.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <algorithm>
#include <cmath>
#include <deque>
#include <iostream>
#include <numeric>
#include <queue>
#include <string>
using J=nlohmann::json;
namespace af=ankurafathom;
struct Employee {int level=0,skill=0,second=-1,state=0,job=-1;bool licensed=false,using_ai=false,ever_used_ai=false;double productivity=1,proficiency=0,leave=0,speed=1,paid=0,worked=0,trained=0,admin=0,idle=0,bd=0,hourly_revenue=0;};
struct Job {int id=0,stage=0,skill=0,owner=-1;bool fixed=false;double size=1,remaining=0,arrived=0,queued=0,wait=0,hours=0,ai_work=0,prior_work=0;};
// State: 0 off shift, 1 available, 2 delivering, 3 training, 4 administration, 5 support.
struct Event {double time;int kind,id;std::uint64_t serial;};
struct Later {bool operator()(const Event&a,const Event&b)const{return std::tie(a.time,a.kind,a.serial)>std::tie(b.time,b.kind,b.serial);}};
class Practice final:public af::devs::Atomic<int>{
 public:
 J config,p;af::abm::SyncPopulation<Employee> people;af::sd::Model finance;
 std::vector<Job> jobs;std::priority_queue<Event,std::vector<Event>,Later> events;std::deque<J> log;
 std::uint64_t serial=0,seed=0;double now=0,revenue_rate=0,cost_rate=0,base_cost=0,completed_wait=0,cycle=0;
 int prospects=0,won=0,completed=0,licensed=0;std::vector<double> bd_days;std::array<double,3> stage_hours{},stage_wait{},stage_starts{};
 double v(const char*k)const{return p.at(k).get<double>();}
 double u(std::uint64_t entity,int day,int stream,int draw=0)const {af::rng::DrawAddress a;a.entity=entity;a.step=day;a.stream=stream;a.draw_index=draw;return af::rng::uniform_open(af::rng::draw(seed,a)[0]);}
 void schedule(double t,int k,int id){events.push({t,k,id,serial++});}
 void note(const std::string&kind,int id,const std::string&text){log.push_back(J{{"time",now},{"kind",kind},{"id",id},{"text",text}});if(log.size()>60)log.pop_front();}
 explicit Practice(J c):config(std::move(c)),p(config.at("parameters")),seed(p.at("seed").get<std::uint64_t>()),bd_days(400,0){
  stage_hours={v("analysis_hours"),v("plan_hours"),v("review_hours")};
  for(int l=0;l<8;++l)for(int n=0;n<config["levels"][l]["count"].get<int>();++n){Employee a;a.level=l;auto id=people.records().size();double skill=u(id,0,1);a.skill=skill<v("liquidity_skill")?0:skill<v("liquidity_skill")+v("restructuring_skill")?1:2;a.second=u(id,0,2)<v("secondary_skill")?(a.skill+1+int(u(id,0,3)*2))%3:-1;a.productivity=1+v("skill_variation")*(2*u(id,0,4)-1);people.spawn(a);base_cost+=config["levels"][l]["salary"].get<double>()*(1+v("cost_load"))/(365*24);}
  std::vector<std::size_t> ranks;for(const auto&r:people.records())if(r.value.level<7)ranks.push_back(r.id);
  std::sort(ranks.begin(),ranks.end(),[&](auto a,auto b){return u(a,0,5)<u(b,0,5);});licensed=int(std::floor(ranks.size()*v("license_share")+.5));
  for(int i=0;i<licensed;++i){auto a=people.records()[ranks[i]].value;a.licensed=true;a.proficiency=v("initial_proficiency");people.replace(ranks[i],a);}
  base_cost+=v("overhead")/(365*24)+licensed*v("license_cost")*12/(365*24);
  finance.add_stock("recognized_revenue",0);finance.add_stock("operating_cost",0);
  finance.add_flow(af::sd::Model::boundary,0,[this](const auto&,double){return revenue_rate;});
  finance.add_flow(af::sd::Model::boundary,1,[this](const auto&,double){return cost_rate;});cost_rate=base_cost;schedule(0,0,0);
 }
 double time_advance()const override{return events.empty()?INFINITY:std::max(0.,events.top().time-now);}
 std::optional<double> next_event_time()const override{return events.empty()?INFINITY:events.top().time;}
 std::vector<af::devs::PortValue<int>> output()const override{return {};}
 void advance(double t){
  const double dt=t-now;if(dt<-1e-8)throw std::runtime_error("clock moved backwards");if(dt<=0)return;
  double at=now;while(at<t){double h=std::min(v("sd_dt"),t-at);finance.step(at,h);at+=h;}
  const int day=int(now/24);const double multiplier=config["training"]=="coaching"?1.5:config["training"]=="self-study"?.5:1.;
  for(const auto&r:people.records()){auto a=r.value;if(a.state){a.paid+=dt;
    if(a.state==2){a.worked+=dt;auto&j=jobs[a.job];double work=std::min(j.remaining,dt*a.speed);j.remaining=std::max(0.,j.remaining-work);j.hours+=dt;if(!j.fixed)a.hourly_revenue+=dt*config["levels"][a.level]["rate"].get<double>()*v("realization");if(j.stage<2){j.prior_work+=work;if(a.using_ai)j.ai_work+=work;}if(a.using_ai)a.proficiency=1-(1-a.proficiency)*std::exp(-v("learning_by_use")*dt);}
    else if(a.state==3){a.trained+=dt;a.proficiency=1-(1-a.proficiency)*std::exp(-v("training_gain")*multiplier*dt);}
    else if(a.state==4 || a.state==5)a.admin+=dt;
    else {const double bd=a.level>=4&&a.level<7?v("bd_fraction")*dt:0;a.bd+=bd;a.idle+=dt-bd;bd_days.at(day)+=bd;}
  }people.replace(r.id,a);}now=t;
 }
 void rates(){revenue_rate=0;cost_rate=base_cost;for(const auto&r:people.records()){const auto&a=r.value;if(a.state==2&&!jobs[a.job].fixed)revenue_rate+=config["levels"][a.level]["rate"].get<double>()*v("realization");if(a.state==3)cost_rate+=v("training_cost");}}
 bool eligible(const Employee&a,const Job&j)const{return (a.skill==j.skill||a.second==j.skill)&&((j.stage==0&&a.level<=2)||(j.stage==1&&a.level>=3&&a.level<=4)||(j.stage==2&&a.level>=5&&a.level<=6));}
 void assign(){
  for(const auto&r:people.records()){auto a=r.value;if(a.state!=1)continue;int chosen=-1;
   if(a.job>=0)chosen=a.job;
   else for(const auto&j:jobs)if(j.stage<3&&j.owner<0&&eligible(a,j)&&(chosen<0||std::tie(j.queued,j.id)<std::tie(jobs[chosen].queued,jobs[chosen].id)))chosen=j.id;
   if(chosen<0)continue;auto&j=jobs[chosen];if(j.owner<0){j.wait+=now-j.queued;stage_wait[j.stage]+=now-j.queued;stage_starts[j.stage]++;j.owner=int(r.id);a.job=chosen;}
   const double exposure=j.stage==0?v("analysis_exposure"):j.stage==1?v("plan_exposure"):v("review_exposure");
   a.using_ai=a.licensed&&u(std::uint64_t(j.id)*512+r.id,j.stage,30)<v("usage");
   a.ever_used_ai|=a.using_ai;const double saving=a.using_ai?v("ai_saving")*exposure*a.proficiency:0;
   a.speed=a.productivity/(1-saving);a.state=2;
   const double end=std::min(a.leave,now+j.remaining/a.speed);schedule(end,1,int(r.id));people.replace(r.id,a);note("assignment",j.id,"Engagement "+std::to_string(j.id+1)+" → "+std::array<std::string,3>{"analysis","plan","review"}[j.stage]+" · employee "+std::to_string(r.id+1));
  }rates();
 }
 void daily(int day){
  if(day+1<=v("days"))schedule((day+1)*24,0,day+1);
  if(day>=v("days")||day%7>=5)return;
  for(const auto&r:people.records()){const double start=day*24+v("arrival_min")+(v("arrival_max")-v("arrival_min"))*u(r.id,day,10);const double leave=day*24+v("leave_min")+(v("leave_max")-v("leave_min"))*u(r.id,day,11);schedule(start,4,int(r.id));schedule(leave,2,int(r.id));}
  const int lag=int(v("bd_lag"));double due=day>=lag?bd_days[day-lag]:0;if(day%7==0)for(int offset=1;offset<=2;++offset)if(day-lag-offset>=0)due+=bd_days[day-lag-offset];double rate=(v("prospects_day")*std::pow(1+v("market_growth"),day/365.)+due*v("bd_conversion"))/10;
  if(rate<=0)return;double t=day*24+8;for(int n=0;n<10000;++n){t+=-std::log(u(n,day,20))/rate;if(t>=day*24+18)break;schedule(t,5,day*10000+n);}
 }
 void arrival(int key){++prospects;if(u(key,0,21)>=v("win_rate"))return;Job j;j.id=int(jobs.size());j.arrived=now;j.queued=now;double skill=u(key,0,22);j.skill=skill<v("liquidity_work")?0:skill<v("liquidity_work")+v("restructuring_work")?1:2;j.fixed=u(key,0,23)<v("fixed_share");double z=std::sqrt(-2*std::log(u(key,0,24)))*std::cos(6.283185307179586*u(key,0,25));j.size=std::exp(v("complexity")*z-.5*v("complexity")*v("complexity"));j.remaining=stage_hours[0]*j.size;jobs.push_back(j);++won;note("won",j.id,"New engagement "+std::to_string(j.id+1));}
 void internal_transition()override{
  const auto e=events.top();events.pop();advance(e.time);
  if(e.kind==0)daily(e.id);
  else if(e.kind==5)arrival(e.id);
  else {auto a=people.records().at(e.id).value;
   if(e.kind==4){int day=int(now/24);a.leave=day*24+v("leave_min")+(v("leave_max")-v("leave_min"))*u(e.id,day,11);a.state=a.level==7?5:4;if(a.level<7)schedule(std::min(a.leave,now+(a.leave-now)*v("admin_fraction")),3,e.id);}
   else if(e.kind==3){if(a.licensed&&v("training_hours")>0){a.state=3;schedule(std::min(a.leave,now+v("training_hours")/5),6,e.id);}else a.state=1;}
   else if(e.kind==6){a.state=now<a.leave?1:0;}
   else if(e.kind==1){auto&j=jobs.at(a.job);if(j.remaining<1e-7){++j.stage;j.owner=-1;a.job=-1;a.using_ai=false;if(j.stage==3){++completed;completed_wait+=j.wait;cycle+=now-j.arrived;if(j.fixed)finance.add_to_stock(0,v("fixed_fee")*j.size);note("completed",j.id,"Completed engagement "+std::to_string(j.id+1));}else {j.remaining=stage_hours[j.stage]*j.size;if(j.stage==2&&j.prior_work>0)j.remaining*=1+v("review_extra")*j.ai_work/j.prior_work;j.queued=now;}}a.state=now<a.leave?1:0;}
   else if(e.kind==2){a.state=0;}
   people.replace(e.id,a);
  }assign();
 }
 void external_transition(double elapsed,const std::vector<af::devs::Input<int>>&)override{advance(now+elapsed);}
 void external_transition_at(double t,double,const std::vector<af::devs::Input<int>>&)override{advance(t);}
 void confluent_transition(const std::vector<af::devs::Input<int>>&)override{internal_transition();}
 J frame()const{
  J agents=J::array(),levels=J::array(),checks=J::array();double paid=0,work=0,train=0,admin=0,idle=0,bd=0,proficiency=0;int busy=0,on=0,ai_users=0,billable=0;bool ownership=true,skills=true;
  for(int l=0;l<8;++l){double lp=0,lw=0;int count=0,lb=0;for(const auto&r:people.records())if(r.value.level==l){++count;lp+=r.value.paid;lw+=r.value.worked;lb+=r.value.state==2;}levels.push_back(J{{"name",config["levels"][l]["name"]},{"count",count},{"busy",lb},{"paid_hours",lp},{"delivery_hours",lw},{"utilization",lp>0?J(lw/lp):J(nullptr)}});}
  for(const auto&r:people.records()){const auto&a=r.value;paid+=a.paid;work+=a.worked;train+=a.trained;admin+=a.admin;idle+=a.idle;bd+=a.bd;busy+=a.state==2;on+=a.state!=0;ai_users+=a.ever_used_ai;billable+=a.level<7;proficiency+=a.licensed?a.proficiency:0;if(a.job>=0){ownership&=jobs[a.job].owner==int(r.id);skills&=eligible(a,jobs[a.job]);}agents.push_back(J{{"id",r.id},{"level",a.level},{"skill",a.skill},{"second_skill",a.second},{"state",a.state},{"job",a.job},{"licensed",a.licensed},{"using_ai",a.state==2&&a.using_ai},{"proficiency",a.proficiency},{"delivery_hours",a.worked},{"training_hours",a.trained}});}
  J queues=J::array();int active=0;for(int s=0;s<3;++s){int waiting=0,service=0;double oldest=0;for(const auto&j:jobs)if(j.stage==s){++active;if(j.owner<0){++waiting;oldest=std::max(oldest,now-j.queued);}else ++service;}queues.push_back(J{{"stage",s},{"waiting",waiting},{"assigned",service},{"oldest_wait_hours",oldest},{"mean_wait_hours",stage_starts[s]>0?J(stage_wait[s]/stage_starts[s]):J(nullptr)}});}
  double billed_paid=paid;for(const auto&r:people.records())if(r.value.level==7)billed_paid-=r.value.paid;
  auto check=[&](const char*name,double lhs,double rhs,double tolerance){if(tolerance>0)tolerance+=1e-10*std::abs(rhs);checks.push_back(J{{"name",name},{"actual",lhs},{"expected",rhs},{"tolerance",tolerance},{"passed",std::abs(lhs-rhs)<=tolerance}});};
  check("Engagement conservation",won,active+completed,0);check("Exclusive employee time",paid,work+train+admin+idle+bd,1e-5);check("Unique ownership",ownership?1:0,1,0);check("Required skills and levels",skills?1:0,1,0);double expected_revenue=0;for(const auto&r:people.records())expected_revenue+=r.value.hourly_revenue;for(const auto&j:jobs)if(j.fixed&&j.stage==3)expected_revenue+=v("fixed_fee")*j.size;check("Revenue vs employee/project ledger",finance.state()[0],expected_revenue,1e-4);check("Costs vs salary/license/training ledger",finance.state()[1],base_cost*now+train*v("training_cost"),1e-4);
  bool ok=true;for(const auto&c:checks)ok&=c["passed"].get<bool>();
  J result={{"time",now},{"complete",now>=v("days")*24},{"checks_passed",ok},{"checks",checks},{"agents",agents},{"levels",levels},{"queues",queues},{"events",log},{"metrics",J{{"ai_users",ai_users},{"ai_use_share",billable?double(ai_users)/billable:0},{"headcount",people.active_count()},{"prospects",prospects},{"won",won},{"completed",completed},{"backlog",active},{"busy",busy},{"on_shift",on},{"licensed",licensed},{"proficiency",licensed?proficiency/licensed:0},{"revenue",finance.state()[0]},{"cost",finance.state()[1]},{"contribution",finance.state()[0]-finance.state()[1]},{"utilization",billed_paid>0?J(work/billed_paid):J(nullptr)},{"delivery_hours",work},{"training_hours",train},{"bd_hours",bd},{"mean_cycle_hours",completed?J(cycle/completed):J(nullptr)},{"mean_wait_hours",completed?J(completed_wait/completed):J(nullptr)}}}};
  for(int l=0;l<8;++l)result["metrics"]["headcount_level"+std::to_string(l)]=config["levels"][l]["count"];return result;
 }
};
#ifndef WORKSHOP_NO_MAIN
int main(){try{std::string line;if(!std::getline(std::cin,line))return 0;auto config=J::parse(line);af::devs::Simulator<int> sim;auto model=std::make_unique<Practice>(config);auto* practice=model.get();auto id=sim.add(std::move(model));std::cout<<practice->frame().dump()<<std::endl;while(std::getline(std::cin,line)){auto command=J::parse(line);const double target=command.at("until").get<double>();if(!std::isfinite(target)||target<=practice->now||target>practice->v("days")*24)throw std::runtime_error("invalid target time");sim.inject(target,id,0,0);sim.run_until(target,1000000);auto f=practice->frame();std::cout<<f.dump()<<std::endl;if(!f["checks_passed"].get<bool>()||f["complete"].get<bool>())break;}return 0;}catch(const std::exception&e){std::cout<<J{{"error",e.what()}}.dump()<<std::endl;return 1;}}

#endif

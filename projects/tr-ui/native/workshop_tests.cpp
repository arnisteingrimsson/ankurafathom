#define WORKSHOP_NO_MAIN
#include "workshop_runner.cpp"
#include <fstream>
void require(bool b,const char*m){if(!b)throw std::runtime_error(m);}
void close(double x,double y,const char*m){if(std::abs(x-y)>1e-7)throw std::runtime_error(m);}
std::unique_ptr<Practice> fixture(J c,bool ai,bool fixed){
 auto&p=c["parameters"];p["prospects_day"]=0;p["bd_conversion"]=0;p["license_share"]=0;p["training_hours"]=0;p["admin_fraction"]=0;p["win_rate"]=1;p["fixed_share"]=fixed?1:0;p["complexity"]=0;p["skill_variation"]=0;p["ai_saving"]=.5;p["review_extra"]=0;p["learning_by_use"]=0;
 auto a=std::make_unique<Practice>(c);a->stage_hours={2,1,1};a->events={};a->now=8;
 // Three known people, each handling exactly one stage, with all others off shift.
 for(const auto&r:a->people.records()){auto e=r.value;e.state=0;e.licensed=false;a->people.replace(r.id,e);}
 std::array<int,3> chosen{-1,-1,-1};for(const auto&r:a->people.records()){int stage=r.value.level<=2?0:r.value.level<=4?1:r.value.level<=6?2:-1;if(stage>=0&&chosen[stage]<0){auto e=r.value;e.state=1;e.leave=18;e.skill=0;e.second=1;e.productivity=1;e.licensed=ai;e.proficiency=1;a->people.replace(r.id,e);chosen[stage]=int(r.id);}}
 a->p["usage"]=1;a->arrival(0);a->jobs[0].skill=0;a->assign();return a;
}
int main(int argc,char**argv){try{if(argc!=2)throw std::runtime_error("default config path required");std::ifstream f(argv[1]);J c;f>>c;
 for(bool ai:{false,true})for(bool fixed:{false,true}){af::devs::Simulator<int> sim;auto a=fixture(c,ai,fixed);auto*p=a.get();sim.add(std::move(a));const double duration=ai?2.75:4;sim.run_until(8+duration);require(p->completed==1,"known schedule did not complete");close(p->cycle,duration,"independent stage-duration calculation");double hours=0,revenue=0;for(const auto&r:p->people.records()){hours+=r.value.worked;revenue+=r.value.worked*c["levels"][r.value.level]["rate"].get<double>()*c["parameters"]["realization"].get<double>();}close(hours,duration,"one person per stage, exclusive time");close(p->finance.state()[0],fixed?c["parameters"]["fixed_fee"].get<double>():revenue,"independent fee formula");}
 {af::devs::Simulator<int> sim;auto a=fixture(c,false,false);auto*p=a.get();for(const auto&r:p->people.records())if(r.value.state==1){auto e=r.value;e.skill=2;e.second=-1;p->people.replace(r.id,e);} // already assigned analyst retains the skill needed for its task
 sim.add(std::move(a));sim.run_until(18);require(p->completed==0&&p->jobs[0].stage==1,"missing planning skill must leave work queued");}
 std::cout<<"Hybrid hand schedules passed: ordinary and AI durations, hourly and fixed fees, skill bottleneck\n";return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}

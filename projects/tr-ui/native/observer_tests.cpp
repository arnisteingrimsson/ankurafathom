#include "ankurafathom/ir/model.hpp"
#include <bit>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
namespace ir=ankurafathom::ir;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(int argc,char** argv){
 try{
  require(argc==2,"repository root required");
  for(const auto* name:{"decay","rk4_growth","lookup","delay","signed_flow","extended_delay"}){
   auto model=ir::load_file(std::string(argv[1])+"/models/"+name+".ir.json");
   const auto expected=ir::run(model);std::vector<ir::Row> streamed;std::size_t ticks=0;
   const auto actual=ir::run_observed_sd(model,{},[&](double t,const auto& values,std::span<const ir::Row> frame){
    require(values.at("t")==t,"clock mismatch");require(!frame.empty(),"empty observation");
    for(const auto& r:frame){require(r.time==t,"future row delivered");streamed.push_back(r);}++ticks;return true;
   });
   require(actual.size()==expected.size()&&streamed.size()==expected.size(),"row count mismatch");
   for(std::size_t i=0;i<actual.size();++i){
    require(actual[i].output_id==expected[i].output_id&&actual[i].time==expected[i].time,"row identity mismatch");
    require(std::bit_cast<std::uint64_t>(actual[i].value)==std::bit_cast<std::uint64_t>(expected[i].value),"batch/observed bit mismatch");
    require(actual[i].value==streamed[i].value,"streamed value mismatch");
   }
   std::size_t callbacks=0;
   auto prefix=ir::run_observed_sd(model,{},[&](double,const auto&,auto){return ++callbacks<2;});
   require(callbacks==2&&prefix.size()==model.outputs.size()*2,"stop did not retain exact prefix");
   std::cout<<name<<": "<<ticks<<" barriers; bit-identical and prefix stop passed\n";
  }
  const auto future_error=ir::load_json(R"({"ir_version":"0.1","name":"future overflow","mode":"sd","time":{"unit":"day","dt":1,"horizon":3},"parameters":[{"id":"rate","value":1e308,"unit":"1/day"}],"components":[{"id":"x","kind":"stock","init":1,"unit":"1"},{"id":"in","kind":"flow","source":null,"destination":"x","expr":"rate","unit":"1/day"}],"outputs":[{"id":"x","stock":"x"}]})");
  auto prefix=ir::run_observed_sd(future_error,{},[](double t,const auto&,auto){return t<1;});
  require(prefix.size()==2,"future failure computed before stop");
  bool failed=false;try{(void)ir::run(future_error);}catch(...){failed=true;}require(failed,"future error test must fail when advanced");
  bool empty=false;try{(void)ir::run_observed_sd(future_error,{},{});}catch(...){empty=true;}require(empty,"empty observer accepted");
  std::cout<<"Future transition not computed at paused boundary; invalid observer rejected\n";
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}

#include "ankurafathom/runtime/data_inputs.hpp"
#include "ankurafathom/sd/model.hpp"
#include <iomanip>
#include <iostream>

int main(int argc,char** argv) {
    namespace data=ankurafathom::runtime::data;
    namespace sd=ankurafathom::sd;
    try {
        if(argc!=3) throw std::invalid_argument("usage: fathom_series_from_data parameters.csv seasonality.csv");
        const auto source=data::load_file(argv[1],{{{"practice",data::Type::string,"",{}},
            {"capacity",data::Type::i32,"people",{}},{"rate",data::Type::f64,"USD/hour",{}}},"practice"});
        const std::vector<data::ParameterField> fields{{"rate","rate","USD/hour"}};
        const data::ParameterTable parameters(source,fields);
        const data::ExogenousSeries series(data::load_file(argv[2],{{{"time",data::Type::f64,"hour",{}},
            {"factor",data::Type::f64,"1",{}}},"time"}),{"time","factor","hour","1",data::Interpolation::linear});
        const auto grid=series.sample({0,1,5});
        std::cout<<"practice,time,synthetic_revenue\n"<<std::setprecision(17);
        for(const auto& row:source.rows()) {
            const auto key=row[source.column_index("practice")];const auto selected=parameters.values(key);
            sd::Model model;const auto revenue=model.add_stock("revenue",0);
            double factor=0;const auto rate=selected.values.at("rate");
            model.add_flow(sd::Model::boundary,revenue,[&](const auto&,double) { return rate*factor; });
            for(std::size_t i=0;i<grid.times.size();++i) {
                std::cout<<std::get<std::string>(key)<<','<<grid.times[i]<<','<<model.state()[revenue]<<'\n';
                if(i+1<grid.times.size()) { factor=grid.values[i];model.step(grid.times[i],1); }
            }
        }
        return 0;
    } catch(const data::Error& error) { std::cerr<<error.code<<' '<<error.pointer<<": "<<error.what()<<'\n';return 1; }
      catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}

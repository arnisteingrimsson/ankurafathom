#include "ankurafathom/runtime/data.hpp"
#include <iomanip>
#include <iostream>

int main(int argc,char** argv) {
    namespace data=ankurafathom::runtime::data;
    try {
        if(argc!=2) throw std::invalid_argument("usage: fathom_data_table parameters.csv|parquet|arrow");
        const data::Schema schema{{{"practice",data::Type::string,"",{}},
            {"capacity",data::Type::i32,"people",{}},{"rate",data::Type::f64,"USD/hour",{}}},"practice"};
        const auto table=data::load_file(argv[1],schema);
        std::cout<<"file_sha256="<<table.file_hash()<<"\ncanonical_sha256="<<table.canonical_hash()<<'\n';
        std::cout<<std::setprecision(17);
        for(const auto& row:table.rows()) std::cout<<std::get<std::string>(row[table.column_index("practice")])<<','
            <<std::get<std::int32_t>(row[table.column_index("capacity")])<<','
            <<std::get<double>(row[table.column_index("rate")])<<'\n';
        return 0;
    } catch(const data::Error& error) {
        std::cerr<<error.code<<' '<<error.pointer<<": "<<error.what()<<'\n';return 1;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}

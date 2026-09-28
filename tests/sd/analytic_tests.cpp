#include "ankurafathom/sd/model.hpp"
#include "ankurafathom/sd/delay.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using ankurafathom::sd::Model;
using ankurafathom::sd::Integrator;
using ankurafathom::sd::DelayKind;
using ankurafathom::sd::EulerDelay;
constexpr double eps = std::numeric_limits<double>::epsilon();
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
struct Error {
    double continuous = 0, output_continuous = 0, discrete = 0, conservation = 0;
};
void compare_discrete(Error& error, double actual, long double expected, const std::string& label) {
    require(std::isfinite(actual) && std::isfinite(expected), label+": nonfinite state/reference");
    const auto scaled = std::abs(static_cast<long double>(actual)-expected)/std::max(1.L,std::abs(expected));
    error.discrete = std::max(error.discrete, static_cast<double>(scaled));
    require(scaled <= 512*eps, label+": discrete closed-form mismatch");
}

// R(z)^k is the exact discrete solution of a linear scalar integrator.
// It is evaluated directly, without repeating the simulator's stage updates.
long double stability(long double z, Integrator method) {
    if (method == Integrator::euler) return 1+z;
    if (method == Integrator::midpoint) return 1+z+z*z/2;
    return 1+z+z*z/2+z*z*z/6+z*z*z*z/24;
}
Error exponential(Integrator method, double rate, double dt) {
    Model model;
    const auto stock = model.add_stock("amount",3);
    if (rate > 0)
        model.add_flow(Model::boundary,stock,[rate,stock](const auto& s,double){return rate*s[stock];});
    else
        model.add_flow(stock,Model::boundary,[rate,stock](const auto& s,double){return -rate*s[stock];});
    const int steps = static_cast<int>(4/dt), stride = static_cast<int>(.25/dt);
    const long double factor = stability(rate*static_cast<long double>(dt),method);
    Error error;
    for (int k=0;k<=steps;++k) {
        const double actual = model.state()[stock];
        compare_discrete(error,actual,3*std::pow(factor,k),"exponential");
        require(actual>0,"exponential positivity");
        if (k%stride==0) {
            const long double exact = 3*std::exp(rate*static_cast<long double>(k)*dt);
            error.continuous=std::max(error.continuous,static_cast<double>(std::abs(actual-exact)/3));
        }
        if (k<steps) {
            model.step(k*dt,dt,method);
            require(rate>0 ? model.state()[stock]>actual : model.state()[stock]<actual,
                    "exponential monotonicity");
        }
    }
    error.output_continuous=error.continuous;
    return error;
}

// For a cascade with step coefficient a, stage m's step response at tick k
// is P[Binomial(k,a)>=m]. This is independent of the iterative stage algorithm.
long double discrete_response(int k, int m, long double a) {
    if (k<m) return 0;
    if (a==1) return 1;
    long double tail=0, choose=1;
    for (int j=0;j<m;++j) {
        if (j>0) choose *= static_cast<long double>(k-j+1)/j;
        tail += choose*std::pow(a,j)*std::pow(1-a,k-j);
    }
    return 1-tail;
}
long double continuous_response(long double time, int m, long double stage_duration) {
    const long double z=time/stage_duration;
    long double term=1, sum=1;
    for (int j=1;j<m;++j) { term*=z/j; sum+=term; }
    return 1-std::exp(-z)*sum;
}
Error cascade(DelayKind kind,int order,bool falling,double dt,bool compare_continuous=true) {
    constexpr double theta=2;
    const double tau=order*theta, initial=falling ? 5 : 2, input=falling ? 2 : 5;
    const double amplitude=input-initial;
    EulerDelay delay(kind,order,tau,initial);
    const int steps=static_cast<int>(8/dt);
    const int stride=compare_continuous ? static_cast<int>(.25/dt) : 1;
    require(stride>0,"invalid observation stride");
    double balance=initial*tau;
    Error error;
    for (int k=0;k<=steps;++k) {
        long double expected_pipeline=0;
        for (int m=1;m<=order;++m) {
            const long double discrete=initial+amplitude*discrete_response(k,m,dt/theta);
            const double stored=delay.stages()[m-1];
            const double value=kind==DelayKind::material ? stored/theta : stored;
            compare_discrete(error,stored,kind==DelayKind::material ? discrete*theta : discrete,"cascade stage");
            require(value>=2-1e-13 && value<=5+1e-13,"cascade convex bounds");
            expected_pipeline+=discrete*theta;
            if (compare_continuous && k%stride==0) {
                const long double exact=initial+amplitude*continuous_response(k*static_cast<long double>(dt),m,theta);
                const auto difference=static_cast<double>(std::abs(value-exact)/std::abs(amplitude));
                error.continuous=std::max(error.continuous,difference);
                if (m==order) error.output_continuous=std::max(error.output_continuous,difference);
            }
        }
        compare_discrete(error,delay.output(tau),initial+amplitude*discrete_response(k,order,dt/theta),"cascade output");
        if (kind==DelayKind::material) {
            compare_discrete(error,delay.pipeline(),expected_pipeline,"material pipeline");
            const double drift=std::abs(delay.pipeline()-balance);
            error.conservation=std::max(error.conservation,drift);
            require(drift<=512*eps*std::max(1.,std::abs(balance)),"material balance at every tick");
        }
        if (k<steps) {
            balance+=dt*(input-delay.output(tau));
            const double prior=delay.output(tau);
            delay.step(input,tau,dt);
            require(falling ? delay.output(tau)<=prior : delay.output(tau)>=prior,"cascade monotonicity");
        }
    }
    return error;
}

void logistic_discrete() {
    // At r*h=1, u[k+1]=2*u[k]-u[k]^2 has the exact closed form
    // u[k]=1-(1-u[0])^(2^k). Test both sides of capacity and equilibria.
    for (const double initial : {0.,2.,6.,8.,10.}) {
        Model model;
        const auto x=model.add_stock("logistic",initial);
        model.add_flow(Model::boundary,x,[x](const auto& s,double){return s[x]*(1-s[x]/8);},false);
        Error error;
        for (int k=0;k<=8;++k) {
            const auto exact=8*(1-std::pow(1-initial/8.L,1<<k));
            compare_discrete(error,model.state()[x],exact,"logistic discrete");
            require(model.state()[x]>=0 && (k==0 || model.state()[x]<=8),"logistic bounds after initial step");
            if (k<8) model.step(k,1);
        }
    }
}

void report(std::ostream& out,const std::string& model,const std::string& method,int refinement,
            double dt,const Error& error,Error& previous,double target,double ceiling) {
    auto observed_order = [&](double current,double prior,const std::string& metric) {
        require(current>0 && std::isfinite(current),model+metric+": convergence error not measurable");
        const double order=refinement ? std::log2(prior/current) : 0;
        if (refinement) require(current<prior,model+metric+": refinement did not reduce error");
        if (refinement>=2) require(order>=target-.1 && order<=target+.1,model+metric+": convergence order outside band");
        if (refinement==3) require(current<=ceiling && current>100*eps,
                                  model+metric+": finest error ceiling or roundoff separation failed");
        return order;
    };
    const double order=observed_order(error.continuous,previous.continuous," stages");
    const double output_order=observed_order(error.output_continuous,previous.output_continuous," output");
    out<<model<<','<<method<<','<<dt<<','<<error.continuous<<','<<error.output_continuous<<','<<error.discrete<<',';
    if (refinement) out<<order;
    out<<',';
    if (refinement) out<<output_order;
    out<<','<<error.conservation<<'\n';
    previous=error;
}
} // namespace

int main(int argc,char** argv) {
    try {
        require(argc==2,"usage: sd_analytic_tests report.csv");
        std::ofstream out(argv[1]);require(static_cast<bool>(out),"cannot open report");
        out<<std::setprecision(17)<<"model,integrator,dt,max_scaled_continuous_error,max_scaled_output_error,max_scaled_discrete_error,observed_order,output_observed_order,max_conservation_drift\n";
        const std::array methods{Integrator::euler,Integrator::midpoint,Integrator::rk4};
        const std::array names{"euler","midpoint","rk4"};
        const std::array orders{1.,2.,4.},ceilings{.13,.001,2e-8};
        for (const double rate : {-.5,.5}) for (std::size_t method=0;method<methods.size();++method) {
            Error previous;
            for (int refinement=0;refinement<4;++refinement) {
                const double dt=.25/(1<<refinement);
                report(out,rate>0 ? "exponential_growth" : "exponential_decay",names[method],refinement,
                       dt,exponential(methods[method],rate,dt),previous,orders[method],ceilings[method]);
            }
        }
        for (const auto kind : {DelayKind::material,DelayKind::information})
            for (const int order : {1,3}) for (const bool falling : {false,true}) {
                const std::string name=std::string(kind==DelayKind::material ? "material" : "information")+
                    "_delay"+std::to_string(order)+(falling ? "_fall" : "_rise");
                Error previous;
                for (int refinement=0;refinement<4;++refinement) {
                    const double dt=.25/(1<<refinement);
                    report(out,name,"euler",refinement,dt,cascade(kind,order,falling,dt),previous,1,.004);
                }
                // Exact boundary and half-boundary steps include the zero-response
                // startup and complete release at the stage-duration stability limit.
                for (const double dt : {1.,2.}) (void)cascade(kind,order,falling,dt,false);
            }
        logistic_discrete();
        out.close();require(static_cast<bool>(out),"failed writing report");
        std::cout<<"56 analytic convergence trajectories, 16 delay boundary trajectories, 45 logistic discrete observations passed\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}

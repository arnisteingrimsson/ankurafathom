#pragma once
#include "ankurafathom/runtime/experiment.hpp"
#include "ankurafathom/runtime/sha256.hpp"
#include <bit>
#include <span>
namespace ankurafathom::runtime {
struct ObservationIdentity {
    std::string sha256;
    std::uint64_t trajectories,rows;
};
inline ObservationIdentity observation_identity(std::span<const Trajectory> trajectories) {
    constexpr char domain[]="AnkuraFathom.observations.v1\0";
    Sha256 hash;hash.update(std::string_view(domain,sizeof(domain)-1));
    const auto word=[&](std::uint64_t value) {
        char bytes[8];for(int i=0;i<8;++i) bytes[i]=static_cast<char>((value>>(8*i))&255);
        hash.update(std::string_view(bytes,8));
    };
    word(trajectories.size());std::uint64_t count=0;
    std::optional<std::pair<std::uint32_t,std::uint32_t>> previous;
    for(const auto& trajectory:trajectories) {
        const auto address=std::pair{trajectory.scenario,trajectory.replication};
        if(trajectory.scenario>65535 || trajectory.replication>65535 || (previous && address<=*previous))
            throw std::invalid_argument("trajectory addresses must increase and fit in 16 bits");
        previous=address;validate_observations(trajectory.observations);
        word(trajectory.scenario);word(trajectory.replication);word(trajectory.observations.size());
        for(const auto& row:trajectory.observations) {
            word(std::bit_cast<std::uint64_t>(row.time));word(row.output_id.size());hash.update(row.output_id);
            word(std::bit_cast<std::uint64_t>(row.value));++count;
        }
    }
    return {hash.digest(),static_cast<std::uint64_t>(trajectories.size()),count};
}
}

#pragma once

namespace ankurafathom::des {

enum class QueueDiscipline { fifo, priority, lifo };

inline bool valid_queue_discipline(QueueDiscipline discipline) noexcept {
    return discipline == QueueDiscipline::fifo || discipline == QueueDiscipline::priority ||
           discipline == QueueDiscipline::lifo;
}

} // namespace ankurafathom::des

"""Hand schedules verify the oracle adapters and their statistical reducer."""
from engine_oracles import ciw_run, simpy_run, compare_traces, summarize, close


def check(arrivals, durations, servers, limit, expected, losses, routes=None):
    traces = [simpy_run(arrivals, durations, servers, limit, routes),
              ciw_run(arrivals, durations, servers, limit, 123, routes)]
    for trace in traces:
        compare_traces(trace, (expected, losses))
    return traces


def main():
    # Capacity means waiting places. Complete cohorts include a job finishing
    # after the measurement window; the rejected arrival contributes no work.
    arrivals, durations = [0.0, .25, .5, 2.25], [[2.0, 1.0, 99.0, 1.0]]
    expected = [(0, 0, 0.0, 0.0, 2.0), (1, 0, .25, 2.0, 3.0), (3, 0, 2.25, 3.0, 4.0)]
    for trace in check(arrivals, durations, 1, 1, expected, [(2, .5)]):
        values = summarize(arrivals, durations, 1, 1, trace, .5, 2.0)
        targets = dict(queue=.875, utilization=1, wait=.75, throughput=.5, blocking=0, p0=0, p1=.125, p2=.875)
        assert values.keys() == targets.keys()
        assert all(close(values[k], v) for k, v in targets.items()), values
    # Two busy service places with no waiting room: a third job is lost.
    check([0.0, .1, .2], [[2.0, 3.0, 10.0]], 2, 0,
          [(0, 0, 0.0, 0.0, 2.0), (1, 0, .1, .1, 3.1)], [(2, .2)])
    # Independent stage durations and exact entries; no finite downstream queue.
    check([0.0, .5], [[1.0, 2.0], [2.0, .5], [1.0, 1.0]], 1, None,
          [(0, 0, 0.0, 0.0, 1.0), (0, 1, 1.0, 1.0, 3.0), (0, 2, 3.0, 3.0, 4.0),
           (1, 0, .5, 1.0, 3.0), (1, 1, 3.0, 3.0, 3.5), (1, 2, 3.5, 4.0, 5.0)], [])
    # Branches skip unused service draws and may finish out of source order.
    arrivals, durations = [.25, .5, .75], [[1., 1., 1.], [3., 99., .5], [99., .25, 99.]]
    routes = [[0, 1], [0, 2], [0, 1]]
    expected = [(0, 0, .25, .25, 1.25), (0, 1, 1.25, 1.25, 4.25),
                (1, 0, .5, 1.25, 2.25), (1, 2, 2.25, 2.25, 2.5),
                (2, 0, .75, 2.25, 3.25), (2, 1, 3.25, 4.25, 4.75)]
    for trace in check(arrivals, durations, 1, None, expected, [], routes):
        values = summarize(arrivals, durations, 1, None, trace, 0, 4, routes)
        targets = dict(cycle=10/3, match_fraction=2/3, s0_queue=2.25/4,
                       s1_queue=.75/4, s2_queue=0, s0_wait=.75, s1_wait=.5, s2_wait=0,
                       s0_throughput=.75, s1_throughput=0, s2_throughput=.25)
        assert all(close(values[k], v) for k, v in targets.items()), values
        assert close(sum(v for k, v in values.items() if k.startswith('p')), 1)
    print('Oracle adapters: hand finite FIFO, loss-only two-server, tandem, branch, window and cohort checks passed')


if __name__ == '__main__':
    main()

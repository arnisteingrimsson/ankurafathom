"""Requested benchmark scope, explicit executable coverage, and remaining gaps.

Each row is a requested workload, not an assertion of whole-library conformance.
The assessor checks exact test names and never treats an empty selection as a pass.
"""
ROWS = []


def add(name, family, scope, tests=(), adapter=None, gap=None, source=None):
    ROWS.append(dict(name=name, family=family, scope=scope, tests=list(tests),
                     adapter=adapter, gap=gap, source=source))


add('ARGESIM C22', 'cross-tool', 'Four policies; deterministic and 16 stochastic paths each; arrival-first and large base case; selected published deterministic tables',
    adapter='c22', gap='General declarative policies and independent-RNG distribution comparison',
    source='https://www.sne-journal.org/benchmarks/c22')
for name, gap in [('C2','Flexible assembly benchmark model adapter'),
                  ('remaining catalog','Select and implement each remaining benchmark definition; no catalog-wide claim')]:
    add('ARGESIM '+name,'cross-tool','Not implemented',gap=gap,source='https://www.argesim.org/benchmarks')
add('ARGESIM C17R','cross-tool','Hexagonal LGCA: exhaustive local rules, 30 full-state trajectories including two 10000-person baselines and once-only interventions; 12 ODE refinements against independent Taylor series',
    adapter='c17',gap='Full parameter-region/tradeoff and spatial ensemble studies; global mixing; repeated/targeted and ODE interventions; published executable docking; general hex/declarative support',
    source='https://www.sne-journal.org/fileadmin/user_upload_sne/SNE_Issues_OA/SNE_25_1/articles/sne.25.1.10283.bn17r.OA.pdf')
add('ARGESIM C21','cross-tool','Event-contact bouncing ball with/without quadratic drag; first 100 bounce times, analytic flight segments and explicit small-height rest cutoff',
    adapter='c21-ball',gap='Dynamic contact, RLC diode/DAE and pendulum cases; compensation fit; general production state-event interface',
    source='https://www.sne-journal.org/fileadmin/user_upload_sne/SNE_Issues_OA/SNE_26_2/articles/sne.26.2.10339.bn21.OA.pdf')
add('SDXorg/test-models','cross-tool','Pinned 67-file XMILE corpus: 8 imported, 59 rejected; canonical outputs for 7 compatible histories, separate RK4 analytic case',
    tests=['xmile_coverage','xmile_import','xmile_functions','xmile_rk4','xmile_delay_source','xmile_dialects','xmile_history','xmile_inputs'],
    gap='Full corpus/MDL/array support; 10 parseable unsupported and 49 malformed files remain rejected',
    source='https://github.com/SDXorg/test-models')
add('StupidModel 1–16','cross-tool','Isaac 2011 computational rules, native grids/populations vs independent record-table oracle: 43 cases with initialization, scheduling, lifecycle and hunter controls',
    adapter='stupidmodel',gap='Original Cell.Data and published Python/NetLogo executable docking; shared random inputs are not distributional equivalence',
    source='https://www.jasss.org/14/2/5.html')

for name, scope, tests in [
    ('M/M/1','Stationary single-server analytic gates',['des_single_server','des_queue_oracle']),
    ('M/M/c (Erlang C)','Stationary multi-server analytic gates',['des_multi_server']),
    ('M/M/1/K','Finite-buffer birth–death stationary gates',['des_statistical']),
    ('M/G/1','Three service laws and Pollaczek–Khinchine targets',['des_statistical']),
    ('Jackson networks','Tandem and branched networks; product-form analytic targets',['des_jackson','des_jackson_plan']),
    ('Erlang B loss','M/M/2/2 loss-system stationary gates',['des_statistical']),
    ('Little/area accounting','Clipped job intervals and time integrals; drained C22 waiting-area identity',['des_statistical','des_jackson']),
    ('Ciw/SimPy reference','Pinned independent reference outputs; native executions regenerated',['des_engine_contract','des_engine_comparison'])]:
    add(name,'DES',scope,tests=tests)
for name, tests in [('Exponential decay / teacup',['sd_model','sd_analytic']),
                     ('Logistic',['sd_nonlinear']),('Bass closed form',['sd_analytic','sd_nonlinear']),
                     ('Lotka–Volterra',['sd_nonlinear']),('Damped oscillator',['sd_nonlinear']),
                     ('DELAY3 / Erlang response',['sd_analytic','sd_delay']),
                     ('Timestep halving and Euler/midpoint/RK4 order',['sd_analytic','sd_nonlinear'])]:
    add(name,'SD','Declared finite parameters, grids and analytic/reference gates',tests=tests)
add('Game of Life','ABM analytic','67 bounded/toroidal cases; exact full-state comparisons plus block, blinker, glider',adapter='abm-known')
add('Random-walk MSD','ABM analytic','Four independent 4096-walker ensembles; finite-time binomial CDF and mean/MSD gates',adapter='abm-known')
add('Forest-fire / site percolation','ABM analytic','Exhaustive 3×3/4×4 four-neighbor bounded static-lattice connectivity and crossing polynomial',
    adapter='lattice',gap='Large-lattice threshold estimate and finite-size extrapolation; no generic dynamic forest-fire claim')
add('2D Ising','ABM analytic','3×3 periodic, zero field, J=kB=1; all states at three temperatures; exact transitions and canonical detailed balance',
    adapter='lattice',gap='Stochastic equilibration, large lattices, finite-size scaling and thermodynamic critical-temperature estimation')
add('Boltzmann wealth','ABM analytic','Finite integer exchange, conservation and pinned Mesa distribution gates',
    tests=['abm_wealth','abm_wealth_native','abm_wealth_contract','abm_wealth_comparison'],
    gap='Stationary finite-integer distribution and continuous/exponential limit are not established by the existing finite-time docking')

for name, prefix in [('SIR ABM↔SD','hybrid_sir'),('Bass ABM↔SD','hybrid_bass'),('DES↔SD fluid limit','hybrid_fluid')]:
    add(name,'hybrid','Predeclared finite-N ensembles and reference gates; timestep and population limits distinguished',
        tests=[prefix+'_native',prefix+'_plan',prefix+'_contract',prefix+'_comparison'])
add('Constant stock, no coupling drift','hybrid','Constant-rate exact oracle with off-grid zero pulses and existing nonzero pulse tests',
    tests=['hybrid_clock'],adapter='coupling')
add('Teacup DEVS atomic vs standalone','hybrid','Same merged event/tick mesh; exact Euler stability product and decreasing continuous-solution error',adapter='coupling')
add('AnyLogic Bass executable parity','hybrid','Not executed',gap='Pinned runnable licensed reference/configuration and exported results')
add('Literature coupling taxonomy','hybrid','Existing pulse, rate, aggregate, stock, entity and lifecycle couplers tested individually',
    tests=['hybrid_des_sd','hybrid_rate','hybrid_population_stock','hybrid_typed_aggregate_comparison','hybrid_lifecycle_bridge_comparison'],
    gap='Explicit mapping/acceptance against all five Morgan–Howick–Belton exchange modes and lifecycle review')

for name, prefix, gap in [
    ('Schelling','abm_schelling','NetLogo executable/version-specific docking'),
    ('Flocking','abm_boids','NetLogo executable/version-specific docking'),
    ('Sugarscape','abm_sugarscape','Full variants including reproduction, inheritance, seasons, pollution and trade'),
    ('Virus','abm_sir','NetLogo Virus-specific rules differ from the existing spatial/well-mixed SIR workloads')]:
    add(name,'classic ABM','Declared native variant and pinned independent Mesa comparison',
        tests=[prefix,prefix+'_native',prefix+'_contract',prefix+'_comparison'],gap=gap)
for name in ['Wolf–Sheep','El Farol','Mesa Epstein civil violence',"Mesa demographic prisoner's dilemma",'Mesa Sugarscape with traders']:
    add(name,'classic ABM','Not implemented',gap='Pin reference version/rules, implement native workload, predeclare stochastic acceptance')
add('CoMSES library','classic ABM','Model catalog, not one executable conformance suite',
    gap='Select a finite replication set; obtain/pin model artifacts, licenses and ODD descriptions',
    source='https://www.comses.net/codebases/')

for name in ['Hudson’s Bay lynx–hare','Bass original adoption series','Technion Anonymous Bank','JHU COVID-19']:
    add(name,'empirical/calibration','Not executed; requires data provenance and an observation/calibration model',
        gap='Pin dataset/license, units/grain and calibration/holdout procedure; empirical fit is not an exact engine oracle')
add('Synthetic Ankura parameter recovery','empirical/calibration','Forward synthetic pilot and independent arithmetic examples exist',
    gap='Fit planted parameters and assess recovery/interval coverage; forward result agreement is not causal or calibration validation')

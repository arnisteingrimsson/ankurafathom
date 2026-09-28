"""Independent structural and semantic invalid cases for the typed ABM dialect."""
import copy
import json


def cases(models):
    fixture = json.loads((models/'typed_abm_async.ir.json').read_text())
    structural, semantic = [], []
    def add(name, mutate, target=structural):
        value=copy.deepcopy(fixture); mutate(value); target.append(('abm_'+name,value))
    for key in ['id','kind','execution','fields','agents','chart']:
        add('missing_'+key,lambda m,k=key: m['components'][0].pop(k))
    for where,path in [('root',[]),('population',['components',0]),('chart',['components',0,'chart']),
                       ('field',['components',0,'fields',0]),('transition',['components',0,'chart','transitions',0]),
                       ('message',['components',0,'messages',0]),('output',['outputs',0])]:
        def mutate(m,path=path):
            for key in path: m=m[key]
            m['unexpected']=1
        add('unknown_'+where,mutate)
    for val in [-1, True, 1.5, 9007199254740992]:
        add('sequence_'+str(val),lambda m,v=val: m['components'][0]['messages'][0].__setitem__('sequence',v))
    add('mixed_trigger',lambda m: m['components'][0]['chart']['transitions'][0].__setitem__('duration','duration'))
    add('phase_in_async',lambda m: m['components'][0].__setitem__('phases',[]))
    add('output_selector',lambda m: m['outputs'][0].__setitem__('metric','active'))
    add('implicit_agent',lambda m: m['outputs'].__setitem__(0,{'id':'bad','metric':'agent','field':'state'}))
    add('time_start',lambda m: m['time'].__setitem__('start',0))
    add('multiple_population',lambda m: m['components'].append(copy.deepcopy(m['components'][0])))
    for name,mutate in [
        ('duplicate_field',lambda m: m['components'][0]['fields'].append(copy.deepcopy(m['components'][0]['fields'][0]))),
        ('foreign_target',lambda m: m['components'][0]['chart']['transitions'][0].__setitem__('target',99)),
        ('wrong_guard_unit',lambda m: m['components'][0]['chart']['transitions'][0].__setitem__('guard','duration')),
        ('wrong_agent_type',lambda m: m['components'][0]['agents'][0].__setitem__('completed',True)),
        ('missing_agent_field',lambda m: m['components'][0]['agents'][0].pop('ready')),
        ('duplicate_message',lambda m: m['components'][0]['messages'].append(copy.deepcopy(m['components'][0]['messages'][0]))),
        ('unknown_message',lambda m: m['components'][0]['messages'][0].__setitem__('event','missing')),
        ('engine_field_write',lambda m: m['components'][0]['chart']['transitions'][1]['assign'][0].__setitem__('field','state')),
        ('initialized_generation',lambda m: m['components'][0]['agents'][0].__setitem__('generation',0)),
        ('record_overflow',lambda m: m['components'][0]['agents'][0].__setitem__('completed',9007199254740992)),
    ]: add(name,mutate,semantic)
    fixture=json.loads((models/'typed_abm_neighbors.ir.json').read_text())
    for name,mutate in [
        ('space_unknown',lambda m:m['components'][0]['space'].__setitem__('unknown',1)),
        ('space_missing_kind',lambda m:m['components'][0]['space'].pop('kind')),
        ('space_wrap_type',lambda m:m['components'][0]['space'].__setitem__('wrap',1)),
        ('space_width_fraction',lambda m:m['components'][0]['space'].__setitem__('width',1.5)),
        ('space_width_zero',lambda m:m['components'][0]['space'].__setitem__('width',0)),
        ('network_unknown',lambda m:m['components'][0]['network'].__setitem__('unknown',1)),
        ('network_directed_type',lambda m:m['components'][0]['network'].__setitem__('directed',1)),
        ('network_edge_arity',lambda m:m['components'][0]['network']['edges'][0].append(2)),
        ('network_edge_type',lambda m:m['components'][0]['network']['edges'][0].__setitem__(0,True)),
        ('query_unknown',lambda m:m['components'][0]['queries'][0].__setitem__('unknown',1)),
        ('query_missing_field',lambda m:m['components'][0]['queries'][0].pop('field')),
        ('query_missing_radius',lambda m:m['components'][0]['queries'][0].pop('radius')),
        ('query_negative_radius',lambda m:m['components'][0]['queries'][0].__setitem__('radius',-1)),
        ('query_missing_unit',lambda m:m['components'][0]['queries'][0].pop('unit')),
        ('query_bad_source',lambda m:m['components'][0]['queries'][0].__setitem__('source','bad')),
        ('query_bad_op',lambda m:m['components'][0]['queries'][0].__setitem__('op','max')),
        ('query_self_type',lambda m:m['components'][0]['queries'][0].__setitem__('include_self',1)),
        ('query_count_field',lambda m:m['components'][0]['queries'][0].__setitem__('op','count')),
        ('query_network_radius',lambda m:m['components'][0]['queries'][2].__setitem__('radius',1)),
        ('query_output_mixed',lambda m:m['outputs'][1].__setitem__('field','x')),
        ('query_output_no_agent',lambda m:m['outputs'][1].pop('agent')),
    ]: add(name,mutate)
    for name,mutate in [
        ('space_duplicate_axis',lambda m:m['components'][0]['space'].__setitem__('y','x')),
        ('space_wrong_type',lambda m:m['components'][0]['space'].__setitem__('x','value')),
        ('space_foreign_field',lambda m:m['components'][0]['space'].__setitem__('x','missing')),
        ('space_collision',lambda m:m['components'][0]['agents'][1].__setitem__('x',0)),
        ('space_periodic_collision',lambda m:m['components'][0]['agents'][1].__setitem__('x',4)),
        ('space_area_limit',lambda m:m['components'][0]['space'].update(width=1001,height=1000)),
        ('network_duplicate_edge',lambda m:m['components'][0]['network']['edges'].append([0,1])),
        ('network_self_edge',lambda m:m['components'][0]['network']['edges'].append([0,0])),
        ('network_unknown_agent',lambda m:m['components'][0]['network']['edges'].append([0,9])),
        ('query_duplicate_name',lambda m:m['components'][0]['queries'][0].__setitem__('id','linked_sum')),
        ('query_field_collision',lambda m:m['components'][0]['queries'][0].__setitem__('id','group')),
        ('query_no_space',lambda m:m['components'][0].pop('space')),
        ('query_no_network',lambda m:m['components'][0].pop('network')),
        ('query_bad_unit',lambda m:m['components'][0]['queries'][0].__setitem__('unit','day')),
        ('query_fraction_grid',lambda m:m['components'][0]['queries'][0].__setitem__('radius',.5)),
        ('query_unknown_field',lambda m:m['components'][0]['queries'][0].__setitem__('field','missing')),
        ('query_recursive_field',lambda m:m['components'][0]['queries'][0].__setitem__('field','linked_sum')),
        ('query_string_sum',lambda m:m['components'][0]['queries'][2].__setitem__('field','group')),
        ('query_unknown_output',lambda m:m['outputs'][1].__setitem__('query','missing')),
    ]: add(name,mutate,semantic)
    fixture=copy.deepcopy(fixture)
    pop=fixture['components'][0]
    for f in pop['fields']:
        if f['name'] in ('x','y'): f['type']='real'
    pop['space']=dict(kind='continuous',fields=['x','y'],lower=[0,0],upper=[4,3],bin_width=.5,unit='1',wrap=True)
    for q in pop['queries']: q.pop('moore',None)
    pop['phases']=[]
    for name,mutate in [
        ('continuous_empty_axes',lambda m:m['components'][0]['space'].__setitem__('fields',[])),
        ('continuous_zero_bin',lambda m:m['components'][0]['space'].__setitem__('bin_width',0)),
        ('continuous_missing_unit',lambda m:m['components'][0]['space'].pop('unit')),
        ('continuous_axis_type',lambda m:m['components'][0]['space']['fields'].__setitem__(0,False)),
        ('continuous_extra_axis',lambda m:m['components'][0]['space'].__setitem__('fields',['x','y','value','group'])),
    ]: add(name,mutate)
    for name,mutate in [
        ('continuous_duplicate_axis',lambda m:m['components'][0]['space'].__setitem__('fields',['x','x'])),
        ('continuous_dimension',lambda m:m['components'][0]['space'].__setitem__('lower',[0])),
        ('continuous_bounds',lambda m:m['components'][0]['space'].__setitem__('upper',[0,3])),
        ('continuous_unit_mismatch',lambda m:m['components'][0]['space'].__setitem__('unit','meter')),
        ('continuous_moore',lambda m:m['components'][0]['queries'][0].__setitem__('moore',True)),
        ('continuous_string_axis',lambda m:m['components'][0]['space'].__setitem__('fields',['x','group'])),
        ('continuous_bins_overflow',lambda m:m['components'][0]['space'].__setitem__('bin_width',1e-20)),
    ]: add(name,mutate,semantic)
    fixture=json.loads((models/'typed_abm_topics.ir.json').read_text())
    for name,mutate in [
        ('topic_unknown',lambda m:m['components'][0]['topics'][0].__setitem__('unexpected',1)),
        ('topic_missing_id',lambda m:m['components'][0]['topics'][0].pop('id')),
        ('topic_missing_unit',lambda m:m['components'][0]['topics'][0].pop('unit')),
        ('topic_capacity_negative',lambda m:m['components'][0]['topics'][0].__setitem__('capacity',-1)),
        ('topic_capacity_fraction',lambda m:m['components'][0]['topics'][0].__setitem__('capacity',1.5)),
        ('topic_missing_capacity',lambda m:m['components'][0]['topics'][0].pop('capacity')),
        ('topic_budget_zero',lambda m:m['components'][0].__setitem__('delivery_budget',0)),
        ('topic_budget_bool',lambda m:m['components'][0].__setitem__('delivery_budget',True)),
        ('topic_no_declaration',lambda m:m['components'][0].pop('topics')),
        ('topic_reply_unknown',lambda m:m['components'][0]['topics'][0]['publish'][0].__setitem__('unexpected',1)),
        ('topic_reply_missing_receiver',lambda m:m['components'][0]['topics'][0]['publish'][0].pop('receiver')),
        ('topic_reply_bad_receiver',lambda m:m['components'][0]['topics'][0]['publish'][0].__setitem__('receiver','missing')),
        ('topic_reply_bool_receiver',lambda m:m['components'][0]['topics'][0]['publish'][0].__setitem__('receiver',True)),
        ('topic_reply_missing_value',lambda m:m['components'][0]['topics'][0]['publish'][0].pop('value')),
        ('topic_input_unknown',lambda m:m['components'][0]['publications'][0].__setitem__('unexpected',1)),
        ('topic_input_missing_sender',lambda m:m['components'][0]['publications'][0].pop('sender')),
        ('topic_input_negative_time',lambda m:m['components'][0]['publications'][0].__setitem__('time',-1)),
        ('topic_input_null_receiver',lambda m:m['components'][0]['publications'][0].__setitem__('receiver',None)),
        ('topic_input_fraction_sequence',lambda m:m['components'][0]['publications'][0].__setitem__('sequence',.5)),
        ('topic_input_bool_value',lambda m:m['components'][0]['publications'][0].__setitem__('value',True)),
    ]: add(name,mutate)
    for name,mutate in [
        ('topic_duplicate_id',lambda m:m['components'][0]['topics'][1].__setitem__('id','request')),
        ('topic_input_missing_topic',lambda m:m['components'][0]['publications'][0].__setitem__('topic','missing')),
        ('topic_input_foreign_sender',lambda m:m['components'][0]['publications'][0].__setitem__('sender',9)),
        ('topic_input_foreign_receiver',lambda m:m['components'][0]['publications'][0].__setitem__('receiver',9)),
        ('topic_input_after_horizon',lambda m:m['components'][0]['publications'][0].__setitem__('time',9)),
        ('topic_input_duplicate',lambda m:m['components'][0]['publications'].append(copy.deepcopy(m['components'][0]['publications'][0]))),
        ('topic_reply_foreign_receiver',lambda m:m['components'][0]['topics'][0]['publish'][0].__setitem__('receiver',9)),
        ('topic_reply_foreign_topic',lambda m:m['components'][0]['topics'][0]['publish'][0].__setitem__('topic','missing')),
        ('topic_reply_wrong_unit',lambda m:m['components'][0]['topics'][1].__setitem__('unit','day')),
        ('topic_unknown_symbol',lambda m:m['components'][0]['topics'][0]['assign'][0].__setitem__('expr','missing')),
        ('topic_reserved_parameter',lambda m:m['parameters'].append(dict(id='message_value',value=1,unit='1'))),
        ('topic_guard_wrong_unit',lambda m:(m['parameters'].append(dict(id='duration',value=1,unit='day')),m['components'][0]['topics'][0].__setitem__('guard','duration'))),
    ]: add(name,mutate,semantic)
    for mode,filename in [('phase','typed_abm_phase_publish.ir.json'),('transition','typed_abm_transition_publish.ir.json')]:
        fixture=json.loads((models/filename).read_text())
        def owner(m,mode=mode):
            p=m['components'][0]
            return p['phases'][0] if mode=='phase' else p['chart']['transitions'][0]
        for name,mutate in [
            ('unknown_key',lambda m:owner(m)['publish'][0].__setitem__('unexpected',1)),
            ('missing_topic',lambda m:owner(m)['publish'][0].pop('topic')),
            ('missing_receiver',lambda m:owner(m)['publish'][0].pop('receiver')),
            ('missing_value',lambda m:owner(m)['publish'][0].pop('value')),
            ('numeric_value',lambda m:owner(m)['publish'][0].__setitem__('value',1)),
            ('sender_target',lambda m:owner(m)['publish'][0].__setitem__('receiver','sender')),
            ('negative_target',lambda m:owner(m)['publish'][0].__setitem__('receiver',-1)),
            ('bool_target',lambda m:owner(m)['publish'][0].__setitem__('receiver',True)),
            ('not_array',lambda m:owner(m).__setitem__('publish',{})),
        ]: add('generated_'+mode+'_'+name,mutate)
        for name,mutate in [
            ('unknown_topic',lambda m:owner(m)['publish'][0].__setitem__('topic','missing')),
            ('missing_topics',lambda m:m['components'][0].pop('topics')),
            ('foreign_target',lambda m:owner(m)['publish'][0].__setitem__('receiver',9)),
            ('payload_context',lambda m:owner(m)['publish'][0].__setitem__('value','message_value')),
            ('sender_context',lambda m:owner(m)['publish'][0].__setitem__('value','message_sender')),
            ('wrong_unit',lambda m:(m['parameters'].append(dict(id='distance',value=1,unit='meter')),owner(m)['publish'][0].__setitem__('value','distance'))),
        ]: add('generated_'+mode+'_'+name,mutate,semantic)
    fixture=json.loads((models/'typed_abm_lifecycle.ir.json').read_text())
    for key in ['time','sequence','retire','births']:
        add('lifecycle_missing_'+key,lambda m,k=key:m['components'][0]['lifecycle'][0].pop(k))
    for name,mutate in [
        ('unknown',lambda m:m['components'][0]['lifecycle'][0].__setitem__('unexpected',1)),
        ('time_negative',lambda m:m['components'][0]['lifecycle'][0].__setitem__('time',-1)),
        ('time_bool',lambda m:m['components'][0]['lifecycle'][0].__setitem__('time',True)),
        ('sequence_fractional',lambda m:m['components'][0]['lifecycle'][0].__setitem__('sequence',.5)),
        ('sequence_overflow',lambda m:m['components'][0]['lifecycle'][0].__setitem__('sequence',9007199254740992)),
        ('retire_bool',lambda m:m['components'][0]['lifecycle'][0].__setitem__('retire',[True])),
        ('birth_scalar',lambda m:m['components'][0]['lifecycle'][0].__setitem__('births',[1])),
        ('fallback_bool',lambda m:m['outputs'][-1].__setitem__('inactive_value',True)),
        ('alive_field',lambda m:m['outputs'][2].__setitem__('field','work')),
        ('alive_fallback',lambda m:m['outputs'][2].__setitem__('inactive_value',0)),
        ('alive_no_agent',lambda m:m['outputs'][2].pop('agent')),
        ('aggregate_fallback',lambda m:m['outputs'][0].__setitem__('inactive_value',0)),
    ]: add('lifecycle_'+name,mutate)
    for name,mutate in [
        ('past_horizon',lambda m:m['components'][0]['lifecycle'][0].__setitem__('time',3)),
        ('duplicate_sequence',lambda m:m['components'][0]['lifecycle'][0].__setitem__('sequence',0)),
        ('duplicate_retire',lambda m:m['components'][0]['lifecycle'][0].__setitem__('retire',[0,0])),
        ('unknown_retire',lambda m:m['components'][0]['lifecycle'][0].__setitem__('retire',[9])),
        ('same_time_retire_birth',lambda m:m['components'][0]['lifecycle'][0].__setitem__('retire',[2])),
        ('retire_twice',lambda m:m['components'][0]['lifecycle'].append(dict(time=2,sequence=0,retire=[0],births=[]))),
        ('future_retire',lambda m:m['components'][0]['lifecycle'].append(dict(time=.5,sequence=0,retire=[2],births=[]))),
        ('birth_missing_field',lambda m:m['components'][0]['lifecycle'][1]['births'][0].pop('work')),
        ('birth_wrong_type',lambda m:m['components'][0]['lifecycle'][1]['births'][0].__setitem__('work',True)),
        ('unknown_alive',lambda m:m['outputs'][2].__setitem__('agent',9)),
    ]: add('lifecycle_'+name,mutate,semantic)
    fixture=json.loads((models/'typed_abm_rates.ir.json').read_text())
    fixture['components'][0]['lifecycle']=[dict(time=1,sequence=0,retire=[],births=[copy.deepcopy(fixture['components'][0]['agents'][0])])]
    add('lifecycle_birth_epoch',lambda m:m['components'][0]['lifecycle'][0]['births'][0].__setitem__('generation',0),semantic)
    fixture=json.loads((models/'typed_abm_behavior.ir.json').read_text())
    def life(m): return m['components'][0]['phases'][0]['lifecycle']
    for name,mutate in [
        ('empty',lambda m:m['components'][0]['phases'][0].__setitem__('lifecycle',{})),
        ('unknown',lambda m:life(m).__setitem__('unexpected',1)),
        ('array',lambda m:m['components'][0]['phases'][0].__setitem__('lifecycle',[])),
        ('retire_bool',lambda m:life(m).__setitem__('retire',True)),
        ('retire_empty',lambda m:life(m).__setitem__('retire','')),
        ('birth_scalar',lambda m:life(m).__setitem__('births',[1])),
        ('birth_missing_record',lambda m:life(m)['births'][0].pop('record')),
        ('birth_unknown',lambda m:life(m)['births'][0].__setitem__('unexpected',1)),
        ('birth_guard_bool',lambda m:life(m)['births'][0].__setitem__('guard',True)),
        ('birth_record_array',lambda m:life(m)['births'][0].__setitem__('record',[])),
        ('birth_assign_array',lambda m:life(m)['births'][0].__setitem__('assign',{})),
        ('limit_zero',lambda m:m['components'][0].__setitem__('agent_limit',0)),
        ('limit_bool',lambda m:m['components'][0].__setitem__('agent_limit',True)),
        ('limit_fractional',lambda m:m['components'][0].__setitem__('agent_limit',2.5)),
        ('limit_large',lambda m:m['components'][0].__setitem__('agent_limit',1000001)),
    ]: add('behavior_'+name,mutate)
    for name,mutate in [
        ('limit_initial',lambda m:m['components'][0].__setitem__('agent_limit',1)),
        ('birth_record_missing',lambda m:life(m)['births'][0]['record'].pop('age')),
        ('birth_record_type',lambda m:life(m)['births'][0]['record'].__setitem__('age',False)),
        ('birth_record_extra',lambda m:life(m)['births'][0]['record'].__setitem__('extra',0)),
        ('birth_unknown_field',lambda m:life(m)['births'][0]['assign'][0].__setitem__('field','missing')),
        ('birth_duplicate_assign',lambda m:life(m)['births'][0]['assign'].append(copy.deepcopy(life(m)['births'][0]['assign'][0]))),
        ('birth_unknown_symbol',lambda m:life(m)['births'][0]['assign'][0].__setitem__('expr','unknown')),
        ('retire_unit',lambda m:(m['parameters'].append(dict(id='duration',value=1,unit='day')),life(m).__setitem__('retire','duration'))),
        ('birth_guard_unit',lambda m:(m['parameters'].append(dict(id='duration',value=1,unit='day')),life(m)['births'][0].__setitem__('guard','duration'))),
        ('birth_assign_unit',lambda m:(m['parameters'].append(dict(id='duration',value=1,unit='day')),life(m)['births'][0]['assign'][0].__setitem__('expr','duration'))),
        ('scheduled_birth_conflict',lambda m:m['components'][0].__setitem__('lifecycle',[dict(time=1,sequence=0,retire=[],births=[copy.deepcopy(m['components'][0]['agents'][0])])])),
        ('birth_message_symbol',lambda m:life(m)['births'][0]['assign'][0].__setitem__('expr','message_sender')),
    ]: add('behavior_'+name,mutate,semantic)
    fixture=json.loads((models/'typed_abm_replacement.ir.json').read_text())
    def birth(m): return m['components'][0]['chart']['transitions'][0]['lifecycle']['births'][0]
    add('behavior_chart_epoch',lambda m:birth(m)['record'].__setitem__('generation',0),semantic)
    for name in ['state','entered','generation']:
        add('behavior_chart_reserved_'+name,lambda m,n=name:birth(m)['assign'][0].__setitem__('field',n),semantic)
    add('behavior_chart_empty',lambda m:m['components'][0]['chart']['transitions'][0].__setitem__('lifecycle',{}))
    fixture=json.loads((models/'typed_abm_network_updates.ir.json').read_text())
    def edit(m): return m['components'][0]['network_updates'][0]
    for key in ['time','sequence','add','remove']:
        add('network_update_missing_'+key,lambda m,k=key:edit(m).pop(k))
    for name,mutate in [
        ('no_network',lambda m:m['components'][0].pop('network')),
        ('unknown',lambda m:edit(m).__setitem__('unknown',1)),
        ('time_negative',lambda m:edit(m).__setitem__('time',-1)),
        ('time_bool',lambda m:edit(m).__setitem__('time',True)),
        ('sequence_fractional',lambda m:edit(m).__setitem__('sequence',.5)),
        ('sequence_overflow',lambda m:edit(m).__setitem__('sequence',9007199254740992)),
        ('add_object',lambda m:edit(m).__setitem__('add',{})),
        ('remove_object',lambda m:edit(m).__setitem__('remove',{})),
        ('edge_short',lambda m:edit(m).__setitem__('add',[[0]])),
        ('edge_long',lambda m:edit(m).__setitem__('remove',[[0,1,2]])),
        ('edge_bool',lambda m:edit(m).__setitem__('add',[[True,1]])),
        ('edge_negative',lambda m:edit(m).__setitem__('add',[[-1,1]])),
        ('edge_fractional',lambda m:edit(m).__setitem__('add',[[.5,1]])),
    ]: add('network_update_'+name,mutate)
    for name,mutate in [
        ('past_horizon',lambda m:edit(m).__setitem__('time',4)),
        ('duplicate_sequence',lambda m:m['components'][0]['network_updates'].append(copy.deepcopy(edit(m)))),
        ('unknown_endpoint',lambda m:edit(m).__setitem__('add',[[0,99]])),
        ('unborn_endpoint',lambda m:edit(m).__setitem__('add',[[0,3]])),
        ('retired_endpoint',lambda m:m['components'][0]['network_updates'][2].__setitem__('add',[[0,1]])),
        ('self_loop',lambda m:edit(m).__setitem__('add',[[0,0]])),
    ]: add('network_update_'+name,mutate,semantic)
    fixture=json.loads((models/'typed_abm_network_generator.ir.json').read_text())
    def generator(m): return m['components'][0]['network']['generator']
    for key in ['kind','stream','probability']:
        add('generator_missing_'+key,lambda m,k=key:generator(m).pop(k))
    for name,mutate in [
        ('mixed_edges',lambda m:m['components'][0]['network'].__setitem__('edges',[])),
        ('directed_flag',lambda m:m['components'][0]['network'].__setitem__('directed',False)),
        ('unknown',lambda m:generator(m).__setitem__('unknown',1)),
        ('kind',lambda m:generator(m).__setitem__('kind','unknown')),
        ('stream_bool',lambda m:generator(m).__setitem__('stream',True)),
        ('stream_negative',lambda m:generator(m).__setitem__('stream',-1)),
        ('stream_fractional',lambda m:generator(m).__setitem__('stream',.5)),
        ('stream_overflow',lambda m:generator(m).__setitem__('stream',65536)),
        ('numeric_probability',lambda m:generator(m).__setitem__('probability',.5)),
        ('empty_probability',lambda m:generator(m).__setitem__('probability','')),
        ('wrong_option',lambda m:generator(m).__setitem__('degree','2')),
        ('ws_missing_degree',lambda m:generator(m).__setitem__('kind','watts_strogatz')),
        ('ba_wrong_option',lambda m:generator(m).update(kind='barabasi_albert',m='2')),
    ]: add('generator_'+name,mutate)
    for name,expression in [('negative','-0.1'),('large','1.1'),('unknown','unknown'),('field','value'),('query','degree'),('nonfinite','1/0')]:
        add('generator_probability_'+name,lambda m,e=expression:generator(m).__setitem__('probability',e),semantic)
    add('generator_unit',lambda m:m['parameters'][0].__setitem__('unit','day'),semantic)
    for kind,option,values in [('watts_strogatz','degree',['-2','1','2.5','8']),('barabasi_albert','m',['0','1.5','8'])]:
        for expression in values:
            def mutate(m,k=kind,o=option,e=expression):
                g=generator(m); g['kind']=k; g[o]=e
                if k=='barabasi_albert': g.pop('probability')
            add('generator_'+kind+'_'+expression,mutate,semantic)
    fixture=json.loads((models/'typed_abm_rates.ir.json').read_text())
    fixture['components'][0]['network']={'generator':dict(kind='erdos_renyi',stream=901,probability='0.3')}
    add('generator_rate_stream_collision',lambda m:m['components'][0]['network']['generator'].__setitem__('stream',71),semantic)
    return structural,semantic

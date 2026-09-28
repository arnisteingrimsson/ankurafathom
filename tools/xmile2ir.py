#!/usr/bin/env python3
"""Strict scalar Euler/RK4 XMILE importer. Source units remain metadata only."""
import argparse
import bisect
import ast
import hashlib
import json
import math
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET


class Unsupported(ValueError):
    pass


def normalized(name):
    return re.sub(r'\s+', '_', name.strip().strip('"')).casefold()


def finite(value):
    try:
        number = float(value)
    except (ValueError, TypeError, OverflowError) as exc:
        raise Unsupported('expected finite number') from exc
    if not math.isfinite(number):
        raise Unsupported('expected finite number')
    return number


TOKEN = re.compile(r'\s*("[^"\n]+"|(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?|[A-Za-z_][A-Za-z_0-9]*|[(),+*/-])')
NUMBER = re.compile(r'(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?\Z')
OPS = {ast.Add: '+', ast.Sub: '-', ast.Mult: '*', ast.Div: '/'}
STATE_FUNCTIONS = {'delay1': ('material', 1), 'delay3': ('material', 3),
                   'smth1': ('information', 1), 'smth3': ('information', 3),
                   'delayn': ('material', None), 'smthn': ('information', None),
                   'delay': ('fixed', 1)}
INPUT_FUNCTIONS = {'step': 'XMILE_STEP', 'ramp': 'XMILE_RAMP', 'pulse': 'XMILE_PULSE'}


def equation(source, names):
    tokens = []
    pos = 0
    source = source.strip()
    if len(source) > 10000:
        raise Unsupported('equation too large')
    while pos < len(source):
        match = TOKEN.match(source, pos)
        if not match:
            raise Unsupported('unsupported equation token')
        token = match[1]
        pos = match.end()
        if NUMBER.fullmatch(token):
            tokens.append(repr(finite(token)))
        elif token in '(),+*/-':
            tokens.append(token)
        else:
            key = normalized(token)
            if key not in names:
                raise Unsupported(f'unknown symbol or unsupported function: {token}')
            tokens.append(names[key])
    try:
        tree = ast.parse(' '.join(tokens), mode='eval').body
    except (SyntaxError, RecursionError) as exc:
        raise Unsupported('unsupported equation syntax') from exc
    for node in ast.walk(tree):
        if not isinstance(node, (ast.Call, ast.Constant, ast.Name, ast.Load, ast.BinOp, ast.UnaryOp,
                                 ast.Add, ast.Sub, ast.Mult, ast.Div, ast.UAdd, ast.USub)):
            raise Unsupported('only scalar arithmetic and supported function calls are supported')
    return tree


def input_snap(value, scale=1):
    tolerance=8*sys.float_info.epsilon*max(1,abs(value),scale)
    if not math.isfinite(value) or abs(value)>2000000 or not math.isfinite(tolerance) or tolerance>1e-6:
        raise Unsupported('input schedule exceeds count or clock precision bounds')
    nearest=round(value)
    return nearest if abs(value-nearest)<=tolerance else value


def next_input_value(function, values, origin, dt, tick=0):
    magnitude,first=values[:2]
    scale=max(1,abs(first/dt),abs(origin/dt))
    position=input_snap((first-origin)/dt,scale)
    if function=='step': return magnitude if tick>=position else 0.
    if function=='ramp': return finite(magnitude*(max(0,tick-position)*dt))
    interval=values[2] if len(values)==3 else 0.
    if interval<0: raise Unsupported('PULSE interval must be nonnegative')
    if interval==0: count=int(tick-1<position<=tick)
    else:
        period=interval/dt
        if not math.isfinite(period) or not 1e-6<=period<=1000000:
            raise Unsupported('PULSE interval exceeds supported tick bounds')
        def cumulative(boundary):
            return max(0,math.floor(input_snap((boundary-position)/period,scale/period))+1)
        count=cumulative(tick)-cumulative(tick-1)
    return finite((magnitude/dt)*count) if count else 0.


def convert(path, *, units, outputs="default", delayn_policy="constant", input_policy="grid"):
    if units != 'metadata':
        raise Unsupported('explicit units=metadata is required; no source unit conversion is performed')
    if outputs not in ('default', 'all'):
        raise Unsupported('outputs must be default or all')
    if delayn_policy not in ('constant', 'cascade', 'history2'):
        raise Unsupported('delayn_policy must be constant, cascade, or history2')
    if input_policy not in ('grid','next_tick'):
        raise Unsupported('input_policy must be grid or next_tick')
    raw = Path(path).read_bytes()
    try:
        source = raw.decode('utf-8-sig')
    except UnicodeDecodeError as exc:
        raise Unsupported('only UTF-8 XML is supported') from exc
    if len(raw) > 2_000_000 or '<!DOCTYPE' in source.upper() or '<!ENTITY' in source.upper():
        raise Unsupported('oversized XML or document/entity declarations')
    try:
        root = ET.fromstring(source)
        return _convert(root, raw, outputs, delayn_policy, input_policy)
    except (ET.ParseError, RecursionError, SyntaxError) as exc:
        raise Unsupported(f'invalid or excessively nested XML/equation: {exc}') from exc


def _convert(root, raw, output_policy, delayn_policy, input_policy):
    namespaces = ('http://docs.oasis-open.org/xmile/ns/XMILE/v1.0', 'http://www.systemdynamics.org/XMILE')
    ns = next(('{' + n + '}' for n in namespaces if root.tag == '{' + n + '}xmile'), None)
    if ns is None or root.get('version') != '1.0':
        raise Unsupported('expected XMILE 1.0 in a supported namespace')

    def attrs(element, allowed=()):
        if set(element.attrib) - set(allowed):
            raise Unsupported(f'unsupported attributes on {element.tag}: {sorted(set(element.attrib)-set(allowed))}')

    def children(element, allowed):
        for child in element:
            if child.tag not in {ns + x for x in allowed}:
                raise Unsupported(f'unsupported element: {child.tag}')

    def single(parent, tag, required=True):
        matches = parent.findall(ns + tag)
        if len(matches) != 1:
            if not matches and not required:
                return None
            raise Unsupported(f'expected exactly one {tag}')
        return matches[0]

    def leaf(element):
        if len(element):
            raise Unsupported(f'expected scalar text in {element.tag}')
        attrs(element)
        return (element.text or '').strip()

    vendor = '{http://iseesystems.com/XMILE}'
    vendor_metadata = {}
    def vendor_leaf(element, allowed):
        attrs(element, allowed)
        if len(element) or (element.text or '').strip():
            raise Unsupported('unsupported vendor metadata content')
        vendor_metadata[element.tag] = dict(element.attrib)

    attrs(root, ('version', 'level'))
    for child in root:
        if child.tag == vendor+'prefs':
            vendor_leaf(child, ('show_module_prefix', 'live_update_on_drag', 'show_restore_buttons',
                'layer', 'interface_scale_ui', 'interface_max_page_width', 'interface_max_page_height',
                'interface_min_page_width', 'interface_min_page_height', 'saved_runs', 'keep', 'rifp'))
            continue
        if child.tag == vendor+'multiplayer_settings':
            vendor_leaf(child, ('include_chat', 'allow_observers', 'advance_time_increment', 'observer_start_page', 'enabled'))
            if child.get('enabled') != 'false':
                raise Unsupported('enabled multiplayer execution is unsupported')
            continue
        if child.tag == vendor+'time_formats':
            attrs(child, ('default_format',))
            if (child.text or '').strip() or len(child)!=1 or child[0].tag!=vendor+'time_format':
                raise Unsupported('unsupported vendor time format')
            vendor_leaf(child[0], ('name', 'type'))
            if child.get('default_format') != 'Builtin' or child[0].attrib != {'name':'Builtin','type':'adaptive'}:
                raise Unsupported('unsupported vendor time format')
            vendor_metadata[child.tag] = dict(child.attrib)
            continue
        if child.tag == ns+'default_format':
            vendor_leaf(child, ())
            continue
        if child.tag == '{isee}equation_prefs':
            continue  # editor ordering metadata only
        if child.tag not in {ns+x for x in ('header', 'sim_specs', 'model', 'dimensions', 'model_units')}:
            raise Unsupported(f'unsupported root element: {child.tag}')
        if child.tag == ns+'dimensions' and (len(child) or child.attrib):
            raise Unsupported('subscripts/dimensions are unsupported')
    spec = single(root, 'sim_specs')
    attrs(spec, ('method', 'time_units', vendor+'simulation_delay', vendor+'restore_on_start', vendor+'instantaneous_flows'))
    settings = {key:value for key,value in spec.attrib.items() if key.startswith(vendor)}
    if vendor+'simulation_delay' in settings and finite(settings[vendor+'simulation_delay'])<0:
        raise Unsupported('simulation_delay must be nonnegative')
    for key in ('restore_on_start', 'instantaneous_flows'):
        if vendor+key in settings and settings[vendor+key] != 'false':
            raise Unsupported(key+' only supports false in this batch Euler subset')
    if settings and spec.get('method', 'Euler').casefold() != 'euler':
        raise Unsupported('vendor simulation settings require Euler')
    if settings: vendor_metadata['sim_specs'] = settings
    method = spec.get('method', 'Euler').casefold()
    if method not in ('euler', 'rk4'):
        raise Unsupported('only Euler and RK4 integration are supported')
    children(spec, ('start', 'stop', 'dt'))
    start = finite(leaf(single(spec, 'start')))
    stop = finite(leaf(single(spec, 'stop')))
    dt_node = single(spec, 'dt')
    attrs(dt_node, ('reciprocal',))
    if len(dt_node):
        raise Unsupported('dt must be scalar')
    dt = finite(dt_node.text)
    reciprocal = dt_node.get('reciprocal', 'false')
    if reciprocal not in ('true', 'false'):
        raise Unsupported('invalid dt reciprocal flag')
    if dt <= 0:
        raise Unsupported('dt must be positive')
    if reciprocal == 'true':
        dt = finite(1/dt)
    horizon = stop-start
    steps = horizon/dt
    if not math.isfinite(horizon) or horizon <= 0 or not math.isfinite(steps) or not 1 <= steps <= 1_000_000 or abs(steps-round(steps)) > 1e-9:
        raise Unsupported('requires positive horizon of 1..1000000 whole ticks')
    if start != 0:
        previous = start
        for index in range(1, round(steps)+1):
            current = start+index*dt
            if (not math.isfinite(current) or current <= previous or not math.isfinite(previous+dt) or
                (method == 'rk4' and not previous < previous+dt/2 < current)):
                raise Unsupported('time grid must be finite and distinguishable at every tick and RK4 stage')
            previous = current
    model = single(root, 'model')
    attrs(model, ('name',))
    children(model, ('variables', 'views'))
    variables = single(model, 'variables')
    attrs(variables)
    names = {'time': '_time', **{name: '_fn_'+name for name in STATE_FUNCTIONS},
             **{name: '_input_'+name for name in INPUT_FUNCTIONS}}
    records, tables = {}, {}

    def register(name, ident):
        key = normalized(name)
        if not key or key in names:
            raise Unsupported('empty, reserved, or colliding normalized variable/function name')
        names[key] = ident

    def graphical_function(element, ident):
        allowed = ('name', 'type', 'discrete') if ns == '{http://www.systemdynamics.org/XMILE}' else ('name', 'type')
        attrs(element, allowed)
        children(element, ('xpts', 'ypts', 'xscale', 'yscale'))
        policy = element.get('type', 'continuous')
        if element.get('discrete', 'false') != 'false' or policy not in ('continuous', 'extrapolate'):
            raise Unsupported('only continuous or extrapolate graphical functions are supported')
        def points(tag, required=True):
            node = single(element, tag, required)
            if node is None:
                return None
            attrs(node, ('sep',))
            separator = node.get('sep', ',')
            if len(separator) != 1 or len(node):
                raise Unsupported('graphical function point separator must be one character; values must be scalar')
            return [finite(v.strip()) for v in (node.text or '').split(separator)]
        def scale(tag):
            node = single(element, tag, False)
            if node is None:
                return None
            attrs(node, ('min', 'max'))
            if len(node) or (node.text or '').strip():
                raise Unsupported('scale must contain only min/max attributes')
            lo, hi = finite(node.get('min')), finite(node.get('max'))
            if hi <= lo or not math.isfinite(hi-lo):
                raise Unsupported('scale bounds must be increasing and have finite span')
            return lo, hi
        y = points('ypts')
        x = points('xpts', False)
        xscale = scale('xscale')
        scale('yscale')  # display metadata, never a clamp on y values
        if not 2 <= len(y) <= 10000:
            raise Unsupported('graphical function requires 2..10000 points')
        if x is None:
            if xscale is None:
                raise Unsupported('graphical function needs xpts or xscale')
            lo, hi = xscale
            x = [lo+(hi-lo)*(i/(len(y)-1)) for i in range(len(y))]
            x[-1] = hi
        if len(x) != len(y) or any(b <= a or not math.isfinite(b-a) for a,b in zip(x,x[1:])):
            raise Unsupported('graphical function knots must match and x must strictly increase with finite spans')
        tables[ident] = dict(id=ident, kind='table', x=x, y=y, x_unit='1', unit='1',
                             extrapolate='clamp' if policy=='continuous' else 'linear')

    if not 1 <= len(variables) <= 1000:
        raise Unsupported('requires 1..1000 scalar variables')
    for index, variable in enumerate(variables):
        if variable.tag not in {ns+x for x in ('stock', 'flow', 'aux', 'gf')}:
            raise Unsupported(f'unsupported variable kind: {variable.tag}')
        ident = f'v{index}'
        name = variable.get('name', '')
        register(name, ident)
        kind = variable.tag[len(ns):]
        if kind == 'gf':
            graphical_function(variable, ident)
            records[ident] = dict(element=variable, name=name, kind=kind, unit=None)
            continue
        attrs(variable, ('name',))
        children(variable, ('eqn', 'doc', 'units', 'inflow', 'outflow', 'non_negative') if kind == 'stock' else ('eqn', 'doc', 'units', 'gf', 'non_negative'))
        nonnegative = single(variable, 'non_negative', required=False)
        if nonnegative is not None:
            if leaf(nonnegative):
                raise Unsupported('non_negative must be an empty tag')
            if method != 'euler':
                raise Unsupported('non_negative clipping requires Euler integration')
        unit = single(variable, 'units', required=False)
        records[ident] = dict(element=variable, name=name, kind=kind,
                              non_negative=nonnegative is not None,
                              equation=leaf(single(variable, 'eqn')),
                              unit=leaf(unit) if unit is not None else None)
        gf = single(variable, 'gf', False)
        if gf is not None:
            table_id = ident+'_table'
            graphical_function(gf, table_id)
            if gf.get('name') is not None:
                register(gf.get('name'), table_id)
            records[ident]['table'] = table_id
    for record in records.values():
        if record['kind'] == 'gf':
            continue
        record['tree'] = equation(record['equation'], names)
        if 'table' in record:
            record['tree'] = ast.Call(func=ast.Name(id=record['table']), args=[record['tree']], keywords=[])

    # One state per syntactic call, shared by every reference to its containing
    # variable. Assign all IDs before expanding input equations, permitting
    # feedback through explicit initialized state without algebraic recursion.
    calls, states, input_calls = {}, {}, []
    for owner, record in records.items():
        if 'tree' not in record:
            continue
        for node in ast.walk(record['tree']):
            if not isinstance(node, ast.Call):
                continue
            if not isinstance(node.func, ast.Name) or node.keywords:
                raise Unsupported('unsupported function call target')
            name = node.func.id
            if name.startswith('_input_'):
                if method != 'euler':
                    raise Unsupported('source input functions require Euler integration')
                if len(node.args) not in ((2,3) if name=='_input_pulse' else (2,)):
                    raise Unsupported('source input function argument count')
                input_calls.append(node)
            elif name.startswith('_fn_'):
                if method != 'euler':
                    raise Unsupported('stateful delay/smoothing functions require Euler integration')
                if record['kind'] == 'stock':
                    raise Unsupported('stateful calls in stock initializers must be declared in a separate auxiliary')
                base_args = 3 if name[4:] in ('delayn', 'smthn') else 2
                if len(node.args) not in (base_args, base_args+1):
                    raise Unsupported('delay/smoothing calls need input, duration, order for N functions, and optional initial value')
                if len(calls) >= 1000:
                    raise Unsupported('too many stateful calls')
                calls[id(node)] = dict(id=f'd{len(calls)}', node=node, owner=owner,
                                      function=name[4:], base_args=base_args)
            elif name not in tables or len(node.args) != 1:
                raise Unsupported('lookup calls require a graphical function and one argument')

    stocks = {ident: record for ident, record in records.items() if record['kind'] == 'stock'}
    endpoints = {ident: dict(source=None, destination=None) for ident, record in records.items() if record['kind'] == 'flow'}
    promoted_aux_flows = set()
    for ident, record in stocks.items():
        for tag, side in (('inflow', 'destination'), ('outflow', 'source')):
            for edge in record['element'].findall(ns+tag):
                target = names.get(normalized(leaf(edge)))
                # Some Vensim exports label stock-linked rates as auxiliaries.
                # Explicit stock edges define their flow role; leave other auxiliaries alone.
                if target in records and records[target]['kind'] == 'aux':
                    endpoints.setdefault(target, dict(source=None, destination=None))
                    promoted_aux_flows.add(target)
                if target not in endpoints or endpoints[target][side] is not None:
                    raise Unsupported('unknown, duplicate, or multiply connected flow endpoint')
                endpoints[target][side] = ident
    clipped_stocks={ident for ident,record in stocks.items() if record['non_negative']}
    clipped_flows={ident for ident in endpoints if records[ident]['non_negative']}
    limited_flows={ident for ident,ends in endpoints.items() if ends['source'] in clipped_stocks}
    if output_policy == 'all' and limited_flows:
        raise Unsupported('all outputs cannot expose stock-limited clipped flows')
    for ident,record in records.items():
        if record.get('non_negative') and record['kind']=='aux' and ident not in promoted_aux_flows:
            raise Unsupported('non_negative on an unlinked auxiliary is unsupported')
        if 'tree' in record and any(isinstance(node,ast.Name) and node.id in limited_flows
                                   for node in ast.walk(record['tree'])):
            raise Unsupported('equations cannot reference clipped flow values limited by source stocks')
    indegree={ident:0 for ident in clipped_stocks}
    for ident,ends in endpoints.items():
        if (ends['source'] in clipped_stocks or ends['destination'] in clipped_stocks) and ident not in clipped_flows:
            raise Unsupported('non_negative stocks require explicitly non_negative incident flows')
        if ends['source'] in clipped_stocks and ends['destination'] in clipped_stocks:
            indegree[ends['destination']]+=1
    ready=[ident for ident,degree in indegree.items() if degree==0]
    for ident in ready:
        for ends in endpoints.values():
            if ends['source']==ident and ends['destination'] in clipped_stocks:
                indegree[ends['destination']]-=1
                if indegree[ends['destination']]==0: ready.append(ends['destination'])
    if len(ready)!=len(clipped_stocks):
        raise Unsupported('cyclic clipped-stock flow dependencies are unsupported')

    def lookup(ident, value):
        table = tables[ident]
        x, y = table['x'], table['y']
        if value <= x[0] and table['extrapolate']=='clamp':
            return y[0]
        if value >= x[-1] and table['extrapolate']=='clamp':
            return y[-1]
        left = min(max(bisect.bisect_right(x,value)-1,0),len(x)-2)
        weight = (value-x[left])/(x[left+1]-x[left])
        return finite((1-weight)*y[left]+weight*y[left+1])

    def initial_value(node, visiting=()):
        if isinstance(node, ast.Constant):
            return finite(node.value)
        if isinstance(node, ast.Name):
            if node.id == '_time':
                return start
            if node.id not in records or records[node.id]['kind']=='gf':
                raise Unsupported('function name must be called, not read as a value')
            record = records[node.id]
            if node.id in visiting:
                raise Unsupported('cyclic initialization of stock, auxiliary, or delay')
            value = initial_value(record['tree'], visiting+(node.id,))
            return max(0.,value) if node.id in clipped_flows else value
        if isinstance(node, ast.Call):
            if node.func.id.startswith('_input_'):
                values=[initial_value(arg,visiting) for arg in node.args]
                if input_policy=='next_tick':
                    return next_input_value(node.func.id[7:],values,start,dt)
                magnitude,first=values[:2]
                ticks=grid((start-first)/dt, max(abs(start/dt),abs(first/dt)))
                if node.func.id=='_input_step': return magnitude if ticks>=0 else 0.
                if node.func.id=='_input_ramp': return finite(magnitude*(max(ticks,0)*dt))
                interval=values[2] if len(values)==3 else 0.
                repeat=grid(interval/dt)
                if interval<0 or (interval>0 and repeat<1):
                    raise Unsupported('PULSE interval must be zero or positive whole ticks')
                return finite(magnitude/dt) if ticks>=0 and (ticks==0 if repeat==0 else ticks%repeat==0) else 0.
            if id(node) in calls:
                call = calls[id(node)]
                if call['id'] in visiting:
                    raise Unsupported('cyclic delay initialization; supply an independent initial value')
                return initial_value(node.args[call['base_args']] if len(node.args)>call['base_args'] else node.args[0],
                                     visiting=visiting+(call['id'],))
            return lookup(node.func.id, initial_value(node.args[0], visiting))
        if isinstance(node, ast.UnaryOp):
            return finite((-1 if isinstance(node.op, ast.USub) else 1)*initial_value(node.operand, visiting))
        left = initial_value(node.left, visiting)
        right = initial_value(node.right, visiting)
        try:
            result = {ast.Add: lambda: left+right, ast.Sub: lambda: left-right,
                      ast.Mult: lambda: left*right, ast.Div: lambda: left/right}[type(node.op)]()
        except ZeroDivisionError as exc:
            raise Unsupported('division by zero in initialization') from exc
        return finite(result)

    def expand(node, visiting=(), duration=False):
        if isinstance(node, ast.Constant):
            return repr(finite(node.value))
        if isinstance(node, ast.Name):
            if node.id == '_time':
                return 't'
            if node.id not in records or records[node.id]['kind']=='gf':
                raise Unsupported('function name must be called, not read as a value')
            record = records[node.id]
            if record['kind'] == 'stock':
                return node.id
            if node.id in visiting:
                raise Unsupported('cyclic auxiliary/flow equations')
            value = expand(record['tree'], visiting+(node.id,), duration)
            return 'NONNEGATIVE('+value+')' if node.id in clipped_flows else value
        if isinstance(node, ast.Call):
            if node.func.id.startswith('_input_'):
                arguments=[expand(arg,visiting,duration) for arg in node.args]
                if node.func.id=='_input_pulse' and len(arguments)==2: arguments.append('0.0')
                function=INPUT_FUNCTIONS[node.func.id[7:]]
                if input_policy=='next_tick':
                    function=function.replace('XMILE_','XMILE_NEXT_')
                    arguments.append(repr(start))
                result = function+'('+','.join(arguments)+')'
            elif id(node) in calls:
                if duration:
                    raise Unsupported('duration cannot depend on delay outputs')
                return calls[id(node)]['id']
            else:
                result = node.func.id+'('+expand(node.args[0], visiting, duration)+')'
        elif isinstance(node, ast.UnaryOp):
            result = '(' + ('-' if isinstance(node.op, ast.USub) else '+') + expand(node.operand, visiting, duration) + ')'
        else:
            result = '('+expand(node.left, visiting, duration)+OPS[type(node.op)]+expand(node.right, visiting, duration)+')'
        if len(result) > 50000:
            raise Unsupported('expanded equation too large')
        return result

    def constant_value(node, label):
        expanded = ast.parse(expand(node, duration=True), mode='eval')
        if any(isinstance(part, ast.Name) and part.id not in tables and part.id != 'NONNEGATIVE' for part in ast.walk(expanded)):
            raise Unsupported(label+' must be constant (no TIME, stocks, or delay state)')
        return initial_value(node)

    def grid(ticks, clock_scale=1):
        tolerance=8*sys.float_info.epsilon*max(1,abs(ticks),clock_scale)
        if not math.isfinite(ticks) or abs(ticks)>1000000 or not math.isfinite(tolerance) or tolerance>1e-6 or abs(ticks-round(ticks))>tolerance:
            raise Unsupported('source input schedule requires bounded whole ticks')
        return round(ticks)

    for node in input_calls:
        first=constant_value(node.args[1], 'input start time')
        if input_policy=='grid':
            grid((first-start)/dt, max(abs(start/dt),abs(first/dt),abs(stop/dt)))
        else:
            if abs((first-start)/dt)>1000000:
                raise Unsupported('input start exceeds supported tick bounds')
            input_snap((first-start)/dt,max(abs(start/dt),abs(first/dt),abs(stop/dt)))
        if not math.isfinite(first+dt) or first+dt<=first:
            raise Unsupported('input start tick must be representable')
        if node.func.id=='_input_pulse':
            interval=constant_value(node.args[2], 'PULSE interval') if len(node.args)==3 else 0.
            if input_policy=='next_tick':
                for tick in (0,round(horizon/dt)):
                    next_input_value('pulse',[0,first,interval],start,dt,tick)
            elif interval<0 or (interval>0 and grid(interval/dt)<1):
                raise Unsupported('PULSE interval must be zero or positive whole ticks')

    for record in records.values():
        if 'tree' in record:
            expand(record['tree'])
    for call in calls.values():
        node = call['node']
        kind, order = STATE_FUNCTIONS[call['function']]
        if order is None:
            value = constant_value(node.args[2], 'delay order')
            if not value.is_integer() or not 1 <= value <= 255:
                raise Unsupported('delay order must be an integer in 1..255')
            order = int(value)
        # Variable-duration cascade interpretation requires an explicit dialect policy.
        if call['function'] == 'delayn' and delayn_policy == 'constant':
            constant_value(node.args[1], 'DELAYN duration')
        if call['function'] == 'delayn' and delayn_policy == 'history2':
            if order != 2:
                raise Unsupported('history2 DELAYN requires order 2; other history orders are unverified')
            kind = 'history2'
        duration_expression = expand(node.args[1], duration=True)
        duration = initial_value(node.args[1])
        initial = initial_value(node)
        if duration <= 0 or dt > duration/order:
            raise Unsupported('delay duration must cover at least one dt per stage')
        if kind == 'fixed':
            ticks = duration/dt
            if not math.isfinite(ticks) or not 1 <= round(ticks) <= 1000000 or abs(ticks-round(ticks)) > 8*sys.float_info.epsilon*round(ticks):
                raise Unsupported('fixed delay requires 1..1000000 whole ticks')
        if kind == 'material' and (initial < 0 or not math.isfinite(initial*(duration/order))):
            raise Unsupported('material delay initial output must be nonnegative with finite pipeline')
        if kind == 'history2' and not math.isfinite(initial*(duration/2)):
            raise Unsupported('history2 delay requires finite initial stage contents')
        states[call['id']] = dict(id=call['id'], kind='delay', type=kind, order=order,
                                 duration=duration_expression, initial=initial, input=expand(node.args[0]), unit='1')
    components, outputs = list(tables.values())+list(states.values()), []
    for ident, record in stocks.items():
        initial = initial_value(ast.Name(id=ident))
        if ident in clipped_stocks and initial<0:
            raise Unsupported('non_negative stock initial value must be nonnegative')
        component=dict(id=ident, kind='stock', init=initial, non_negative=ident in clipped_stocks, unit='1')
        if ident in clipped_stocks:
            component.update(clip_outflows=True, outflow_order=[names[normalized(leaf(edge))]
                for edge in record['element'].findall(ns+'outflow')])
        components.append(component)
        outputs.append(dict(id=ident+'_ts', stock=ident))
    if not stocks or output_policy == 'all':
        for ident, record in records.items():
            if record['kind'] == 'aux' or (output_policy == 'all' and record['kind'] == 'flow'):
                outputs.append(dict(id=ident+'_ts', expr=expand(ast.Name(id=ident)), unit='1'))
        if not outputs:
            raise Unsupported('requires a stock or scalar auxiliary output')
    for ident, ends in endpoints.items():
        if ends['source'] is None and ends['destination'] is None:
            raise Unsupported('unconnected flow')
        component=dict(id=ident, kind='flow', **ends, expr=expand(records[ident]['tree']), unit='1', non_negative=ident in clipped_flows)
        if ident in clipped_flows: component['clip_negative']=True
        components.append(component)
    ir = dict(ir_version='0.1', name=model.get('name') or 'xmile_import',
              integrator=method, time=dict(unit='1', dt=dt, horizon=horizon, start=start), parameters=[], components=components, outputs=outputs)
    metadata = dict(importer_version='0.11', delayn_policy=delayn_policy,
                    input_policy=('euler_next_tick_quantity_pulse' if input_policy=='next_tick' else 'euler_grid_aligned_quantity_pulse') if input_calls else None,
                    source_sha256=hashlib.sha256(raw).hexdigest(),
                    unit_policy='metadata_only', source_start=start, source_stop=stop, vendor_metadata=vendor_metadata, source_time_unit=spec.get('time_units'),
                    method='RK4' if method=='rk4' else 'Euler', flow_policy='signed_with_explicit_clipping' if clipped_flows or clipped_stocks else 'signed',
                    clipping_policy='euler_current_inflows_priority_acyclic' if clipped_flows or clipped_stocks else None,
                    promoted_aux_flows=sorted(promoted_aux_flows),
                    outputs='all' if output_policy=='all' else 'stocks_only' if stocks else 'auxiliaries_only', stateful_calls=[dict(id=c['id'], owner=c['owner'], function=c['function']) for c in calls.values()], variables=[dict(id=i, name=r['name'], kind=r['kind'], unit=r['unit']) for i,r in records.items()])
    return ir, metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--units', required=True, choices=['metadata'])
    parser.add_argument('--outputs', choices=['default', 'all'], default='default')
    parser.add_argument('--delayn-policy', choices=['constant', 'cascade', 'history2'], default='constant')
    parser.add_argument('--input-policy', choices=['grid','next_tick'], default='grid')
    args = parser.parse_args()
    try:
        ir, metadata = convert(args.source, units=args.units, outputs=args.outputs, delayn_policy=args.delayn_policy, input_policy=args.input_policy)
        sidecar = args.out.with_suffix(args.out.suffix+'.metadata.json')
        # Conversion and all path checks precede writes. Never overwrite a source/artifact.
        if args.out.resolve() == args.source.resolve() or sidecar.resolve() == args.source.resolve():
            raise Unsupported('output would overwrite source')
        if args.out.exists() or sidecar.exists():
            raise Unsupported('output or metadata already exists')
        created = []
        try:
            for path, value in ((args.out, ir), (sidecar, metadata)):
                with path.open('x') as stream:
                    created.append(path)
                    stream.write(json.dumps(value, indent=2, allow_nan=False)+'\n')
        except OSError:
            for path in created:
                path.unlink(missing_ok=True)
            raise
    except (Unsupported, OSError) as exc:
        print(f'XMILE_UNSUPPORTED: {exc}', file=sys.stderr)
        return 1
    print(f'{args.out}\n{sidecar}')
    return 0


if __name__ == '__main__':
    sys.exit(main())

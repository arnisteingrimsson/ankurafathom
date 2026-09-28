"""Declarative auxiliary DAGs: independent stage values, units, cycles and order."""
import copy
import csv
import io
import itertools
import json
from pathlib import Path
import subprocess
import sys
import tempfile

exe=Path(sys.argv[1]).resolve()
base=dict(ir_version='0.1',name='auxiliary decay',time=dict(unit='day',dt=.1,horizon=1),
    parameters=[dict(id='k',value=.2,unit='1/day')],components=[
        dict(id='x',kind='stock',init=100,unit='kg'),
        dict(id='a',kind='aux',expr='k*x',unit='kg/day'),
        dict(id='b',kind='aux',expr='2*a',unit='kg/day'),
        dict(id='loss',kind='flow',source='x',destination=None,expr='b',unit='kg/day')],
    outputs=[dict(id='stock',stock='x'),dict(id='rate',expr='b',unit='kg/day')])
with tempfile.TemporaryDirectory() as directory:
    path=Path(directory)/'model.json';checks=0
    def call(doc,code=None,pointer=None):
        global checks
        path.write_text(json.dumps(doc))
        result=subprocess.run([str(exe),'run',str(path)],capture_output=True,text=True);checks+=1
        if code:
            assert result.returncode==1,result.stdout
            error=json.loads(result.stderr)['diagnostics'][0]
            assert error['code']==code,error
            if pointer is not None:assert error['pointer']==pointer,error
        else:assert result.returncode==0,result.stderr
        return result.stdout
    for method in ('euler','rk4'):
        expected=None
        for order in itertools.permutations(range(4)):
            model=copy.deepcopy(base);model['integrator']=method
            model['components']=[model['components'][i] for i in order]
            output=call(model)
            if expected is None:expected=output
            assert output==expected
            z=-.04;factor=1+z if method=='euler' else 1+z+z*z/2+z**3/6+z**4/24
            for row in csv.DictReader(io.StringIO(output)):
                value=100*factor**round(float(row['time'])/.1)
                if row['output_id']=='rate':value*=.4
                assert abs(float(row['value'])-value)<1e-10,row
    bad=copy.deepcopy(base);bad['components'][1]['expr']='b'
    call(bad,'IR_AUX_CYCLE','/components/1/expr')
    bad=copy.deepcopy(base);bad['components'][1]['expr']='a'
    call(bad,'IR_AUX_CYCLE','/components/1/expr')
    bad=copy.deepcopy(base);bad['components'][1]['expr']='missing'
    call(bad,'IR_SYMBOL','/components/1/expr')
    bad=copy.deepcopy(base);bad['components'][1]['unit']='kg'
    call(bad,'IR_UNIT','/components/1/unit')
    bad=copy.deepcopy(base);bad['components'][1]['expr']='k*x / 0'
    call(bad,'IR_AUX_RUNTIME','/components/1/expr')
    bad=copy.deepcopy(base);bad['integrator']='rk4';bad['components'][1]['expr']='k*x*PULSE(0,1)'
    call(bad,'IR_INTEGRATOR','/components/1/expr')
    clock=copy.deepcopy(base);clock['time']['dt']=.5;clock['parameters']=[dict(id='scale',value=1,unit='kg/day/day')]
    clock['components']=[dict(id='x',kind='stock',init=0,unit='kg'),
        dict(id='rate',kind='aux',expr='t*scale',unit='kg/day'),
        dict(id='in',kind='flow',source=None,destination='x',expr='rate',unit='kg/day')]
    clock['outputs']=[dict(id='x',stock='x')]
    for method,expected in [('euler',.25),('rk4',.5)]:
        clock['integrator']=method
        assert float(list(csv.DictReader(io.StringIO(call(clock))))[-1]['value'])==expected
    delay=json.loads((Path(__file__).resolve().parents[1]/'models/delay.ir.json').read_text())
    reference=call(delay)
    delay['components'] += [dict(id='input_aux',kind='aux',expr='intake',unit='kg/day'),
                            dict(id='duration_aux',kind='aux',expr='2*dt',unit='day')]
    delay['components'][1]['input']='input_aux';delay['components'][1]['duration']='duration_aux'
    assert call(delay)==reference
    delay['components'][3]['expr']='queue'
    call(delay,'IR_AUX','/components/3/expr')
    print(f'Auxiliary DAGs: {checks} checks; declaration permutations, analytic stage/state/time values, delay integration and diagnostics pass')

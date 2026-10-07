"""Persistent local jobs and isolated CLI workers; no modifications to the engine."""
import concurrent.futures
import csv
import fcntl
import json
import os
from pathlib import Path
import platform
import signal
import subprocess
import threading
import time
import uuid

from . import VERSION
from .contracts import APIError, canonical, capture, describe, digest, fields, integer, loads, require, validate_overrides
from .metrics import Results, compare

TERMINAL = {'completed', 'failed', 'cancelled', 'interrupted'}


def save(path, value):
    temporary = path.with_name(path.name+'.'+uuid.uuid4().hex+'.tmp')
    temporary.write_bytes(canonical(value))
    os.replace(temporary, path)


class Store:
    def __init__(self, registry, state, engine, workers=2):
        self.state = Path(state).resolve()
        self.state.mkdir(parents=True, exist_ok=True)
        self._file_lock = (self.state/'service.lock').open('a')
        try:
            fcntl.flock(self._file_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            self._file_lock.close()
            raise APIError('STATE_IN_USE', 'Another service owns this state directory', 409) from error
        self.engine = Path(engine).resolve()
        require(self.engine.is_file() and os.access(self.engine, os.X_OK), 'Engine executable is unavailable')
        require(integer(workers, 1, 16), 'workers must be 1..16')
        self.engine_hash = digest(self.engine.read_bytes())
        self.identity = dict(binary_sha256=self.engine_hash, platform=platform.platform(), application_version=VERSION,
            application_sha256=digest(canonical({p.name: digest(p.read_bytes()) for p in sorted(Path(__file__).parent.glob('*.py'))})))
        self.lock = threading.RLock()
        self.jobs, self.cache, self.models, self.processes = {}, {}, {}, {}
        self.closed = False
        for child in ('models', 'runs'):
            (self.state/child).mkdir(exist_ok=True)
        registry = Path(registry).resolve()
        config = loads(registry.read_bytes())
        fields(config, ('models',), ('models',))
        require(isinstance(config['models'], list) and config['models'], 'Registry needs models')
        for item in config['models']:
            fields(item, ('model', 'descriptor'), ('model', 'descriptor'))
            model, descriptor, files = capture(registry.parent/item['model'], registry.parent/item['descriptor'])
            hashes = {name: digest(data) for name, data in files.items()}
            version = digest(canonical(hashes))
            directory = self.state/'models'/version
            directory.mkdir(exist_ok=True)
            for name, data in files.items():
                target = directory/name
                target.parent.mkdir(parents=True, exist_ok=True)
                if target.exists():
                    require(target.read_bytes() == data, 'Captured model was modified', 'INPUT_CORRUPT', 409)
                else:
                    target.write_bytes(data)
            checked = subprocess.run([str(self.engine), 'lint', str(directory/'model.json')], capture_output=True, text=True, timeout=120)
            require(checked.returncode == 0, 'Native model validation failed: '+checked.stderr[-1500:], 'INVALID_MODEL')
            description = describe(model, descriptor, directory)
            require(description['id'] not in self.models, 'Duplicate model ID')
            description.update(model_version=version, engine_identity=self.identity,
                               input_sha256=hashes, api_version='v1')
            self.models[description['id']] = dict(description=description, directory=directory, hashes=hashes)
        for path in sorted((self.state/'runs').glob('*/job.json')):
            job = loads(path.read_bytes())
            if job['status'] not in TERMINAL:
                job.update(status='interrupted', stage='interrupted', error=dict(code='SERVICE_RESTART', message='Service restarted before completion'))
                save(path, job)
            self.jobs[job['id']] = job
            if job['status'] == 'completed' and job['engine_identity'] == self.identity:
                self.cache[job['cache_key']] = job['id']
        self.pool = concurrent.futures.ThreadPoolExecutor(max_workers=workers, thread_name_prefix='fathom-run')

    def list_models(self):
        return [dict(id=key, label=value['description']['label'], model_version=value['description']['model_version'])
                for key, value in sorted(self.models.items())]

    def model(self, key):
        require(isinstance(key, str) and key in self.models, 'Unknown model', 'MODEL_NOT_FOUND', 404)
        return self.models[key]

    def _verify_inputs(self, model):
        for name, expected in model['hashes'].items():
            path = model['directory']/name
            require(path.is_file() and digest(path.read_bytes()) == expected, 'Captured input was modified', 'INPUT_CORRUPT', 409)
        require(digest(self.engine.read_bytes()) == self.engine_hash, 'Engine binary changed; restart the service', 'ENGINE_CHANGED', 409)

    def submit(self, request):
        fields(request, ('model_id', 'model_version', 'overrides', 'scenarios', 'seed', 'replications', 'threads', 'require_check'),
               ('model_id', 'model_version'))
        model = self.model(request['model_id'])
        description = model['description']
        require(request['model_version'] == description['model_version'], 'Model version changed; fetch describe again', 'MODEL_VERSION_MISMATCH', 409)
        self._verify_inputs(model)
        require(not ('overrides' in request and 'scenarios' in request), 'Use overrides or scenarios, not both')
        scenarios = request.get('scenarios', [dict(id=0, overrides=request.get('overrides', {}))])
        require(isinstance(scenarios, list) and 1 <= len(scenarios) <= 1024, 'Expected 1..1024 scenarios')
        expanded, effective = [], []
        ids = set()
        for scenario in scenarios:
            fields(scenario, ('id', 'overrides'), ('id', 'overrides'))
            require(integer(scenario['id'], 0, 65535) and scenario['id'] not in ids, 'Invalid/duplicate scenario ID')
            ids.add(scenario['id'])
            parameters = validate_overrides(description, scenario['overrides'])
            expanded.append(dict(id=scenario['id'], parameters=scenario['overrides']))
            effective.append(dict(id=scenario['id'], parameters=parameters))
        seed, replications, threads = request.get('seed', 0), request.get('replications', 1), request.get('threads', 1)
        require(integer(seed, 0, 2**64-1) and integer(replications, 1, 1024) and integer(threads, 1, 32), 'Invalid seed, replications or threads')
        require(len(scenarios)*replications <= 4096, 'Service limit: 4096 trajectories per job')
        require(type(request.get('require_check', False)) is bool, 'require_check must be boolean')
        check = request.get('require_check', description['capabilities']['validation'])
        require(not check or description['capabilities']['validation'], 'Model has no declared checks')
        experiment = dict(seed=seed, replications=replications, scenarios=sorted(expanded, key=lambda s: s['id']))
        normalized = dict(model_id=description['id'], model_version=description['model_version'], experiment=experiment,
                          threads=threads, require_check=check, engine_identity=self.identity)
        key = digest(canonical(normalized))
        with self.lock:
            require(not self.closed, 'Service is shutting down', 'UNAVAILABLE', 503)
            if key in self.cache:
                job = self.jobs[self.cache[key]]
                self._verify_artifacts(job)
                return dict(self.public(job), cache_hit=True)
            job_id = uuid.uuid4().hex
            directory = self.state/'runs'/job_id
            directory.mkdir()
            save(directory/'experiment.json', experiment)
            save(directory/'description.json', description)
            job = dict(id=job_id, status='queued', stage='queued', created_at=time.time(), updated_at=time.time(), revision=0,
                       cache_key=key, model_id=description['id'], model_version=description['model_version'],
                       engine_identity=self.identity, request=normalized, effective_parameters=effective,
                       progress=dict(kind='execution_stages', completed_trajectories=None, total_trajectories=len(scenarios)*replications),
                       error=None, artifacts={}, cancel_requested=False)
            self.jobs[job_id] = job
            save(directory/'job.json', job)
            self.pool.submit(self._worker, job_id)
            return dict(self.public(job), cache_hit=False)

    @staticmethod
    def public(job):
        return {k: v for k, v in job.items() if k not in ('cache_key', 'cancel_requested')}

    def _job(self, key):
        require(isinstance(key, str) and key in self.jobs, 'Unknown run', 'RUN_NOT_FOUND', 404)
        return self.jobs[key]

    def get(self, key):
        with self.lock:
            return loads(canonical(self.public(self._job(key))))

    def _update(self, job, **changes):
        job.update(changes, revision=job['revision']+1, updated_at=time.time())
        save(self.state/'runs'/job['id']/'job.json', job)

    def cancel(self, key):
        with self.lock:
            job = self._job(key)
            if job['status'] not in TERMINAL:
                state = 'cancelled' if job['status'] == 'queued' else 'cancelling'
                self._update(job, cancel_requested=True, status=state, stage=state)
            return self.public(job)

    def _command(self, job, command, name):
        directory = self.state/'runs'/job['id']
        with self.lock:
            if job['cancel_requested']:
                raise APIError('CANCELLED', 'Run cancelled')
            with (directory/(name+'.log')).open('wb') as log:
                process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            self.processes[job['id']] = process
        try:
            started = time.monotonic()
            while process.poll() is None:
                with self.lock:
                    cancelled = job['cancel_requested']
                if cancelled or time.monotonic()-started > 3600:
                    os.killpg(process.pid, signal.SIGTERM)
                    try:
                        process.wait(timeout=1)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait()
                    raise APIError('CANCELLED' if cancelled else 'RUN_TIMEOUT', 'Run cancelled' if cancelled else 'One-hour stage limit exceeded')
                time.sleep(.025)
            require(process.returncode == 0, (directory/(name+'.log')).read_text(errors='replace')[-3000:], 'ENGINE_ERROR')
        finally:
            with self.lock:
                self.processes.pop(job['id'], None)

    def _worker(self, key):
        job = self.jobs[key]
        directory = self.state/'runs'/key
        try:
            with self.lock:
                if job['status'] == 'cancelled':
                    return
                self._update(job, status='running', stage='executing')
            model = self.model(job['model_id'])
            self._verify_inputs(model)
            command = [str(self.engine), 'run', str(model['directory']/'model.json'), '--experiment', str(directory/'experiment.json'),
                       '--threads', str(job['request']['threads']), '--out', str(directory/'results.csv'), '--manifest', str(directory/'manifest.json')]
            if job['request']['require_check']:
                command.append('--require-check')
            self._command(job, command, 'execute')
            with self.lock:
                self._update(job, stage='verifying')
            self._command(job, [str(self.engine), 'verify-results', str(directory/'manifest.json'), '--results', str(directory/'results.csv')], 'verify')
            self._verify_inputs(model)
            artifacts = {name: digest((directory/name).read_bytes()) for name in ('results.csv', 'manifest.json', 'description.json', 'experiment.json')}
            manifest_id = loads((directory/'manifest.json').read_bytes())['id']
            with self.lock:
                require(not job['cancel_requested'], 'Run cancelled', 'CANCELLED')
                progress = dict(job['progress'], completed_trajectories=job['progress']['total_trajectories'])
                self._update(job, status='completed', stage='completed', artifacts=artifacts, progress=progress, native_manifest_id=manifest_id)
                self.cache[job['cache_key']] = key
        except Exception as error:
            with self.lock:
                cancelled = job['cancel_requested'] or isinstance(error, APIError) and error.code == 'CANCELLED'
                self._update(job, status='cancelled' if cancelled else 'failed', stage='cancelled' if cancelled else 'failed',
                             error=dict(code='CANCELLED' if cancelled else getattr(error, 'code', 'EXECUTION_ERROR'), message=str(error)), artifacts={})

    def _verify_artifacts(self, job):
        require(job['status'] == 'completed', 'Results require a completed run', 'RUN_NOT_COMPLETED', 409)
        directory = self.state/'runs'/job['id']
        for name, expected in job['artifacts'].items():
            path = directory/name
            require(path.is_file() and digest(path.read_bytes()) == expected, 'Saved run artifact was modified', 'RESULT_CORRUPT', 409)
        return directory

    def results(self, key):
        with self.lock:
            directory = self._verify_artifacts(self._job(key))
        description = loads((directory/'description.json').read_bytes())
        with (directory/'results.csv').open(newline='') as stream:
            rows = list(csv.DictReader(stream))
        return Results(description, rows)

    def summary(self, key, request):
        result = self.results(key).summary(request)
        return dict(result, provenance=self.provenance(key))

    def provenance(self, key):
        job = self.get(key)
        return dict(run_id=key, model_id=job['model_id'], model_version=job['model_version'],
                    native_manifest_id=job.get('native_manifest_id'),
                    engine_identity=job['engine_identity'], artifacts=job['artifacts'])

    def comparison(self, request):
        require(isinstance(request, dict) and 'baseline_run' in request and 'candidate_run' in request, 'Comparison requires two run IDs')
        left, right = self.get(request['baseline_run']), self.get(request['candidate_run'])
        require(left['model_version'] == right['model_version'] and left['engine_identity'] == right['engine_identity'],
                'Compare runs from the same model version and engine identity', 'INCOMPATIBLE_RUNS', 409)
        result = compare(self.results(left['id']), self.results(right['id']), request)
        return dict(result, baseline_provenance=self.provenance(left['id']), candidate_provenance=self.provenance(right['id']))

    def explain(self, key, request):
        fields(request, ('metric', 'at', 'from', 'scenario', 'replication'), ('metric', 'at'))
        result = self.results(key)
        from .contracts import number
        from .metrics import selection
        scenario, replication = selection(request)
        require(request['metric'] in result.metrics, 'Unknown metric')
        metric = result.metrics[request['metric']]
        require(result.description['capabilities']['explain'] and 'output' in metric, 'Native explanation unavailable for this model/derived metric', 'EXPLAIN_UNSUPPORTED')
        require(number(request['at']) and ('from' not in request or number(request['from'])), 'Invalid explanation times')
        directory = self.state/'runs'/key
        command = [str(self.engine), 'explain', str(directory/'manifest.json'), '--output', metric['output'],
                   '--at', str(request['at']), '--scenario', str(scenario), '--replication', str(replication)]
        if 'from' in request:
            command += ['--from', str(request['from'])]
        output = subprocess.run(command, capture_output=True, text=True, timeout=120)
        require(output.returncode == 0, output.stderr[-3000:], 'EXPLAIN_FAILED')
        return dict(explanation=loads(output.stdout), provenance=self.provenance(key))

    def close(self):
        with self.lock:
            self.closed = True
            for key in self.jobs:
                self.cancel(key)
        self.pool.shutdown(wait=True)
        fcntl.flock(self._file_lock, fcntl.LOCK_UN)
        self._file_lock.close()

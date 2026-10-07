"""Versioned visual definitions and explicit live execution through project APIs."""
import copy
import importlib.util
import json
import threading
import time
import uuid
from urllib.error import HTTPError
from urllib.request import Request

from application.fathom_service.contracts import canonical, digest, fields, require, number
from application.fathom_service.core import save


def differences(before, after, path=''):
    if isinstance(before, dict) and isinstance(after, dict):
        result = []
        for key in sorted(before.keys() | after.keys()):
            result += differences(before.get(key), after.get(key), path + '/' + key)
        return result
    if isinstance(before, list) and isinstance(after, list) and len(before) == len(after):
        return [change for index, (a, b) in enumerate(zip(before, after)) for change in differences(a, b, path + '/' + str(index))]
    return [] if before == after else [dict(path=path, before=before, after=after)]


class VisualWorkspace:
    def __init__(self, center):
        self.center = center
        self.lock = threading.RLock()
        self.sessions = {}

    def mapping(self, key):
        project = self.center.project(key)
        path = self.center.local(project['folder']) / 'visual-model.json'
        if not path.is_file():
            return dict(available=False, message='A visual definition has not been registered for this project yet.')
        mapping = json.loads(path.read_text())
        catalog = self.center.catalog(key)
        require(mapping['model_version'] == catalog['version'], 'Visual mapping model version differs')
        ids = {n['id'] for n in mapping['nodes']}
        require(len(ids) == len(mapping['nodes']), 'Duplicate visual component ID')
        params = {p['id'] for p in catalog['parameters']}
        for node in mapping['nodes']:
            require(set(node['parameters']) <= params, 'Visual component references unknown parameters')
            require(isinstance(node.get('checks', []), list) and all(isinstance(c, str) for c in node.get('checks', [])), 'Invalid check binding')
            require('metric' not in node or node['metric'] in catalog['metric_units'], 'Unknown visual metric')
        for edge in mapping['edges']:
            require(edge['source'] in ids and edge['target'] in ids, 'Visual connection references unknown component')
        changed = self.center.sources(mapping['source_sha256'])
        return dict(available=True, current=not changed, changed=changed, sha256=digest(canonical(mapping)), definition=mapping)

    def path(self, key):
        self.center.project(key)
        return self.center.state / (key + '-visual-workspace.json')

    def _read(self, key):
        path = self.path(key)
        if path.exists():
            return json.loads(path.read_text())
        config = self.center.catalog(key)['default']
        return dict(versions=[dict(revision=0, config=config, config_sha256=digest(canonical(config)), created_at=None, author='Project default', changes=[])], proposals=[])

    def get(self, key):
        with self.lock:
            mapping = self.mapping(key)
            if not mapping['available']:
                return dict(mapping=mapping)
            state = self._read(key)
            return dict(mapping=mapping, current=state['versions'][-1],
                        history=[{k:v for k,v in version.items() if k!='config'} for version in state['versions']],
                        proposals=state['proposals'])

    def _validate(self, key, config):
        path = self.center.local(self.center.project(key)['spec'])
        spec = importlib.util.spec_from_file_location('workspace_spec_' + key.replace('-', '_'), path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module.validate(config)

    def propose(self, key, body):
        fields(body, ('base_revision','config','author','note'), ('base_revision','config','author','note'))
        require(isinstance(body['author'], str) and 0 < len(body['author']) <= 100, 'Name the proposal author')
        require(isinstance(body['note'], str) and len(body['note']) <= 2000, 'Proposal note too long')
        with self.lock:
            mapping = self.mapping(key)
            require(mapping.get('current'), 'Visual mapping must match the native model before proposing changes')
            state = self._read(key)
            current = state['versions'][-1]
            require(type(body['base_revision']) is int and body['base_revision'] == current['revision'], 'Definition changed; reload before proposing', 'REVISION', 409)
            config = self._validate(key, body['config'])
            changes = differences(current['config'], config)
            require(bool(changes), 'No configuration changes to propose')
            proposal = dict(id=uuid.uuid4().hex, base_revision=current['revision'], config=config,
                            author=body['author'], note=body['note'], changes=changes, created_at=time.time(),
                            mapping_sha256=mapping['sha256'], status='proposed', contract_valid=True,
                            validation_scope='Project configuration contract only. Runtime checks execute when you run; business effects are unvalidated.')
            state['proposals'].append(proposal)
            save(self.path(key), state)
            return proposal

    def accept(self, key, body):
        fields(body, ('proposal_id',), ('proposal_id',))
        with self.lock:
            state = self._read(key)
            proposal = next((p for p in state['proposals'] if p['id'] == body['proposal_id']), None)
            require(proposal is not None and proposal['status'] == 'proposed', 'Proposal is unavailable')
            require(proposal['base_revision'] == state['versions'][-1]['revision'], 'Proposal is stale; submit against the current version', 'REVISION', 409)
            mapping = self.mapping(key)
            require(mapping.get('current') and mapping['sha256'] == proposal['mapping_sha256'], 'Visual mapping changed; propose again')
            config = self._validate(key, proposal['config'])
            version = dict(revision=proposal['base_revision'] + 1, config=config, config_sha256=digest(canonical(config)),
                           created_at=time.time(), author=proposal['author'], note=proposal['note'], changes=proposal['changes'], mapping_sha256=mapping['sha256'])
            proposal['status'] = 'accepted'
            state['versions'].append(version)
            save(self.path(key), state)
            return version

    def remote(self, key, path, body):
        project = self.center.project(key)
        request = Request(f'http://127.0.0.1:{project["port"]}{path}', data=canonical(body), headers={'Content-Type':'application/json'}, method='POST')
        try:
            with self.center.http.open(request, timeout=125) as response:
                return json.load(response)
        except HTTPError as error:
            try:
                detail = json.load(error).get('error', {}).get('message', 'Project request failed')
            finally:
                error.close()
            require(False, detail, 'PROJECT_REQUEST', 422)
        except OSError:
            require(False, 'Customer service is unavailable. Start it in Runtime, then try again.', 'PROJECT_OFFLINE', 503)

    def start(self, key, body):
        fields(body, ('revision',), ('revision',))
        require(key == 'equinix-ai', 'Live visual adapter is not registered for this project')
        with self.lock:
            mapping = self.mapping(key)
            require(mapping.get('current'), 'Visual mapping differs from the native model')
            current = self._read(key)['versions'][-1]
            require(type(body['revision']) is int and body['revision'] == current['revision'], 'Refresh the accepted definition before running', 'REVISION', 409)
            config = self._validate(key, current['config'])
            result = self.remote(key, '/api/sessions', config)
            require(result['config_sha256'] == current['config_sha256'], 'Project executed a different definition')
            self.sessions[result['id']] = dict(project=key, revision=current['revision'], config_sha256=current['config_sha256'])
            save(self.center.state / ('visual-run-' + result['id'] + '.json'), dict(**self.sessions[result['id']], session=result['id'], mapping_sha256=mapping['sha256']))
            return dict(**result, definition_revision=current['revision'])

    def run_action(self, key, run, action, body):
        with self.lock:
            require(run in self.sessions and self.sessions[run]['project'] == key, 'Unknown visual-workspace session')
            require(action in ('step','stop','evidence'), 'Unknown run action')
            if action == 'stop':
                require(body == {}, 'Empty object required')
            if action == 'step':
                fields(body, ('expected_revision','days'), ('expected_revision','days'))
                require(type(body['expected_revision']) is int and number(body['days']) and .25 <= body['days'] <= 7, 'Invalid advance request')
            return self.remote(key, '/api/sessions/' + run + '/' + action, body)

    def close(self):
        for run, owner in self.sessions.items():
            try:
                self.remote(owner['project'], '/api/sessions/' + run + '/stop', {})
            except Exception:
                pass

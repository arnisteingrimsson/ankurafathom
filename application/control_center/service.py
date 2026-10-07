"""Read real project artifacts and manage only explicitly launched local services."""
import copy
import importlib.util
import json
import re
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path
from urllib.error import URLError
from urllib.request import build_opener, ProxyHandler

from application.fathom_service.contracts import APIError, canonical, digest, fields, require
from application.fathom_service.core import save
from .registry import PROJECTS
from .workspace import VisualWorkspace


class ControlCenter:
    def __init__(self, root, state, projects=None):
        self.root, self.state = root.resolve(), state.resolve()
        self.state.mkdir(parents=True, exist_ok=True)
        self.projects = copy.deepcopy(PROJECTS if projects is None else projects)
        self.lock = threading.RLock()
        self.children, self.logs, self.cache = {}, {}, {}
        self.http = build_opener(ProxyHandler({}))
        self.workspace = VisualWorkspace(self)

    def project(self, key):
        require(key in self.projects, 'Unknown project', 'NOT_FOUND', 404)
        return self.projects[key]

    def local(self, relative):
        path = (self.root / relative).resolve()
        require(path.is_relative_to(self.root), 'Artifact escapes the repository')
        return path

    def sha(self, path):
        stat = path.stat()
        key = ('sha', str(path), stat.st_mtime_ns, stat.st_size)
        with self.lock:
            if key not in self.cache:
                self.cache[key] = digest(path.read_bytes())
            return self.cache[key]

    def sources(self, sources):
        changed = []
        for relative, expected in sources.items():
            path = self.local(relative)
            if not path.is_file() or self.sha(path) != expected:
                changed.append(relative)
        return changed

    def catalog(self, key):
        path = self.local(self.project(key)['spec'])
        spec = importlib.util.spec_from_file_location('fathom_console_spec_' + key.replace('-', '_'), path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module.catalog()

    def runtime(self, key):
        project = self.project(key)
        url = 'http://127.0.0.1:' + str(project['port'])
        child = self.children.get(key)
        owned = child is not None and child.poll() is None
        try:
            with self.http.open(url + project['health'], timeout=.6) as response:
                data = json.loads(response.read(1_048_576))
            status = 'online' if data.get('version') == project['version'] else 'port_conflict'
        except (OSError, URLError):
            status = 'starting' if owned else 'offline'
        except (ValueError, AttributeError):
            status = 'port_conflict'
        return dict(status=status, url=url, owned=owned,
                    exit_code=child.poll() if child else None,
                    message={'online': 'Customer application is available. Opening it does not run the simulation.',
                             'offline': 'Customer application is stopped.',
                             'starting': 'The service process is starting; refresh to check readiness.',
                             'port_conflict': 'This port is not serving the registered project.'}[status])

    def build_status(self, key):
        project = self.project(key)
        runner = self.local(project['runner'])
        receipt = runner.with_suffix('.build.json')
        missing = [name for name in project['prerequisites'] if not self.local(name).is_file()]
        result = dict(ready=False, runner=project['runner'], missing=missing,
                      command=f'.venv-runtime/bin/python {project["build"]}', changed=[])
        if not runner.is_file() or not receipt.is_file():
            return dict(result, status='missing', message='Native runner or its build receipt is missing.')
        try:
            build = json.loads(receipt.read_text())
            changed = self.sources(build['sources'])
            binary_matches = self.sha(runner) == build['runner_sha256']
            ready = not changed and binary_matches and not missing
            return dict(result, ready=ready, status='ready' if ready else 'needs_attention',
                        changed=changed, binary_matches=binary_matches, sha256=build['runner_sha256'],
                        source_count=len(build['sources']), compiler=build.get('compiler'),
                        message='Runner matches its recorded sources.' if ready else 'Build identity or startup prerequisites need attention.')
        except (KeyError, ValueError, OSError, APIError) as error:
            return dict(result, status='invalid_receipt', message=str(error))

    def verification(self, key):
        project = self.project(key)
        path = self.local(project['verification'])
        if not path.exists():
            return dict(available=False, status='not_recorded', path=project['verification'])
        try:
            receipt = json.loads(path.read_text())
            changed = self.sources(receipt['sources'])
            return dict(available=True, status='current' if receipt.get('passed') and not changed else 'review',
                        passed=receipt.get('passed', False), sources_current=not changed, changed=changed,
                        scope=receipt.get('scope', ''), created_at=receipt.get('created_at'),
                        source_count=len(receipt['sources']), path=project['verification'], receipt=receipt)
        except (KeyError, ValueError, OSError, APIError) as error:
            return dict(available=True, status='invalid_receipt', error=str(error), path=project['verification'])

    def run_paths(self, key):
        paths = []
        for origin, folder in self.project(key)['run_roots'].items():
            for path in self.local(folder).glob('*/session.json'):
                if re.fullmatch('[0-9a-f]{32}', path.parent.name) and path.resolve().is_relative_to(self.root):
                    paths.append((origin, path))
        return sorted(paths, key=lambda pair: pair[1].stat().st_mtime_ns, reverse=True)

    def record_path(self, key, run):
        match = re.fullmatch(r'([a-z]+)-([0-9a-f]{32})', run)
        roots = self.project(key)['run_roots']
        require(match is not None and match[1] in roots, 'Unknown run', 'NOT_FOUND', 404)
        path = self.local(roots[match[1]]) / match[2] / 'session.json'
        path = self.local(path)
        require(path.is_file(), 'Run record not found', 'NOT_FOUND', 404)
        return path

    @staticmethod
    def integrity(record):
        errors = []
        if record.get('record_sha256') != digest(canonical({k: v for k, v in record.items() if k != 'record_sha256'})):
            errors.append('Whole-record hash mismatch')
        if record.get('config_sha256') != digest(canonical(record.get('config'))):
            errors.append('Definition hash mismatch')
        previous = None
        for frame in record.get('frames', []):
            if frame.get('previous_sha256') != previous or frame.get('sha256') != digest(canonical({k: v for k, v in frame.items() if k != 'sha256'})):
                errors.append('Frame chain/hash mismatch at ' + str(frame.get('time')))
                break
            previous = frame.get('sha256')
        if not record.get('frames'):
            errors.append('No frames recorded')
        return dict(passed=not errors, errors=errors, scope='Artifact consistency only; not proof of business validity or external authenticity.')

    def summary(self, origin, path):
        stat = path.stat()
        key = ('run', str(path), stat.st_mtime_ns, stat.st_size)
        with self.lock:
            if key in self.cache:
                return copy.deepcopy(self.cache[key])
        try:
            record = json.loads(path.read_text())
            frames = record.get('frames', [])
            final = frames[-1] if frames else {}
            value = dict(id=origin + '-' + path.parent.name, origin=origin, created_at=record.get('created_at'),
                         saved_status=record.get('status', 'unknown'), model=record.get('model'),
                         seed=record.get('config', {}).get('parameters', {}).get('seed'),
                         time=record.get('computed_through', final.get('time')), frames=len(frames),
                         checks_passed=all(frame.get('checks_passed') is True for frame in frames) if frames else None,
                         config_sha256=record.get('config_sha256'), record_sha256=record.get('record_sha256'),
                         metrics=final.get('metrics', {}), bytes=stat.st_size)
        except (ValueError, OSError, TypeError, AttributeError) as error:
            value = dict(id=origin + '-' + path.parent.name, origin=origin, saved_status='unreadable', error=str(error), bytes=stat.st_size)
        with self.lock:
            self.cache[key] = value
        return copy.deepcopy(value)

    def runs(self, key, offset=0, limit=20):
        paths = self.run_paths(key)
        return dict(total=len(paths), offset=offset, limit=limit,
                    runs=[self.summary(origin, path) for origin, path in paths[offset:offset + limit]],
                    next_offset=offset + limit if offset + limit < len(paths) else None,
                    time_unit=self.project(key)['time_unit'])

    def run(self, key, run):
        path = self.record_path(key, run)
        record = json.loads(path.read_text())
        frame = record['frames'][-1] if record.get('frames') else {}
        return dict(**self.summary(run.split('-')[0], path), config=record.get('config'),
                    methods=record.get('methods', []), limitations=record.get('limitations', []),
                    checks=frame.get('checks', []), runner_sha256=record.get('runner_sha256'),
                    integrity=self.integrity(record), path=str(path.relative_to(self.root)),
                    source_count=len(record.get('source_sha256', {})), source_sha256=record.get('source_sha256', {}), build=record.get('build', {}),
                    observations=[dict(index=i, time=f['time'], checks_passed=f.get('checks_passed'), events=len(f.get('events', []))) for i, f in enumerate(record.get('frames', []))],
                    time_unit=self.project(key)['time_unit'])

    def frame(self, key, run, index):
        record = json.loads(self.record_path(key, run).read_text())
        require(type(index) is int and 0 <= index < len(record.get('frames', [])), 'Frame index outside recorded range')
        return dict(index=index, total=len(record['frames']), frame=record['frames'][index],
                    time_unit=self.project(key)['time_unit'],
                    scope='Saved observation, not live execution. Events are the recent-event window retained by this project; intervening events may be absent.')

    def source(self, key, kind):
        require(kind in ('source', 'spec'), 'Unknown source type')
        relative = self.project(key)[kind]
        path = self.local(relative)
        return dict(path=relative, sha256=self.sha(path), content=path.read_text(),
                    scope='Current project source; saved run identities may refer to an earlier version.')

    def notes(self, key, body=None):
        self.project(key)
        path = self.state / (key + '-notes.json')
        with self.lock:
            current = json.loads(path.read_text()) if path.exists() else dict(revision=0, text='', updated_at=None)
            if body is not None:
                fields(body, ('revision', 'text'), ('revision', 'text'))
                require(type(body['revision']) is int and body['revision'] == current['revision'], 'Notes changed; refresh before saving', 'REVISION', 409)
                require(isinstance(body['text'], str) and len(body['text']) <= 20000, 'Notes must be at most 20,000 characters')
                current = dict(revision=current['revision'] + 1, text=body['text'], updated_at=time.time())
                save(path, current)
            return current

    def detail(self, key):
        project = self.project(key)
        paths = self.run_paths(key)
        return dict(id=key, **{k: project[k] for k in ('name', 'client', 'domain', 'description', 'folder', 'inputs', 'time_unit')},
                    catalog=self.catalog(key), runtime=self.runtime(key), build=self.build_status(key),
                    verification=self.verification(key), notes=self.notes(key), run_count=len(paths),
                    latest_run=self.summary(*paths[0]) if paths else None)

    def list_projects(self):
        return dict(projects=[dict(id=key, name=p['name'], client=p['client'], description=p['description'])
                              for key, p in self.projects.items()])

    def start(self, key):
        with self.lock:
            project = self.project(key)
            runtime = self.runtime(key)
            if runtime['status'] in ('online', 'starting'):
                return runtime
            require(runtime['status'] != 'port_conflict', 'A different service occupies the project port')
            require(self.build_status(key)['ready'], 'Resolve the build/startup prerequisites first')
            log = (self.state / (key + '-server.log')).open('ab')
            try:
                child = subprocess.Popen([sys.executable, str(self.local(project['server'])), '--port', str(project['port']), '--state', str(self.local(project['state']))],
                                         cwd=self.root, stdin=subprocess.DEVNULL, stdout=log, stderr=log)
            except Exception:
                log.close()
                raise
            if key in self.logs:
                self.logs[key].close()
            self.children[key], self.logs[key] = child, log
            return self.runtime(key)

    def stop(self, key):
        with self.lock:
            self.project(key)
            child = self.children.get(key)
            require(child is not None and child.poll() is None, 'This console can stop only a service it started')
            child.send_signal(signal.SIGINT)
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.terminate()
                child.wait(timeout=5)
            self.logs[key].close()
            return self.runtime(key)

    def close(self):
        self.workspace.close()
        for key, child in self.children.items():
            if child.poll() is None:
                self.stop(key)
        for log in self.logs.values():
            log.close()

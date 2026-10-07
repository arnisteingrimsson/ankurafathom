"""Control-center contracts: provenance, routing, notes and process ownership."""
import copy
import json
import tempfile
import threading
import unittest
from pathlib import Path
from unittest.mock import Mock, patch
from urllib.error import HTTPError
from urllib.request import build_opener, ProxyHandler, Request

from application.fathom_service.contracts import APIError, canonical, digest
from application.fathom_service.http import Server
from .server import ConsoleHandler
from .service import ControlCenter


class ConsoleTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name).resolve()
        (self.root / 'model.cpp').write_text('// fixture\n')
        (self.root / 'runner').write_bytes(b'fixture-runner')
        (self.root / 'runner.build.json').write_text(json.dumps(dict(runner_sha256=digest(b'fixture-runner'), sources={'model.cpp': digest(b'// fixture\n')})))
        (self.root / 'verification.json').write_text(json.dumps(dict(passed=True, scope='fixture only', sources={'model.cpp': digest(b'// fixture\n')})))
        self.registry = {'fixture': dict(name='Fixture', client='Test', description='Test project', runner='runner', build='build.py', server='server.py', state='runs', port=1, health='/api/catalog', version='fixture-1', verification='verification.json', prerequisites=[], run_roots={'customer':'runs'}, source='model.cpp', time_unit='day')}
        self.c = ControlCenter(self.root, self.root / 'console', self.registry)
        config = dict(parameters=dict(seed=42, rate=2))
        frames = []
        for t in (0, 1):
            frame = dict(time=t, metrics={'value':t * 2}, checks=[], events=[dict(time=t, kind='fixture', text='Recorded event')], checks_passed=True, previous_sha256=frames[-1]['sha256'] if frames else None)
            frame['sha256'] = digest(canonical(frame))
            frames.append(frame)
        self.record = dict(config=config, config_sha256=digest(canonical(config)), model='fixture-1', status='completed', frames=frames, computed_through=1, source_sha256={}, created_at=123)
        self.record['record_sha256'] = digest(canonical(self.record))
        self.run_id = 'customer-' + 'a' * 32
        self.path = self.root / 'runs' / ('a' * 32) / 'session.json'
        self.path.parent.mkdir(parents=True)
        self.path.write_text(json.dumps(self.record))

    def tearDown(self):
        self.c.close()
        self.tmp.cleanup()

    def test_record_identity_trace_and_tampering(self):
        result = self.c.run('fixture', self.run_id)
        self.assertTrue(result['integrity']['passed'])
        self.assertEqual(result['config']['parameters']['rate'], 2)
        self.assertEqual(self.c.frame('fixture', self.run_id, 1)['frame'], self.record['frames'][1])
        self.record['frames'][0]['metrics']['value'] = 99
        # Rehashing the envelope must not hide a tampered frame.
        self.record['record_sha256'] = digest(canonical({k:v for k,v in self.record.items() if k != 'record_sha256'}))
        self.assertFalse(self.c.integrity(self.record)['passed'])

    def test_source_and_build_freshness(self):
        self.assertTrue(self.c.build_status('fixture')['ready'])
        self.assertEqual(self.c.verification('fixture')['status'], 'current')
        (self.root / 'model.cpp').write_text('// changed source\n')
        self.assertFalse(self.c.build_status('fixture')['ready'])
        self.assertFalse(self.c.verification('fixture')['sources_current'])
        self.assertEqual(self.c.source('fixture', 'source')['content'], '// changed source\n')

    def test_run_cache_invalidation_and_pagination(self):
        self.assertEqual(self.c.runs('fixture')['runs'][0]['saved_status'], 'completed')
        self.record['status'] = 'stopped'
        self.path.write_text(json.dumps(self.record))
        self.assertEqual(self.c.runs('fixture')['runs'][0]['saved_status'], 'stopped')
        self.assertEqual(self.c.runs('fixture', 1)['runs'], [])

    def test_path_and_frame_boundaries(self):
        for key in ('../../model.cpp', 'acceptance-'+'a'*32, 'customer-not-a-run'):
            with self.assertRaises(APIError):
                self.c.record_path('fixture', key)
        with self.assertRaises(APIError): self.c.local('../outside')
        with self.assertRaises(APIError): self.c.frame('fixture', self.run_id, 2)
        with self.assertRaises(APIError): self.c.source('fixture', '../../secret')
        with self.assertRaises(APIError): self.c.project('missing')

    def test_notes_are_versioned_and_do_not_change_model(self):
        before = self.path.read_bytes()
        result = self.c.notes('fixture', dict(revision=0, text='Review assumptions'))
        self.assertEqual(result['revision'], 1)
        with self.assertRaises(APIError): self.c.notes('fixture', dict(revision=0, text='Stale edit'))
        self.assertEqual(self.c.notes('fixture')['text'], 'Review assumptions')
        self.assertEqual(self.path.read_bytes(), before)

    def test_start_stop_ownership_and_fixed_command(self):
        with self.assertRaises(APIError): self.c.stop('fixture')
        child = Mock()
        child.poll.return_value = None
        with patch.object(self.c, 'runtime', return_value=dict(status='offline')), patch('application.control_center.service.subprocess.Popen', return_value=child) as launch:
            self.c.start('fixture')
            self.assertEqual(launch.call_args.args[0][1], str(self.root / 'server.py'))
            self.assertNotIn('shell', launch.call_args.kwargs)
        child.wait.side_effect = lambda **_: setattr(child.poll, 'return_value', 0)
        with patch.object(self.c, 'runtime', return_value=dict(status='offline')):
            self.c.stop('fixture')
        child.send_signal.assert_called_once()

    def test_http_trace_download_and_origin_guard(self):
        server = Server(('127.0.0.1', 0), None)
        server.RequestHandlerClass = ConsoleHandler
        server.control_center = self.c
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        client = build_opener(ProxyHandler({}))
        base = f'http://127.0.0.1:{server.server_port}'
        try:
            with client.open(base + '/api/projects/fixture/runs/' + self.run_id + '/frames/1') as response:
                self.assertEqual(json.load(response)['frame']['metrics']['value'], 2)
            with client.open(base + '/api/projects/fixture/runs/' + self.run_id + '/record') as response:
                self.assertIn('attachment', response.headers['Content-Disposition'])
                self.assertEqual(response.read(), self.path.read_bytes())
            with self.assertRaises(HTTPError) as error:
                client.open(Request(base + '/api/projects', headers={'Origin':'https://untrusted.example'}))
            self.assertEqual(error.exception.code, 403)
            error.exception.close()
            with client.open(base + '/') as response:
                self.assertIn(b'Open customer UI', response.read())
        finally:
            server.shutdown()
            server.server_close()
            worker.join()


if __name__ == '__main__':
    unittest.main(verbosity=2)

"""Visual mapping, accepted configuration versions and live adapter contracts."""
import copy
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from application.fathom_service.contracts import APIError, canonical, digest
from .service import ControlCenter

ROOT = Path(__file__).resolve().parents[2]


class WorkspaceTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.center = ControlCenter(ROOT, Path(self.tmp.name))
        self.w = self.center.workspace

    def tearDown(self):
        self.w.sessions.clear()
        self.center.close()
        self.tmp.cleanup()

    def proposal(self, saving=.3, base=0):
        config = copy.deepcopy(self.w.get('equinix-ai')['current']['config'])
        config['parameters']['design_saving'] = saving
        return self.w.propose('equinix-ai', dict(base_revision=base, config=config, author='Test author', note='Test proposal'))

    def test_graph_binds_parameters_states_and_native_source(self):
        result = self.w.get('equinix-ai')
        self.assertTrue(result['mapping']['current'])
        graph = result['mapping']['definition']
        catalog = self.center.catalog('equinix-ai')
        bound = {p for n in graph['nodes'] for p in n['parameters']} | set(graph['global_parameters'])
        self.assertEqual(bound, {p['id'] for p in catalog['parameters']})
        self.assertEqual(next(n for n in graph['nodes'] if n['id']=='design')['states'], [2,3])
        self.assertFalse(self.w.get('tr-operating')['mapping']['available'])

    def test_propose_accept_and_persist_immutable_versions(self):
        proposal = self.proposal()
        self.assertEqual(self.w.get('equinix-ai')['current']['revision'], 0)
        self.assertEqual(proposal['changes'], [dict(path='/parameters/design_saving',before=0,after=.3)])
        current = self.w.accept('equinix-ai', dict(proposal_id=proposal['id']))
        self.assertEqual(current['revision'], 1)
        self.assertEqual(current['config']['parameters']['design_saving'], .3)
        stored = json.loads(self.w.path('equinix-ai').read_text())
        self.assertEqual(stored['versions'][0]['config']['parameters']['design_saving'], 0)
        self.assertEqual(stored['versions'][1]['config_sha256'], digest(canonical(current['config'])))
        with self.assertRaises(APIError):
            self.w.accept('equinix-ai', dict(proposal_id=proposal['id']))

    def test_stale_and_invalid_proposals_are_rejected(self):
        first = self.proposal(.2)
        second = self.proposal(.4)
        self.w.accept('equinix-ai', dict(proposal_id=first['id']))
        with self.assertRaises(APIError): self.w.accept('equinix-ai', dict(proposal_id=second['id']))
        with self.assertRaises(APIError): self.proposal(.5, base=0)
        with self.assertRaises(APIError): self.proposal(1.5, base=1)
        with patch.object(self.center, 'sources', return_value=['native/model.cpp']):
            with self.assertRaises(APIError): self.proposal(.3, base=1)

    def test_execution_uses_only_accepted_definition(self):
        proposed = self.proposal(.4)
        current = self.w.get('equinix-ai')['current']
        response = dict(id='a'*32, config_sha256=current['config_sha256'], frame={'time':0}, revision=0, status='paused')
        with patch.object(self.w, 'remote', return_value=response) as remote:
            result = self.w.start('equinix-ai', dict(revision=0))
            self.assertEqual(remote.call_args.args[2]['parameters']['design_saving'], 0)
            self.assertEqual(result['frame']['time'], 0)
            self.assertEqual(result['definition_revision'], 0)
        with self.assertRaises(APIError): self.w.run_action('equinix-ai', 'b'*32, 'step', dict(expected_revision=0,days=1))
        with self.assertRaises(APIError): self.w.run_action('tr-operating', 'a'*32, 'stop', {})
        with patch.object(self.w, 'remote', return_value=dict(status='paused')) as remote:
            self.w.run_action('equinix-ai','a'*32,'step',dict(expected_revision=0,days=1))
            self.assertEqual(remote.call_args.args[1], '/api/sessions/'+'a'*32+'/step')
        self.w.accept('equinix-ai', dict(proposal_id=proposed['id']))
        with self.assertRaises(APIError): self.w.start('equinix-ai', dict(revision=0))

    def test_model_parameters_reject_illegal_site_capacity(self):
        config = copy.deepcopy(self.w.get('equinix-ai')['current']['config'])
        config['sites'][0]['liquid_racks'] = config['sites'][0]['racks'] + 1
        with self.assertRaises(APIError):
            self.w.propose('equinix-ai',dict(base_revision=0,config=config,author='Test',note='Invalid'))


if __name__ == '__main__':
    unittest.main(verbosity=2)

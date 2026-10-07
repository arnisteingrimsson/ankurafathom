"""Dependency-free client; the same JSON contract can be used from browser fetch."""
import json
import time
from urllib.error import HTTPError
from urllib.parse import urlencode
from urllib.request import Request, urlopen


class Client:
    def __init__(self, base_url='http://127.0.0.1:8765'):
        self.base_url = base_url.rstrip('/')

    def request(self, method, path, body=None):
        data = None if body is None else json.dumps(body, allow_nan=False).encode()
        request = Request(self.base_url+path, data=data, method=method, headers={'Content-Type': 'application/json'})
        try:
            with urlopen(request, timeout=150) as response:
                return json.load(response)
        except HTTPError as error:
            with error:
                payload = json.load(error)
            raise RuntimeError(f'HTTP {error.code}: {payload}') from error

    def models(self): return self.request('GET', '/v1/models')
    def describe(self, model): return self.request('GET', f'/v1/models/{model}/describe')
    def run(self, request): return self.request('POST', '/v1/runs', request)
    def status(self, run): return self.request('GET', f'/v1/runs/{run}')
    def cancel(self, run): return self.request('POST', f'/v1/runs/{run}/cancel', {})
    def results(self, run, **query): return self.request('GET', f'/v1/runs/{run}/results?'+urlencode(query, doseq=True))
    def summary(self, run, request): return self.request('POST', f'/v1/runs/{run}/summary', request)
    def compare(self, request): return self.request('POST', '/v1/compare', request)
    def explain(self, run, request): return self.request('POST', f'/v1/runs/{run}/explain', request)

    def wait(self, run, timeout=120):
        end = time.monotonic()+timeout
        while time.monotonic() < end:
            job = self.status(run)
            if job['status'] in ('completed', 'failed', 'cancelled', 'interrupted'):
                return job
            time.sleep(.1)
        raise TimeoutError('Run is still in progress')

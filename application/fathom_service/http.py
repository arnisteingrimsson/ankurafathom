"""Loopback-only HTTP/JSON and stage-event streaming adapter."""
import json
import math
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import time
from urllib.parse import parse_qs, urlsplit

from .contracts import APIError, canonical, integer, loads, number, require


class Server(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address, store, origins=()):
        require(address[0] in ('127.0.0.1', '::1', 'localhost'), 'This service supports loopback binding only')
        self.store, self.origins = store, set(origins)
        super().__init__(address, Handler)


class Handler(BaseHTTPRequestHandler):
    server_version = 'FathomLocal/1.0'

    def log_message(self, *_):
        pass

    def respond(self, status, value):
        data = canonical(value)
        self.send_response(status)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.cors()
        self.end_headers()
        self.wfile.write(data)

    def cors(self):
        origin = self.headers.get('Origin')
        if origin in self.server.origins:
            self.send_header('Access-Control-Allow-Origin', origin)
            self.send_header('Vary', 'Origin')
            self.send_header('Access-Control-Allow-Methods', 'GET, POST, OPTIONS')
            self.send_header('Access-Control-Allow-Headers', 'Content-Type')

    def guard(self):
        port = self.server.server_port
        require(self.headers.get('Host') in (f'localhost:{port}', f'127.0.0.1:{port}', f'[::1]:{port}'),
                'Host is not this loopback service', 'HOST_REJECTED', 403)
        origin = self.headers.get('Origin')
        same = {f'http://localhost:{port}', f'http://127.0.0.1:{port}'}
        require(origin is None or origin in self.server.origins or origin in same, 'Browser origin is not allowed', 'ORIGIN_REJECTED', 403)

    def body(self):
        require(self.headers.get_content_type() == 'application/json', 'Use application/json', 'CONTENT_TYPE', 415)
        length = self.headers.get('Content-Length', '')
        require(length.isdecimal() and 0 < int(length) <= 1024*1024, 'JSON body must be 1..1048576 bytes', 'BODY_SIZE', 413)
        return loads(self.rfile.read(int(length)))

    def dispatch(self):
        try:
            self.guard()
            if self.command == 'OPTIONS':
                self.respond(200, dict(methods=['GET', 'POST', 'OPTIONS']))
                return
            parsed = urlsplit(self.path)
            path = parsed.path.strip('/').split('/')
            query = parse_qs(parsed.query, keep_blank_values=True, strict_parsing=True)
            store = self.server.store
            if self.command == 'GET' and path == ['health']:
                self.respond(200, dict(status='ok', api_version='v1'))
            elif self.command == 'GET' and path == ['openapi.json']:
                from .openapi import document
                self.respond(200, document())
            elif self.command == 'GET' and path == ['v1', 'models']:
                self.respond(200, dict(models=store.list_models()))
            elif self.command == 'GET' and len(path) == 4 and path[:2] == ['v1', 'models'] and path[3] == 'describe':
                self.respond(200, store.model(path[2])['description'])
            elif self.command == 'POST' and path == ['v1', 'runs']:
                job = store.submit(self.body())
                self.respond(200 if job['cache_hit'] else 202, job)
            elif self.command == 'POST' and path == ['v1', 'compare']:
                self.respond(200, store.comparison(self.body()))
            elif len(path) >= 3 and path[:2] == ['v1', 'runs']:
                key = path[2]
                if len(path) == 3 and self.command == 'GET':
                    self.respond(200, store.get(key))
                elif len(path) == 4 and path[3] == 'cancel' and self.command == 'POST':
                    require(self.body() == {}, 'Cancel accepts an empty JSON object')
                    self.respond(200, store.cancel(key))
                elif len(path) == 4 and path[3] == 'results' and self.command == 'GET':
                    self.respond(200, query_results(store, key, query))
                elif len(path) == 4 and path[3] == 'summary' and self.command == 'POST':
                    self.respond(200, store.summary(key, self.body()))
                elif len(path) == 4 and path[3] == 'explain' and self.command == 'POST':
                    self.respond(200, store.explain(key, self.body()))
                elif len(path) == 4 and path[3] == 'events' and self.command == 'GET':
                    self.events(key)
                else:
                    raise APIError('NOT_FOUND', 'Unknown endpoint or method', 404)
            else:
                raise APIError('NOT_FOUND', 'Unknown endpoint or method', 404)
        except APIError as error:
            self.respond(error.status, dict(error=dict(code=error.code, message=str(error))))
        except (ValueError, TypeError, KeyError, OverflowError) as error:
            self.respond(422, dict(error=dict(code='INVALID_REQUEST', message=str(error))))
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception:
            self.respond(500, dict(error=dict(code='INTERNAL_ERROR', message='Service operation failed; inspect local service state')))

    def events(self, key):
        from .core import TERMINAL
        current = self.server.store.get(key)
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.send_header('Cache-Control', 'no-cache')
        self.send_header('Connection', 'close')
        self.cors()
        self.end_headers()
        previous = None
        while True:
            if current['revision'] != previous:
                self.wfile.write(b'event: status\nid: '+str(current['revision']).encode()+b'\ndata: '+canonical(current)+b'\n\n')
                previous = current['revision']
            else:
                self.wfile.write(b': heartbeat\n\n')
            self.wfile.flush()
            if current['status'] in TERMINAL:
                break
            time.sleep(.5)
            current = self.server.store.get(key)
        self.close_connection = True

    do_GET = dispatch
    do_POST = dispatch
    do_OPTIONS = dispatch


def query_results(store, key, query):
    allowed = {'metric', 'dimension', 'scenario', 'replication', 'from', 'to', 'max_points', 'offset', 'limit'}
    require(not query.keys()-allowed, 'Unknown query parameters')
    require(all(isinstance(v, list) and v and all(isinstance(x, str) for x in v) for v in query.values()), 'Invalid query')
    require(all(len(v) == 1 for k, v in query.items() if k not in ('metric', 'dimension')), 'Duplicate scalar query parameter')
    def int_value(name, default, lo, hi):
        raw = query.get(name, [str(default)])[0]
        require(raw.isdecimal(), name+' must be an integer')
        value = int(raw)
        require(integer(value, lo, hi), name+' outside range')
        return value
    scenario = int_value('scenario', 0, 0, 65535)
    replication = int_value('replication', 0, 0, 65535)
    limit, offset = int_value('limit', 1000, 1, 10000), int_value('offset', 0, 0, 10**9)
    max_points = int_value('max_points', 1000000000, 2, 1000000000)
    start, end = float(query.get('from', ['-inf'])[0]), float(query.get('to', ['inf'])[0])
    require(('from' not in query or number(start)) and ('to' not in query or number(end)) and start <= end, 'Invalid time filter')
    result = store.results(key)
    metrics = query.get('metric', [m for m, spec in result.metrics.items() if 'output' in spec])
    require(metrics and len(set(metrics)) == len(metrics), 'Metrics must be unique')
    dimensions = {}
    known_dimensions = {k for spec in result.metrics.values() for k in spec['dimensions']}
    for item in query.get('dimension', []):
        pair = item.split(':', 1)
        require(len(pair) == 2 and pair[0] in known_dimensions and pair[0] not in dimensions, 'Use unique known dimension:name filters')
        dimensions[pair[0]] = pair[1]
    rows, source_rows = [], 0
    for metric in metrics:
        require(metric in result.metrics, 'Unknown metric')
        spec = result.metrics[metric]
        if any(spec['dimensions'].get(k) != v for k, v in dimensions.items()):
            continue
        samples = [(t, value) for t, value in result.raw(metric, scenario, replication) if start <= t <= end]
        source_rows += len(samples)
        if len(samples) > max_points:
            indexes = [i*(len(samples)-1)//(max_points-1) for i in range(max_points)]
            samples = [samples[i] for i in indexes]
        rows += [dict(metric=metric, time=t, value=value, scenario=scenario, replication=replication, dimensions=spec['dimensions']) for t, value in samples]
    page = rows[offset:offset+limit]
    return dict(rows=page, total_rows=len(rows), source_rows=source_rows,
                next_offset=offset+len(page) if offset+len(page) < len(rows) else None,
                downsampling='endpoint_preserving_sample_selection' if source_rows > len(rows) else 'none',
                authoritative_for_aggregation=False, time_unit=result.description['time']['unit'],
                provenance=store.provenance(key))

"""AnkuraFathom operator control center; customer applications remain independent."""
import argparse
import mimetypes
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

from application.fathom_service.contracts import APIError, require
from application.fathom_service.http import Handler, Server
from .service import ControlCenter

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


class ConsoleHandler(Handler):
    def file(self, path, download=False):
        data = path.read_bytes()
        self.send_response(200)
        self.send_header('Content-Type', mimetypes.guess_type(path.name)[0] or 'application/octet-stream')
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.send_header('Content-Security-Policy', "default-src 'self'; style-src 'self'; script-src 'self'; connect-src 'self'; img-src 'self' data:; object-src 'none'; base-uri 'none'; frame-ancestors 'none'")
        if download:
            self.send_header('Content-Disposition', 'attachment; filename="fathom-run.json"')
        self.end_headers()
        self.wfile.write(data)

    def dispatch(self):
        try:
            self.guard()
            c = self.server.control_center
            parsed = urlsplit(self.path)
            parts = parsed.path.strip('/').split('/')
            if self.command == 'GET' and parts == ['api', 'projects']:
                return self.respond(200, c.list_projects())
            if parts[:2] == ['api', 'projects'] and len(parts) >= 3:
                key = parts[2]
                c.project(key)
                if len(parts) >= 4 and parts[3] == 'workspace':
                    if self.command == 'GET' and len(parts) == 4:
                        return self.respond(200, c.workspace.get(key))
                    if self.command == 'POST':
                        body = self.body()
                        if len(parts) == 5 and parts[4] in ('propose','accept','start'):
                            return self.respond(200, getattr(c.workspace, parts[4])(key, body))
                        if len(parts) == 7 and parts[4] == 'runs':
                            return self.respond(200, c.workspace.run_action(key, parts[5], parts[6], body))
                    raise APIError('NOT_FOUND', 'Unknown workspace endpoint', 404)
                if self.command == 'GET':
                    if len(parts) == 3:
                        return self.respond(200, c.detail(key))
                    if parts[3:] == ['runtime']:
                        return self.respond(200, c.runtime(key))
                    if len(parts) == 5 and parts[3] == 'source':
                        return self.respond(200, c.source(key, parts[4]))
                    if parts[3:] == ['runs']:
                        query = parse_qs(parsed.query)
                        require(not query.keys() - {'offset'}, 'Unknown query parameters')
                        values = query.get('offset', ['0'])
                        require(len(values) == 1 and values[0].isdecimal() and int(values[0]) <= 1000000, 'Invalid offset')
                        return self.respond(200, c.runs(key, int(values[0])))
                    if len(parts) == 5 and parts[3] == 'runs':
                        return self.respond(200, c.run(key, parts[4]))
                    if len(parts) == 6 and parts[3] == 'runs' and parts[5] == 'record':
                        return self.file(c.record_path(key, parts[4]), download=True)
                    if len(parts) == 7 and parts[3] == 'runs' and parts[5] == 'frames':
                        require(parts[6].isdecimal(), 'Invalid frame index')
                        return self.respond(200, c.frame(key, parts[4], int(parts[6])))
                if self.command == 'POST' and len(parts) == 4:
                    body = self.body()
                    if parts[3] == 'notes':
                        return self.respond(200, c.notes(key, body))
                    if parts[3] in ('start', 'stop'):
                        require(body == {}, 'Empty object required')
                        return self.respond(200, getattr(c, parts[3])(key))
            assets = {'/': 'index.html', '/console.js': 'console.js', '/console.css': 'console.css', '/workspace.js':'workspace.js', '/workspace.css':'workspace.css'}
            if self.command == 'GET' and parsed.path in assets:
                return self.file(HERE / 'web' / assets[parsed.path])
            raise APIError('NOT_FOUND', 'Not found', 404)
        except (BrokenPipeError, ConnectionResetError):
            pass
        except APIError as error:
            self.respond(error.status, dict(error=dict(code=error.code, message=str(error))))
        except (OSError, ValueError, KeyError, TypeError) as error:
            self.respond(422, dict(error=dict(message=str(error))))

    do_GET = dispatch
    do_POST = dispatch


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=8086)
    parser.add_argument('--state', type=Path, default=ROOT / 'artifacts/control-center')
    args = parser.parse_args()
    center = ControlCenter(ROOT, args.state)
    server = Server(('127.0.0.1', args.port), None)
    server.RequestHandlerClass = ConsoleHandler
    server.control_center = center
    print(f'AnkuraFathom control center: http://127.0.0.1:{server.server_port}', flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        center.close()
        server.server_close()


if __name__ == '__main__':
    main()

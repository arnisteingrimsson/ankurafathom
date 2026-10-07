import argparse
from .core import Store
from .http import Server


def main():
    parser = argparse.ArgumentParser(description='Local AnkuraFathom project API (engine execution in isolated processes)')
    parser.add_argument('--registry', required=True)
    parser.add_argument('--state', required=True)
    parser.add_argument('--engine', required=True)
    parser.add_argument('--port', type=int, default=8765)
    parser.add_argument('--workers', type=int, default=2)
    parser.add_argument('--allow-origin', action='append', default=[])
    args = parser.parse_args()
    store = Store(args.registry, args.state, args.engine, args.workers)
    server = Server(('127.0.0.1', args.port), store, args.allow_origin)
    print(f'Fathom API: http://127.0.0.1:{server.server_port} (OpenAPI: /openapi.json)', flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        store.close()


if __name__ == '__main__':
    main()

"""FBZZのローカルCommand Busへ、レビュー可能なJSONファイルの要求を順に渡す。"""

import ctypes
import json
import sys
import time
import uuid
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PIPE = r'\\.\pipe\FBZZEditorCommandBus'
KERNEL = ctypes.WinDLL('kernel32', use_last_error=True)
KERNEL.WaitNamedPipeW.argtypes = (ctypes.c_wchar_p, ctypes.c_uint32)
KERNEL.WaitNamedPipeW.restype = ctypes.c_int


def main(request_file, response_file):
    request_path, response_path = Path(request_file).resolve(), Path(response_file).resolve()
    assert request_path.is_relative_to(ROOT) and response_path.is_relative_to(ROOT)
    requests = json.loads(request_path.read_text(encoding='utf-8'))
    results = []
    for request in requests:
        assert request['kind'] in ('query', 'command')
        assert isinstance(request['payload'], dict) and isinstance(request['payload']['t'], str)
        envelope = {'protocol': 'fbzz.editor.v1', 'id': str(uuid.uuid4()), 'source': 'mcp', **request}
        envelope.setdefault('dryRun', request['kind'] == 'query')
        deadline = time.monotonic() + 10.0
        while not KERNEL.WaitNamedPipeW(PIPE, 1000):
            error = ctypes.get_last_error()
            if error not in (2, 121, 231) or time.monotonic() >= deadline:
                raise ctypes.WinError(error)
            time.sleep(0.1)
        with open(PIPE, 'r+b', buffering=0) as stream:
            stream.write((json.dumps(envelope, ensure_ascii=False) + '\n').encode('utf-8'))
            response = bytearray()
            while not response.endswith(b'\n'):
                data = stream.read(1)
                if not data:
                    raise RuntimeError('Editor closed the connection without a complete response')
                response.extend(data)
        result = json.loads(response)
        results.append({'request': request, 'response': result})
        response_path.parent.mkdir(parents=True, exist_ok=True)
        response_path.write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps({'request': request['payload']['t'], 'ok': result.get('ok'),
                          'result': result.get('result') if request['payload']['t'] == 'editor.state' else None,
                          'error': result.get('error')}, ensure_ascii=False), flush=True)
        if not result.get('ok'):
            raise RuntimeError('Editor rejected request; see ' + str(response_path))


if __name__ == '__main__':
    main(*sys.argv[1:])

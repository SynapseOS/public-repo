#!/usr/bin/env python3
# test-javascript.py: локальная страница проверки JavaScript NetSurf
# Автор: Елчинян Арен Артурович, 2026
# Архитектура: host Python 3, ABI: HTTP и JavaScript ES5
# Подсистема: проверки графических пакетов
# Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
# Статус доступа: общедоступный
"""Сервер проверки, запускаемый на хосте отдельно от гостевого браузера

python3 pkgs/netsurf/test-javascript.py --out pkgs/.work/netsurf/new-js-run
В тестовой VM открыть http://10.0.2.2:8876/
/result с passed=31 подтверждает inline, внешний скрипт, DOM, JSON и таймер
/clicked с value=42 подтверждает обработчик нажатия кнопки
Проверить журнал: python3 pkgs/netsurf/test-javascript.py --check КАТАЛОГ_ПРОВЕРКИ
Отсутствие этих запросов не считается успехом
"""
import argparse
import html
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import time
import threading
import ssl
from urllib.parse import parse_qs, urlsplit

PAGE = '''<!doctype html><html><head><meta charset="utf-8"><title>SynapseOS JavaScript test</title>
<script src="/external.js"></script></head><body>
<h1>SynapseOS JavaScript test</h1><p id="state">PENDING</p>
<script>
(function () {
    var passed = 1, errors = [], features = {};
    function feature(name, read) {
        try { features[name] = read(); } catch (e) { features[name] = "error: " + String(e); }
    }
    try {
        if (window.externalAnswer === 42) { passed |= 2; }
        var node = document.createElement("p");
        node.setAttribute("id", "created");
        node.appendChild(document.createTextNode("DOM created"));
        document.body.appendChild(node);
        document.getElementById("state").textContent = "DOM changed";
        if (document.getElementById("created").textContent === "DOM created" &&
            document.getElementById("state").textContent === "DOM changed") { passed |= 4; }
        if (JSON.parse(JSON.stringify({value: 42})).value === 42) { passed |= 8; }
    } catch (e) { errors.push(String(e)); }
    feature("fetch", function () { return typeof window.fetch; });
    feature("Promise", function () { return typeof window.Promise; });
    feature("XMLHttpRequest", function () { return typeof window.XMLHttpRequest; });
    feature("querySelector", function () { return typeof document.querySelector; });
    feature("localStorage", function () { return typeof window.localStorage; });
    window.setTimeout(function () {
        passed |= 16;
        window.location.href = "/result?passed=" + passed + "&features=" +
            encodeURIComponent(JSON.stringify(features)) + "&errors=" + encodeURIComponent(errors.join(" | "));
    }, 150);
}());
</script></body></html>'''


class Handler(BaseHTTPRequestHandler):
    # Сохранить запрос до формирования ответа, результат проверяется по журналу
    def do_GET(self):
        url = urlsplit(self.path)
        values = parse_qs(url.query, keep_blank_values=True)
        record = {'time': time.time(), 'path': url.path, 'query': values}
        with self.server.result_file.open('a') as output:
            output.write(json.dumps(record) + '\n')
        status = 200
        kind = 'text/html; charset=utf-8'
        if url.path == '/web':
            body = '<!doctype html><meta charset="utf-8"><title>Web API test</title>'
            body += '<h1>Testing web APIs...</h1><script src="/web-test.js"></script>'
        elif url.path == '/web-test.js':
            kind = 'application/javascript'
            body = (Path(__file__).parent / 'test-web.js').read_text()
        elif url.path in ('/api', '/cors', '/slow'):
            if url.path == '/slow':
                time.sleep(0.4)
            kind = 'application/json'
            body = '{"value":42}'
        elif url.path in ('/web-result', '/persist-result', '/isolation-result'):
            body = '<!doctype html><meta charset="utf-8"><h1>Web API result</h1>'
            body += '<pre style="white-space:pre-wrap">' + html.escape(json.dumps(values, indent=2)) + '</pre>'
        elif url.path in ('/persist', '/isolation'):
            target = '/persist-result' if url.path == '/persist' else 'http://10.0.2.2:8876/isolation-result'
            body = '<!doctype html><script>location.href=' + json.dumps(target + '?value=')
            body += '+encodeURIComponent(String(localStorage.getItem("synapse-persist")));</script>'
        elif url.path == '/': 
            body = PAGE
        elif url.path == '/external.js':
            kind = 'application/javascript'
            body = 'window.externalAnswer = 6 * 7;'
        elif url.path == '/result':
            passed = values.get('passed', ['0'])[0]
            details = html.escape(json.dumps(values, ensure_ascii=False))
            body = '<!doctype html><meta charset="utf-8"><title>JavaScript result</title>'
            body += '<h1>JavaScript ' + ('PASS' if passed == '31' else 'FAIL') + '</h1>'
            body += '<p>Inline, external script, DOM, JSON, timer: mask ' + html.escape(passed) + '</p>'
            body += '<pre style="white-space:pre-wrap">' + details + '</pre>'
            body += '<button id="check" style="font-size:24px" '
            body += 'onclick="window.location.href=\'/clicked?value=\'+(40+2)">Check click handler</button>'
        elif url.path == '/clicked':
            passed = values.get('value') == ['42']
            body = '<!doctype html><title>Click result</title><h1>Click ' + ('PASS' if passed else 'FAIL') + '</h1>'
            body += '<p>Event handler executed JavaScript and navigated to this page</p>'
        elif url.path == '/favicon.ico':
            status = 404
            body = ''
        else:
            status = 404
            body = 'Not found'
        data = body.encode()
        self.send_response(status)
        self.send_header('Content-Type', kind)
        if url.path == '/cors':
            self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass

    # Принять тело POST для проверки настоящей передачи данных
    def do_POST(self):
        length = int(self.headers.get('Content-Length', '0'))
        if length < 0 or length > 2097152:
            self.send_error(413)
            return
        value = self.rfile.read(length).decode()
        data = json.dumps({'body': value}).encode()
        with self.server.result_file.open('a') as output:
            output.write(json.dumps({'path': self.path, 'method': 'POST', 'body': value}) + '\n')
        self.send_response(201)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    # Основной журнал содержит только структурированные результаты
    def log_message(self, format, *args):
        pass


def main():
    # Не заменять результаты предыдущей проверки
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--out', type=Path)
    mode.add_argument('--check', type=Path)
    mode.add_argument('--check-web', type=Path)
    parser.add_argument('--port', default=8876, type=int)
    parser.add_argument('--tls-cert', type=Path)
    parser.add_argument('--tls-key', type=Path)
    args = parser.parse_args()
    if args.check_web is not None:
        records = [json.loads(line) for line in (args.check_web / 'requests.jsonl').read_text().splitlines()]
        results = [record['query'] for record in records if record['path'] == '/web-result']
        assert results, 'No guest result'
        passed = json.loads(results[-1]['passed'][0])
        failed = json.loads(results[-1]['failed'][0])
        assert not failed, failed
        required = {'promise-value', 'thenable-once', 'promise-reject', 'promise-all', 'storage-get',
                    'storage-property', 'storage-remove', 'fetch-json', 'fetch-post', 'xhr-get', 'xhr-states',
                    'xhr-post', 'xhr-timeout', 'xhr-abort', 'fetch-abort', 'cors-allowed', 'cors-blocked',
                    'forbidden-header', 'tls-untrusted', 'fetch-same-origin-mode', 'native-cors-no-leak'}
        assert required.issubset(passed), required.difference(passed)
        assert 'microtask-order=sync,microtask,timer' in passed, 'Promise job order failed'
        assert any(record['path'] == '/persist-result' and record['query'].get('value') == ['42']
                   for record in records), 'Storage did not persist in another browser process'
        assert any(record['path'] == '/isolation-result' and record['query'].get('value') == ['null']
                   for record in records), 'Storage leaked across origins'
        print('NETSURF_WEB_APIS_PASS', len(passed))
        print('NETSURF_STORAGE_PERSISTENCE_AND_ORIGIN_ISOLATION_PASS')
        return
    if args.check is not None:
        records = [json.loads(line) for line in (args.check / 'requests.jsonl').read_text().splitlines()]
        results = [record['query'] for record in records if record['path'] == '/result']
        assert results and results[-1].get('passed') == ['31'], 'JavaScript checks did not pass'
        assert results[-1].get('errors') == [''], 'JavaScript reported an error'
        assert any(record['path'] == '/clicked' and record['query'].get('value') == ['42']
                   for record in records), 'Click handler did not pass'
        print('NETSURF_JAVASCRIPT_INLINE_EXTERNAL_DOM_JSON_TIMER_PASS')
        print('NETSURF_JAVASCRIPT_CLICK_PASS')
        print(results[-1].get('features', ['{}'])[0])
        return
    args.out.mkdir(parents=True, exist_ok=False)
    server = ThreadingHTTPServer(('127.0.0.1', args.port), Handler)
    server.result_file = args.out / 'requests.jsonl'
    print(f'Open http://10.0.2.2:{args.port}/ in the test guest', flush=True)
    secondary = ThreadingHTTPServer(('127.0.0.1', args.port + 1), Handler)
    secondary.result_file = server.result_file
    threading.Thread(target=secondary.serve_forever, daemon=True).start()
    if args.tls_cert and args.tls_key:
        secure = ThreadingHTTPServer(('127.0.0.1', args.port + 2), Handler)
        secure.result_file = server.result_file
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(args.tls_cert, args.tls_key)
        secure.socket = context.wrap_socket(secure.socket, server_side=True)
        threading.Thread(target=secure.serve_forever, daemon=True).start()
    server.serve_forever()


if __name__ == '__main__':
    main()

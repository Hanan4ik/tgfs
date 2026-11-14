"""Exercise the real curl transport against a loopback Bot API double."""
import email.parser
import email.policy
import http.server
import json
import os
from pathlib import Path
import socket
import socketserver
import select
import subprocess
import sys
import tempfile
import threading
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())
TOKEN = '123456:TEST_ONLY_TOKEN'
STORAGE, SERVICE = -1003916921678, -1004400939172

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, body, status=200):
        if not isinstance(body, bytes):
            body = json.dumps({'ok': True, 'result': body}).encode()
        self.send_response(status)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_POST(self):
        s = self.server
        method = self.path.rsplit('/', 1)[1]
        s.calls.append(method)
        raw = self.rfile.read(int(self.headers['Content-Length']))
        if method == 'sendDocument':
            msg = email.parser.BytesParser(policy=email.policy.default).parsebytes(
                ('Content-Type: ' + self.headers['Content-Type'] + '\r\n\r\n').encode() + raw)
            fields = {p.get_param('name', header='content-disposition'): p for p in msg.iter_parts()}
            s.payload = fields['document'].get_payload(decode=True)
            s.filename = fields['document'].get_filename()
            s.caption = fields['caption'].get_payload(decode=True).decode()
            assert fields['chat_id'].get_payload(decode=True).decode() == str(STORAGE)
            assert s.filename == 'tgfs-probe.bin.0'
            assert len(s.payload) == 4096
            assert 'parse_mode' not in fields
        else:
            p = json.loads(raw)
        if method == 'sendDocument' and s.mode == 'lost':
            self.connection.shutdown(socket.SHUT_RDWR)
            self.connection.close()
            return
        if method == 'getMe':
            if s.mode == 'rate':
                return self.reply(json.dumps({'ok': False, 'error_code': 429,
                    'description': TOKEN, 'parameters': {'retry_after': 17}}).encode(), 429)
            if s.mode == 'invalid':
                return self.reply(b'{"ok":true,"ok":false}')
            if s.mode == 'huge':
                return self.reply(b' ' * (1048576 + 1))
            if s.mode == 'redirect':
                self.send_response(302)
                self.send_header('Location', '/unexpected')
                self.send_header('Content-Length', '0')
                self.end_headers()
                return
            return self.reply({'id': 123456, 'is_bot': True})
        if method == 'getChat':
            return self.reply({'id': int(p['chat_id']), 'type': 'channel'})
        if method == 'editMessageCaption':
            assert p['message_id'] == 101
            assert p['chat_id'] == str(STORAGE)
            s.caption = p['caption']
        if method in ('sendDocument', 'editMessageCaption', 'forwardMessage'):
            forwarded = method == 'forwardMessage'
            result = {'message_id': 9001 if forwarded else 101,
                      'chat': {'id': SERVICE if forwarded else STORAGE, 'type': 'channel'},
                      'caption': s.caption,
                      'document': {'file_id': 'testfile', 'file_name': s.filename,
                                   'file_size': len(s.payload)}}
            if forwarded:
                assert p['from_chat_id'] == str(STORAGE) and p['chat_id'] == str(SERVICE)
                assert p['message_id'] == 101
                result['forward_origin'] = {'type': 'channel', 'date': 1,
                    'chat': {'id': STORAGE, 'type': 'channel'},
                    'message_id': 102 if s.mode == 'origin' else 101}
            return self.reply(result)
        if method == 'getFile':
            assert p['file_id'] == 'testfile'
            return self.reply({'file_id': 'testfile', 'file_size': len(s.payload),
                'file_path': '../secret' if s.mode == 'path' else 'documents/file.bin'})
        raise AssertionError(method)

    def do_GET(self):
        self.server.calls.append('download')
        assert self.path == '/file/bot' + TOKEN + '/documents/file.bin'
        payload = self.server.payload
        if self.server.mode == 'corrupt':
            payload = bytes([payload[0] ^ 1]) + payload[1:]
        self.reply(payload)

class Socks(socketserver.BaseRequestHandler):
    def exact(self, n):
        data = b''
        while len(data) < n:
            chunk = self.request.recv(n - len(data))
            if not chunk:
                raise ConnectionError('short SOCKS request')
            data += chunk
        return data

    def handle(self):
        version, count = self.exact(2)
        assert version == 5 and 0 in self.exact(count)
        self.request.sendall(b'\x05\x00')
        version, command, _, kind = self.exact(4)
        assert version == 5 and command == 1
        if kind == 1:
            host = socket.inet_ntoa(self.exact(4))
        elif kind == 3:
            host = self.exact(self.exact(1)[0]).decode()
        else:
            raise AssertionError(kind)
        port = int.from_bytes(self.exact(2), 'big')
        self.server.targets.append((host, port))
        with socket.create_connection((host, port), timeout=3) as upstream:
            self.request.sendall(b'\x05\x00\x00\x01\x7f\x00\x00\x01\x00\x00')
            while True:
                ready, _, _ = select.select([self.request, upstream], [], [], 3)
                if not ready:
                    return
                for source in ready:
                    data = source.recv(65536)
                    if not data:
                        return
                    (upstream if source is self.request else self.request).sendall(data)

class Transport(unittest.TestCase):
    def run_probe(self, mode='ok', permissions=0o600, extra=()):
        with tempfile.TemporaryDirectory() as temp:
            token = Path(temp) / 'token'
            token.write_text(TOKEN + '\n')
            token.chmod(permissions)
            server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
            server.mode, server.calls = mode, []
            thread = threading.Thread(target=server.serve_forever, kwargs={'poll_interval': .01}, daemon=True)
            thread.start()
            try:
                env = dict(os.environ, TGFS_TEST_API_BASE=f'http://127.0.0.1:{server.server_port}')
                result = subprocess.run([BINARY, 'probe', '--token-file', str(token),
                    '--storage-chat-id', str(STORAGE), '--service-chat-id', str(SERVICE),
                    '--timeout-seconds', '2', *extra], env=env, capture_output=True, text=True, timeout=10)
                output = result.stdout + result.stderr
                self.assertNotIn(TOKEN, output)
                self.assertNotIn('AddressSanitizer', output)
                self.assertNotIn('runtime error:', output)
                return result.returncode, output, server.calls
            finally:
                server.shutdown()
                server.server_close()
                thread.join()

    def test_socks_proxy_all_requests(self):
        proxy = socketserver.ThreadingTCPServer(('127.0.0.1', 0), Socks)
        proxy.daemon_threads = True
        proxy.targets = []
        worker = threading.Thread(target=proxy.serve_forever, kwargs={'poll_interval': .01}, daemon=True)
        worker.start()
        try:
            rc, out, calls = self.run_probe(extra=('--proxy', f'socks5h://127.0.0.1:{proxy.server_address[1]}'))
            self.assertEqual(rc, 0, out)
            self.assertEqual(len(proxy.targets), len(calls))
            self.assertEqual(len(calls), 8)
        finally:
            proxy.shutdown()
            proxy.server_close()
            worker.join()

    def test_proxy_failure_and_validation(self):
        with socket.socket() as closed:
            closed.bind(('127.0.0.1', 0))
            port = closed.getsockname()[1]
        rc, out, calls = self.run_probe(extra=('--proxy', f'socks5h://127.0.0.1:{port}'))
        self.assertEqual(rc, 1)
        self.assertEqual(calls, [])  # Never silently fall back to direct.
        rc, out, calls = self.run_probe(extra=('--proxy', 'http://user:SECRET@localhost:80'))
        self.assertEqual(rc, 1)
        self.assertNotIn('SECRET', out)
        self.assertEqual(calls, [])
        rc, out, calls = self.run_probe(extra=('--direct',))
        self.assertEqual(rc, 0, out)

    def test_roundtrip(self):
        rc, out, calls = self.run_probe()
        self.assertEqual(rc, 0, out)
        self.assertIn('PASS transport probe', out)
        self.assertIn('copy_id=9001 source_id=101', out)
        self.assertEqual(calls, ['getMe', 'getChat', 'getChat', 'sendDocument',
            'editMessageCaption', 'forwardMessage', 'getFile', 'download'])

    def test_rate_limit_no_retry(self):
        rc, out, calls = self.run_probe('rate')
        self.assertEqual(rc, 1)
        self.assertIn('retry_after=17', out)
        self.assertEqual(calls, ['getMe'])

    def test_lost_response_no_duplicate(self):
        rc, out, calls = self.run_probe('lost')
        self.assertEqual(rc, 1)
        self.assertIn('outcome_unknown=1', out)
        self.assertEqual(calls.count('sendDocument'), 1)

    def test_reject_invalid_responses(self):
        for mode in ('invalid', 'huge', 'redirect', 'origin', 'path', 'corrupt'):
            with self.subTest(mode=mode):
                rc, out, calls = self.run_probe(mode)
                self.assertEqual(rc, 1, out)
                if mode in ('path', 'origin', 'redirect'):
                    self.assertNotIn('download', calls)

    def test_token_permissions(self):
        rc, out, calls = self.run_probe(permissions=0o644)
        self.assertEqual(rc, 1)
        self.assertEqual(calls, [])

unittest.main()

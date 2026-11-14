"""Finite journal scan, source identity, failure publication and cleanup."""
import http.server
import json
import os
from pathlib import Path
import subprocess
import socket
import sys
import tempfile
import threading
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())
TOKEN = '123456:SCAN_TEST_TOKEN'
STORAGE, SERVICE = -1003916921678, -1004400939172
ROOT = 'a30d487f-167a-4eb3-8b06-b0676a039470'
SUPER = dict(magic='TGFS', format_version=1, type='SUPERBLOCK', start_id='1',
             root_object_id=ROOT, next_message='4')

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, result=None, error=None, status=200):
        raw = json.dumps(error if error else dict(ok=True, result=result)).encode()
        self.send_response(status)
        self.send_header('Content-Length', str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_POST(self):
        method = self.path.rsplit('/', 1)[-1]
        p = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        s = self.server
        s.calls.append((method, p))
        if method == 'getChat':
            chat = dict(id=int(p['chat_id']), type='channel')
            if int(p['chat_id']) == STORAGE and s.mode != 'noheader':
                chat['pinned_message'] = dict(message_id=1, chat=dict(id=STORAGE, type='channel'), text=json.dumps(SUPER))
            return self.reply(chat)
        if method == 'sendMessage':
            marker = dict(message_id=5, chat=dict(id=STORAGE, type='channel'), text=p['text'])
            if s.mode == 'marker_identity':
                marker['chat']['id'] = SERVICE
            return self.reply(marker)
        if method == 'deleteMessage':
            if s.mode == 'cleanup':
                return self.reply(error=dict(ok=False, error_code=403, description=TOKEN), status=403)
            return self.reply(True)
        assert method == 'forwardMessage'
        mid = p['message_id']
        if s.mode == 'lost' and mid == 3:
            self.connection.shutdown(socket.SHUT_RDWR)
            self.connection.close()
            return
        if s.mode in ('missing', 'unforwardable', 'unforwardable_upper', 'unforwardable_the', 'invalid_id', 'other400', 'rate', 'forbidden') and mid == 3:
            if s.mode in ('invalid_id','other400','unforwardable_upper','unforwardable_the'):
                description={'invalid_id':'Bad Request: MESSAGE_ID_INVALID',
                    'other400':'Bad Request: chat not found',
                    'unforwardable_upper':"Bad Request: Message can't be forwarded",
                    'unforwardable_the':"Bad Request: the message can't be forwarded"}[s.mode]
                return self.reply(error=dict(ok=False,error_code=400,description=description),status=400)
            if s.mode == 'unforwardable':
                return self.reply(error=dict(ok=False, error_code=400,
                    description="Bad Request: message can't be forwarded"), status=400)
            if s.mode == 'missing':
                return self.reply(error=dict(ok=False, error_code=400,
                    description='Bad Request: message to forward not found'), status=400)
            code = 429 if s.mode == 'rate' else 403
            return self.reply(error=dict(ok=False, error_code=code, description=TOKEN,
                parameters=dict(retry_after=10)), status=code)
        if mid == 1:
            raw = json.dumps(SUPER)
        elif mid == 2:
            raw = 'unrelated message'
        else:
            r = dict(magic='TGFS', format_version=1, type='DIRECTORY',
                object_id='c5ccf4d2-79ba-4ab7-a4d0-d9b84b40a8cd', parent_id=ROOT, next_message=None)
            if s.mode == 'bad_envelope':
                del r['next_message']
            raw = '{"magic":"TGFS"' if s.mode == 'malformed' else json.dumps(r)
        origin = dict(type='channel', chat=dict(id=STORAGE, type='channel'), message_id=mid)
        if s.mode == 'origin' and mid != 1:
            origin['message_id'] = 999
        msg = dict(message_id=1000+mid, chat=dict(id=SERVICE, type='channel'), forward_origin=origin)
        if mid in (1, 2):
            msg['text'] = raw
        else:
            msg['caption'] = raw
            msg['document'] = dict(file_id='DOC_'+str(mid), file_name='directory.json', file_size=14)
        return self.reply(msg)

class Scan(unittest.TestCase):
    def scan(self, mode='ok', extra=()):
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        server.mode, server.calls = mode, []
        thread = threading.Thread(target=server.serve_forever, kwargs={'poll_interval': .01}, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as temp:
                token = Path(temp)/'token'; token.write_text(TOKEN); token.chmod(0o600)
                out = Path(temp)/'capture'
                p = subprocess.run([BINARY, 'scan', '--token-file', str(token),
                    '--storage-chat-id', str(STORAGE), '--service-chat-id', str(SERVICE),
                    '--output', str(out), *extra], capture_output=True, text=True, timeout=10,
                    env=dict(os.environ, TGFS_TEST_API_BASE=f'http://127.0.0.1:{server.server_port}'))
                output = p.stdout+p.stderr
                self.assertNotIn(TOKEN, output)
                self.assertNotIn('AddressSanitizer', output)
                self.assertNotIn('runtime error:', output)
                journal = json.loads((out/'journal.json').read_text()) if (out/'journal.json').exists() else None
                queue = json.loads((out/'cleanup_pending.json').read_text()) if (out/'cleanup_pending.json').exists() else None
                log = out/'service-copies.log'
                logged = set(log.read_text().splitlines()) if log.exists() else set()
                deleted = {str(p['message_id']) for m,p in server.calls if m == 'deleteMessage'}
                self.assertTrue(deleted <= logged)
                return p.returncode, output, journal, server.calls, queue
        finally:
            server.shutdown(); server.server_close(); thread.join()

    def test_finite_capture(self):
        rc, out, j, calls, _ = self.scan()
        self.assertEqual(rc, 0, out)
        self.assertTrue(j['coverage_verified'])
        self.assertEqual(j['boundary_id'], '5')
        self.assertEqual([r['message_id'] for r in j['records']], ['3', '4'])
        self.assertEqual(j['records'][0]['document']['file_id'], 'DOC_3')
        self.assertEqual(j['object_messages']['c5ccf4d2-79ba-4ab7-a4d0-d9b84b40a8cd'], ['3','4'])
        self.assertEqual([p['message_id'] for m,p in calls if m=='forwardMessage'], [2,3,4])
        self.assertEqual([p['message_id'] for m,p in calls if m=='deleteMessage'], [1002,1003,1004])
        self.assertFalse(j['cleanup_pending'])

    def test_locator_without_pin(self):
        rc, out, j, calls, _ = self.scan('noheader', ('--start-id','1'))
        self.assertEqual(rc, 0, out)
        self.assertEqual(j['superblock'], SUPER)
        self.assertIn(1001, [p['message_id'] for m,p in calls if m=='deleteMessage'])

    def test_missing_is_not_verified(self):
        rc, out, j, _, _ = self.scan('missing')
        self.assertEqual(rc, 3, out)
        self.assertEqual(j['unavailable_ids'], ['3'])
        self.assertFalse(j['coverage_verified'])

    def test_unforwardable_is_not_verified(self):
        rc, out, j, _, _ = self.scan('unforwardable')
        self.assertEqual(rc, 3, out)
        self.assertEqual(j['unavailable_ids'], ['3'])
        self.assertFalse(j['coverage_verified'])

    def test_failure_no_published_journal(self):
        for mode in ('rate','forbidden','origin','bad_envelope','marker_identity','noheader','lost','malformed'):
            with self.subTest(mode=mode):
                rc, out, j, calls, _ = self.scan(mode)
                self.assertEqual(rc, 1, out)
                self.assertIsNone(j)
                if mode=='noheader':
                    self.assertNotIn('sendMessage', [m for m,p in calls])
                if mode=='lost':
                    self.assertIn('outcome_unknown=1',out)
                    self.assertEqual(sum(m=='forwardMessage' and p['message_id']==3 for m,p in calls),1)
                if mode=='rate':
                    self.assertIn('retry_after=10',out)
                    self.assertEqual(sum(m=='forwardMessage' and p['message_id']==3 for m,p in calls),1)

    def test_error_variants_and_diagnostics(self):
        for mode in ('invalid_id','unforwardable_upper','unforwardable_the'):
            rc,out,j,_,_=self.scan(mode)
            self.assertEqual(rc,3,out)
            self.assertFalse(j['coverage_verified'])
            self.assertEqual(j['unavailable_ids'],['3'])
        rc,out,j,_,_=self.scan('other400')
        self.assertEqual(rc,1,out)
        self.assertIsNone(j)
        self.assertIn('requested_id=3',out)
        self.assertIn('Bad Request: chat not found',out)

    def test_cleanup_queue(self):
        rc,out,j,_,_ = self.scan('cleanup')
        self.assertEqual(rc,0,out)
        self.assertEqual(j['cleanup_pending'],['1002','1003','1004'])

    def test_scan_budget(self):
        rc,out,j,calls,_ = self.scan(extra=('--max-messages','2'))
        self.assertEqual(rc,1,out)
        self.assertIsNone(j)
        self.assertNotIn('forwardMessage',[m for m,p in calls])

unittest.main()

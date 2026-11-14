import http.server
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import unittest
import uuid

BINARY=str(Path(sys.argv.pop(1)).resolve())
CHAT=-1003916921678
TOKEN='123456:INIT_TEST_TOKEN'
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self,*args): pass
    def reply(self,value):
        raw=json.dumps(dict(ok=True,result=value)).encode()
        self.send_response(200);self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw)
    def do_POST(self):
        s=self.server
        method=self.path.rsplit('/',1)[-1]
        p=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        s.calls.append(method)
        if method=='getMe': return self.reply(dict(id=123456,is_bot=True))
        if method=='getChat':
            c=dict(id=CHAT,type='channel')
            if s.mode=='existing': c['pinned_message']=dict(message_id=99,text='other pin')
            elif s.pinned: c['pinned_message']=dict(message_id=7,chat=dict(id=CHAT,type='channel'),text=s.text)
            return self.reply(c)
        if method=='getChatMember':
            return self.reply(dict(user=dict(id=123456),status='administrator',can_post_messages=True,can_edit_messages=s.mode!='rights'))
        if method in ('sendMessage','editMessageText'):
            intent=json.loads(s.intent.read_text())
            assert intent['phase']==('send_pending' if method=='sendMessage' else 'edit_pending')
            s.text=p['text']
            if s.mode=='lost' and method=='sendMessage':
                self.connection.shutdown(socket.SHUT_RDWR);self.connection.close();return
            return self.reply(dict(message_id=7,chat=dict(id=CHAT,type='channel'),text=s.text))
        assert method=='pinChatMessage'
        assert p['message_id']==7
        assert json.loads(s.intent.read_text())['phase']=='pin_pending'
        s.pinned=True
        return self.reply(True)
class Init(unittest.TestCase):
    def run_init(self,mode='ok'):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);token=root/'token';token.write_text(TOKEN);token.chmod(0o600)
            server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
            server.calls=[];server.mode=mode;server.text='';server.pinned=False;server.intent=root/'state'/'intent.json'
            worker=threading.Thread(target=server.serve_forever,kwargs=dict(poll_interval=.01),daemon=True);worker.start()
            args=[BINARY,'init','--new-volume','--token-file',str(token),'--storage-chat-id',str(CHAT),'--state-dir',str(root/'state')]
            env=dict(os.environ,TGFS_TEST_API_BASE=f'http://127.0.0.1:{server.server_port}')
            try:
                p=subprocess.run(args,env=env,capture_output=True,text=True,timeout=10)
                out=p.stdout+p.stderr
                self.assertNotIn(TOKEN,out);self.assertNotIn('AddressSanitizer',out);self.assertNotIn('runtime error:',out)
                intent=json.loads(server.intent.read_text()) if server.intent.exists() else None
                if intent:
                    count=server.calls.count('sendMessage')
                    retry=subprocess.run(args,env=env,capture_output=True,text=True,timeout=10)
                    self.assertEqual(retry.returncode,1)
                    self.assertEqual(server.calls.count('sendMessage'),count)
                return p.returncode,out,intent,server.text,server.calls
            finally:
                server.shutdown();server.server_close();worker.join()
    def test_success(self):
        rc,out,intent,text,calls=self.run_init()
        self.assertEqual(rc,0,out)
        self.assertEqual(intent['phase'],'complete')
        self.assertEqual(intent['superblock_message_id'],'7')
        header=json.loads(text)
        self.assertEqual(header['start_id'],'7')
        self.assertEqual(header['root_object_id'],intent['root_object_id'])
        self.assertEqual(uuid.UUID(header['root_object_id']).version,4)
        self.assertIsNone(header['next_message'])
        self.assertEqual(calls.count('pinChatMessage'),1)
    def test_refuse_existing_pin_or_missing_rights(self):
        for mode in ('existing','rights'):
            rc,out,intent,_,calls=self.run_init(mode)
            self.assertEqual(rc,1,out);self.assertIsNone(intent);self.assertNotIn('sendMessage',calls)
    def test_lost_send_preserves_intent(self):
        rc,out,intent,_,calls=self.run_init('lost')
        self.assertEqual(rc,1,out);self.assertIn('outcome_unknown=1',out)
        self.assertEqual(intent['phase'],'send_pending');self.assertEqual(calls.count('sendMessage'),1)
unittest.main()

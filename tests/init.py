import http.server
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import uuid

BINARY=str(Path(sys.argv.pop(1)).resolve())
CHAT=-1003916921678
TOKEN='123456:INIT_TEST_TOKEN'
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self,*args): pass
    def reply(self,value=None,*,error=None,status=200):
        raw=json.dumps(error if error else dict(ok=True,result=value)).encode()
        self.send_response(status);self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw)
    def do_POST(self):
        s=self.server
        method=self.path.rsplit('/',1)[-1]
        p=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        s.calls.append(method)
        if method=='getMe': return self.reply(dict(id=s.bot_id,is_bot=True))
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
            if method=='sendMessage':
                assert 'send_retry_not_before' not in intent
                s.placeholders.append(p['text'])
                if s.mode.startswith('reject-'):
                    status=int(s.mode.split('-')[1])
                    error=dict(ok=False,error_code=status,description=TOKEN)
                    if status==429 and s.mode!='reject-429-no-retry': error['parameters']=dict(retry_after=1)
                    return self.reply(error=error,status=status)
            s.text=p['text']
            if s.mode=='lost' and method=='sendMessage':
                self.connection.shutdown(socket.SHUT_RDWR);self.connection.close();return
            return self.reply(dict(message_id=7,chat=dict(id=CHAT,type='channel'),text=s.text))
        assert method=='pinChatMessage'
        assert p['message_id']==7
        assert json.loads(s.intent.read_text())['phase']=='pin_pending'
        s.pinned=True
        if s.mode=='lost-pin':
            self.connection.shutdown(socket.SHUT_RDWR);self.connection.close();return
        return self.reply(True)
class Init(unittest.TestCase):
    def run_init(self,mode='ok',finish=None,resume=None):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);token=root/'token';token.write_text(TOKEN);token.chmod(0o600)
            server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
            server.calls=[];server.placeholders=[];server.bot_id=123456;server.mode=mode;server.text='';server.pinned=False;server.intent=root/'state'/'intent.json'
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
                if finish:
                    finalize=args.copy();finalize[finalize.index('--new-volume')]='--finalize-existing'
                    if finish=='foreign': server.text=server.text.replace(intent['root_object_id'],str(uuid.uuid4()))
                    if finish=='absent': server.pinned=False
                    original=server.intent.read_bytes()
                    temp=server.intent.with_name('intent.tmp')
                    if finish=='crash-file':
                        complete=dict(json.loads(original),phase='complete')
                        temp.write_text(json.dumps(complete,separators=(',',':')));temp.chmod(0o600)
                    if finish=='partial-file':
                        temp.write_text('{"storage_chat_id":');temp.chmod(0o600)
                    for _ in range(2):
                        start=len(server.calls)
                        check=subprocess.run(finalize,env=env,capture_output=True,text=True,timeout=10)
                        self.assertEqual(check.returncode,0 if finish in ('ok','crash-file') else 1,check.stderr)
                        self.assertEqual(server.calls[start:],[] if mode=='lost' else ['getChat'])
                        if finish in ('ok','crash-file'): self.assertEqual(json.loads(server.intent.read_text())['phase'],'complete')
                        else: self.assertEqual(server.intent.read_bytes(),original)
                    if finish=='crash-file': self.assertFalse(temp.exists())
                    if finish=='partial-file': self.assertEqual(temp.read_text(),'{"storage_chat_id":')
                if resume:
                    self.assertEqual(p.returncode,1,out)
                    resume_args=[*args,'--resume-send']
                    def retry_send(expected,diagnostic=None):
                        result=subprocess.run(resume_args,env=env,capture_output=True,text=True,timeout=10)
                        self.assertEqual(result.returncode,expected,result.stdout+result.stderr)
                        self.assertNotIn(TOKEN,result.stderr)
                        if diagnostic: self.assertIn(diagnostic,result.stderr)
                    rejected=mode in ('reject-400','reject-403','reject-429')
                    self.assertEqual(intent['phase'],'send_rejected' if rejected else 'send_pending')
                    self.assertEqual(intent['initializer_bot_id'],'123456')
                    self.assertEqual(intent['superblock_message_id'],'0')
                    self.assertEqual(server.placeholders,['TGFS_INIT_PENDING '+intent['root_object_id']])
                    server.mode='ok'
                    if resume=='blocked':
                        retry_send(1,'requires a confirmed sendMessage rejection')
                        self.assertEqual(server.calls.count('sendMessage'),1)
                        return
                    if resume=='wrong-bot':
                        server.bot_id+=1
                        retry_send(1,'requires a confirmed sendMessage rejection')
                        self.assertEqual(server.calls.count('sendMessage'),1)
                        return
                    if resume=='pin':
                        server.mode='existing'
                        retry_send(1,'already has a pin')
                        self.assertEqual(server.calls.count('sendMessage'),1)
                        return
                    if resume=='cooldown':
                        retry_send(1,'cooldown')
                        self.assertEqual(server.calls.count('sendMessage'),1)
                        time.sleep(max(0,int(intent['send_retry_not_before'])-time.time()+1.05))
                    if resume=='lost':
                        server.mode='lost'
                        retry_send(1,'outcome_unknown=1')
                        server.mode='ok'
                        retry_send(1,'requires a confirmed sendMessage rejection')
                        self.assertEqual(server.calls.count('sendMessage'),2)
                        self.assertEqual(json.loads(server.intent.read_text())['phase'],'send_pending')
                        return
                    if resume=='repeat':
                        server.mode='reject-403'
                        retry_send(1)
                        self.assertEqual(json.loads(server.intent.read_text())['phase'],'send_rejected')
                        server.mode='ok'
                    retry_send(0)
                    final=json.loads(server.intent.read_text())
                    self.assertEqual(final['phase'],'complete')
                    self.assertEqual(final['root_object_id'],intent['root_object_id'])
                    self.assertEqual(final['superblock_message_id'],'7')
                    self.assertEqual(len(set(server.placeholders)),1)
                    self.assertEqual(server.calls.count('pinChatMessage'),1)
                    return
                return p.returncode,out,intent,server.text,server.calls
            finally:
                server.shutdown();server.server_close();worker.join()
    def test_resume_rejected_send(self):
        for mode,resume in (('reject-400','repeat'),('reject-429','cooldown'),('reject-400','lost'),
                            ('reject-403','wrong-bot'),('reject-403','pin'),('reject-500','blocked'),
                            ('reject-429-no-retry','blocked'),('lost','blocked')):
            with self.subTest(mode=mode,resume=resume): self.run_init(mode,resume=resume)

    def test_finalize_existing(self):
        for mode,finish in (('ok','ok'),('lost-pin','ok'),('lost-pin','foreign'),('lost-pin','absent'),('lost-pin','crash-file'),('lost-pin','partial-file'),('lost','blocked')):
            with self.subTest(mode=mode,finish=finish): self.run_init(mode,finish)

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

"""Real multipart -> commit/backlinks/ADD -> scan -> remote read end-to-end."""
import copy
import email.parser
import email.policy
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
STORAGE,SERVICE=-1003916921678,-1004400939172
TOKEN='123456:PUT_TEST_TOKEN'
ROOT='a30d487f-167a-4eb3-8b06-b0676a039470'
SB=dict(magic='TGFS',format_version=1,type='SUPERBLOCK',start_id='1',root_object_id=ROOT,next_message=None)
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self,*args): pass
    def reply(self,result=None,error=None,status=200):
        data=json.dumps(error if error else dict(ok=True,result=result)).encode()
        self.send_response(status);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
    def do_POST(self):
        s=self.server;method=self.path.rsplit('/',1)[-1]
        data=self.rfile.read(int(self.headers['Content-Length']))
        s.calls.append(method)
        if method=='sendDocument':
            mime=email.parser.BytesParser(policy=email.policy.default).parsebytes(
                ('Content-Type: '+self.headers['Content-Type']+'\r\n\r\n').encode()+data)
            fields={p.get_param('name',header='content-disposition'):p for p in mime.iter_parts()}
            raw=fields['document'].get_payload(decode=True)
            filename=fields['document'].get_filename()
            caption=fields['caption'].get_payload(decode=True).decode()
            record=json.loads(caption);kind=record['type']
            intent=json.loads(s.intent.read_text())
            expected={'CHUNK':'chunk_send_pending','FILE_COMMIT':'commit_send_pending','DIRECTORY':'directory_send_pending'}[kind]
            assert intent['phase']==expected
            assert (s.intent.parent/'data.bin').read_bytes()==s.input_data
            mid=s.next;s.next+=1
            fid='file_'+str(mid);s.files[fid]=raw
            msg=dict(message_id=mid,chat=dict(id=STORAGE,type='channel'),caption=caption,
                document=dict(file_id=fid,file_name=filename,file_size=len(raw)))
            s.messages[mid]=msg;s.sent.append(kind)
            if s.mode=='lost' and kind=='CHUNK':
                self.connection.shutdown(socket.SHUT_RDWR);self.connection.close();return
            if s.mode=='response' and kind=='CHUNK':
                msg=copy.deepcopy(msg);msg['document']['file_name']='wrong'
            return self.reply(msg)
        p=json.loads(data)
        if method=='getChat':
            chat=dict(id=int(p['chat_id']),type='channel')
            if int(p['chat_id'])==STORAGE:
                chat['pinned_message']=copy.deepcopy(s.messages[1])
                if s.mode=='pin':
                    header=copy.deepcopy(SB);header['root_object_id']=str(uuid.uuid4())
                    chat['pinned_message']['text']=json.dumps(header)
            return self.reply(chat)
        if method=='editMessageCaption':
            if s.mode=='edit':
                return self.reply(error=dict(ok=False,error_code=429,description=TOKEN,parameters=dict(retry_after=5)),status=429)
            msg=s.messages[p['message_id']];msg['caption']=p['caption']
            return self.reply(msg)
        if method=='sendMessage':
            mid=s.next;s.next+=1
            msg=dict(message_id=mid,chat=dict(id=STORAGE,type='channel'),text=p['text'])
            s.messages[mid]=msg;return self.reply(msg)
        if method=='forwardMessage':
            assert int(p['chat_id'])==SERVICE and int(p['from_chat_id'])==STORAGE
            msg=copy.deepcopy(s.messages[p['message_id']]);msg['message_id']=s.copy_id;s.copy_id+=1
            msg['chat']=dict(id=SERVICE,type='channel')
            msg['forward_origin']=dict(type='channel',chat=dict(id=STORAGE,type='channel'),message_id=p['message_id'])
            return self.reply(msg)
        if method=='getFile':
            fid=p['file_id'];return self.reply(dict(file_id=fid,file_size=len(s.files[fid]),file_path='documents/'+fid))
        if method=='deleteMessage':
            assert int(p['chat_id'])==SERVICE
            return self.reply(True)
        raise AssertionError(method)
    def do_GET(self):
        fid=self.path.rsplit('/',1)[-1];data=self.server.files[fid]
        self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)

class Put(unittest.TestCase):
    def exercise(self,data=b'hello\n',mode='ok',readback=True):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);capture=root/'capture';capture.mkdir()
            journal=dict(schema='TGFS_JOURNAL_CAPTURE_V1',scan_completed=True,gap_policy='forward_400_is_gap',
                storage_chat_id=str(STORAGE),service_chat_id=str(SERVICE),superblock=SB,boundary_id='2',records=[])
            (capture/'journal.json').write_text(json.dumps(journal))
            source=root/'source';source.write_bytes(data)
            token=root/'token';token.write_text(TOKEN);token.chmod(0o600)
            state=root/'put-state'
            s=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
            s.intent=state/'intent.json';s.input_data=data;s.mode=mode;s.calls=[];s.sent=[];s.files={};s.next=3;s.copy_id=1001
            s.messages={1:dict(message_id=1,chat=dict(id=STORAGE,type='channel'),text=json.dumps(SB)),
                        2:dict(message_id=2,chat=dict(id=STORAGE,type='channel'),text=json.dumps(dict(magic='TGFS',format_version=1,type='SCAN_MARKER',next_message=None)))}
            t=threading.Thread(target=s.serve_forever,kwargs=dict(poll_interval=.01),daemon=True);t.start()
            env=dict(os.environ,TGFS_TEST_API_BASE=f'http://127.0.0.1:{s.server_port}')
            def run(args):
                p=subprocess.run([BINARY,*args],env=env,capture_output=True,timeout=15)
                err=p.stderr.decode()
                self.assertNotIn(TOKEN,err);self.assertNotIn('AddressSanitizer',err);self.assertNotIn('runtime error:',err)
                return p
            args=['put','--capture',str(capture),'--file',str(source),'--name','hello.txt',
                  '--state-dir',str(state),'--token-file',str(token),'--chunk-bytes','3','--send-interval-ms','0']
            try:
                p=run(args)
                if mode!='ok':
                    self.assertEqual(p.returncode,1,p.stderr)
                    self.assertNotIn('DIRECTORY',s.sent)
                    if mode=='lost': self.assertIn(b'outcome_unknown=1',p.stderr)
                    if mode in ('lost','response','edit'):
                        count=len(s.sent);retry=run(args);self.assertEqual(retry.returncode,1);self.assertEqual(len(s.sent),count)
                    return
                self.assertEqual(p.returncode,0,p.stderr)
                intent=json.loads(s.intent.read_text());self.assertEqual(intent['phase'],'complete')
                self.assertEqual(uuid.UUID(intent['object_id']).version,4)
                self.assertEqual(len(intent['parts']),(len(data)+2)//3)
                for part in intent['parts']:
                    caption=json.loads(s.messages[int(part['message_id'])]['caption'])
                    self.assertEqual(caption['file_commit_id'],intent['file_commit_id'])
                    self.assertGreater(int(caption['next_message']),int(part['message_id']))
                commit=json.loads(s.messages[int(intent['file_commit_id'])]['caption'])
                self.assertEqual(commit['next_message'],intent['directory_message_id'])
                self.assertEqual(s.sent[-2:],['FILE_COMMIT','DIRECTORY'])
                self.assertEqual((state/'data.bin').read_bytes(),data)
                before=len(s.sent);retry=run(args);self.assertEqual(retry.returncode,1);self.assertEqual(len(s.sent),before)
                if readback:
                    scanned=root/'scan'
                    result=run(['scan','--token-file',str(token),'--storage-chat-id',str(STORAGE),
                        '--service-chat-id',str(SERVICE),'--output',str(scanned)])
                    self.assertEqual(result.returncode,0,result.stderr)
                    listing=run(['read','--no-cache','--capture',str(scanned),'--token-file',str(token),'ls','/'])
                    self.assertEqual(listing.returncode,0,listing.stderr);self.assertEqual(listing.stdout,b'hello.txt\n')
                    result=run(['read','--no-cache','--capture',str(scanned),'--token-file',str(token),'cat','/hello.txt'])
                    self.assertEqual(result.returncode,0,result.stderr);self.assertEqual(result.stdout,data)
                    # A fresh capture detects the existing name before another upload.
                    args[args.index('--capture')+1]=str(scanned)
                    args[args.index('--state-dir')+1]=str(root/'other-state')
                    before=len(s.sent);duplicate=run(args)
                    self.assertEqual(duplicate.returncode,1);self.assertIn(b'name already exists',duplicate.stderr)
                    self.assertEqual(len(s.sent),before)
                    if data:
                        # Independent additions based on the same old capture merge.
                        args[args.index('--capture')+1]=str(capture)
                        args[args.index('--name')+1]='notes.txt'
                        s.intent=root/'second-state'/'intent.json'
                        args[args.index('--state-dir')+1]=str(s.intent.parent)
                        added=run(args);self.assertEqual(added.returncode,0,added.stderr)
                        fresh=root/'scan2'
                        result=run(['scan','--token-file',str(token),'--storage-chat-id',str(STORAGE),
                            '--service-chat-id',str(SERVICE),'--output',str(fresh)])
                        self.assertEqual(result.returncode,0,result.stderr)
                        listing=run(['read','--no-cache','--capture',str(fresh),'--token-file',str(token),'ls','/'])
                        self.assertEqual(listing.returncode,0,listing.stderr)
                        self.assertEqual(listing.stdout,b'hello.txt\nnotes.txt\n')
            finally:
                s.shutdown();s.server_close();t.join()
    def test_roundtrip_multipart(self): self.exercise()
    def test_roundtrip_empty(self): self.exercise(b'')
    def test_roundtrip_one_chunk(self): self.exercise(b'hi')
    def test_failures(self):
        for mode in ('pin','lost','response','edit'):
            with self.subTest(mode=mode): self.exercise(mode=mode)
unittest.main()

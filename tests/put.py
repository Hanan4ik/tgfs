"""Real multipart -> commit/backlinks/ADD -> scan -> remote read end-to-end."""
import copy
import email.parser
import email.policy
import http.server
import json
import os
from pathlib import Path
import socket
import time
import shutil
import errno
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
            if intent.get('kind')=='mkdir' and record['object_id']==intent['object_id']: expected='child_send_pending'
            assert intent['phase']==expected
            if intent.get('kind') not in ('mkdir','unlink'): assert (s.intent.parent/'data.bin').read_bytes()==s.input_data
            if s.mode=='unlink-apierror' and intent.get('kind')=='unlink':
                return self.reply(error=dict(ok=False,error_code=429,description=TOKEN,parameters=dict(retry_after=5)),status=429)
            mid=s.next;s.next+=1
            fid='file_'+str(mid);s.files[fid]=raw
            msg=dict(message_id=mid,chat=dict(id=STORAGE,type='channel'),caption=caption,
                document=dict(file_id=fid,file_name=filename,file_size=len(raw)))
            s.messages[mid]=msg;s.sent.append(kind)
            if (s.mode=='lost' and kind=='CHUNK') or (s.mode=='mkdir-lost-child' and expected=='child_send_pending') or (s.mode=='mkdir-lost-parent' and expected=='directory_send_pending') or (s.mode=='replace-lost-chunk' and kind=='CHUNK') or (s.mode=='replace-lost-commit' and kind=='FILE_COMMIT') or (s.mode=='unlink-lost' and intent.get('kind')=='unlink'):
                self.connection.shutdown(socket.SHUT_RDWR);self.connection.close();return
            if (s.mode=='response' and kind=='CHUNK') or (s.mode=='mkdir-response' and expected=='child_send_pending') or (s.mode=='unlink-response' and intent.get('kind')=='unlink'):
                msg=copy.deepcopy(msg);msg['document']['file_name']='wrong'
            return self.reply(msg)
        p=json.loads(data)
        if method=='getChat':
            chat=dict(id=int(p['chat_id']),type='channel')
            if int(p['chat_id'])==STORAGE:
                chat['pinned_message']=copy.deepcopy(s.messages[1])
                if s.mode=='unlink-pin': s.unlink_pin_reads+=1
                if (s.mode=='unlink-pin' and s.unlink_pin_reads>=3) or s.mode=='pin' or (s.mode=='mkdir-pin' and s.sent) or (s.mode=='replace-pin' and len(s.sent)>s.before_replace):
                    header=copy.deepcopy(SB);header['root_object_id']=str(uuid.uuid4())
                    chat['pinned_message']['text']=json.dumps(header)
            return self.reply(chat)
        if method=='editMessageCaption':
            if s.mode in ('edit','mkdir-edit','replace-edit'):
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
    def directories(self,mode='ok',mount=False,replace=False,replace_mode='ok',unlink=False,unlink_mode='ok',diagnose=False):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);capture=root/'capture';capture.mkdir()
            journal=dict(schema='TGFS_JOURNAL_CAPTURE_V1',scan_completed=True,gap_policy='forward_400_is_gap',
                storage_chat_id=str(STORAGE),service_chat_id=str(SERVICE),superblock=SB,boundary_id='2',records=[])
            (capture/'journal.json').write_text(json.dumps(journal))
            token=root/'token';token.write_text(TOKEN);token.chmod(0o600)
            source=root/'source';source.write_bytes(b'abcdefghij\n')
            s=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
            s.mode=mode;s.calls=[];s.sent=[];s.files={};s.next=3;s.copy_id=1001;s.input_data=source.read_bytes()
            s.messages={1:dict(message_id=1,chat=dict(id=STORAGE,type='channel'),text=json.dumps(SB)),
                2:dict(message_id=2,chat=dict(id=STORAGE,type='channel'),text=json.dumps(dict(magic='TGFS',format_version=1,type='SCAN_MARKER',next_message=None)))}
            t=threading.Thread(target=s.serve_forever,kwargs=dict(poll_interval=.01),daemon=True);t.start()
            env=dict(os.environ,TGFS_TEST_API_BASE=f'http://127.0.0.1:{s.server_port}')
            serial=0
            def run(args,code=0):
                result=subprocess.run([BINARY,*args],env=env,capture_output=True,timeout=15)
                self.assertEqual(result.returncode,code,result.stderr)
                self.assertNotIn(TOKEN,result.stderr.decode());self.assertNotIn(b'AddressSanitizer',result.stderr)
                self.assertNotIn(b'runtime error:',result.stderr);return result
            def write(command,cap,*args,code=0,state=None):
                nonlocal serial
                serial+=1;s.intent=(state or root/f'state-{serial}')/'intent.json'
                result=run([command,'--capture',str(cap),'--state-dir',str(s.intent.parent),'--token-file',str(token),
                    '--send-interval-ms','0',*args],code)
                return result,json.loads(s.intent.read_text()) if s.intent.exists() else None
            def scan():
                nonlocal serial
                serial+=1;out=root/f'capture-{serial}'
                run(['scan','--token-file',str(token),'--storage-chat-id',str(STORAGE),
                    '--service-chat-id',str(SERVICE),'--output',str(out)])
                return out
            def read(cap,action,path):
                return run(['read','--no-cache','--capture',str(cap),'--token-file',str(token),action,path]).stdout
            def check_intent(state,cap,expected):
                before=(state/'intent.json').read_bytes();start=len(s.calls)
                result=run(['intent','--state-dir',str(state),'--capture',str(cap),'--token-file',str(token)])
                report=json.loads(result.stdout);self.assertEqual(report['publication'],expected)
                self.assertFalse(report['can_resume'])
                if report['kind']=='put': self.assertEqual(report['staging'],'verified')
                self.assertFalse(report['resumed']);self.assertEqual((state/'intent.json').read_bytes(),before)
                self.assertTrue(set(s.calls[start:])<={'getChat','getFile'})
                return report
            try:
                run(['intent','--help']);self.assertEqual(s.calls,[])
                run(['mkdir','--help']);self.assertEqual(s.calls,[])
                result,directory=write('mkdir',capture,'--path','/documents',code=0 if mode=='ok' else 1)
                if mode!='ok':
                    self.assertIsNotNone(directory)
                    expected={'mkdir-lost-child':'child_response_uncertain','mkdir-response':'child_response_uncertain',
                        'mkdir-lost-parent':'directory_response_uncertain','mkdir-pin':'child_sent',
                        'mkdir-edit':'published_navigation_pending'}[mode]
                    self.assertEqual(directory['phase'],expected)
                    self.assertEqual(len(s.sent),1 if mode in ('mkdir-lost-child','mkdir-response','mkdir-pin') else 2)
                    before=len(s.sent);saved=s.intent.parent
                    write('mkdir',capture,'--path','/documents',state=saved,code=1)
                    self.assertEqual(len(s.sent),before)
                    if mode in ('mkdir-lost-parent','mkdir-edit'):
                        s.mode='ok';fresh=scan();self.assertEqual(read(fresh,'ls','/'),b'documents\n')
                        self.assertEqual(read(fresh,'ls','/documents'),b'')
                    return
                self.assertEqual(directory['phase'],'complete');self.assertEqual(uuid.UUID(directory['object_id']).version,4)
                self.assertEqual(s.sent,['DIRECTORY','DIRECTORY'])
                child=json.loads(s.messages[int(directory['child_message_id'])]['caption'])
                self.assertEqual(child['parent_id'],ROOT);self.assertEqual(child['next_message'],directory['directory_message_id'])
                self.assertEqual(json.loads(s.files['file_'+directory['child_message_id']]),{'operations':[]})
                # A stale capture cannot address a directory it has never observed.
                before=len(s.sent)
                write('put',capture,'--file',str(source),'--name','too-early.txt','--parent','/documents',code=1)
                self.assertEqual(len(s.sent),before)
                cap=scan();self.assertEqual(read(cap,'ls','/'),b'documents\n');self.assertEqual(read(cap,'ls','/documents'),b'')
                write('mkdir',cap,'--path','/documents',code=1)
                write('mkdir',cap,'--path','/missing/sub',code=1)
                write('mkdir',cap,'--path','/documents/../bad',code=1)
                for path in ('/','relative','/documents/.','/documents/..'):
                    write('mkdir',cap,'--path',path,code=2)
                self.assertEqual(len(s.sent),before)
                _,sub=write('mkdir',cap,'--path','/documents/sub/')
                fresh=scan()
                self.assertEqual(read(fresh,'ls','/documents'),b'sub\n')
                _,file=write('put',fresh,'--file',str(source),'--name','multi.txt','--parent','/documents/sub','--chunk-bytes','4')
                # ADDs independently based on the same capture must merge in this nested parent.
                source.write_bytes(b'');s.input_data=b''
                _,empty=write('put',fresh,'--file',str(source),'--name','empty.txt','--parent','/documents/sub')
                _,sibling=write('mkdir',fresh,'--path','/documents/other')
                self.assertEqual(file['parent_id'],sub['object_id']);self.assertEqual(empty['parent_id'],sub['object_id'])
                for msg in s.messages.values():
                    if 'caption' not in msg: continue
                    c=json.loads(msg['caption'])
                    if c.get('object_id')==file['object_id']: self.assertEqual(c['parent_id'],sub['object_id'])
                published=json.loads(s.messages[int(file['directory_message_id'])]['caption'])
                self.assertEqual(published['object_id'],sub['object_id']);self.assertEqual(published['parent_id'],directory['object_id'])
                final=scan()
                self.assertEqual(read(final,'ls','/documents'),b'sub\nother\n')
                self.assertEqual(read(final,'ls','/documents/sub'),b'multi.txt\nempty.txt\n')
                self.assertEqual(read(final,'cat','/documents/sub/multi.txt'),b'abcdefghij\n')
                self.assertEqual(read(final,'cat','/documents/sub/empty.txt'),b'')
                before=len(s.sent)
                write('put',final,'--file',str(source),'--name','multi.txt','--parent','/documents/sub',code=1)
                write('mkdir',final,'--path','/documents/sub/multi.txt',code=1)
                write('mkdir',final,'--path','/documents/sub/multi.txt/child',code=1)
                write('put',final,'--file',str(source),'--name','bad.txt','--parent','/documents/sub/multi.txt',code=1)
                write('put',final,'--file',str(source),'--name','bad.txt','--parent','/missing',code=1)
                self.assertEqual(len(s.sent),before)
                expected_data=b'abcdefghij\n'
                if replace:
                    original_cap=final;old_messages=copy.deepcopy(s.messages);previous=file
                    for args in (['--parent','/documents/sub','--name','missing.txt','--replace'],
                        ['--parent','/documents','--name','sub','--replace']):
                        write('put',final,'--file',str(source),*args,code=1)
                    write('put',final,'--file',str(source),'--parent','/documents/sub','--name','multi.txt','--replace','--replace',code=2)
                    self.assertEqual(len(s.sent),before)
                    s.mode=replace_mode;s.before_replace=len(s.sent)
                    payloads=[b'replaced content\n',b'',b'12345678\n'] if replace_mode=='ok' else [b'replaced content\n']
                    for data in payloads:
                        source.write_bytes(data);s.input_data=data
                        before=len(s.sent)
                        _,updated=write('put',final,'--file',str(source),'--name','multi.txt','--parent','/documents/sub',
                            '--replace','--chunk-bytes','4',code=0 if replace_mode=='ok' else 1)
                        state=s.intent.parent
                        self.assertEqual(updated['object_id'],file['object_id'])
                        self.assertNotEqual(updated['version_id'],previous['version_id'])
                        self.assertNotEqual(updated['operation_id'],previous['operation_id'])
                        self.assertEqual(updated['base_commit_id'],previous['file_commit_id'])
                        self.assertEqual(updated['base_version_id'],previous['version_id'])
                        self.assertTrue(updated['replace']);self.assertNotIn('add_operation_id',updated)
                        self.assertNotIn('DIRECTORY',s.sent[before:])
                        if replace_mode!='ok':
                            phase={'replace-lost-chunk':'chunk_response_uncertain','replace-lost-commit':'commit_response_uncertain',
                                'replace-edit':'published_backlinks_pending','replace-pin':'chunk_sent'}[replace_mode]
                            self.assertEqual(updated['phase'],phase)
                            expected_data=data if replace_mode in ('replace-lost-commit','replace-edit') else expected_data
                            s.mode='ok'
                        else:
                            self.assertEqual(updated['phase'],'complete');self.assertNotIn('directory_message_id',updated)
                            self.assertEqual(len(updated['parts']),(len(data)+3)//4)
                            commit=json.loads(s.messages[int(updated['file_commit_id'])]['caption'])
                            self.assertIsNone(commit['next_message'])
                            expected_data=data
                        count=len(s.sent)
                        write('put',original_cap,'--file',str(source),'--name','multi.txt','--parent','/documents/sub',
                            '--replace',state=state,code=1)
                        self.assertEqual(len(s.sent),count)
                        final=scan()
                        if diagnose:
                            evidence=check_intent(state,final,'confirmed' if replace_mode in ('ok','replace-lost-commit','replace-edit') else 'not_observed')
                            if replace_mode=='replace-edit': self.assertTrue(evidence['backlink_repairs'])
                            if replace_mode=='replace-lost-chunk': self.assertTrue(evidence['unobserved_part_indexes'])
                            if replace_mode=='replace-lost-commit': self.assertEqual(len(evidence['verified_chunk_ids']),len(updated['parts']))
                        self.assertEqual(read(final,'cat','/documents/sub/multi.txt'),expected_data)
                        self.assertEqual(read(original_cap,'cat','/documents/sub/multi.txt'),b'abcdefghij\n')
                        self.assertEqual(read(final,'ls','/documents/sub'),b'multi.txt\nempty.txt\n')
                        previous=updated
                    if replace_mode=='ok':
                        # Concurrent writers based on one older capture publish normally; later canonical commit wins.
                        for data in (b'A updated\n',b'B updated\n'):
                            source.write_bytes(data);s.input_data=data
                            _,updated=write('put',original_cap,'--file',str(source),'--name','multi.txt',
                                '--parent','/documents/sub','--replace','--chunk-bytes','4')
                            self.assertEqual(updated['object_id'],file['object_id'])
                            self.assertEqual(updated['base_commit_id'],file['file_commit_id'])
                        final=scan();expected_data=b'B updated\n'
                        self.assertEqual(read(final,'cat','/documents/sub/multi.txt'),expected_data)
                    for mid,msg in old_messages.items(): self.assertEqual(s.messages[mid],msg)
                file_exists=True
                if unlink:
                    run(['unlink','--help']);before=len(s.sent)
                    for path,code in (('/missing',1),('/documents/sub',1),('/documents/sub/multi.txt/child',1),
                        ('/missing/file',1),('/documents/../bad',1),('/',2),('relative',2),('/documents/sub/multi.txt/',2)):
                        write('unlink',final,'--path',path,code=code)
                    self.assertEqual(len(s.sent),before)
                    if unlink_mode=='legacy':
                        journal=json.loads((final/'journal.json').read_text());records=[]
                        for record in journal['records']:
                            if record['record'].get('object_id')==sub['object_id'] and record['record']['type']=='DIRECTORY':
                                if record['message_id']!=empty['directory_message_id']: continue
                                raw=json.dumps(dict(entries=[dict(name='multi.txt',object_id=file['object_id']),
                                    dict(name='empty.txt',object_id=empty['object_id'])])).encode()
                                s.files[record['document']['file_id']]=raw;record['document']['file_size']=len(raw)
                            records.append(record)
                        journal['records']=records;legacy=root/'legacy';legacy.mkdir()
                        (legacy/'journal.json').write_text(json.dumps(journal))
                        result,_=write('unlink',legacy,'--path','/documents/sub/multi.txt',code=1)
                        self.assertIn(b'legacy entries',result.stderr);self.assertEqual(len(s.sent),before);return
                    def inject_ops(ops):
                        mid=s.next;s.next+=1;fid='file_'+str(mid)
                        raw=json.dumps(dict(operations=ops)).encode();s.files[fid]=raw
                        c=dict(magic='TGFS',format_version=1,type='DIRECTORY',object_id=sub['object_id'],
                            parent_id=directory['object_id'],next_message=None)
                        s.messages[mid]=dict(message_id=mid,chat=dict(id=STORAGE,type='channel'),caption=json.dumps(c),
                            document=dict(file_id=fid,file_name='directory.json',file_size=len(raw)))
                    original_add=json.loads(s.files['file_'+file['directory_message_id']])['operations'][0]
                    removed=str(uuid.uuid4());active=str(uuid.uuid4())
                    inject_ops([dict(operation_id=removed,action='ADD',name='multi.txt',object_id=file['object_id']),
                        dict(operation_id=str(uuid.uuid4()),action='REMOVE',name='multi.txt',observed_adds=[removed]),
                        dict(operation_id=active,action='ADD',name='multi.txt',object_id=file['object_id']),copy.deepcopy(original_add)])
                    before_capture=scan();old_messages=copy.deepcopy(s.messages)
                    if unlink_mode=='concurrent':
                        # A second machine publishes the same name using a capture from before the first ADD.
                        source.write_bytes(b'concurrent generation\n');s.input_data=source.read_bytes()
                        _,other=write('put',fresh,'--file',str(source),'--name','multi.txt','--parent','/documents/sub')
                        self.assertNotEqual(other['object_id'],file['object_id']);expected_data=s.input_data
                    s.mode=unlink_mode if unlink_mode.startswith('unlink-') else 'ok';s.unlink_pin_reads=0
                    before=len(s.sent)
                    result,removed_intent=write('unlink',before_capture,'--path','/documents/sub/multi.txt',
                        code=1 if unlink_mode.startswith('unlink-') else 0)
                    self.assertIsNotNone(removed_intent);saved=s.intent.parent
                    self.assertFalse((saved/'data.bin').exists())
                    self.assertEqual(removed_intent['object_id'],file['object_id'])
                    self.assertEqual(set(removed_intent['observed_adds']),{original_add['operation_id'],active})
                    self.assertEqual(len(removed_intent['observed_adds']),2)
                    self.assertNotIn(removed,removed_intent['observed_adds'])
                    phase='prepared' if unlink_mode=='unlink-pin' else ('directory_response_uncertain' if unlink_mode.startswith('unlink-') else 'complete')
                    self.assertEqual(removed_intent['phase'],phase)
                    if unlink_mode not in ('unlink-pin','unlink-apierror'):
                        self.assertEqual(s.sent[before:],['DIRECTORY'])
                        msg=s.messages[int(removed_intent.get('directory_message_id',max(s.messages)))]
                        if unlink_mode=='unlink-lost': self.assertNotIn('directory_message_id',removed_intent)
                        c=json.loads(msg['caption']);self.assertEqual(c['object_id'],sub['object_id'])
                        self.assertEqual(c['parent_id'],directory['object_id']);self.assertIsNone(c['next_message'])
                        op=json.loads(s.files[msg['document']['file_id']])['operations'][0]
                        self.assertEqual(op,dict(operation_id=removed_intent['remove_operation_id'],action='REMOVE',
                            name='multi.txt',observed_adds=removed_intent['observed_adds']))
                    else: self.assertEqual(len(s.sent),before)
                    s.mode='ok';count=len(s.sent)
                    write('unlink',before_capture,'--path','/documents/sub/multi.txt',state=saved,code=1)
                    self.assertEqual(len(s.sent),count)
                    inject_ops([copy.deepcopy(original_add)]) # Retry of the old ADD cannot resurrect the name.
                    final=scan()
                    if diagnose: check_intent(saved,final,'not_observed' if unlink_mode in ('unlink-pin','unlink-apierror') else 'confirmed')
                    file_exists=unlink_mode in ('unlink-pin','unlink-apierror','concurrent')
                    listing=read(final,'ls','/documents/sub')
                    self.assertEqual(b'multi.txt\n' in listing,file_exists)
                    self.assertIn(b'empty.txt\n',listing)
                    if file_exists: self.assertEqual(read(final,'cat','/documents/sub/multi.txt'),expected_data)
                    else:
                        result=run(['read','--no-cache','--capture',str(final),'--token-file',str(token),
                            'cat','/documents/sub/multi.txt'],code=1)
                        self.assertIn(b'does not exist',result.stderr)
                    self.assertEqual(read(before_capture,'cat','/documents/sub/multi.txt'),b'abcdefghij\n')
                    for mid,msg in old_messages.items(): self.assertEqual(s.messages[mid],msg)
                    if unlink_mode=='recreate':
                        source.write_bytes(b'new generation\n');s.input_data=source.read_bytes()
                        _,new=write('put',final,'--file',str(source),'--parent','/documents/sub','--name','multi.txt')
                        self.assertNotEqual(new['object_id'],file['object_id'])
                        write('unlink',before_capture,'--path','/documents/sub/multi.txt')
                        final=scan();self.assertEqual(read(final,'cat','/documents/sub/multi.txt'),s.input_data)
                        # Empty file and root directory mutations use the same observed-remove contract.
                        write('unlink',final,'--path','/documents/sub/empty.txt')
                        write('put',final,'--file',str(source),'--name','root.txt')
                        cap=scan();write('unlink',cap,'--path','/root.txt')
                        final=scan();self.assertEqual(read(final,'ls','/'),b'documents\n')
                        self.assertEqual(read(final,'ls','/documents/sub'),b'multi.txt\n')
                if diagnose and not unlink and not replace:
                    for state_file in root.glob('state-*/intent.json'):
                        if json.loads(state_file.read_text())['phase']=='complete': check_intent(state_file.parent,final,'confirmed')
                    state_file=next(f for f in root.glob('state-*/intent.json') if json.loads(f.read_text()).get('object_id')==file['object_id'])
                    run(['intent','--state-dir',str(state_file.parent),'--capture',str(capture),'--token-file',str(token)],code=1)
                    original_state=state_file.read_bytes();bad=json.loads(original_state)
                    bad['commit_document']['parts'][0]['part_sha256']='0'*64;state_file.write_text(json.dumps(bad))
                    result=run(['intent','--state-dir',str(state_file.parent),'--capture',str(final),'--token-file',str(token)],code=1)
                    self.assertIn(b'disagrees',result.stderr);state_file.write_bytes(original_state)
                    state_file.chmod(0o644)
                    run(['intent','--state-dir',str(state_file.parent),'--capture',str(final),'--token-file',str(token)],code=1)
                    state_file.chmod(0o600)
                    duplicate_journal=json.loads((final/'journal.json').read_text())
                    record=copy.deepcopy(next(r for r in duplicate_journal['records'] if r['message_id']==file['parts'][0]['message_id']))
                    record['message_id']=duplicate_journal['boundary_id'];record['record']['next_message']=None;record['record']['file_commit_id']=None
                    duplicate_journal['records'].append(record);duplicate_journal['boundary_id']=str(int(duplicate_journal['boundary_id'])+2)
                    duplicated=root/'duplicated';duplicated.mkdir();(duplicated/'journal.json').write_text(json.dumps(duplicate_journal))
                    evidence=check_intent(state_file.parent,duplicated,'confirmed')
                    self.assertEqual(evidence['ambiguous_part_indexes'],[0]);self.assertEqual(evidence['next_step'],'resolve_duplicate_parts')
                    data_path=state_file.parent/'data.bin';original_data=data_path.read_bytes();data_path.write_bytes(b'bad')
                    result=run(['intent','--state-dir',str(state_file.parent),'--capture',str(final),'--token-file',str(token)],code=1)
                    self.assertIn(b'staging',result.stderr);data_path.write_bytes(original_data)
                    fid='file_'+file['parts'][0]['message_id'];original_chunk=s.files[fid];s.files[fid]=b'X'*len(original_chunk)
                    result=run(['intent','--state-dir',str(state_file.parent),'--capture',str(final),'--token-file',str(token)],code=1)
                    self.assertIn(b'CHUNK',result.stderr);s.files[fid]=original_chunk

                # Parent documents stay append-only; only the new child caption was edited.

                original=json.loads(s.messages[int(directory['child_message_id'])]['caption'])
                self.assertEqual(original,child)
                if mount:
                    point=root/'mnt';point.mkdir();log=root/'mount.log'
                    with log.open('wb') as stream:
                        proc=subprocess.Popen([BINARY,'mount','--capture',str(final),'--token-file',str(token),
                            '--mountpoint',str(point),'--no-cache'],env=env,stdout=stream,stderr=stream)
                        try:
                            end=time.monotonic()+15
                            while proc.poll() is None and not os.path.ismount(point) and time.monotonic()<end: time.sleep(.05)
                            self.assertTrue(os.path.ismount(point),log.read_text())
                            if file_exists: self.assertEqual((point/'documents/sub/multi.txt').read_bytes(),expected_data)
                            else:
                                with self.assertRaises(FileNotFoundError): (point/'documents/sub/multi.txt').read_bytes()
                                self.assertNotIn('multi.txt',[p.name for p in (point/'documents/sub').iterdir()])
                            self.assertEqual((point/'documents/sub/empty.txt').stat().st_size,0)
                            self.assertEqual((point/'documents').stat().st_nlink,4)
                            self.assertEqual((point/'documents/sub').stat().st_nlink,2)
                            if file_exists:
                                with (point/'documents/sub/multi.txt').open('rb',buffering=0) as f:
                                    self.assertEqual(os.pread(f.fileno(),5,2),expected_data[2:7])
                            with self.assertRaises(OSError) as caught: (point/'documents/new').mkdir()
                            self.assertEqual(caught.exception.errno,errno.EROFS)
                        finally:
                            if os.path.ismount(point): subprocess.run(['fusermount3','-u',str(point)],check=True,timeout=10)
                            proc.wait(timeout=10);self.assertEqual(proc.returncode,0,log.read_text())
                            self.assertNotIn('AddressSanitizer',log.read_text());self.assertNotIn('runtime error:',log.read_text())
            finally:
                s.shutdown();s.server_close();t.join()
    def test_intent_complete_and_validation(self): self.directories(diagnose=True)
    def test_intent_lost_commit(self):
        for mode in ('replace-lost-commit','replace-lost-chunk','replace-edit'):
            with self.subTest(mode=mode): self.directories(replace=True,replace_mode=mode,diagnose=True)
    def test_intent_lost_remove(self):
        for mode in ('unlink-lost','unlink-apierror','unlink-pin'):
            with self.subTest(mode=mode): self.directories(unlink=True,unlink_mode=mode,diagnose=True)
    def test_unlink_and_observed_adds(self):
        for mode in ('ok','recreate','concurrent','legacy'):
            with self.subTest(mode=mode): self.directories(unlink=True,unlink_mode=mode)
    def test_unlink_failures(self):
        for mode in ('unlink-lost','unlink-response','unlink-apierror','unlink-pin'):
            with self.subTest(mode=mode): self.directories(unlink=True,unlink_mode=mode)
    @unittest.skipUnless(Path('/dev/fuse').exists() and shutil.which('fusermount3'),'kernel mount unavailable')
    def test_unlinked_capture_mount(self): self.directories(unlink=True,mount=True)
    def test_replace_contents_and_stale_writers(self): self.directories(replace=True)
    def test_replace_failures(self):
        for mode in ('replace-lost-chunk','replace-lost-commit','replace-edit','replace-pin'):
            with self.subTest(mode=mode): self.directories(replace=True,replace_mode=mode)
    @unittest.skipUnless(Path('/dev/fuse').exists() and shutil.which('fusermount3'),'kernel mount unavailable')
    def test_replaced_capture_mount(self): self.directories(mount=True,replace=True)
    def test_nested_directory_publication(self): self.directories()
    def test_directory_failures(self):
        for mode in ('mkdir-lost-child','mkdir-lost-parent','mkdir-response','mkdir-pin','mkdir-edit'):
            with self.subTest(mode=mode): self.directories(mode)
    @unittest.skipUnless(Path('/dev/fuse').exists() and shutil.which('fusermount3'),'kernel mount unavailable')
    def test_nested_telegram_capture_mount(self): self.directories(mount=True)
    def test_roundtrip_multipart(self): self.exercise()
    def test_roundtrip_empty(self): self.exercise(b'')
    def test_roundtrip_one_chunk(self): self.exercise(b'hi')
    def test_failures(self):
        for mode in ('pin','lost','response','edit'):
            with self.subTest(mode=mode): self.exercise(mode=mode)
unittest.main()

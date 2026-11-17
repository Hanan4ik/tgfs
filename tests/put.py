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
            if intent['phase']!=expected and kind=='DIRECTORY':
                assert 'directory_send_pending' in (s.intent.parent/'backlink-repair.log').read_text()
            else: assert intent['phase']==expected
            if intent.get('kind') not in ('mkdir','unlink'): assert (s.intent.parent/'data.bin').read_bytes()==s.input_data
            if s.mode.startswith('reject-') and (not getattr(s,'reject_kind',None) or kind==s.reject_kind):
                case=s.mode[7:];code=429 if case in ('429','badretry','no-retry') else 403 if case=='403' else 500 if case=='500' else 400
                http=500 if case=='http500' else 403 if case=='mismatch' else code
                error=dict(ok=False,error_code=code,description=TOKEN)
                if case=='nodescription': error.pop('description')
                if case=='result': error['result']=dict(message_id=999)
                if case=='429': error['parameters']=dict(retry_after=1)
                if case=='badretry': error['parameters']=dict(retry_after='invalid')
                if case=='log-write':
                    # Fail the next journal append in the real C process, after its pending intent was saved.
                    import resource
                    limit=(s.intent.parent/'backlink-repair.log').stat().st_size+16
                    resource.prlimit(s.client_pid,resource.RLIMIT_FSIZE,(limit,limit))
                return self.reply(error=error,status=http)
            if (s.mode=='unlink-apierror' and intent.get('kind')=='unlink') or s.mode=='resume-apierror':
                return self.reply(error=dict(ok=False,error_code=429,description=TOKEN,parameters=dict(retry_after=5)),status=429)
            mid=s.next;s.next+=1
            fid='file_'+str(mid);s.files[fid]=raw
            msg=dict(message_id=mid,chat=dict(id=STORAGE,type='channel'),caption=caption,
                document=dict(file_id=fid,file_unique_id='unique_'+fid,file_name=filename,file_size=len(raw)))
            s.messages[mid]=msg;s.sent.append(kind)
            if (s.mode=='lost' and kind=='CHUNK') or (s.mode=='mkdir-lost-child' and expected=='child_send_pending') or (s.mode=='mkdir-lost-parent' and expected=='directory_send_pending') or (s.mode=='replace-lost-chunk' and kind=='CHUNK') or (s.mode=='replace-lost-commit' and kind=='FILE_COMMIT') or (s.mode=='unlink-lost' and intent.get('kind')=='unlink') or s.mode=='resume-lost' or (s.mode=='upload-lost-chunk' and kind=='CHUNK') or (s.mode=='upload-lost-commit' and kind=='FILE_COMMIT'):
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
                if (s.mode=='create-pin-after-commit' and 'FILE_COMMIT' in s.sent) or (s.mode=='unlink-pin' and s.unlink_pin_reads>=3) or s.mode=='pin' or (s.mode=='mkdir-pin' and len(s.sent)>getattr(s,'mkdir_pin_after',0)) or (s.mode=='replace-pin' and len(s.sent)>s.before_replace):
                    header=copy.deepcopy(SB);header['root_object_id']=str(uuid.uuid4())
                    chat['pinned_message']['text']=json.dumps(header)
            return self.reply(chat)
        if method=='editMessageCaption':
            if s.mode in ('edit','mkdir-edit','replace-edit'):
                return self.reply(error=dict(ok=False,error_code=429,description=TOKEN,parameters=dict(retry_after=5)),status=429)
            msg=s.messages[p['message_id']];msg['caption']=p['caption']
            if s.mode=='repair-lost-edit':
                self.connection.shutdown(socket.SHUT_RDWR);self.connection.close();return
            return self.reply(msg)
        if method=='sendMessage':
            mid=s.next;s.next+=1
            msg=dict(message_id=mid,chat=dict(id=STORAGE,type='channel'),text=p['text'])
            s.messages[mid]=msg;return self.reply(msg)
        if method=='forwardMessage':
            assert int(p['chat_id'])==SERVICE and int(p['from_chat_id'])==STORAGE
            if p['message_id'] not in s.messages: return self.reply(error=dict(ok=False,error_code=400,description='message unavailable'),status=400)
            msg=copy.deepcopy(s.messages[p['message_id']]);msg['message_id']=s.copy_id;s.copy_id+=1
            msg['chat']=dict(id=SERVICE,type='channel')
            msg['forward_origin']=dict(type='channel',chat=dict(id=STORAGE,type='channel'),message_id=p['message_id'])
            if s.mode in ('file-alias','file-alias-missing-unique','file-alias-corrupt','file-alias-unique-conflict'):
                doc=msg['document'];old=doc['file_id'];alias='alias_'+old
                s.files[alias]=(b'X'*len(s.files[old])) if s.mode=='file-alias-corrupt' else s.files[old]
                doc['file_id']=alias
                if s.mode!='file-alias-missing-unique': doc['file_unique_id']='unique_'+old
                else: doc.pop('file_unique_id',None)
                if s.mode=='file-alias-unique-conflict': doc['file_unique_id']='different'
            if s.mode=='repair-origin': msg['forward_origin']['message_id']+=1
            return self.reply(msg)
        if method=='getFile':
            fid=p['file_id'];return self.reply(dict(file_id=fid,file_size=len(s.files[fid]),file_path='documents/'+fid))
        if method=='deleteMessage':
            if s.mode=='repair-cleanup': return self.reply(error=dict(ok=False,error_code=429,description='cleanup rejected'),status=429)
            assert int(p['chat_id'])==SERVICE
            return self.reply(True)
        raise AssertionError(method)
    def do_GET(self):
        fid=self.path.rsplit('/',1)[-1];data=self.server.files[fid]
        self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)

class Put(unittest.TestCase):
    def retry_add(self,run,command,s,state,case):
        original=(state/'intent.json').read_bytes();before=list(s.sent)
        s.mode='reject-'+('400' if case in ('twice','then-lost','bad-log','wrong-operation','orphan-rejection','confirmed') else case)
        result=run(command);self.assertEqual(result.returncode,1,result.stderr)
        self.assertEqual(s.sent,before);self.assertEqual((state/'intent.json').read_bytes(),original)
        log=state/'backlink-repair.log';events=[json.loads(line) for line in log.read_text().splitlines()]
        rejected=[event for event in events if event['event']=='directory_send_rejected']
        safe=case in ('400','403','429','twice','then-lost','bad-log','wrong-operation','orphan-rejection','confirmed')
        self.assertEqual(bool(rejected),safe)
        if case=='429':
            start=len(s.calls);s.mode='ok';result=run(command)
            self.assertEqual(result.returncode,1);self.assertIn(b'cooldown',result.stderr)
            self.assertNotIn('sendDocument',s.calls[start:])
            time.sleep(max(0,int(rejected[-1]['retry_not_before'])-time.time()+1.05))
        if case in ('bad-log','wrong-operation','orphan-rejection','confirmed'):
            if case=='bad-log': log.write_text(log.read_text()[:-8])
            elif case=='wrong-operation':
                events[-1]['message_id']=str(uuid.uuid4());log.write_text(''.join(json.dumps(event)+'\n' for event in events))
            elif case=='orphan-rejection': log.write_text(json.dumps(rejected[-1])+'\n')
            else:
                events.insert(-1,dict(event='directory_publication_confirmed',message_id='999'))
                log.write_text(''.join(json.dumps(event)+'\n' for event in events))
            safe=False
        if case=='twice':
            result=run(command);self.assertEqual(result.returncode,1)
            self.assertEqual(s.sent,before)
        s.mode='resume-lost' if case=='then-lost' else 'ok';start=len(s.calls)
        result=run(command);self.assertEqual(result.returncode,0 if safe and case!='then-lost' else 1,result.stderr)
        if safe:
            self.assertEqual(s.sent,before+['DIRECTORY'])
            if case!='then-lost': self.assertEqual(json.loads(result.stdout)['directory_resume'],'published')
            s.mode='ok';result=run(command);self.assertEqual(result.returncode,1)
            self.assertEqual(s.sent,before+['DIRECTORY'])
        else:
            self.assertNotIn('sendDocument',s.calls[start:]);self.assertEqual(s.sent,before)
        self.assertEqual((state/'intent.json').read_bytes(),original)
    def exercise(self,data=b'hello\n',mode='ok',readback=True,recover=False,recovery_mode='ok',finalize_case=None):
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
                    if recover:
                        s.mode='ok'
                        def scan_to(name):
                            out=root/name
                            result=run(['scan','--token-file',str(token),'--storage-chat-id',str(STORAGE),'--service-chat-id',str(SERVICE),'--output',str(out)])
                            self.assertEqual(result.returncode,0,result.stderr);return out
                        cap=scan_to('recovery-capture');before_intent=s.intent.read_bytes();before=list(s.sent)
                        if recovery_mode=='log':
                            log=state/'backlink-repair.log';log.write_text('{broken');log.chmod(0o600)
                        if recovery_mode=='phase':
                            value=json.loads(before_intent);value['phase']='directory_send_pending';s.intent.write_text(json.dumps(value))
                        if recovery_mode=='conflict':
                            saved=s.intent;s.intent=root/'other-state'/'intent.json'
                            other=args.copy();other[other.index('--state-dir')+1]=str(s.intent.parent)
                            result=run(other);self.assertEqual(result.returncode,0,result.stderr)
                            s.intent=saved;cap=scan_to('conflict-capture');before=list(s.sent)
                        command=['intent','--resume-directory','--state-dir',str(state),'--capture',str(cap),'--token-file',str(token)]
                        if recovery_mode.startswith('retry:'):
                            self.retry_add(run,command,s,state,recovery_mode[6:]);return
                        s.mode={'lost':'resume-lost','reject':'resume-apierror'}.get(recovery_mode,'ok')
                        result=run(command)
                        self.assertEqual(result.returncode,0 if recovery_mode=='ok' else 1,result.stderr)
                        s.mode='ok'
                        if recovery_mode in ('phase','conflict','reject','log'):
                            self.assertEqual(s.sent,before)
                            if recovery_mode=='reject': self.assertEqual(run(command).returncode,1)
                            return
                        self.assertEqual(s.sent,before+['DIRECTORY'])
                        if recovery_mode=='ok': self.assertTrue(json.loads(result.stdout)['resumed'])
                        self.assertEqual(s.intent.read_bytes(),before_intent)
                        count=len(s.sent);self.assertEqual(run(command).returncode,1)
                        self.assertEqual(len(s.sent),count) # Old capture + durable pending event must block a resend.
                        fresh=scan_to('recovered')
                        command[command.index('--capture')+1]=str(fresh)
                        result=run(command);self.assertEqual(result.returncode,0,result.stderr)
                        self.assertEqual(json.loads(result.stdout)['directory_resume'],'already_observed')
                        result=run(['read','--no-cache','--capture',str(fresh),'--token-file',str(token),'cat','/hello.txt'])
                        self.assertEqual(result.returncode,0,result.stderr);self.assertEqual(result.stdout,data)
                        self.assertEqual(len(s.sent),count)
                        if finalize_case:
                            finalize_command=command.copy();finalize_command[finalize_command.index('--resume-directory')]='--finalize'
                            original_intent=s.intent.read_bytes()
                            if finalize_case=='conflict':
                                cid=int(json.loads(original_intent)['file_commit_id']);bad=json.loads(s.messages[cid]['caption'])
                                bad['next_message']='99999';s.messages[cid]['caption']=json.dumps(bad)
                            if finalize_case in ('alias','alias-legacy','alias-corrupt','alias-unique'):
                                for message in s.messages.values():
                                    if 'document' in message: message['document']['file_unique_id']='unique_'+message['document']['file_id']
                            if finalize_case=='chunk':
                                part_id=int(json.loads(original_intent)['parts'][0]['message_id']);bad=json.loads(s.messages[part_id]['caption'])
                                bad['part_sha256']='0'*64;s.messages[part_id]['caption']=json.dumps(bad)
                            if finalize_case=='local':
                                temp=state/'intent.finalize.tmp';temp.write_text('preserve');temp.chmod(0o600)
                            if finalize_case=='alias-capture':
                                captured=json.loads((fresh/'journal.json').read_text())
                                for record in captured['records']: record.get('document',{}).pop('file_unique_id',None)
                                (fresh/'journal.json').write_text(json.dumps(captured))
                            s.mode={'alias-capture':'file-alias','lost':'repair-lost-edit','alias':'file-alias','alias-legacy':'file-alias-missing-unique',
                                'alias-corrupt':'file-alias-corrupt','alias-unique':'file-alias-unique-conflict'}.get(finalize_case,'ok')
                            calls=len(s.calls);result=run(finalize_command)
                            self.assertEqual(result.returncode,0 if finalize_case in ('ok','alias','alias-legacy','alias-capture') else 1,result.stderr)
                            s.mode='ok'
                            if finalize_case in ('conflict','local','chunk','alias-corrupt','alias-unique'):
                                if finalize_case=='alias-corrupt': self.assertIn(b'document bytes differ',result.stderr)
                                if finalize_case=='alias-unique': self.assertIn(b'document.file_unique_id',result.stderr)
                                self.assertEqual(s.intent.read_bytes(),original_intent)
                                if finalize_case in ('conflict','chunk','alias-corrupt','alias-unique'): self.assertNotIn('editMessageCaption',s.calls[calls:])
                                if finalize_case=='local': self.assertEqual(temp.read_text(),'preserve')
                                return
                            if finalize_case=='lost':
                                self.assertEqual(s.intent.read_bytes(),original_intent)
                                result=run(finalize_command);self.assertEqual(result.returncode,0,result.stderr)
                            completed=json.loads(s.intent.read_text());self.assertEqual(completed['phase'],'complete')
                            self.assertEqual(s.intent.stat().st_mode&0o777,0o600)
                            self.assertTrue(json.loads(result.stdout)['finalized'])
                            persisted=s.intent.read_bytes();calls=len(s.calls)
                            again=run(finalize_command);self.assertEqual(again.returncode,0,again.stderr)
                            self.assertFalse(json.loads(again.stdout)['intent_updated']);self.assertNotIn('editMessageCaption',s.calls[calls:])
                            self.assertEqual(s.intent.read_bytes(),persisted)
                        saved=s.intent;s.intent=root/'unlink-recovered'/'intent.json'
                        deleted=run(['unlink','--capture',str(fresh),'--path','/hello.txt','--state-dir',str(s.intent.parent),
                            '--token-file',str(token),'--send-interval-ms','0'])
                        self.assertEqual(deleted.returncode,0,deleted.stderr);s.intent=saved
                        removed=scan_to('removed-recovered');command[command.index('--capture')+1]=str(removed)
                        count=len(s.sent);result=run(command)
                        self.assertEqual(result.returncode,0,result.stderr)
                        self.assertEqual(json.loads(result.stdout)['directory_resume'],'already_observed')
                        self.assertEqual(len(s.sent),count) # Never resurrect an observed ADD after REMOVE.
                        if finalize_case:
                            calls=len(s.calls);result=run(finalize_command[:]) # old capture still valid; current messages must be verified
                            self.assertEqual(result.returncode,0,result.stderr);self.assertNotIn('sendDocument',s.calls[calls:])
                        return
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
    def directories(self,mode='ok',mount=False,replace=False,replace_mode='ok',unlink=False,unlink_mode='ok',diagnose=False,repair_case=None,finish_replace=None,metadata_finish=False,metadata_resume=None):
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
                if code is not None: self.assertEqual(result.returncode,code,result.stderr)
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
            def finish_metadata(state,cap,expected=0):
                original=(state/'intent.json').read_bytes();count=len(s.sent)
                command=['intent','--finalize','--state-dir',str(state),'--capture',str(cap),'--token-file',str(token)]
                current=json.loads(original)
                if metadata_finish=='conflict' and current['kind']=='mkdir':
                    mid=int(current['child_message_id']);old=s.messages[mid]['caption'];bad=json.loads(old)
                    bad['next_message']='999999';s.messages[mid]['caption']=json.dumps(bad)
                    run(command,code=1);s.messages[mid]['caption']=old
                    self.assertEqual((state/'intent.json').read_bytes(),original);return
                if metadata_finish=='lost' and current['kind']=='mkdir' and current['phase']!='complete' and not expected:
                    s.mode='repair-lost-edit';run(command,code=1);s.mode='ok'
                    self.assertEqual((state/'intent.json').read_bytes(),original)
                    calls=len(s.calls);run(command);self.assertNotIn('editMessageCaption',s.calls[calls:])
                result=run(command,code=expected);self.assertEqual(len(s.sent),count)
                if expected:
                    self.assertEqual((state/'intent.json').read_bytes(),original);return
                report=json.loads(result.stdout);self.assertTrue(report['finalized'])
                current=json.loads((state/'intent.json').read_text());self.assertEqual(current['phase'],'complete')
                self.assertEqual(report['finalize_kind'],current['kind']);self.assertNotIn('file_commit_id',current)
                persisted=(state/'intent.json').read_bytes();calls=len(s.calls)
                again=json.loads(run(command).stdout);self.assertFalse(again['intent_updated'])
                self.assertNotIn('editMessageCaption',s.calls[calls:]);self.assertEqual((state/'intent.json').read_bytes(),persisted)
            try:
                run(['intent','--help']);self.assertEqual(s.calls,[])
                run(['mkdir','--help']);self.assertEqual(s.calls,[])
                mkdir_path='/documents'
                if metadata_resume=='nested':
                    s.mode='ok';write('mkdir',capture,'--path','/container');capture=scan()
                    s.mode=mode;s.mkdir_pin_after=len(s.sent);mkdir_path='/container/documents/'
                result,directory=write('mkdir',capture,'--path',mkdir_path,code=0 if mode=='ok' else 1)
                if mode!='ok':
                    self.assertIsNotNone(directory)
                    expected={'mkdir-lost-child':'child_response_uncertain','mkdir-response':'child_response_uncertain',
                        'mkdir-lost-parent':'directory_response_uncertain','mkdir-pin':'child_sent',
                        'mkdir-edit':'published_navigation_pending'}[mode]
                    self.assertEqual(directory['phase'],expected)
                    self.assertEqual(len(s.sent),(2 if metadata_resume=='nested' else 0)+(1 if mode in ('mkdir-lost-child','mkdir-response','mkdir-pin') else 2))
                    before=len(s.sent);saved=s.intent.parent
                    write('mkdir',capture,'--path','/documents',state=saved,code=1)
                    self.assertEqual(len(s.sent),before)
                    if metadata_resume:
                        s.mode='ok';cap=scan();s.intent=saved/'intent.json'
                        original=s.intent.read_bytes();count=len(s.sent)
                        command=['intent','--resume-directory','--state-dir',str(saved),'--capture',str(cap),'--token-file',str(token)]
                        if metadata_resume.startswith('retry:'):
                            self.retry_add(lambda args:run(args,code=None),command,s,saved,metadata_resume[6:]);return
                        if metadata_resume=='pending':
                            mid=next(mid for mid,msg in s.messages.items() if json.loads(msg.get('caption','{}')).get('object_id')==ROOT)
                            del s.messages[mid]
                            cap=scan();command[command.index('--capture')+1]=str(cap)
                        if metadata_resume in ('missing','duplicate'):
                            mid=int(directory['child_message_id'])
                            if metadata_resume=='missing': del s.messages[mid]
                            else:
                                dup=copy.deepcopy(s.messages[mid]);dup['message_id']=s.next
                                s.messages[s.next]=dup;s.next+=1
                            cap=scan();command[command.index('--capture')+1]=str(cap)
                        if metadata_resume=='conflict':
                            saved_intent=s.intent
                            write('mkdir',cap,'--path','/documents');s.intent=saved_intent
                            cap=scan();command[command.index('--capture')+1]=str(cap);count=len(s.sent)
                        if metadata_resume=='known-id':
                            changed=json.loads(original);changed['child_message_id']='99999'
                            s.intent.write_text(json.dumps(changed));original=s.intent.read_bytes()
                        if metadata_resume in ('child-pending','directory-ref','wrong-path','phase'):
                            changed=json.loads(original)
                            if metadata_resume=='child-pending':
                                changed['phase']='child_send_pending';changed.pop('child_message_id',None)
                            elif metadata_resume=='directory-ref': changed['directory_message_id']='99999'
                            elif metadata_resume=='wrong-path': changed['path']='/wrong-name'
                            else: changed['phase']='complete'
                            s.intent.write_text(json.dumps(changed));original=s.intent.read_bytes()
                        if metadata_resume=='invalid-child':
                            changed=json.loads(original);changed['child_caption']['parent_id']=str(uuid.uuid4())
                            s.intent.write_text(json.dumps(changed));original=s.intent.read_bytes()
                        if metadata_resume=='log':
                            log=saved/'backlink-repair.log';log.write_text('{bad');log.chmod(0o600)
                        if metadata_resume=='navigation':
                            mid=int(directory['child_message_id']);bad=json.loads(s.messages[mid]['caption'])
                            bad['next_message']='99999';s.messages[mid]['caption']=json.dumps(bad)
                        s.mode={'lost':'resume-lost','reject':'resume-apierror','origin':'repair-origin',
                                'cleanup':'repair-cleanup','pin':'pin'}.get(metadata_resume,'ok')
                        blocked=metadata_resume in ('directory-ref','wrong-path','phase','pending','missing','duplicate','conflict','known-id','invalid-child','log','navigation','origin','cleanup','pin','reject')
                        result=run(command,code=1 if blocked or metadata_resume=='lost' else 0)
                        self.assertEqual(s.intent.read_bytes(),original)
                        if blocked:
                            self.assertEqual(len(s.sent),count)
                            if metadata_resume=='reject': run(command,code=1)
                            return
                        already=mode in ('mkdir-lost-parent','mkdir-edit')
                        self.assertEqual(len(s.sent),count if already else count+1)
                        if metadata_resume!='lost':
                            report=json.loads(result.stdout)
                            self.assertEqual(report['directory_resume'],'already_observed' if already else 'published')
                            self.assertEqual(report['resumed'],not already)
                        s.mode='ok'
                        if not already: run(command,code=1) # pending log + stale capture
                        final=scan();command[command.index('--capture')+1]=str(final)
                        self.assertEqual(json.loads(run(command).stdout)['directory_resume'],'already_observed')
                        self.assertEqual(len(s.sent),count if already else count+1)
                        self.assertEqual(read(final,'ls',mkdir_path.rstrip('/')),b'')
                        finish_metadata(saved,final)
                        return
                    if metadata_finish:
                        s.mode='ok';cap=scan()
                        finish_metadata(saved,cap,0 if mode in ('mkdir-lost-parent','mkdir-edit') else 1)
                        return
                    if mode in ('mkdir-lost-parent','mkdir-edit'):
                        s.mode='ok';fresh=scan();self.assertEqual(read(fresh,'ls','/'),b'documents\n')
                        self.assertEqual(read(fresh,'ls','/documents'),b'')
                    return
                mkdir_intent=s.intent
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
                if metadata_resume:
                    s.intent=mkdir_intent
                    original=s.intent.read_bytes();count=len(s.sent)
                    command=['intent','--resume-directory','--state-dir',str(s.intent.parent),'--capture',str(cap),'--token-file',str(token)]
                    report=json.loads(run(command).stdout)
                    self.assertEqual(report['directory_resume'],'already_observed');self.assertFalse(report['resumed'])
                    self.assertEqual(s.intent.read_bytes(),original);self.assertEqual(len(s.sent),count)
                    return
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
                        if finish_replace:
                            before_state=s.intent.read_bytes();before_sent=list(s.sent)
                            if finish_replace=='invalid':
                                malformed=json.loads(before_state);malformed['add_operation_id']=str(uuid.uuid4())
                                s.intent.write_text(json.dumps(malformed));before_state=s.intent.read_bytes()
                            command=['intent','--finalize','--state-dir',str(state),'--capture',str(final),'--token-file',str(token)]
                            if finish_replace=='conflict':
                                cid=int(updated['file_commit_id']);changed=json.loads(s.messages[cid]['caption'])
                                changed['next_message']='99999';s.messages[cid]['caption']=json.dumps(changed)
                            expected=0 if replace_mode in ('ok','replace-edit','replace-lost-commit') and finish_replace not in ('conflict','invalid') else 1
                            result=run(command,code=expected)
                            self.assertEqual(s.sent,before_sent)
                            if expected:
                                self.assertEqual(s.intent.read_bytes(),before_state)
                                if finish_replace in ('conflict','invalid'): return
                            else:
                                completed=json.loads(s.intent.read_text());evidence=json.loads(result.stdout)
                                self.assertEqual(completed['phase'],'complete');self.assertNotIn('directory_message_id',completed)
                                self.assertEqual(evidence['directory_ids'],[]);self.assertEqual(evidence['finalize_kind'],'replace')
                                self.assertFalse(evidence['commit_navigation_repaired']);self.assertTrue(evidence['finalized'])
                                persisted=s.intent.read_bytes();calls=len(s.calls)
                                evidence=json.loads(run(command).stdout)
                                self.assertFalse(evidence['intent_updated']);self.assertNotIn('editMessageCaption',s.calls[calls:])
                                self.assertEqual(s.intent.read_bytes(),persisted)
                                self.assertEqual(completed['file_commit_id'],evidence['commit_ids'][0])
                        if repair_case:
                            before_intent=(state/'intent.json').read_bytes();before_sent=len(s.sent)
                            command=['intent','--repair-backlinks','--state-dir',str(state),'--capture',str(final),'--token-file',str(token)]
                            first_id=int(updated['parts'][0]['message_id']);original_caption=s.messages[first_id]['caption']
                            if repair_case=='changed':
                                changed=json.loads(original_caption);changed['part_sha256']='0'*64;s.messages[first_id]['caption']=json.dumps(changed)
                            s.mode={'lost':'repair-lost-edit','origin':'repair-origin','cleanup':'repair-cleanup','pin':'pin'}.get(repair_case,'ok')
                            calls=len(s.calls)
                            result=run(command,code=0 if repair_case=='ok' else 1)
                            if repair_case not in ('ok','lost'): self.assertNotIn('editMessageCaption',s.calls[calls:])
                            s.mode='ok'
                            if repair_case=='changed': s.messages[first_id]['caption']=original_caption
                            if repair_case!='ok': result=run(command)
                            fixed=json.loads(result.stdout);self.assertTrue(fixed['repair_applied'])
                            self.assertEqual(len(s.sent),before_sent);self.assertEqual((state/'intent.json').read_bytes(),before_intent)
                            calls=len(s.calls);again=json.loads(run(command).stdout)
                            self.assertFalse(again['repaired_chunk_ids']);self.assertNotIn('editMessageCaption',s.calls[calls:])
                            self.assertEqual(len(again['repair_skipped_ids']),len(updated['parts']))
                            log=(state/'backlink-repair.log').read_text();self.assertIn('edit_pending',log);self.assertIn('service_copy_known',log)
                            import fcntl
                            with (state/'backlink-repair.log').open('a') as held:
                                fcntl.flock(held,fcntl.LOCK_EX|fcntl.LOCK_NB);run(command,code=1)
                            checked=scan();self.assertEqual(check_intent(state,checked,'confirmed')['backlink_repairs'],[])
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
                    if metadata_finish: finish_metadata(saved,final,1 if unlink_mode in ('unlink-pin','unlink-apierror') else 0)
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
                    start=len(s.calls)
                    run(['intent','--repair-backlinks','--state-dir',str(state_file.parent),'--capture',str(duplicated),'--token-file',str(token)],code=1)
                    self.assertNotIn('editMessageCaption',s.calls[start:])
                    noop=json.loads(run(['intent','--repair-backlinks','--state-dir',str(state_file.parent),'--capture',str(final),'--token-file',str(token)]).stdout)
                    self.assertEqual(noop['repaired_chunk_ids'],[])

                    data_path=state_file.parent/'data.bin';original_data=data_path.read_bytes();data_path.write_bytes(b'bad')
                    result=run(['intent','--state-dir',str(state_file.parent),'--capture',str(final),'--token-file',str(token)],code=1)
                    self.assertIn(b'staging',result.stderr);data_path.write_bytes(original_data)
                    fid='file_'+file['parts'][0]['message_id'];original_chunk=s.files[fid];s.files[fid]=b'X'*len(original_chunk)
                    result=run(['intent','--state-dir',str(state_file.parent),'--capture',str(final),'--token-file',str(token)],code=1)
                    self.assertIn(b'CHUNK',result.stderr);s.files[fid]=original_chunk

                if metadata_finish and not unlink:
                    for candidate in root.glob('state-*/intent.json'):
                        if json.loads(candidate.read_text()).get('kind')=='mkdir': finish_metadata(candidate.parent,final)
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
    def upload_recovery(self,case='ok',replace=False,retry=None):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);capture=root/'capture';capture.mkdir()
            journal=dict(schema='TGFS_JOURNAL_CAPTURE_V1',scan_completed=True,gap_policy='forward_400_is_gap',
                storage_chat_id=str(STORAGE),service_chat_id=str(SERVICE),superblock=SB,boundary_id='2',records=[])
            (capture/'journal.json').write_text(json.dumps(journal))
            data=b'abcdefgh';source=root/'source';source.write_bytes(data)
            token=root/'token';token.write_text(TOKEN);token.chmod(0o600)
            state=root/'state'
            s=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
            s.intent=state/'intent.json';s.input_data=data;s.mode='lost';s.calls=[];s.sent=[];s.files={};s.next=3;s.copy_id=1001
            s.messages={1:dict(message_id=1,chat=dict(id=STORAGE,type='channel'),text=json.dumps(SB))}
            t=threading.Thread(target=s.serve_forever,kwargs=dict(poll_interval=.01),daemon=True);t.start()
            env=dict(os.environ,TGFS_TEST_API_BASE=f'http://127.0.0.1:{s.server_port}')
            def run(args):
                process=subprocess.Popen([BINARY,*args],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
                s.client_pid=process.pid
                try: stdout,stderr=process.communicate(timeout=30)
                except subprocess.TimeoutExpired:
                    process.kill();process.communicate();raise
                result=subprocess.CompletedProcess(process.args,process.returncode,stdout,stderr)
                for text in (TOKEN,'AddressSanitizer','runtime error:'): self.assertNotIn(text,result.stderr.decode())
                return result
            def scan(name):
                cap=root/name
                result=run(['scan','--token-file',str(token),'--storage-chat-id',str(STORAGE),'--service-chat-id',str(SERVICE),'--output',str(cap)])
                self.assertEqual(result.returncode,0,result.stderr);return cap
            args=['put','--capture',str(capture),'--file',str(source),'--name','hello.txt','--state-dir',str(state),
                '--token-file',str(token),'--chunk-bytes','3','--send-interval-ms','0']
            try:
                if replace:
                    s.mode='ok';self.assertEqual(run(args).returncode,0)
                    capture=scan('base');state=root/'replacement';s.intent=state/'intent.json'
                    args[args.index('--capture')+1]=str(capture);args[args.index('--state-dir')+1]=str(state);args+=['--replace']
                    s.mode='lost'
                result=run(args);self.assertEqual(result.returncode,1,result.stderr)
                saved=json.loads(s.intent.read_text());original_ids={k:saved[k] for k in ('object_id','version_id','operation_id')}
                first=s.next-1
                if case in ('prepared','empty'):
                    # Model a crash before the first request, using an explicitly constructed saved state.
                    del s.messages[first];s.sent.pop();saved['phase']='prepared';saved['parts']=[]
                    saved.pop('last_attempted_part',None)
                    if case=='empty':
                        import hashlib
                        data=b'';s.input_data=data;(state/'data.bin').write_bytes(data)
                        saved['file_size']='0';saved['file_sha256']=hashlib.sha256(data).hexdigest()
                        saved['commit_caption'].update(file_size='0',file_sha256=saved['file_sha256'],part_count=0)
                    s.intent.write_text(json.dumps(saved))
                if case=='legacy':
                    saved.pop('last_attempted_part');s.intent.write_text(json.dumps(saved))
                if case=='pending':
                    saved['phase']='chunk_send_pending';saved['pending_part']=0;s.intent.write_text(json.dumps(saved))
                if case in ('missing','commit-uncertain'): del s.messages[first]
                if case=='commit-uncertain':
                    saved['phase']='commit_send_pending';s.intent.write_text(json.dumps(saved))
                if case=='duplicate':
                    msg=copy.deepcopy(s.messages[first]);msg['message_id']=s.next;s.messages[s.next]=msg;s.next+=1
                s.mode='ok';cap=scan('before-recovery')
                if case=='staging': (state/'data.bin').write_bytes(b'XXXXXXXX')
                if case=='log':
                    log=state/'backlink-repair.log';log.write_text('{broken');log.chmod(0o600)
                if case=='legacy-log':
                    log=state/'backlink-repair.log'
                    log.write_text(json.dumps(dict(event='upload_chunk_pending',message_id='0'))+'\n');log.chmod(0o600)
                before=list(s.sent);before_intent=s.intent.read_bytes()
                s.mode={'lost-chunk':'upload-lost-chunk','lost-commit':'upload-lost-commit','response':'response',
                    'origin':'repair-origin','cleanup':'repair-cleanup','pin':'pin','reject':'resume-apierror'}.get(case,'ok')
                cmd=['intent','--resume-upload','--state-dir',str(state),'--capture',str(cap),'--token-file',str(token)]
                if retry:
                    phase,reason=retry.split(':',1)
                    self.retry_upload(run,scan,cmd,s,state,phase,reason,data,replace)
                    return
                result=run(cmd)
                blocked=case in ('missing','commit-uncertain','duplicate','staging','log','origin','cleanup','pin')
                uncertain=case in ('lost-chunk','lost-commit','response','reject')
                self.assertEqual(result.returncode,1 if blocked or uncertain else 0,result.stderr)
                if blocked:
                    self.assertEqual(s.sent,before);self.assertEqual(s.intent.read_bytes(),before_intent);return
                s.mode='ok'
                if uncertain:
                    before=list(s.sent);self.assertEqual(run(cmd).returncode,1)
                    self.assertEqual(s.sent,before) # Same capture + persistent barrier cannot repeat a request.
                    cap=scan('after-uncertain');cmd[cmd.index('--capture')+1]=str(cap)
                    if case=='reject':
                        self.assertEqual(run(cmd).returncode,1);self.assertEqual(s.sent,before);return
                    result=run(cmd)
                    self.assertEqual(result.returncode,0,result.stderr)
                else:
                    report=json.loads(result.stdout);self.assertEqual(report['upload_resume'],'committed')
                    self.assertEqual(report['next_step'],'capture_after_upload_resume')
                now=json.loads(s.intent.read_text())
                for key,value in original_ids.items(): self.assertEqual(now[key],value)
                before=list(s.sent)
                self.assertEqual(run(cmd).returncode,1 if case!='lost-commit' else 0)
                self.assertEqual(s.sent,before)
                cap=scan('committed');cmd[cmd.index('--capture')+1]=str(cap)
                result=run(cmd);self.assertEqual(result.returncode,0,result.stderr)
                self.assertEqual(json.loads(result.stdout)['upload_resume'],'already_committed')
                self.assertEqual(s.sent,before)
                if not replace:
                    cmd[1]='--resume-directory';result=run(cmd);self.assertEqual(result.returncode,0,result.stderr)
                    cap=scan('published');cmd[cmd.index('--capture')+1]=str(cap)
                cmd[1]='--finalize';result=run(cmd);self.assertEqual(result.returncode,0,result.stderr)
                self.assertTrue(json.loads(result.stdout)['finalized'])
                self.assertEqual(json.loads(s.intent.read_text())['phase'],'complete')
                result=run(['read','--no-cache','--capture',str(cap),'--token-file',str(token),'cat','/hello.txt'])
                self.assertEqual(result.returncode,0,result.stderr);self.assertEqual(result.stdout,data)
            finally:
                s.shutdown();s.server_close();t.join()
    def retry_upload(self,run,scan,cmd,s,state,phase,case,data,replace):
        s.reject_kind='CHUNK' if phase=='chunk' else 'FILE_COMMIT'
        s.mode='reject-'+('400' if case in ('twice','then-lost','staging','manifest','wrong-op','orphan','bad-log','old-pending','missing-known','pin','origin','cleanup') else case)
        result=run(cmd)
        if case=='log-write':
            self.assertNotEqual(result.returncode,0,result.stderr);before=list(s.sent)
            s.mode='ok';cap=scan('after-log-failure');cmd[cmd.index('--capture')+1]=str(cap)
            calls=len(s.calls);result=run(cmd);self.assertEqual(result.returncode,1,result.stderr)
            self.assertNotIn('sendDocument',s.calls[calls:]);self.assertEqual(s.sent,before);return
        self.assertEqual(result.returncode,1,result.stderr)
        log=state/'backlink-repair.log';events=[json.loads(line) for line in log.read_text().splitlines()]
        rejected=[event for event in events if event['event']=='upload_'+phase+'_rejected']
        safe=case in ('400','403','429','twice','then-lost','staging','manifest','wrong-op','orphan','bad-log','old-pending','missing-known','pin','origin','cleanup')
        self.assertEqual(bool(rejected),safe);before=list(s.sent)
        saved=json.loads(s.intent.read_text());self.assertEqual(saved['phase'],phase+'_send_pending')
        ids={key:saved[key] for key in ('object_id','version_id','operation_id')}
        s.mode='ok';cap=scan('retry-capture');cmd[cmd.index('--capture')+1]=str(cap)
        if case=='429':
            calls=len(s.calls);result=run(cmd);self.assertEqual(result.returncode,1,result.stderr)
            self.assertIn(b'cooldown',result.stderr);self.assertNotIn('sendDocument',s.calls[calls:])
            time.sleep(max(0,int(rejected[-1]['retry_not_before'])-time.time()+1.05))
        if case in ('staging','manifest','wrong-op','orphan','bad-log','old-pending','missing-known','pin','origin','cleanup'):
            if case=='staging': (state/'data.bin').write_bytes(b'X'*len(data))
            elif case=='manifest':
                saved['commit_document']['unexpected']=True;s.intent.write_text(json.dumps(saved))
            elif case=='wrong-op':
                events[-1]['operation_id']=str(uuid.uuid4());log.write_text(''.join(json.dumps(event)+'\n' for event in events))
            elif case=='orphan':
                log.write_text(''.join(json.dumps(event)+'\n' for event in events if event['event']!='upload_'+phase+'_pending'))
            elif case=='old-pending':
                for event in events:
                    if event['event']=='upload_'+phase+'_pending': event.pop('operation_id',None)
                log.write_text(''.join(json.dumps(event)+'\n' for event in events))
            elif case=='missing-known':
                del s.messages[int(saved['parts'][0]['message_id'])]
                cap=scan('missing-known');cmd[cmd.index('--capture')+1]=str(cap)
            elif case in ('pin','origin','cleanup'): s.mode={'pin':'pin','origin':'repair-origin','cleanup':'repair-cleanup'}[case]
            else: log.write_text(log.read_text()[:-8])
            safe=False
        if case=='twice':
            s.mode='reject-400';result=run(cmd);self.assertEqual(result.returncode,1,result.stderr)
            self.assertEqual(s.sent,before);s.mode='ok'
        if case=='then-lost': s.mode='upload-lost-'+phase
        calls=len(s.calls);result=run(cmd)
        self.assertEqual(result.returncode,0 if safe and case!='then-lost' else 1,result.stderr)
        if not safe:
            self.assertNotIn('sendDocument',s.calls[calls:]);self.assertEqual(s.sent,before);return
        self.assertEqual(s.sent.count('FILE_COMMIT'),before.count('FILE_COMMIT')+1 if case!='then-lost' or phase=='commit' else before.count('FILE_COMMIT'))
        if phase=='commit': self.assertEqual(s.sent.count('CHUNK'),before.count('CHUNK'))
        if case=='then-lost':
            s.mode='ok';after=list(s.sent);result=run(cmd)
            self.assertEqual(result.returncode,1,result.stderr);self.assertEqual(s.sent,after)
            cap=scan('retry-lost');cmd[cmd.index('--capture')+1]=str(cap)
            result=run(cmd);self.assertEqual(result.returncode,0,result.stderr)
        current=[json.loads(message['caption']) for message in s.messages.values() if 'caption' in message and json.loads(message['caption']).get('version_id')==ids['version_id']]
        self.assertEqual(sum(record['type']=='CHUNK' for record in current),(len(data)+2)//3)
        self.assertEqual(sum(record['type']=='FILE_COMMIT' for record in current),1)
        self.assertEqual({key:json.loads(s.intent.read_text())[key] for key in ids},ids)
        cap=scan('retry-committed');cmd[cmd.index('--capture')+1]=str(cap)
        result=run(cmd);self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(json.loads(result.stdout)['upload_resume'],'already_committed')
        if not replace:
            cmd[1]='--resume-directory';result=run(cmd);self.assertEqual(result.returncode,0,result.stderr)
            cap=scan('retry-published');cmd[cmd.index('--capture')+1]=str(cap)
        cmd[1]='--finalize';result=run(cmd);self.assertEqual(result.returncode,0,result.stderr)
        self.assertTrue(json.loads(result.stdout)['finalized'])
        result=run(['read','--no-cache','--capture',str(cap),'--token-file',cmd[-1],'cat','/hello.txt'])
        self.assertEqual(result.returncode,0,result.stderr);self.assertEqual(result.stdout,data)
    def test_retry_upload_rejection_write_failure(self):
        for phase in ('chunk','commit'):
            with self.subTest(phase=phase): self.upload_recovery(retry=phase+':log-write')
    def test_retry_rejected_chunk(self):
        for case in ('400','403','429','twice','then-lost','staging','wrong-op','orphan','bad-log','old-pending','missing-known','pin','origin','cleanup','500','http500','mismatch','nodescription','result','badretry','no-retry'):
            with self.subTest(case=case): self.upload_recovery(retry='chunk:'+case)
        self.upload_recovery(replace=True,retry='chunk:400')
    def test_retry_rejected_commit(self):
        for case in ('400','403','429','twice','then-lost','staging','manifest','wrong-op','orphan','bad-log','old-pending','missing-known','pin','origin','cleanup','500','http500','mismatch','nodescription','result','badretry','no-retry'):
            with self.subTest(case=case): self.upload_recovery(retry='commit:'+case)
        self.upload_recovery(case='empty',retry='commit:400')
        self.upload_recovery(replace=True,retry='commit:400')
    def test_resume_upload(self):
        for case in ('ok','prepared','empty','legacy','legacy-log','pending','lost-chunk','lost-commit','response'):
            with self.subTest(case=case): self.upload_recovery(case)
    def test_resume_upload_barriers(self):
        for case in ('missing','commit-uncertain','duplicate','staging','log','origin','cleanup','pin','reject'):
            with self.subTest(case=case): self.upload_recovery(case)
    def test_resume_replacement_upload(self): self.upload_recovery(replace=True)
    def test_retry_rejected_put_add(self):
        for case in ('400','403','429','twice','then-lost','bad-log','wrong-operation','orphan-rejection','confirmed','500','http500','mismatch','nodescription','result','badretry','no-retry'):
            with self.subTest(case=case): self.exercise(mode='edit',recover=True,recovery_mode='retry:'+case)
    def test_retry_rejected_mkdir_add(self):
        for case in ('400','429','then-lost','500'):
            with self.subTest(case=case): self.directories(mode='mkdir-pin',metadata_resume='retry:'+case)
    def test_resume_mkdir_directory(self):
        self.directories(metadata_resume='ok')
        for mode in ('mkdir-lost-child','mkdir-response','mkdir-pin','mkdir-lost-parent','mkdir-edit'):
            with self.subTest(mode=mode): self.directories(mode=mode,metadata_resume='ok')
        self.directories(mode='mkdir-lost-parent',metadata_resume='pending')
        self.directories(mode='mkdir-pin',metadata_resume='nested')
        for case in ('child-pending','directory-ref','wrong-path','phase','lost','reject','missing','duplicate','conflict','known-id','invalid-child','log','navigation','origin','cleanup','pin'):
            with self.subTest(case=case): self.directories(mode='mkdir-pin',metadata_resume=case)
    def test_finalize_mkdir(self):
        self.directories(metadata_finish=True)
        self.directories(metadata_finish='conflict')
        self.directories(mode='mkdir-edit',metadata_finish='lost')
        for mode in ('mkdir-lost-child','mkdir-lost-parent','mkdir-edit','mkdir-pin'):
            with self.subTest(mode=mode): self.directories(mode=mode,metadata_finish=True)
    def test_finalize_unlink(self):
        for mode in ('ok','unlink-lost','unlink-response','unlink-apierror','unlink-pin'):
            with self.subTest(mode=mode): self.directories(unlink=True,unlink_mode=mode,metadata_finish=True)
    def test_finalize_replace(self):
        for mode in ('ok','replace-edit','replace-lost-commit','replace-lost-chunk','replace-pin'):
            with self.subTest(mode=mode): self.directories(replace=True,replace_mode=mode,diagnose=True,finish_replace='ok')
        self.directories(replace=True,diagnose=True,finish_replace='conflict')
        self.directories(replace=True,diagnose=True,finish_replace='invalid')
    def test_finalize_put(self):
        for mode in ('ok','lost','conflict','local','chunk','alias','alias-legacy','alias-capture','alias-corrupt','alias-unique'):
            with self.subTest(mode=mode): self.exercise(mode='edit',recover=True,finalize_case=mode)
    def test_finalize_empty(self): self.exercise(b'',mode='create-pin-after-commit',recover=True,finalize_case='ok')
    def test_resume_directory(self):
        for case in ('ok','lost','reject','phase','conflict','log'):
            with self.subTest(case=case): self.exercise(mode='edit',recover=True,recovery_mode=case)
    def test_resume_empty_directory_publication(self): self.exercise(b'',mode='create-pin-after-commit',recover=True)
    def test_backlink_repair(self):
        for mode in ('ok','lost','changed','origin','cleanup','pin'):
            with self.subTest(mode=mode): self.directories(replace=True,replace_mode='replace-edit',diagnose=True,repair_case=mode)
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

"""Captured journal -> real curl -> metadata merge -> lazy verified cat."""
import copy
import hashlib
import http.server
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest
import uuid

BINARY=str(Path(sys.argv.pop(1)).resolve())
EXAMPLE=Path(__file__).resolve().parent.parent/'examples/directory-merge'
TOKEN='123456:REMOTE_TEST_TOKEN'
STORAGE=-1003916921678
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self,*args): pass
    def reply(self,value,status=200):
        if not isinstance(value,bytes): value=json.dumps(dict(ok=True,result=value)).encode()
        self.send_response(status);self.send_header('Content-Length',str(len(value)));self.end_headers();self.wfile.write(value)
    def do_POST(self):
        method=self.path.rsplit('/',1)[-1]
        p=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        self.server.calls.append((method,p))
        if method=='getChat':
            return self.reply(dict(id=STORAGE+1 if self.server.mode=='channel' else STORAGE,type='channel'))
        assert method=='getFile'
        fid=p['file_id']
        if self.server.mode=='api' and fid=='file_101':
            return self.reply(json.dumps(dict(ok=False,error_code=429,description=TOKEN,parameters=dict(retry_after=9))).encode(),429)
        payload=self.server.payloads[fid]
        return self.reply(dict(file_id='wrong' if self.server.mode=='fileid' else fid,file_size=len(payload)+(1 if self.server.mode=='size' else 0),
            file_path='../secret' if self.server.mode=='path' else 'documents/'+fid))
    def do_GET(self):
        fid=self.path.rsplit('/',1)[-1]
        self.server.downloads.append(fid)
        data=self.server.payloads[fid]
        if ((self.server.mode=='corrupt' and fid=='file_101') or (self.server.mode=='latecorrupt' and fid=='file_102')
            or (self.server.mode=='gc-corrupt' and fid=='file_113')):
            data=b'X'+data[1:]
        self.reply(data)
class Remote(unittest.TestCase):
    def run_read(self,command='ls',path='/documents',mode='ok'):
        snap=json.loads((EXAMPLE/'snapshot.json').read_text())
        records=[];payloads={}
        for m in sorted(snap['messages'],key=lambda r:int(r['message_id'])):
            fid='file_'+m['message_id'];data=(EXAMPLE/m['document']).read_bytes()
            payloads[fid]=data
            records.append(dict(message_id=m['message_id'],record=m['caption'],
                document=dict(file_id=fid,file_size=len(data),file_name=m.get('filename','metadata.json'))))
        journal=dict(schema='TGFS_JOURNAL_CAPTURE_V1',superblock=snap['superblock'],boundary_id='200',
            storage_chat_id=str(STORAGE),scan_completed=True,gap_policy='forward_400_is_gap',
            coverage_verified=False,unavailable_ids=['2'],records=records)
        if mode in ('multipart','latecorrupt'):
            chunk=next(r for r in records if r['message_id']=='101')
            extra=copy.deepcopy(chunk);extra['message_id']='102';extra['document']['file_id']='file_102'
            parts=[]
            for r,offset,data in ((chunk,0,b'hel'),(extra,3,b'lo\n')):
                fid=r['document']['file_id'];payloads[fid]=data;r['document']['file_size']=len(data)
                r['document']['file_name']='hello.txt.'+str(offset//3)
                digest=hashlib.sha256(data).hexdigest()
                r['record'].update(part_count=2,offset=str(offset),part_size='3',part_sha256=digest)
                parts.append(dict(message_id=r['message_id'],offset=str(offset),part_size='3',part_sha256=digest))
            commit=next(r for r in records if r['message_id']=='110')
            commit['record']['part_count']=2
            payloads['file_110']=json.dumps(dict(parts=parts)).encode()
            commit['document']['file_size']=len(payloads['file_110'])
            records.append(extra);records.sort(key=lambda r:int(r['message_id']))
        if mode=='remove':
            observed=json.loads(payloads['file_20'])['operations'][0]['operation_id']
            r=copy.deepcopy(next(r for r in records if r['message_id']=='20'))
            r['message_id']='40';r['document']['file_id']='file_40'
            payloads['file_40']=json.dumps(dict(operations=[dict(operation_id=str(uuid.uuid4()),action='REMOVE',
                name='notes.txt',observed_adds=[observed])])).encode()
            r['document']['file_size']=len(payloads['file_40'])
            records.append(r);records.sort(key=lambda r:int(r['message_id']))
        if mode.startswith('gc-'):
            original_commit=next(r for r in records if r['message_id']=='110')
            original_chunk=next(r for r in records if r['message_id']=='101')
            if mode=='gc-bad-full-hash':
                original_commit['record']['file_sha256']='0'*64
                original_chunk['record']['file_sha256']='0'*64
            retry=copy.deepcopy(original_commit)
            retry['message_id']='111';retry['document']['file_id']='file_111'
            payloads['file_111']=payloads['file_110']
            records.append(retry)
            if mode!='gc-retry-only':
                other=copy.deepcopy(original_commit)
                other['message_id']='140';other['document']['file_id']='file_140'
                other['record'].update(object_id=str(uuid.uuid4()),operation_id=str(uuid.uuid4()),
                    version_id=str(uuid.uuid4()),part_count=2)
                parts=[]
                for index,data in enumerate((b'hel',b'lo\n')):
                    mid=str(112+index);fid='file_'+mid
                    chunk=copy.deepcopy(original_chunk)
                    chunk['message_id']=mid
                    part_hash=hashlib.sha256(data).hexdigest()
                    chunk['record'].update(object_id=other['record']['object_id'],version_id=other['record']['version_id'],
                        offset=str(index*3),part_size='3',part_sha256=part_hash,part_count=2,file_commit_id='140')
                    chunk['document'].update(file_id=fid,file_size=len(data),file_name='renamed.bin.'+str(index))
                    parts.append(dict(message_id=mid,offset=str(index*3),part_size='3',part_sha256=part_hash))
                    if mode!='gc-missing' or index==0:
                        records.append(chunk)
                        payloads[fid]=data
                payloads['file_140']=json.dumps(dict(parts=parts)).encode()
                other['document']['file_size']=len(payloads['file_140'])
                records.append(other)
            records.sort(key=lambda r:int(r['message_id']))
        if mode=='badjson': payloads['file_10']=b'NOT JSON';records[0]['document']['file_size']=8
        if mode=='duplicate': records.append(copy.deepcopy(records[-1]))
        if mode=='incomplete': journal['scan_completed']=False
        if mode=='empty': journal['records']=[]
        if mode=='missingpart': journal['records']=[r for r in records if r['message_id']!='101']
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);(root/'journal.json').write_text(json.dumps(journal))
            token=root/'token';token.write_text(TOKEN);token.chmod(0o600)
            server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
            server.calls=[];server.downloads=[];server.payloads=payloads;server.mode=mode
            t=threading.Thread(target=server.serve_forever,kwargs=dict(poll_interval=.01),daemon=True);t.start()
            try:
                common=['--capture',str(root),'--token-file',str(token)]
                args=['gc-plan',*common] if command=='gc-plan' else ['read','--no-cache',*common,command,path]
                p=subprocess.run([BINARY,*args],
                    env=dict(os.environ,TGFS_TEST_API_BASE=f'http://127.0.0.1:{server.server_port}'),
                    capture_output=True,timeout=10)
                err=p.stderr.decode()
                self.assertNotIn(TOKEN,err);self.assertNotIn('AddressSanitizer',err);self.assertNotIn('runtime error:',err)
                self.assertTrue(all(m in ('getChat','getFile') for m,_ in server.calls))
                return p.returncode,p.stdout,err,server.downloads
            finally:
                server.shutdown();server.server_close();t.join()
    def test_ls_merges_without_chunks(self):
        rc,out,err,files=self.run_read()
        self.assertEqual(rc,0,err);self.assertEqual(out,b'hello.txt\nnotes.txt\nphoto.jpg\n')
        self.assertNotIn('file_101',files)
    def test_cat_downloads_selected_chunk(self):
        rc,out,err,files=self.run_read('cat','/documents/hello.txt')
        self.assertEqual(rc,0,err);self.assertEqual(out,b'hello\n');self.assertEqual(files.count('file_101'),1)
    def test_empty_file_no_chunk_download(self):
        rc,out,err,files=self.run_read('cat','/documents/notes.txt')
        self.assertEqual(rc,0,err);self.assertEqual(out,b'');self.assertNotIn('file_101',files)
    def test_marker_only_journal_has_empty_root(self):
        rc,out,err,_=self.run_read('ls','/',mode='empty')
        self.assertEqual(rc,0,err);self.assertEqual(out,b'')
    def test_remote_remove_is_applied(self):
        rc,out,err,_=self.run_read(mode='remove')
        self.assertEqual(rc,0,err);self.assertEqual(out,b'hello.txt\nphoto.jpg\n')
    def test_multipart_and_late_error(self):
        rc,out,err,files=self.run_read('cat','/documents/hello.txt','multipart')
        self.assertEqual(rc,0,err);self.assertEqual(out,b'hello\n')
        self.assertIn('file_102',files)
        rc,out,err,_=self.run_read('cat','/documents/hello.txt','latecorrupt')
        self.assertEqual(rc,1,err);self.assertEqual(out,b'hel');self.assertIn('SHA-256',err)
    def test_gc_plan_verifies_distinct_layouts(self):
        rc,out,err,files=self.run_read('gc-plan',mode='gc-duplicates')
        self.assertEqual(rc,0,err)
        report=json.loads(out)
        self.assertEqual(report['scope'],'provided_capture_only')
        self.assertEqual(report['boundary_id'],'200')
        self.assertFalse(report['coverage_verified'])
        self.assertEqual(len(report['groups']),1)
        group=report['groups'][0]
        self.assertEqual(group['file_sha256'],hashlib.sha256(b'hello\n').hexdigest())
        self.assertEqual(group['file_size'],'6')
        self.assertEqual({c['commit_id']:c['chunk_ids'] for c in group['verified_commits']},
                         {'110':['101'],'140':['112','113']})
        for fid in ('file_101','file_112','file_113'): self.assertEqual(files.count(fid),1)

    def test_gc_plan_retries_and_incomplete_copies(self):
        for mode in ('ok','gc-retry-only','empty'):
            with self.subTest(mode=mode):
                rc,out,err,files=self.run_read('gc-plan',mode=mode)
                self.assertEqual(rc,0,err);self.assertEqual(json.loads(out)['groups'],[])
                self.assertNotIn('file_101',files)
        for mode,expected in (('gc-corrupt','SHA-256 mismatch'),('gc-bad-full-hash','whole-file SHA-256 mismatch'),
                              ('gc-missing','missing CHUNK'),('incomplete','incomplete journal')):
            with self.subTest(mode=mode):
                rc,out,err,_=self.run_read('gc-plan',mode=mode)
                self.assertEqual(rc,1,err);self.assertEqual(out,b'');self.assertIn(expected,err)

    def test_failures(self):
        for mode,expected in [('corrupt','SHA-256'),('fileid','getFile metadata'),('size','getFile metadata'),('path','invalid relative'),
                              ('badjson','invalid JSON'),('duplicate','duplicate or unsorted'),
                              ('incomplete','incomplete journal'),('channel','identity mismatch'),
                              ('missingpart','missing CHUNK'),('api','retry_after=9')]:
            with self.subTest(mode=mode):
                rc,out,err,_=self.run_read('cat','/documents/hello.txt',mode)
                self.assertEqual(rc,1,err);self.assertEqual(out,b'');self.assertIn(expected,err)
unittest.main()

"""Exercise the actual FUSE callback table without a kernel mount, plus mount when available."""
from contextlib import contextmanager
import copy
import errno
import hashlib
import http.server
import json
import os
from pathlib import Path
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import uuid

DRIVER=str(Path(sys.argv.pop(1)).resolve())
BINARY=str(Path(sys.argv.pop(1)).resolve())
EXAMPLE=Path(__file__).resolve().parent.parent/'examples/directory-merge'
TOKEN='123456:FUSE_TEST_TOKEN'
CHANNEL=-1003916921678
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self,*args): pass
    def do_POST(self):
        method=self.path.rsplit('/',1)[-1]
        p=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        self.server.calls.append(method)
        if method=='getChat': result=dict(id=CHANNEL,type='channel')
        else:
            assert method=='getFile'
            fid=p['file_id'];result=dict(file_id=fid,file_size=len(self.server.payloads[fid]),file_path='documents/'+fid)
        data=json.dumps(dict(ok=True,result=result)).encode()
        self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
    def do_GET(self):
        fid=self.path.rsplit('/',1)[-1];self.server.calls.append(fid);data=self.server.payloads[fid]
        self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
class Fuse(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.snapshot=self.root/'snapshot'
        shutil.copytree(EXAMPLE,self.snapshot)
        self.doc=json.loads((self.snapshot/'snapshot.json').read_text())
        chunk=next(r for r in self.doc['messages'] if r['message_id']=='101')
        second=copy.deepcopy(chunk);second['message_id']='102';second['document']='102.bin'
        parts=[]
        for record,offset,data in ((chunk,0,b'hel'),(second,3,b'lo\n')):
            (self.snapshot/record['document']).write_bytes(data)
            record['filename']='hello.txt.'+str(offset//3)
            digest=hashlib.sha256(data).hexdigest()
            record['caption'].update(part_count=2,offset=str(offset),part_size='3',part_sha256=digest)
            parts.append(dict(message_id=record['message_id'],offset=str(offset),part_size='3',part_sha256=digest))
        commit=next(r for r in self.doc['messages'] if r['message_id']=='110')
        commit['caption']['part_count']=2
        (self.snapshot/'110.json').write_text(json.dumps(dict(parts=parts)))
        self.doc['messages'].append(second);self.save()
    def save(self): (self.snapshot/'snapshot.json').write_text(json.dumps(self.doc))
    def driver(self,scenario,*args,env=None):
        result=subprocess.run([DRIVER,str(self.snapshot),scenario,*map(str,args)],env=env,capture_output=True,timeout=15)
        stderr=result.stderr.decode()
        self.assertEqual(result.returncode,0,stderr)
        self.assertNotIn('AddressSanitizer',stderr);self.assertNotIn('runtime error:',stderr)
        self.assertNotIn(TOKEN,stderr)
        return json.loads(result.stdout)
    def test_metadata_errors_and_paginated_directory(self):
        r=self.driver('basic')
        self.assertEqual(r['root_rc'],0);self.assertEqual(r['root_inode'],1);self.assertEqual(r['root_links'],3)
        self.assertEqual(r['root_mode'],0o555);self.assertEqual(r['file_mode'],0o444)
        self.assertEqual(r['file_size'],6);self.assertGreater(r['file_inode'],1)
        for key,code in [('missing',errno.ENOENT),('notdir',errno.ENOTDIR),('isdir',errno.EISDIR),
                         ('opendir_file',errno.ENOTDIR),('write_access',errno.EROFS),('exec_access',errno.EACCES),('stale_read',errno.EBADF)]:
            self.assertEqual(r[key],-code,key)
        self.assertEqual(r['names'],['.','..','hello.txt','notes.txt','photo.jpg'])
        self.assertEqual(r['readdir_rc'],0);self.assertEqual(r['direct_io'],1)
        self.assertEqual(r['release'],0);self.assertEqual(r['loads'],[])
    def test_all_write_handlers_are_readonly(self):
        r=self.driver('writes')
        for key,value in r.items():
            if key!='loads': self.assertEqual(value,-errno.EROFS,key)
        self.assertEqual(r['loads'],[])
    def test_read_ranges_only_load_overlapping_parts(self):
        for offset,size,expected,loads in [(0,1,b'h',[101]),(3,1,b'l',[102]),(2,3,b'llo',[101,102]),
                                          (5,20,b'\n',[102]),(6,20,b'',[]),(100,20,b'',[]),(0,0,b'',[])]:
            with self.subTest(offset=offset,size=size):
                r=self.driver('range',offset,size)
                self.assertEqual(r['open'],0);self.assertEqual(r['read'],len(expected))
                self.assertEqual(bytes.fromhex(r['hex']),expected);self.assertEqual(r['loads'],loads)
    def test_negative_offsets_and_oversized_request(self):
        for offset,size in [(-1,1),(0,2**31)]:
            r=self.driver('range',offset,size);self.assertEqual(r['read'],-errno.EINVAL);self.assertEqual(r['loads'],[])
    def test_last_verified_part_is_shared_between_handles(self):
        r=self.driver('ram')
        for key in ('first','second','other_read','third','fourth'): self.assertEqual(r[key],1)
        self.assertEqual(r['loads'],[101,102])
    def test_sequential_reads_verify_whole_file(self):
        r=self.driver('sequence');self.assertEqual(r['reads'],[2,2,2]);self.assertEqual(r['hex'],['6865','6c6c','6f0a'])
        self.assertEqual(r['loads'],[101,102,101]);self.assertEqual(r['again'],1)
    def test_wrong_full_hash_is_eio_at_completion(self):
        # Select by filename/object instead of assuming physical message order.
        file_id=next(m['caption']['object_id'] for m in self.doc['messages'] if m.get('filename')=='hello.txt.0')
        for m in self.doc['messages']:
            if m['caption'].get('object_id')==file_id: m['caption']['file_sha256']='0'*64
        self.save();r=self.driver('sequence')
        self.assertEqual(r['reads'],[2,2,-errno.EIO]);self.assertEqual(r['again'],-errno.EIO)
    def test_corrupt_unrelated_part_is_not_fetched(self):
        (self.snapshot/'102.bin').write_bytes(b'BAD')
        r=self.driver('range',0,1);self.assertEqual(r['read'],1);self.assertEqual(r['loads'],[101])
        r=self.driver('range',3,1);self.assertEqual(r['read'],-errno.EIO);self.assertEqual(r['hex'],'')
    def test_missing_part_bytes_and_reference(self):
        (self.snapshot/'102.bin').unlink()
        r=self.driver('range',3,1);self.assertEqual(r['read'],-errno.EIO)
        self.doc['messages']=[r for r in self.doc['messages'] if r['message_id']!='102'];self.save()
        r=self.driver('range',0,1);self.assertEqual(r['open'],-errno.EIO);self.assertEqual(r['loads'],[])
    def test_empty_file_and_immutable_loaded_view(self):
        r=self.driver('empty');self.assertEqual(r['open'],0);self.assertEqual(r['read'],0);self.assertEqual(r['loads'],[])
        r=self.driver('immutable');self.assertEqual(bytes.fromhex(r['hex']),b'hello\n')
    @contextmanager
    def remote_session(self):
        capture=self.root/'capture';capture.mkdir();records=[];payloads={}
        for m in sorted(self.doc['messages'],key=lambda r:int(r['message_id'])):
            fid='file_'+m['message_id'];data=(self.snapshot/m['document']).read_bytes();payloads[fid]=data
            records.append(dict(message_id=m['message_id'],record=m['caption'],
                document=dict(file_id=fid,file_size=len(data),file_name=m.get('filename','metadata.json'))))
        (capture/'journal.json').write_text(json.dumps(dict(schema='TGFS_JOURNAL_CAPTURE_V1',scan_completed=True,
            gap_policy='forward_400_is_gap',storage_chat_id=str(CHANNEL),superblock=self.doc['superblock'],boundary_id='200',records=records)))
        token=self.root/'token';token.write_text(TOKEN);token.chmod(0o600)
        server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler);server.calls=[];server.payloads=payloads
        t=threading.Thread(target=server.serve_forever,kwargs=dict(poll_interval=.01),daemon=True);t.start()
        try:
            env=dict(os.environ,TGFS_TEST_API_BASE=f'http://127.0.0.1:{server.server_port}',TGFS_DRIVER_CAPTURE=str(capture),
                TGFS_DRIVER_TOKEN=str(token),TGFS_DRIVER_CACHE=str(self.root/'cache'))
            yield capture,token,server,env
        finally:
            server.shutdown();server.server_close();t.join()

    def test_remote_session_lifetime_and_offline_parts(self):
        with self.remote_session() as (_,_,server,env):
            r=self.driver('range',0,1,env=env);self.assertEqual(r['read'],1)
            self.assertIn('file_101',server.calls);self.assertNotIn('file_102',server.calls)
            server.calls.clear();env['TGFS_DRIVER_OFFLINE']='1'
            r=self.driver('range',0,1,env=env);self.assertEqual(r['read'],1);self.assertEqual(server.calls,[])
            r=self.driver('range',3,1,env=env);self.assertEqual(r['read'],-errno.EIO);self.assertEqual(server.calls,[])

    def test_gc_plan_tracks_old_open_file(self):
        with self.remote_session() as (capture,token,server,env):
            registry=self.root/'open-files'
            env['TGFS_DRIVER_OPEN_FILES']=str(registry)
            newer=self.root/'newer';newer.mkdir()
            updated=json.loads((capture/'journal.json').read_text())
            updated['boundary_id']='205'
            empty=copy.deepcopy(next(r for r in updated['records'] if r['message_id']=='110'))
            empty['message_id']='201'
            empty['record'].update(operation_id=str(uuid.uuid4()),version_id=str(uuid.uuid4()),part_count=0,
                                   file_sha256=hashlib.sha256(b'').hexdigest(),file_size='0')
            server.payloads['file_201']=b'{"parts":[]}'
            empty['document'].update(file_id='file_201',file_size=len(server.payloads['file_201']))
            updated['records'].append(empty)
            (newer/'journal.json').write_text(json.dumps(updated))
            proc=subprocess.Popen([DRIVER,str(self.snapshot),'retain-open'],env=env,
                                  stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            try:
                self.assertTrue(select.select([proc.stdout],[],[],10)[0])
                self.assertEqual(proc.stdout.readline(),'ready\n')
                report_file=registry/'open-files.json'
                saved=json.loads(report_file.read_text())
                self.assertEqual(registry.stat().st_mode&0o777,0o700)
                self.assertEqual(report_file.stat().st_mode&0o777,0o600)
                self.assertEqual(saved['files'],[dict(commit_id='110',chunk_ids=['101','102'])])
                args=['--capture',str(newer),'--token-file',str(token)]
                current=subprocess.run([BINARY,'read','--no-cache',*args,'cat','/documents/hello.txt'],
                                       env=env,capture_output=True,timeout=10)
                self.assertEqual(current.returncode,0,current.stderr)
                self.assertEqual(current.stdout,b'')
                plan=[BINARY,'gc-plan',*args,'--retain-open-files',str(registry)]
                result=subprocess.run(plan,env=env,capture_output=True,timeout=10)
                self.assertEqual(result.returncode,0,result.stderr)
                mounts=json.loads(result.stdout)['retained_open_files']
                self.assertEqual(mounts,[dict(state_dir=str(registry),boundary_id='200',files=saved['files'])])
                self.assertEqual(json.loads(result.stdout)['groups'],[])
                for key,value in (('root_object_id',str(uuid.uuid4())),('boundary_id','206'),
                                  ('files',[dict(commit_id='110',chunk_ids=['110'])])):
                    with self.subTest(key=key):
                        report_file.write_text(json.dumps(dict(saved,**{key:value})))
                        rejected=subprocess.run(plan,env=env,capture_output=True,timeout=10)
                        self.assertEqual(rejected.returncode,1,rejected.stderr)
                        self.assertEqual(rejected.stdout,b'')
                report_file.write_text(json.dumps(saved))
                stdout,stderr=proc.communicate('r',timeout=10)
                self.assertEqual(proc.returncode,0,stderr)
                self.assertNotIn('AddressSanitizer',stderr);self.assertNotIn('runtime error:',stderr)
                reply=json.loads(stdout)
                self.assertEqual(reply['open'],0);self.assertEqual(reply['second_open'],0)
                self.assertEqual(reply['first_release'],0);self.assertEqual(reply['release'],0)
                self.assertEqual(bytes.fromhex(reply['hex']),b'hello\n')
                self.assertEqual(json.loads(report_file.read_text())['files'],[])
            finally:
                if proc.poll() is None:
                    proc.kill();proc.communicate(timeout=5)

    def test_mount_arguments_and_directory_validation(self):
        p=subprocess.run([BINARY,'mount','--help'],capture_output=True)
        self.assertEqual(p.returncode,0);self.assertIn(b'--offline',p.stdout)
        point=self.root/'mnt';point.mkdir();(point/'existing').write_text('keep')
        p=subprocess.run([BINARY,'mount','--snapshot',str(self.snapshot),'--mountpoint',str(point)],capture_output=True)
        self.assertEqual(p.returncode,1);self.assertIn(b'must be empty',p.stderr)
        self.assertEqual((point/'existing').read_text(),'keep')
        p=subprocess.run([BINARY,'mount','--snapshot',str(self.snapshot),'--capture',str(self.root),'--mountpoint',str(point)],capture_output=True)
        self.assertEqual(p.returncode,2)
    @unittest.skipUnless(Path('/dev/fuse').exists() and shutil.which('fusermount3'),'kernel mount unavailable: /dev/fuse or fusermount3 missing')
    def test_kernel_mount_when_available(self):
        point=self.root/'mnt';point.mkdir();log=self.root/'fuse.log'
        guard=self.root/'volume-lock';guard.mkdir(mode=0o700)
        prefix=[BINARY,'--volume-lock',str(guard)]
        with log.open('wb') as stream:
            process=subprocess.Popen([*prefix,'mount','--snapshot',str(self.snapshot),'--mountpoint',str(point)],stdout=stream,stderr=stream)
            try:
                end=time.monotonic()+15
                while time.monotonic()<end and process.poll() is None and not os.path.ismount(point): time.sleep(.05)
                self.assertTrue(os.path.ismount(point),log.read_text())
                blocked=subprocess.run([*prefix,'gc-plan'],capture_output=True,timeout=5)
                self.assertEqual(blocked.returncode,1,blocked.stderr)
                self.assertIn(b'volume lock is busy',blocked.stderr)
                self.assertEqual(sorted(p.name for p in (point/'documents').iterdir()),['hello.txt','notes.txt','photo.jpg'])
                with (point/'documents/hello.txt').open('rb',buffering=0) as f:
                    self.assertEqual(os.pread(f.fileno(),3,2),b'llo');self.assertEqual(os.pread(f.fileno(),1,6),b'')
                self.assertEqual((point/'documents/notes.txt').read_bytes(),b'')
                self.assertEqual((point/'documents/hello.txt').read_bytes(),b'hello\n')
                with self.assertRaises(OSError) as caught: (point/'new').write_bytes(b'x')
                self.assertEqual(caught.exception.errno,errno.EROFS)
            finally:
                if os.path.ismount(point): subprocess.run(['fusermount3','-u',str(point)],capture_output=True,timeout=10)
                try: process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.send_signal(signal.SIGINT);process.wait(timeout=10)
                self.assertEqual(process.returncode,0,log.read_text())
                next_plan=subprocess.run([*prefix,'gc-plan'],capture_output=True,timeout=5)
                self.assertEqual(next_plan.returncode,2,next_plan.stderr)
                self.assertNotIn('AddressSanitizer',log.read_text())
                self.assertNotIn('runtime error:',log.read_text())
unittest.main()

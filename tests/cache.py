"""Persistent SQLite index and verified document cache across CLI processes."""
import concurrent.futures
import copy
import hashlib
import http.server
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import threading
import unittest

BINARY=str(Path(sys.argv.pop(1)).resolve())
EXAMPLE=Path(__file__).resolve().parent.parent/'examples/directory-merge'
TOKEN='123456:CACHE_TEST_TOKEN'
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
            fid=p['file_id']
            result=dict(file_id=fid,file_size=len(self.server.payloads[fid]),file_path='documents/'+fid)
        data=json.dumps(dict(ok=True,result=result)).encode()
        self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
    def do_GET(self):
        fid=self.path.rsplit('/',1)[-1];self.server.calls.append(fid)
        data=self.server.payloads[fid]
        self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
class Cache(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name);self.cache=self.root/'cache'
        self.token=self.root/'token';self.token.write_text(TOKEN);self.token.chmod(0o600)
        snap=json.loads((EXAMPLE/'snapshot.json').read_text());records=[];payloads={}
        for m in sorted(snap['messages'],key=lambda r:int(r['message_id'])):
            data=(EXAMPLE/m['document']).read_bytes();fid='file_'+m['message_id'];payloads[fid]=data
            records.append(dict(message_id=m['message_id'],record=m['caption'],
                document=dict(file_id=fid,file_size=len(data),file_name=m.get('filename','metadata.json'))))
        self.journal=dict(schema='TGFS_JOURNAL_CAPTURE_V1',scan_completed=True,gap_policy='forward_400_is_gap',
            storage_chat_id=str(CHANNEL),superblock=snap['superblock'],boundary_id='200',records=records)
        self.save()
        self.server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
        self.server.calls=[];self.server.payloads=payloads
        t=threading.Thread(target=self.server.serve_forever,kwargs=dict(poll_interval=.01),daemon=True);t.start()
        def stop(): self.server.shutdown();self.server.server_close();t.join()
        self.addCleanup(stop)
    def save(self): (self.root/'journal.json').write_text(json.dumps(self.journal))
    def run_read(self,command='ls',path='/documents',options=(),ok=True,cache=None,extra_env=None,default_cache=False):
        args=[BINARY,'read','--capture',str(self.root),'--token-file',str(self.token),'--cache-stats']
        if '--no-cache' not in options and not default_cache: args+=['--cache-dir',str(cache or self.cache)]
        args += [*options,command,path]
        env=dict(os.environ,TGFS_TEST_API_BASE=f'http://127.0.0.1:{self.server.server_port}')
        if extra_env: env.update(extra_env)
        p=subprocess.run(args,env=env,capture_output=True,timeout=20)
        err=p.stderr.decode()
        self.assertEqual(p.returncode,0 if ok else 1,err)
        self.assertNotIn(TOKEN,err);self.assertNotIn('AddressSanitizer',err);self.assertNotIn('runtime error:',err)
        return p.stdout,err
    def sql(self,query,args=()):
        with sqlite3.connect(self.cache/'index.sqlite3') as db:
            return db.execute(query,args).fetchall()
    def test_warm_index_and_lazy_parts(self):
        out,_=self.run_read();self.assertEqual(out,b'hello.txt\nnotes.txt\nphoto.jpg\n')
        self.assertNotIn('file_101',self.server.calls)
        self.assertEqual(self.sql('SELECT count(*) FROM captures')[0][0],1)
        self.assertEqual(self.sql('SELECT count(*) FROM records')[0][0],len(self.journal['records']))
        self.server.calls.clear();out,err=self.run_read()
        self.assertEqual(self.server.calls,['getChat']);self.assertIn('index=hit',err);self.assertIn('downloads=0',err)
        self.server.calls.clear();out,err=self.run_read('cat','/documents/hello.txt')
        self.assertEqual(out,b'hello\n');self.assertEqual(self.server.calls,['getChat','getFile','file_101'])
        self.server.calls.clear();out,err=self.run_read('cat','/documents/hello.txt',('--offline',))
        self.assertEqual(out,b'hello\n');self.assertEqual(self.server.calls,[]);self.assertIn('downloads=0',err)
        self.assertNotIn(TOKEN.encode(),(self.cache/'index.sqlite3').read_bytes())
    def test_offline_cold_and_unloaded_chunk(self):
        _,err=self.run_read(options=('--offline',),ok=False)
        self.assertIn('offline cache miss',err);self.assertEqual(self.server.calls,[])
        self.assertEqual(self.sql('SELECT count(*) FROM captures')[0][0],0)
        self.run_read();self.server.calls.clear()
        _,err=self.run_read('cat','/documents/hello.txt',('--offline',),ok=False)
        self.assertIn('offline cache miss',err);self.assertEqual(self.server.calls,[])
        out,_=self.run_read('cat','/documents/notes.txt',('--offline',));self.assertEqual(out,b'')
    def test_refresh_and_no_cache(self):
        self.run_read('cat','/documents/hello.txt')
        self.server.calls.clear();self.run_read('cat','/documents/hello.txt',('--refresh-cache',))
        self.assertEqual(self.server.calls.count('getFile'),len(self.journal['records']))
        self.server.calls.clear();self.run_read(options=('--no-cache',))
        self.assertGreater(self.server.calls.count('getFile'),0)
        self.assertEqual(self.sql('SELECT count(*) FROM captures')[0][0],1)
    def test_new_capture_reuses_immutable_documents(self):
        self.run_read('cat','/documents/hello.txt')
        self.journal['boundary_id']='201'
        chunk=next(r for r in self.journal['records'] if r['message_id']=='101')
        chunk['record']['file_commit_id']='110';self.save()
        self.server.calls.clear();out,err=self.run_read('cat','/documents/hello.txt')
        self.assertEqual(out,b'hello\n');self.assertIn('index=miss',err);self.assertIn('downloads=0',err)
        self.assertEqual(self.server.calls,['getChat']);self.assertEqual(self.sql('SELECT count(*) FROM captures')[0][0],2)
    def test_new_directory_operation_updates_index(self):
        self.run_read();old=copy.deepcopy(self.journal)
        observed=json.loads(self.server.payloads['file_20'])['operations'][0]['operation_id']
        payload=json.dumps(dict(operations=[dict(operation_id='80a66f96-38a9-4725-b3de-b8cf23ba017b',
            action='REMOVE',name='notes.txt',observed_adds=[observed])])).encode()
        self.server.payloads['file_40']=payload
        r=copy.deepcopy(next(r for r in self.journal['records'] if r['message_id']=='20'))
        r['message_id']='40';r['document'].update(file_id='file_40',file_size=len(payload))
        self.journal['records'].append(r);self.journal['records'].sort(key=lambda r:int(r['message_id']))
        self.journal['boundary_id']='201';self.save();self.server.calls.clear()
        out,err=self.run_read();self.assertEqual(out,b'hello.txt\nphoto.jpg\n')
        self.assertEqual(self.server.calls,['getChat','getFile','file_40']);self.assertIn('downloads=1',err)
        self.journal=old;self.save();self.server.calls.clear()
        out,err=self.run_read(options=('--offline',))
        self.assertEqual(out,b'hello.txt\nnotes.txt\nphoto.jpg\n');self.assertIn('index=hit',err)
        self.assertEqual(self.server.calls,[])
    def test_metadata_corruption_repairs_one_document(self):
        self.run_read();payload=self.server.payloads['file_10']
        self.sql('UPDATE blobs SET payload=? WHERE payload=?',(b'X'+payload[1:],payload))
        self.server.calls.clear();out,_=self.run_read()
        self.assertEqual(out,b'hello.txt\nnotes.txt\nphoto.jpg\n')
        self.assertEqual(self.server.calls,['getChat','getFile','file_10'])
    def test_default_xdg_and_home_paths(self):
        xdg=self.root/'xdg'
        self.run_read(default_cache=True,extra_env=dict(XDG_CACHE_HOME=str(xdg)))
        db=xdg/'tgfs'/'index.sqlite3'
        self.assertTrue(db.exists());self.assertEqual(db.stat().st_mode&0o777,0o600)
        self.assertEqual(db.parent.stat().st_mode&0o777,0o700)
        self.server.calls.clear()
        self.run_read(default_cache=True,options=('--offline',),extra_env=dict(XDG_CACHE_HOME=str(xdg)))
        self.assertEqual(self.server.calls,[])
        home=self.root/'home'
        self.run_read(default_cache=True,extra_env=dict(XDG_CACHE_HOME='relative',HOME=str(home)))
        self.assertTrue((home/'.cache/tgfs/index.sqlite3').exists())
    def test_other_database_is_preserved(self):
        self.cache.mkdir(mode=0o700);path=self.cache/'index.sqlite3'
        with sqlite3.connect(path) as db:
            db.execute('CREATE TABLE personal(value TEXT)');db.execute("INSERT INTO personal VALUES('preserved')")
        path.chmod(0o600)
        _,err=self.run_read(ok=False);self.assertIn('another SQLite database',err)
        self.assertEqual(self.sql('SELECT value FROM personal'),[('preserved',)])
        self.assertEqual(self.sql('PRAGMA user_version')[0][0],0)
    def test_bot_and_root_scopes(self):
        self.run_read();self.server.calls.clear()
        self.token.write_text('123457:OTHER_CACHE_TOKEN')
        self.run_read();self.assertGreater(self.server.calls.count('getFile'),0)
        old=self.journal['superblock']['root_object_id'];new='550e8400-e29b-41d4-a716-446655440099'
        self.journal['superblock']['root_object_id']=new
        for r in self.journal['records']:
            for key in ('object_id','parent_id'):
                if r['record'].get(key)==old: r['record'][key]=new
        self.save();self.server.calls.clear();self.run_read()
        self.assertGreater(self.server.calls.count('getFile'),0)
        self.assertEqual(self.sql('SELECT count(*) FROM captures')[0][0],3)
    def test_corrupt_chunk_repair_and_expected_sha(self):
        self.run_read('cat','/documents/hello.txt')
        self.sql("UPDATE blobs SET payload=? WHERE payload=?",(b'Xello\n',b'hello\n'))
        self.server.calls.clear()
        self.run_read('cat','/documents/hello.txt',('--offline',),ok=False)
        self.assertEqual(self.server.calls,[])
        out,_=self.run_read('cat','/documents/hello.txt');self.assertEqual(out,b'hello\n')
        self.assertIn('file_101',self.server.calls)
        self.sql('UPDATE blobs SET payload=?,sha=? WHERE payload=?',
            (b'Xello\n',hashlib.sha256(b'Xello\n').hexdigest(),b'hello\n'))
        self.server.calls.clear();out,_=self.run_read('cat','/documents/hello.txt')
        self.assertEqual(out,b'hello\n');self.assertIn('file_101',self.server.calls)
    def test_corrupt_index_rebuilt_without_download(self):
        self.run_read();self.sql('UPDATE captures SET snapshot=?',(b'broken',))
        self.server.calls.clear();out,err=self.run_read(options=('--offline',))
        self.assertEqual(out,b'hello.txt\nnotes.txt\nphoto.jpg\n');self.assertIn('index=miss',err)
        self.assertEqual(self.server.calls,[])
        self.assertEqual(self.sql('SELECT count(*) FROM records')[0][0],len(self.journal['records']))
    def test_schema_and_permissions_are_not_overwritten(self):
        self.run_read();self.sql('PRAGMA user_version=99')
        _,err=self.run_read(ok=False);self.assertIn('unsupported cache database schema',err)
        self.assertEqual(self.sql('PRAGMA user_version')[0][0],99)
        self.cache.chmod(0o755)
        _,err=self.run_read(ok=False);self.assertIn('owner-only',err)
    def test_symlinks_rejected(self):
        target=self.root/'target';target.write_bytes(b'untouched');target.chmod(0o600)
        self.cache.mkdir(mode=0o700);(self.cache/'index.sqlite3').symlink_to(target)
        self.run_read(ok=False);self.assertEqual(target.read_bytes(),b'untouched')
        link=self.root/'link';link.symlink_to(self.cache,target_is_directory=True)
        _,err=self.run_read(cache=str(link)+'/',ok=False);self.assertIn('owner-only',err)
    def test_database_corruption_can_be_bypassed(self):
        self.run_read();(self.cache/'index.sqlite3').write_bytes(b'not SQLite')
        self.run_read(ok=False);self.run_read(options=('--no-cache',))
        self.assertEqual((self.cache/'index.sqlite3').read_bytes(),b'not SQLite')
    def test_concurrent_openers(self):
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            results=list(pool.map(lambda _:self.run_read('cat','/documents/hello.txt'),range(2)))
        self.assertEqual([r[0] for r in results],[b'hello\n',b'hello\n'])
        self.assertEqual(self.sql('SELECT count(*) FROM captures')[0][0],1)
        self.assertEqual(self.sql('PRAGMA integrity_check')[0][0],'ok')
    def test_quota_keeps_large_chunk_out(self):
        data=b'a'*(2*1024*1024);digest=hashlib.sha256(data).hexdigest()
        self.server.payloads['file_101']=data
        for r in self.journal['records']:
            if r['message_id']=='101':
                r['document']['file_size']=len(data)
                r['record'].update(part_size=str(len(data)),file_size=str(len(data)),part_sha256=digest,file_sha256=digest)
            if r['message_id']=='110':
                r['record'].update(file_size=str(len(data)),file_sha256=digest)
                body=dict(parts=[dict(message_id='101',offset='0',part_size=str(len(data)),part_sha256=digest)])
                self.server.payloads['file_110']=json.dumps(body).encode()
                r['document']['file_size']=len(self.server.payloads['file_110'])
        self.save()
        out,_=self.run_read('cat','/documents/hello.txt',('--cache-limit-mib','1'))
        self.assertEqual(out,data)
        self.assertLessEqual(self.sql('SELECT coalesce(sum(length(payload)),0) FROM blobs')[0][0],1024*1024)
        self.server.calls.clear();self.run_read('cat','/documents/hello.txt',('--offline','--cache-limit-mib','1'),ok=False)
        self.assertEqual(self.server.calls,[])
        self.run_read('cat','/documents/hello.txt')
        self.run_read('cat','/documents/hello.txt',('--offline','--cache-limit-mib','1'),ok=False)
        self.assertLessEqual(self.sql('SELECT coalesce(sum(length(payload)),0) FROM blobs')[0][0],1024*1024)
unittest.main()

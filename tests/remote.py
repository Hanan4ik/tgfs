"""Captured journal -> real curl -> metadata merge -> lazy verified cat."""
import copy
import fcntl
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
    def run_read(self,command='ls',path='/documents',mode='ok',retained=(),intents=(),open_files=()):
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
            if mode in ('gc-catalog','gc-history','gc-detached'):
                directory=next(r for r in records if r['message_id']=='10')
                parent_id=directory['record']['object_id']
                root_record=next(r for r in records if r['message_id']=='15')
                def append_ops(record,operations):
                    doc=record['document'];fid=doc['file_id']
                    body=json.loads(payloads[fid]);body['operations'].extend(operations)
                    payloads[fid]=json.dumps(body).encode();doc['file_size']=len(payloads[fid])
                append_ops(directory,[dict(operation_id=str(uuid.uuid4()),action='ADD',name='renamed.bin',
                    object_id=other['record']['object_id']),dict(operation_id=str(uuid.uuid4()),action='ADD',
                    name='loop',object_id=parent_id)])
                append_ops(root_record,[dict(operation_id=str(uuid.uuid4()),action='ADD',
                    name='alias.txt',object_id=original_commit['record']['object_id'])])
                if mode=='gc-detached':
                    observed=json.loads(payloads['file_15'])['operations'][0]['operation_id']
                    removal=copy.deepcopy(root_record)
                    removal['message_id']='145';removal['document']['file_id']='file_145'
                    payloads['file_145']=b'{"operations":[]}'
                    append_ops(removal,[dict(operation_id=str(uuid.uuid4()),action='REMOVE',name='documents',
                        observed_adds=[observed])])
                    records.append(removal)
                if mode=='gc-history':
                    replacement=copy.deepcopy(original_commit)
                    replacement['message_id']='150';replacement['document']['file_id']='file_150'
                    replacement['record'].update(version_id=str(uuid.uuid4()),operation_id=str(uuid.uuid4()),
                        file_size='0',file_sha256=hashlib.sha256(b'').hexdigest(),part_count=0)
                    payloads['file_150']=b'{"parts":[]}'
                    replacement['document']['file_size']=len(payloads['file_150'])
                    records.append(replacement)
                    retry['message_id']='160'  # A late replay cannot select the old version.
            records.sort(key=lambda r:int(r['message_id']))
        if mode=='gc-navigation':
            journal['superblock']['next_message']='101'
            links={'101':'999','112':'113','113':'112'}
            for record in records:
                if record['message_id'] in links:
                    record['record']['next_message']=links[record['message_id']]
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
                for index,case in enumerate(retained):
                    old=root/f'retained-{index}';old.mkdir()
                    saved=copy.deepcopy(journal)
                    boundary='110' if case=='before-commit' else '200' if case=='same-boundary' else '139'
                    saved['boundary_id']=boundary
                    saved['records']=[r for r in saved['records'] if int(r['message_id'])<int(boundary)
                                      and not (case=='missing-chunk' and r['message_id']=='101')]
                    if case=='navigation':
                        saved['superblock']['next_message']='113'
                        saved['records']=[r for r in saved['records'] if r['message_id']!='101']
                        for record in saved['records']:
                            record['record']['next_message']='101' if record['message_id']=='10' else None
                    if case=='foreign-root': saved['superblock']['root_object_id']=str(uuid.uuid4())
                    if case=='foreign-channel': saved['storage_chat_id']=str(STORAGE+1)
                    if case=='foreign-start': saved['superblock']['start_id']='2'
                    if case=='incomplete': saved['scan_completed']=False
                    (old/'journal.json').write_text(json.dumps(saved))
                    args.extend(['--retain-capture',str(old)])
                if open_files:
                    mounted=root/'mounted';mounted.mkdir(mode=0o700)
                    report=mounted/'open-files.json'
                    report.write_text(json.dumps(dict(schema='TGFS_OPEN_FILES_V1',storage_chat_id=str(STORAGE),
                        root_object_id=journal['superblock']['root_object_id'],start_id=journal['superblock']['start_id'],
                        boundary_id=journal['boundary_id'],files=open_files)))
                    report.chmod(0o600)
                    args.extend(['--retain-open-files',str(mounted)])
                locks=[]
                try:
                    for index,case in enumerate(intents):
                        state=root/f'intent-{index}';state.mkdir(mode=0o700)
                        caption=next(r['record'] for r in records if r['message_id']=='110')
                        upload=dict(kind='put',phase='commit_send_pending',storage_chat_id=str(STORAGE),
                                    source_boundary_id='80',object_id=caption['object_id'],
                                    version_id=caption['version_id'],operation_id=caption['operation_id'],
                                    parts=[dict(message_id='101')],commit_document=dict(parts=[dict(message_id='101')]))
                        if case=='unknown':
                            upload.update(phase='chunk_response_uncertain',parts=[])
                            upload.pop('commit_document')
                        if case=='manifest-only':
                            upload.update(parts=[],file_commit_id='110',base_commit_id='90')
                        if case=='base-only':
                            upload.update(phase='prepared',parts=[],base_commit_id='110',
                                          operation_id=str(uuid.uuid4()),version_id=str(uuid.uuid4()))
                            upload.pop('commit_document')
                        if case=='complete': upload['phase']='complete'
                        if case=='foreign': upload['storage_chat_id']=str(STORAGE+1)
                        if case=='bad-parts': upload['parts']={}
                        if case=='bad-id': upload['parts']=[dict(message_id='0')]
                        if case=='bad-manifest': upload['commit_document']={}
                        if case=='public': state.chmod(0o755)
                        local=state/'intent.json';local.write_text(json.dumps(upload));local.chmod(0o600)
                        log=state/'backlink-repair.log'
                        if case!='unknown':
                            log.touch(mode=0o600)
                        if case=='busy':
                            handle=log.open()
                            locks.append(handle)
                            fcntl.flock(handle,fcntl.LOCK_EX|fcntl.LOCK_NB)
                        if case=='pending-local': (state/'intent.tmp').write_text('pending')
                        args.extend(['--retain-intent',str(state)])
                    p=subprocess.run([BINARY,*args],
                        env=dict(os.environ,TGFS_TEST_API_BASE=f'http://127.0.0.1:{server.server_port}'),
                        capture_output=True,timeout=10)
                finally:
                    for handle in locks: handle.close()
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

    def test_gc_plan_reports_capture_references(self):
        for mode in ('gc-catalog','gc-history','gc-detached'):
            with self.subTest(mode=mode):
                rc,out,err,files=self.run_read('gc-plan',mode=mode)
                self.assertEqual(rc,0,err)
                report=json.loads(out)
                commits={c['commit_id']:c for c in report['groups'][0]['verified_commits']}
                refs={c['chunk_id']:c['commit_ids'] for c in report['chunk_references']}
                self.assertEqual(refs,{'101':['110','160' if mode=='gc-history' else '111'],
                                       '112':['140'],'113':['140']})
                names={mid:{n['name'] for n in c['catalog_entries']} for mid,c in commits.items()}
                self.assertEqual(names['110'],{'alias.txt','hello.txt'} if mode=='gc-catalog'
                                 else {'alias.txt'} if mode=='gc-detached' else set())
                self.assertEqual(names['140'],set() if mode=='gc-detached' else {'renamed.bin'})
                for name in commits['110']['catalog_entries']:
                    if name['name']=='alias.txt':
                        self.assertEqual(name['parent_object_id'],report['root_object_id'])
                self.assertEqual(files.count('file_101'),1)

    def test_gc_plan_combines_retention_reasons(self):
        rc,out,err,_=self.run_read('gc-plan',mode='gc-history',retained=('older','older'),
                                    intents=('base-only','complete'),
                                    open_files=[dict(commit_id='110',chunk_ids=['101'])]*2)
        self.assertEqual(rc,0,err)
        report=json.loads(out)
        reasons={c['chunk_id']:c['retention_reasons'] for c in report['chunk_references']}
        self.assertEqual(reasons,{
            '101':['manifest_in_capture','retained_capture','unfinished_put','open_file'],
            '112':['manifest_in_capture','visible_in_current_capture'],
            '113':['manifest_in_capture','visible_in_current_capture'],
        })
        intent,=report['retained_intents']
        self.assertEqual(intent['base_commit_id'],'110')
        for field in ('saved_chunk_ids','observed_chunk_ids','observed_commit_ids'):
            self.assertEqual(intent[field],[])

        rc,out,err,_=self.run_read('gc-plan',mode='gc-history')
        self.assertEqual(rc,0,err)
        reasons={c['chunk_id']:c['retention_reasons'] for c in json.loads(out)['chunk_references']}
        self.assertEqual(reasons['101'],['manifest_in_capture'])

    def test_gc_plan_retains_navigation_edges(self):
        rc,out,err,files=self.run_read('gc-plan',mode='gc-navigation',retained=('navigation',))
        self.assertEqual(rc,0,err)
        report=json.loads(out)
        self.assertEqual(report['navigation_references'],[
            dict(source_message_id='1',source_type='SUPERBLOCK',chunk_id='101'),
            dict(source_message_id='112',source_type='CHUNK',chunk_id='113'),
            dict(source_message_id='113',source_type='CHUNK',chunk_id='112'),
        ])
        older,=report['retained_captures']
        self.assertEqual(older['navigation_references'],[
            dict(source_message_id='1',source_type='SUPERBLOCK',chunk_id='113'),
            dict(source_message_id='10',source_type='DIRECTORY',chunk_id='101'),
        ])
        reasons={c['chunk_id']:c['retention_reasons'] for c in report['chunk_references']}
        self.assertIn('navigation_in_capture',reasons['112'])
        self.assertNotIn('navigation_in_retained_capture',reasons['112'])
        self.assertIn('navigation_in_retained_capture',reasons['113'])
        self.assertNotIn('retained_capture',reasons['113'])
        for fid in ('file_101','file_112','file_113'): self.assertEqual(files.count(fid),1)

    def test_gc_plan_retained_captures(self):
        rc,out,err,files=self.run_read('gc-plan',mode='gc-detached',
                                       retained=('older','before-commit','missing-chunk'))
        self.assertEqual(rc,0,err)
        report=json.loads(out)
        self.assertEqual(report['scope'],'provided_captures_only')
        current={c['commit_id']:c for c in report['groups'][0]['verified_commits']}
        self.assertEqual([n['name'] for n in current['110']['catalog_entries']],['alias.txt'])
        self.assertEqual(current['140']['catalog_entries'],[])
        older,empty,gapped=report['retained_captures']
        self.assertEqual([older['boundary_id'],empty['boundary_id'],gapped['boundary_id']],['139','110','139'])
        self.assertEqual(empty['commit_references'],[])
        self.assertFalse(gapped['coverage_verified'])
        self.assertEqual(older['commit_references'],gapped['commit_references'])
        first,replay=older['commit_references']
        self.assertEqual(first['commit_id'],'110')
        self.assertEqual(first['chunk_ids'],['101'])
        self.assertEqual({name['name'] for name in first['catalog_entries']},{'alias.txt','hello.txt'})
        self.assertEqual(replay,dict(commit_id='111',chunk_ids=['101'],catalog_entries=[]))
        self.assertEqual(files.count('file_101'),1)

    def test_gc_plan_rejects_invalid_retained_capture(self):
        for case,error in (('foreign-root','different volume'),('foreign-channel','different volume'),
                           ('foreign-start','different volume'),('same-boundary','earlier boundary'),
                           ('incomplete','incomplete journal')):
            with self.subTest(case=case):
                rc,out,err,_=self.run_read('gc-plan',mode='gc-detached',retained=(case,))
                self.assertEqual(rc,1,err);self.assertEqual(out,b'');self.assertIn(error,err)

    def test_gc_plan_retained_intents(self):
        rc,out,err,files=self.run_read('gc-plan',mode='gc-detached',retained=('older',),
                                       intents=('unknown','complete','manifest-only'))
        self.assertEqual(rc,0,err)
        report=json.loads(out)
        self.assertEqual(report['scope'],'provided_captures_and_intents_only')
        self.assertEqual(len(report['retained_captures']),1)
        unknown,manifest=report['retained_intents']
        self.assertEqual(unknown['saved_chunk_ids'],[])
        self.assertEqual(unknown['observed_chunk_ids'],['101'])
        self.assertEqual(unknown['observed_commit_ids'],['110','111'])
        self.assertFalse(unknown['journal_locked'])
        self.assertEqual(manifest['manifest_chunk_ids'],['101'])
        self.assertEqual(manifest['file_commit_id'],'110')
        self.assertEqual(manifest['base_commit_id'],'90')
        self.assertTrue(manifest['journal_locked'])
        self.assertEqual(files.count('file_101'),1)
        rc,out,err,files=self.run_read('gc-plan',mode='ok',intents=('saved',))
        self.assertEqual(rc,0,err)
        self.assertEqual(json.loads(out)['retained_intents'][0]['saved_chunk_ids'],['101'])
        self.assertNotIn('file_101',files)

    def test_gc_plan_rejects_invalid_retained_intent(self):
        for case,error in (('public','private state directory'),('busy','lock retained intent journal'),
                           ('pending-local','pending local state file'),('foreign','different storage channel'),
                           ('bad-parts','invalid saved upload parts'),('bad-manifest','invalid saved upload parts'),
                           ('bad-id','invalid saved upload part ID')):
            with self.subTest(case=case):
                rc,out,err,_=self.run_read('gc-plan',intents=(case,))
                self.assertEqual(rc,1,err);self.assertEqual(out,b'');self.assertIn(error,err)

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

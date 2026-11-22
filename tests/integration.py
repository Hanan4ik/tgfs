"""End-to-end tests of the C executable with local message snapshots."""
import copy
import fcntl
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())
ROOT = "a30d487f-167a-4eb3-8b06-b0676a039470"
DIRECTORY = "c5ccf4d2-79ba-4ab7-a4d0-d9b84b40a8cd"
FILE = "550e8400-e29b-41d4-a716-446655440000"
EMPTY = "550e8400-e29b-41d4-a716-446655440001"
VERSION = "5d5d82f0-1445-4b81-9a45-501cc939b56e"
OP = "3f9c7148-61a2-4c70-9a8d-53f98166e510"
OP2 = "3f9c7148-61a2-4c70-9a8d-53f98166e511"
SHA = lambda data: hashlib.sha256(data).hexdigest()


def envelope(kind, **fields):
    return dict(magic="TGFS", format_version=1, type=kind, next_message=None, **fields)


class ReadCore(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)
        self.data = b"hello\n"
        self.snapshot = dict(
            superblock=envelope("SUPERBLOCK", start_id="1", root_object_id=ROOT),
            root_entries=[dict(name="documents", object_id=DIRECTORY)],
            messages=[],
        )
        self.add(10, envelope("DIRECTORY", object_id=DIRECTORY, parent_id=ROOT),
                 dict(entries=[dict(name="hello.txt", object_id=FILE),
                               dict(name="empty", object_id=EMPTY)]))
        parts = []
        offset = 0
        for index, data in enumerate((self.data[:3], self.data[3:])):
            mid = 101 + index * 3
            part = dict(message_id=str(mid), offset=str(offset), part_size=str(len(data)), part_sha256=SHA(data))
            parts.append(part)
            self.add(mid, envelope("CHUNK", object_id=FILE, parent_id=DIRECTORY,
                     version_id=VERSION, part_count=2, offset=str(offset), part_size=str(len(data)),
                     part_sha256=SHA(data), file_size=str(len(self.data)), file_sha256=SHA(self.data),
                     file_commit_id=None), data, f"hello.txt.{index}")
            offset += len(data)
        self.add(110, envelope("FILE_COMMIT", object_id=FILE, version_id=VERSION,
                 operation_id=OP, part_count=2, file_size=str(len(self.data)), file_sha256=SHA(self.data)),
                 dict(parts=parts))
        self.add(115, envelope("FILE_COMMIT", object_id=EMPTY, version_id=VERSION,
                 operation_id=OP2, part_count=0, file_size="0", file_sha256=SHA(b"")), dict(parts=[]))

    def add(self, mid, caption, body, filename=None):
        document = f"{mid}.bin" if isinstance(body, bytes) else f"{mid}.json"
        self.write(document, body)
        record = dict(message_id=str(mid), caption=caption, document=document)
        if filename is not None:
            record["filename"] = filename
        self.snapshot["messages"].append(record)
        return record

    def write(self, name, value):
        (self.path / name).write_bytes(value if isinstance(value, bytes)
                                      else json.dumps(value, ensure_ascii=False).encode())

    def record(self, mid):
        return next(r for r in self.snapshot["messages"] if r["message_id"] == str(mid))

    def run_cli(self, command="cat", path="/documents/hello.txt", *, ok=True, error=None):
        self.write("snapshot.json", self.snapshot)
        result = subprocess.run([BINARY, "--snapshot", str(self.path), command, path], capture_output=True)
        self.assertEqual(result.returncode, 0 if ok else 1, result.stderr.decode(errors="replace"))
        if error:
            self.assertIn(error, result.stderr.decode())
        return result.stdout

    def test_list_root_and_directory(self):
        self.assertEqual(self.run_cli("ls", "/"), b"documents\n")
        self.assertEqual(self.run_cli("ls", "/documents"), b"hello.txt\nempty\n")

    def test_list_does_not_read_chunk_bytes(self):
        (self.path / "101.bin").unlink()
        self.assertEqual(self.run_cli("ls", "/documents"), b"hello.txt\nempty\n")

    def test_read_multiple_parts(self):
        self.assertEqual(self.run_cli(), self.data)

    def test_empty_file(self):
        self.assertEqual(self.run_cli(path="/documents/empty"), b"")

    def test_root_identity_cannot_be_file(self):
        self.record(110)["caption"]["object_id"] = ROOT
        self.run_cli(ok=False, error="root object")

    def test_empty_directory(self):
        self.write("10.json", dict(entries=[]))
        self.assertEqual(self.run_cli("ls", "/documents"), b"")

    def test_unicode_names(self):
        body = dict(entries=[dict(name="привет.v2.txt", object_id=FILE)])
        self.write("10.json", body)
        self.assertEqual(self.run_cli("ls", "/documents"), "привет.v2.txt\n".encode())
        self.assertEqual(self.run_cli(path="/documents/привет.v2.txt"), self.data)

    def test_missing_and_wrong_path_type(self):
        self.run_cli(path="/missing", ok=False, error="does not exist")
        self.run_cli(path="/documents", ok=False, error="directory as a file")
        self.run_cli("ls", "/documents/hello.txt", ok=False, error="not an available directory")
        self.run_cli(path="/documents/hello.txt/", ok=False)
        self.run_cli(path="relative", ok=False)
        self.run_cli(path="/documents/../hello.txt", ok=False)

    def test_corruption_same_size(self):
        self.write("101.bin", b"jel")
        self.assertEqual(self.run_cli(ok=False, error="SHA-256 mismatch"), b"")

    def test_truncated_chunk(self):
        self.write("101.bin", b"he")
        self.assertEqual(self.run_cli(ok=False, error="size or SHA-256"), b"")

    def test_missing_chunk_record(self):
        self.snapshot["messages"].remove(self.record(104))
        self.assertEqual(self.run_cli(ok=False, error="missing CHUNK"), b"")

    def test_missing_chunk_document(self):
        (self.path / "101.bin").unlink()
        self.run_cli(ok=False, error="cannot open document")

    def test_unpublished_file(self):
        self.snapshot["messages"].remove(self.record(110))
        self.assertEqual(self.run_cli(ok=False, error="no published FILE_COMMIT"), b"")

    def test_wrong_version_and_parent(self):
        self.record(101)["caption"]["version_id"] = ROOT
        self.run_cli(ok=False, error="metadata disagrees")
        self.record(101)["caption"]["version_id"] = VERSION
        self.record(101)["caption"]["parent_id"] = FILE
        self.run_cli(ok=False, error="metadata disagrees")

    def test_backlink_optional_but_if_set_checked(self):
        self.record(101)["caption"]["file_commit_id"] = "110"
        self.assertEqual(self.run_cli(), self.data)
        self.record(101)["caption"]["file_commit_id"] = "115"
        self.run_cli(ok=False, error="backlink disagrees")

    def test_filename_suffix(self):
        for filename in ("hello.txt", "hello.txt.01", "hello.txt.2", "hello.txt.-1", ".0", "hello.txt.99999999999999999999"):
            self.record(101)["filename"] = filename
            self.run_cli(ok=False, error="filename suffix")
        self.record(101)["filename"] = "report.v2.txt.0"
        self.assertEqual(self.run_cli(), self.data)

    def test_metadata_decimal_overflow(self):
        for value in ("9223372036854775808", "-1", "01", "1e3", " 6", 6):
            self.record(110)["caption"]["file_size"] = value
            self.run_cli(ok=False, error="invalid FILE_COMMIT")

    def test_ranges(self):
        body = json.loads((self.path / "110.json").read_text())
        body["parts"][1]["offset"] = "4"
        self.write("110.json", body)
        self.run_cli(ok=False, error="range or reference")

    def test_part_count(self):
        self.record(110)["caption"]["part_count"] = 3
        self.run_cli(ok=False, error="invalid FILE_COMMIT")

    def test_unknown_format_and_missing_navigation(self):
        self.record(110)["caption"]["format_version"] = 2
        self.run_cli(ok=False, error="unsupported format_version")
        self.record(110)["caption"]["format_version"] = 1
        del self.record(110)["caption"]["next_message"]
        self.run_cli(ok=False, error="invalid FILE_COMMIT envelope")

    def test_duplicate_message_id(self):
        self.snapshot["messages"].append(copy.deepcopy(self.record(110)))
        self.run_cli(ok=False, error="duplicate message_id")

    def test_duplicate_directory_names(self):
        self.write("10.json", dict(entries=[dict(name="a", object_id=FILE), dict(name="a", object_id=EMPTY)]))
        self.run_cli("ls", "/documents", ok=False, error="invalid DIRECTORY")

    def test_ambiguous_directory(self):
        self.add(20, envelope("DIRECTORY", object_id=DIRECTORY, parent_id=ROOT), dict(entries=[]))
        self.run_cli("ls", "/documents", ok=False, error="ambiguous DIRECTORY")

    def test_repeat_does_not_promote_old_version(self):
        # A/110, B/120 (empty), retry A/130: B remains current.
        b = copy.deepcopy(self.record(110)["caption"])
        b.update(operation_id="3f9c7148-61a2-4c70-9a8d-53f98166e512", version_id=ROOT,
                 file_size="0", part_count=0, file_sha256=SHA(b""))
        self.add(120, b, dict(parts=[]))
        a = copy.deepcopy(self.record(110)["caption"])
        a["next_message"] = "140"
        original_parts = json.loads((self.path / "110.json").read_text())
        self.add(130, a, original_parts)
        self.snapshot["messages"].reverse()  # replay order must not change selection
        self.assertEqual(self.run_cli(), b"")

    def test_conflicting_repeat(self):
        a = copy.deepcopy(self.record(110)["caption"])
        a["version_id"] = ROOT
        self.add(130, a, json.loads((self.path / "110.json").read_text()))
        self.run_cli(ok=False, error="operation_id reused")

    def test_whole_file_hash(self):
        self.record(110)["caption"]["file_sha256"] = SHA(b"xxxxxx")
        for mid in (101, 104):
            self.record(mid)["caption"]["file_sha256"] = SHA(b"xxxxxx")
        # Parts verify; the aggregate hash can fail after a verified prefix was emitted.
        self.assertEqual(self.run_cli(ok=False, error="whole-file SHA-256"), self.data)

    def test_duplicate_json_keys_and_nul(self):
        for raw in (b'{"entries": [], "entries": []}',
                    b'{"entries":[{"name":"x\\u0000y","object_id":"' + FILE.encode() + b'"}]}',
                    b'{"entries": []} trailing', b'{"entries": [], "n":01}',
                    b'{"entries": [], "x":"\xff"}', b'\xef\xbb\xbf{"entries": []}'):
            self.write("10.json", raw)
            self.run_cli("ls", "/documents", ok=False, error="invalid JSON")

    def test_snapshot_document_cannot_escape(self):
        self.record(101)["document"] = "../outside"
        self.run_cli(ok=False, error="invalid snapshot record")

    def test_symlink_document_not_followed(self):
        outside = self.path / "101.bin"
        outside.unlink()
        outside.symlink_to("104.bin")
        self.run_cli(ok=False, error="cannot open document")

    def test_chunk_metadata_limit_without_allocating_20mb(self):
        self.record(110)["caption"].update(part_count=1, file_size="20971521")
        self.write("110.json", dict(parts=[dict(message_id="101", offset="0", part_size="20971521", part_sha256=SHA(b""))]))
        self.run_cli(ok=False, error="range or reference")

    def directory_op(self, index, action='ADD', name='hello.txt', object_id=FILE, observed=None):
        import uuid
        op=dict(operation_id=str(uuid.UUID(int=(4<<76)|(2<<62)|index)), action=action, name=name)
        if action=='ADD': op['object_id']=object_id
        else: op['observed_adds']=observed
        return op

    def operation_directory(self, *ops):
        self.write('10.json',dict(operations=list(ops)))

    def directory_batch(self, mid, *ops, directory=DIRECTORY, parent=ROOT):
        return self.add(mid,envelope('DIRECTORY',object_id=directory,parent_id=parent),dict(operations=list(ops)))

    def test_directory_independent_additions_merge_and_read(self):
        hello=self.directory_op(1)
        self.operation_directory(hello)
        self.directory_batch(20,self.directory_op(2,name='notes.txt',object_id=EMPTY))
        self.directory_batch(30,self.directory_op(3,name='photo.jpg',object_id=EMPTY))
        expected=b'hello.txt\nnotes.txt\nphoto.jpg\n'
        self.assertEqual(self.run_cli('ls','/documents'),expected)
        self.snapshot['messages'].reverse()
        self.assertEqual(self.run_cli('ls','/documents'),expected)
        self.assertEqual(self.run_cli(),self.data)

    def test_directory_remove_and_old_retry_do_not_resurrect(self):
        add=self.directory_op(1)
        self.operation_directory(add)
        remove=self.directory_op(2,'REMOVE',observed=[add['operation_id']])
        self.directory_batch(20,remove)
        self.directory_batch(30,add)
        self.directory_batch(40,remove)
        self.assertEqual(self.run_cli('ls','/documents'),b'')
        self.run_cli(ok=False,error='path does not exist')

    def test_directory_unobserved_add_survives_remove(self):
        add=self.directory_op(1)
        self.operation_directory(add)
        self.directory_batch(20,self.directory_op(2,name='hello.txt',object_id=FILE))
        self.directory_batch(30,self.directory_op(3,'REMOVE',observed=[add['operation_id']]))
        self.assertEqual(self.run_cli('ls','/documents'),b'hello.txt\n')
        self.assertEqual(self.run_cli(),self.data)

    def test_directory_readd_after_remove(self):
        add=self.directory_op(1)
        self.operation_directory(add)
        self.directory_batch(20,self.directory_op(2,'REMOVE',observed=[add['operation_id']]))
        self.directory_batch(30,self.directory_op(3))
        self.assertEqual(self.run_cli(),self.data)

    def test_directory_same_name_different_object_conflict(self):
        self.operation_directory(self.directory_op(1))
        self.directory_batch(20,self.directory_op(2,object_id=EMPTY))
        self.run_cli('ls','/documents',ok=False,error='name conflict')

    def test_directory_conflict_resolved_by_observed_remove(self):
        a,b=self.directory_op(1),self.directory_op(2,object_id=EMPTY)
        self.operation_directory(a)
        self.directory_batch(20,b)
        self.directory_batch(30,self.directory_op(3,'REMOVE',observed=[b['operation_id']]))
        self.assertEqual(self.run_cli(),self.data)

    def test_directory_missing_or_wrong_remove_target(self):
        a=self.directory_op(1)
        for target,name in ((self.directory_op(9)['operation_id'],'hello.txt'),(a['operation_id'],'wrong.txt')):
            self.operation_directory(a)
            self.write('10.json',dict(operations=[a,self.directory_op(2,'REMOVE',name=name,observed=[target])]))
            self.run_cli('ls','/documents',ok=False,error='observed ADD')

    def test_directory_operation_id_reuse_is_rejected(self):
        a=self.directory_op(1)
        self.operation_directory(a)
        self.directory_batch(20,dict(a,name='different.txt'))
        self.run_cli(ok=False,error='operation_id reused')

    def test_directory_root_operations_and_empty_root(self):
        del self.snapshot['root_entries']
        self.assertEqual(self.run_cli('ls','/'),b'')
        self.directory_batch(20,self.directory_op(1,name='documents',object_id=DIRECTORY),directory=ROOT,parent=None)
        self.assertEqual(self.run_cli(),self.data)
        self.assertEqual(self.run_cli('ls','/'),b'documents\n')

    def test_directory_root_cannot_mix_with_adapter_seed(self):
        self.directory_batch(20,directory=ROOT,parent=None)
        self.run_cli(ok=False,error='root_entries cannot be mixed')

    def test_directory_cannot_merge_legacy_snapshots_and_operations(self):
        self.directory_batch(20,self.directory_op(1))
        self.run_cli(ok=False,error='document mode changed')

    def test_directory_parent_cannot_change_on_merge(self):
        self.operation_directory(self.directory_op(1))
        self.directory_batch(20,parent=DIRECTORY)
        self.run_cli(ok=False,error='parent or document mode changed')

    def test_volume_lock_modes(self):
        self.write('snapshot.json',self.snapshot)
        guard=self.path/'volume-lock'
        guard.mkdir(mode=0o700)
        prefix=[BINARY,'--volume-lock',str(guard)]
        read=['--snapshot',str(self.path),'cat','/documents/hello.txt']
        directory_fd=os.open(guard,os.O_RDONLY|os.O_DIRECTORY)
        try:
            fcntl.flock(directory_fd,fcntl.LOCK_SH)
            shared=subprocess.run([*prefix,*read],capture_output=True,timeout=5)
            self.assertEqual(shared.returncode,0,shared.stderr)
            self.assertEqual(shared.stdout,self.data)
            blocked=subprocess.run([*prefix,'gc-plan'],capture_output=True,timeout=5)
            self.assertEqual(blocked.returncode,1,blocked.stderr)
            self.assertIn(b'volume lock is busy',blocked.stderr)
            fcntl.flock(directory_fd,fcntl.LOCK_EX)
            for command in (read,['gc-plan']):
                blocked=subprocess.run([*prefix,*command],capture_output=True,timeout=5)
                self.assertEqual(blocked.returncode,1,blocked.stderr)
                self.assertIn(b'volume lock is busy',blocked.stderr)
                self.assertEqual(blocked.stdout,b'')
            help_result=subprocess.run([*prefix,'gc-plan','--help'],capture_output=True,timeout=5)
            self.assertEqual(help_result.returncode,0,help_result.stderr)
        finally:
            os.close(directory_fd)

        unlocked=subprocess.run([*prefix,*read],capture_output=True,timeout=5)
        self.assertEqual(unlocked.returncode,0,unlocked.stderr)
        link=self.path/'volume-lock-link';link.symlink_to(guard,target_is_directory=True)
        guard.chmod(0o755)
        for invalid in (guard,link,self.path/'missing'):
            rejected=subprocess.run([BINARY,'--volume-lock',str(invalid),*read],capture_output=True,timeout=5)
            self.assertEqual(rejected.returncode,1,rejected.stderr)
            self.assertIn(b'volume lock must be an existing private directory',rejected.stderr)

    def test_directory_malformed_operations(self):
        a=self.directory_op(1)
        for body in (dict(entries=[],operations=[]),dict(operations=[dict(a,action='INVALID')]),
                     dict(operations=[self.directory_op(2,'REMOVE',observed=[])]),
                     dict(operations=[dict(a,operation_id='invalid')])):
            self.write('10.json',body)
            self.run_cli(ok=False,error='invalid DIRECTORY')


if __name__ == "__main__":
    unittest.main()

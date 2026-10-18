#!/usr/bin/env python3
"""Opt-in runtime checks: QGA_SOCKET must identify a test SynapseOS VM."""
from pathlib import Path
import os,subprocess,sys,time
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'tests'))
from guest_runtime import Guest
work=ROOT/'pkgs/.work/unqlite'
subprocess.run([str(ROOT/'apps/apps/common/musl-cc'),'-O2','-I'+str(work/'payload/usr/include'),str(ROOT/'pkgs/unqlite/runtime-test.c'),str(work/'payload/usr/lib/libunqlite.a'),'-Wl,--wrap=pwrite','-o',str(work/'runtime-test')],check=True)
g=Guest()
try:
 g.write('/tmp/unqlite-test',(work/'runtime-test').read_bytes());g.write('/tmp/unqlite',(work/'payload/bin/unqlite').read_bytes())
 g.run('/bin/chmod',['755','/tmp/unqlite-test','/tmp/unqlite'])
 print(g.run('/tmp/unqlite',['--version']).decode().strip())
 path='/var/lib/unqlite-runtime.db';g.run('/bin/rm',['-rf',path,path+'.unqlite-lock',path+'_unqlite_journal','/tmp/unqlite-ready'])
 g.run('/tmp/unqlite-test',['init',path]);print(g.run('/tmp/unqlite-test',['verify',path]).decode().strip())
 pid=g.start('/tmp/unqlite-test',['hold',path])
 for i in range(50):
  status,out,err=g.result(g.start('/bin/cat',['/tmp/unqlite-ready']))
  if status.get('exitcode')==0 and out==b'ready':break
  time.sleep(.1)
 else:raise AssertionError('holder not ready')
 print(g.run('/tmp/unqlite-test',['busy',path]).decode().strip())
 status,out,err=g.result(pid);assert status.get('exitcode')==0,(status,out,err)
 print(g.run('/tmp/unqlite-test',['verify',path]).decode().strip())
 assert g.run('/tmp/unqlite',[path,'get','key']).decode()=='Привет\n'
 print('CLI fetch OK')
 assert g.run('/tmp/unqlite-test',['jx9',path])==b'Jx9 OK'
 print('Jx9 execution OK')
 before=g.read(path)
 status,out,err=g.result(g.start('/tmp/unqlite-test',['crash',path]))
 assert status.get('exitcode')==71,(status,out,err)
 journal=g.read(path+'_unqlite_journal')
 assert journal[:8]==bytes.fromhex('a6e8cd2b1c92db9f'),journal[:8]
 assert len(journal)>512,len(journal)
 assert g.read(path)!=before,'dirty pages did not spill to the database'
 print(g.run('/tmp/unqlite-test',['busy',path]).decode().strip())
 # No surviving users of this test DB: safely remove its stale directory lock.
 g.run('/bin/rmdir',[path+'.unqlite-lock'])
 print(g.run('/tmp/unqlite-test',['verify',path]).decode().strip())
 after=g.read(path)
 differences=[(i,a,b) for i,(a,b) in enumerate(zip(before,after)) if a!=b]
 assert after==before,('hot-journal rollback did not restore database bytes',len(before),len(after),differences[:16])
 print('Unclean process exit: hot journal, stale lock and recovery OK (not a power-cut test)')
finally:g.close()

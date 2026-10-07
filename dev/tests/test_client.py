"""Run actual client parsers, persistence and installer against a PSP I/O adapter.
The adapter preserves same-directory rename semantics and supports power cuts.
"""
import gzip, hashlib, json, os, pathlib, random, shutil, subprocess, tempfile, time, unittest, zipfile
BIN=os.environ.get('PSPDX_TEST_BIN','/tmp/pspdx-host-test')
IMAGE_BIN=os.environ.get('PSPDX_IMAGE_BIN')
SCHEMA='https://chriopter.github.io/pspdx/schema/pspdx-v1.json'
ID='io.github.test.demo'
PRESETS=['https://chriopter.github.io/pspdx-catalog/','https://pspdev.github.io/homebrew/']
SPEC=dict(schema=SCHEMA,source='https://github.com/test/demo',name='Demo',tags=['demo'],installdir='PSP/GAME/Demo',author='test',summary='Demo',license='MIT')
# A catalog stamped now: one a day old is asked about at the origin, which is another test.
NOW=lambda:time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())
def raw_zip(entries):
 """A stored zip whose names are the bytes given, with no UTF-8 flag: how a
 zip made on an old Windows keeps a name in its code page."""
 import struct,zlib
 local=b'';central=b''
 for name,data in entries.items():
  crc=zlib.crc32(data);off=len(local)
  local+=struct.pack('<IHHHHHIIIHH',0x04034b50,20,0,0,0,0,crc,len(data),len(data),len(name),0)+name+data
  central+=struct.pack('<IHHHHHHIIIHHHHHII',0x02014b50,20,20,0,0,0,0,crc,len(data),len(data),len(name),0,0,0,0,0,off)+name
 return local+central+struct.pack('<IHHHHIIH',0x06054b50,0,0,len(entries),len(entries),len(central),len(local),0)
class ClientTests(unittest.TestCase):
 def setUp(self):
  self.tmp=tempfile.TemporaryDirectory();self.root=pathlib.Path(self.tmp.name);(self.root/'ms0:').mkdir();(self.root/'ef0:').mkdir()
  self.spec=SPEC.copy();self.write('manifest.json',self.spec)
  self.zip('new.zip',{'EBOOT.PBP':b'new package','data.txt':b'new data'})
 def tearDown(self):self.tmp.cleanup()
 def write(self,path,data):
  p=self.root/path;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(json.dumps(data))
 def zip(self,name,entries):
  with zipfile.ZipFile(self.root/name,'w',compression=zipfile.ZIP_DEFLATED) as z:
   for path,data in entries.items():z.writestr(path,data)
 def run_client(self,*args,ok=True,**env):
  e=dict(os.environ,ASAN_OPTIONS='detect_leaks=0',ZIP_FILE=str(self.root/'new.zip'));e.update({k:str(v) for k,v in env.items()})
  r=subprocess.run([BIN,*map(str,args)],cwd=self.root,env=e,capture_output=True,text=True)
  self.assertNotIn('AddressSanitizer',r.stderr);self.assertNotIn('runtime error:',r.stderr)
  if ok:self.assertEqual(r.returncode,0,(args,r.stdout,r.stderr))
  return r
 def state(self):return {p.name[:-11]:json.loads(p.read_text()) for p in (self.root/'ms0:/PSP/PSPDX/INSTALLED').glob('*.state.json')}
 def second_record(self):
  record=json.loads(json.dumps(self.state()[ID]));record['source']='https://github.com/test/other';record['installed']['installdir']='PSP/GAME/Other'
  self.write('ms0:/PSP/PSPDX/INSTALLED/io.github.test.other.state.json',record)
  return record
 def test_install_target_survives_update_launch_path_and_removal(self):
  other=self.root/'ms0:/PSP/GAME/Demo';other.mkdir(parents=True);(other/'EBOOT.PBP').write_bytes(b'unmanaged')
  self.run_client('install',INSTALL_DEVICE='ef0:',VERSION=1)
  game=self.root/'ef0:/PSP/GAME/Demo';(game/'save.dat').write_bytes(b'my save')
  self.assertEqual(self.state()[ID]['installed']['device'],'ef0:')
  self.assertEqual(self.run_client('installed-path',ID).stdout.strip(),'ef0:/PSP/GAME/Demo/EBOOT.PBP')
  self.zip('new.zip',{'EBOOT.PBP':b'update','data.txt':b'updated data'})
  self.run_client('install',INSTALL_DEVICE='ms0:')
  self.assertEqual((game/'EBOOT.PBP').read_bytes(),b'update')
  self.assertEqual((game/'save.dat').read_bytes(),b'my save')
  self.assertEqual((other/'EBOOT.PBP').read_bytes(),b'unmanaged')
  self.run_client('remove');self.assertFalse(game.exists());self.assertTrue(other.exists())
 def test_go_alias_is_not_offered_as_a_second_install_device(self):
  (self.root/'ms0:').rmdir();(self.root/'ms0:').symlink_to('ef0:',target_is_directory=True)
  self.assertEqual(self.run_client('storage').stdout.strip(),'ef0: internal=1 card=0')
  self.assertNotEqual(self.run_client('install',INSTALL_DEVICE='ms0:',ok=False).returncode,0)
  self.assertFalse(list((self.root/'ef0:').glob('.pspdx-device-*')))
  self.assertFalse((self.root/'ef0:/PSP/GAME/Demo').exists())
 def test_go_separate_devices_remain_available(self):
  self.assertEqual(self.run_client('storage').stdout.strip(),'ms0: internal=1 card=1')
  self.assertEqual(self.run_client('storage',DEVICE='host0:/pspdx.prx').stdout.strip(),'ef0: internal=1 card=1')
  self.assertFalse(list((self.root/'ef0:').glob('.pspdx-device-*')))
 def test_install_from_internal_storage_to_card(self):
  boot='ef0:/PSP/GAME/PSPDX/EBOOT.PBP'
  self.run_client('install',DEVICE=boot,INSTALL_DEVICE='ms0:')
  state=json.loads((self.root/f'ef0:/PSP/PSPDX/INSTALLED/{ID}.state.json').read_text())
  self.assertEqual(state['installed']['device'],'ms0:')
  self.assertTrue((self.root/'ms0:/PSP/GAME/Demo/EBOOT.PBP').exists())
  self.assertFalse((self.root/'ef0:/PSP/GAME/Demo').exists())
  self.assertEqual(self.run_client('installed-path',ID,DEVICE=boot).stdout.strip(),'ms0:/PSP/GAME/Demo/EBOOT.PBP')
  self.run_client('remove',DEVICE=boot);self.assertFalse((self.root/'ms0:/PSP/GAME/Demo').exists())
 def test_install_device_is_validated(self):
  for dev in ['flash0:','host0:','ef0:/../ms0:','',42]:
   self.assertNotEqual(self.run_client('install',INSTALL_DEVICE=dev,ok=False).returncode,0)
  self.run_client('install')
  record=self.state()[ID];record['installed']['device']='flash0:'
  self.write(f'ms0:/PSP/PSPDX/INSTALLED/{ID}.state.json',record)
  self.assertNotEqual(self.run_client('remove',ok=False).returncode,0)
  self.assertTrue((self.root/'ms0:/PSP/GAME/Demo/EBOOT.PBP').exists())
 def test_missing_target_leaves_transaction_until_reinserted(self):
  self.run_client('install',INSTALL_DEVICE='ef0:',VERSION=1)
  snapshot=self.state();game=self.root/'ef0:/PSP/GAME/Demo'
  j=dict(id=ID,dir='Demo',prior='Demo',device='ef0:',phase='prepared',op='install',old_state=snapshot,old_manifest=json.dumps(SPEC))
  self.write('ms0:/PSP/PSPDX/TMP/transaction.json',j)
  (self.root/'ef0:').rename(self.root/'removed-card')
  self.run_client('recover')
  self.assertTrue((self.root/'ms0:/PSP/PSPDX/TMP/transaction.json').exists())
  self.assertNotEqual(self.run_client('install',ok=False).returncode,0)
  self.assertEqual(self.state(),snapshot)
  (self.root/'removed-card').rename(self.root/'ef0:');self.run_client('recover')
  self.assertFalse((self.root/'ms0:/PSP/PSPDX/TMP/transaction.json').exists())
  self.assertTrue((game/'EBOOT.PBP').exists())
 def test_cross_device_power_cuts(self):
  self.zip('new.zip',{'EBOOT.PBP':b'old','data.txt':b'old data'})
  self.run_client('install',INSTALL_DEVICE='ef0:',VERSION=1)
  (self.root/'ef0:/PSP/GAME/Demo/save.dat').write_bytes(b'save')
  for dev in ['ms0:','ef0:']:shutil.copytree(self.root/dev,self.root/('baseline-'+dev))
  self.zip('new.zip',{'EBOOT.PBP':b'new','data.txt':b'new data'})
  for op in ['fresh','update','move','remove']:
   self.write('manifest.json',dict(SPEC,installdir='PSP/GAME/Moved') if op=='move' else SPEC)
   for fault in range(1,220):
    for dev in ['ms0:','ef0:']:
     shutil.rmtree(self.root/dev);shutil.copytree(self.root/('baseline-'+dev),self.root/dev)
    if op=='fresh':
     shutil.rmtree(self.root/'ef0:/PSP/GAME/Demo')
     shutil.rmtree(self.root/'ms0:/PSP/PSPDX/INSTALLED')
    r=self.run_client('remove' if op=='remove' else 'install',fault,INSTALL_DEVICE='ef0:',ok=False)
    self.assertIn(r.returncode,[0,77],(op,fault,r.stderr))
    self.run_client('recover')
    state=self.state()
    self.assertFalse((self.root/'ms0:/PSP/PSPDX/TMP/transaction.json').exists(),(op,fault))
    self.assertFalse((self.root/'ms0:/PSP/GAME/Demo').exists(),(op,fault))
    if ID in state:
     ins=state[ID]['installed'];self.assertEqual(ins['device'],'ef0:')
     game=self.root/ins['device']/ins['installdir']
     self.assertEqual((game/'EBOOT.PBP').read_bytes(),b'old' if ins['version']=='1' else b'new',(op,fault))
     if op!='fresh':self.assertEqual((game/'save.dat').read_bytes(),b'save',(op,fault))
    else:self.assertFalse((self.root/'ef0:/PSP/GAME/Demo').exists(),(op,fault))
    if r.returncode==0:break
   else:self.fail('cross-device fault sweep did not complete: '+op)
 def test_space_is_checked_on_selected_device(self):
  self.assertNotEqual(self.run_client('install',INSTALL_DEVICE='ef0:',EF_FREE=1,ok=False).returncode,0)
  self.assertFalse((self.root/'ef0:/PSP/GAME/Demo').exists())
  self.assertEqual(self.state(),{})
  self.run_client('install',INSTALL_DEVICE='ef0:',EF_FREE=10000000)
 def test_missing_install_device_never_falls_back_to_other_drive(self):
  self.run_client('install',INSTALL_DEVICE='ef0:',VERSION=1)
  before=self.state()
  other=self.root/'ms0:/PSP/GAME/Demo';other.mkdir(parents=True)
  (other/'EBOOT.PBP').write_bytes(b'unmanaged other drive')
  (self.root/'ef0:').rename(self.root/'absent')
  self.assertNotEqual(self.run_client('install',INSTALL_DEVICE='ms0:',ok=False).returncode,0)
  self.assertNotEqual(self.run_client('remove',ok=False).returncode,0)
  self.assertEqual(self.state(),before)
  self.assertEqual((other/'EBOOT.PBP').read_bytes(),b'unmanaged other drive')
  (self.root/'absent').rename(self.root/'ef0:')
  self.run_client('remove')
  self.assertFalse((self.root/'ef0:/PSP/GAME/Demo').exists())
  self.assertTrue(other.exists())
 def test_legacy_install_record_keeps_startup_device(self):
  self.run_client('install',VERSION=1)
  record=self.state()[ID];del record['installed']['device']
  self.write(f'ms0:/PSP/PSPDX/INSTALLED/{ID}.state.json',record)
  self.assertEqual(self.run_client('installed-path',ID).stdout.strip(),'ms0:/PSP/GAME/Demo/EBOOT.PBP')
  self.run_client('install',INSTALL_DEVICE='ef0:')
  self.assertEqual(self.state()[ID]['installed']['device'],'ms0:')
  self.assertFalse((self.root/'ef0:/PSP/GAME/Demo').exists())
 def test_separate_records_and_latest(self):
  self.fixtures();other=self.second_record()
  path=self.root/'ms0:/PSP/PSPDX/INSTALLED/io.github.test.other.state.json'
  before=path.stat();installed=self.state()[ID]['installed']
  self.run_client('fetch')
  self.assertEqual(self.state()[ID]['installed'],installed)
  self.assertEqual(self.state()[ID]['latest']['version'],'2')
  self.assertEqual(self.state()['io.github.test.other'],other)
  self.assertEqual((path.stat().st_ino,path.stat().st_mtime_ns),(before.st_ino,before.st_mtime_ns))
  self.assertFalse((path.parent/'state.json').exists())
  self.run_client('remove');self.assertEqual(self.state(),{'io.github.test.other':other})
 def test_record_backup_recovery(self):
  self.run_client('install');expected=self.state()
  path=self.root/f'ms0:/PSP/PSPDX/INSTALLED/{ID}.state.json'
  path.rename(str(path)+'.bak')
  self.run_client('recover');self.assertEqual(self.state(),expected)
  path.write_text('{broken')
  self.assertNotEqual(self.run_client('install',ok=False).returncode,0)
  self.assertEqual(path.read_text(),'{broken')
 def test_rollback_only_restores_its_app(self):
  self.run_client('install',VERSION=1);other=self.second_record()
  snapshot=self.state()
  journal=dict(id=ID,dir='Demo',prior='Demo',phase='prepared',op='install',old_state=snapshot,old_manifest=json.dumps(SPEC))
  self.write('ms0:/PSP/PSPDX/TMP/transaction.json',journal)
  other['latest']['version']='newer-check'
  self.write('ms0:/PSP/PSPDX/INSTALLED/io.github.test.other.state.json',other)
  self.run_client('recover')
  self.assertEqual(self.state()['io.github.test.other'],other)
  self.assertFalse((self.root/'ms0:/PSP/PSPDX/TMP/transaction.json').exists())
 def test_manifest_validation(self):
  self.run_client('parse','manifest.json')
  for key,value in [('source','https://github.com/test/demo/issues'),('installdir','PSP/GAME/..'),('name','x'*41),('license','x'*61),('schema','old')]:
   with self.subTest(key=key):self.write('bad.json',dict(SPEC,**{key:value}));self.assertNotEqual(self.run_client('parse','bad.json',ok=False).returncode,0)
  for key in ['schema','source','name']:
   d=SPEC.copy();del d[key];self.write('bad.json',d);self.assertNotEqual(self.run_client('parse','bad.json',ok=False).returncode,0)
  self.write('unicode.json',dict(SPEC,name='ä'*39,license='GPL-2.0-or-later'));self.run_client('parse','unicode.json')
  for text in ['{"name":"first","name":"second"}',json.dumps(SPEC)+' garbage',json.dumps(SPEC).replace('Demo','D\\u0000emo')]:
   (self.root/'bad.json').write_text(text);self.assertNotEqual(self.run_client('parse','bad.json',ok=False).returncode,0)
 def test_install_and_remove(self):
  self.run_client('install');self.assertEqual(self.state()[ID]['installed']['version'],'2');self.assertEqual((self.root/'ms0:/PSP/GAME/Demo/EBOOT.PBP').read_bytes(),b'new package')
  self.assertEqual(json.loads((self.root/f'ms0:/PSP/PSPDX/INSTALLED/{ID}.pspdx').read_text()),SPEC)
  self.run_client('remove');self.assertNotIn(ID,self.state());self.assertFalse((self.root/'ms0:/PSP/GAME/Demo').exists())
 def test_collisions_and_corrupt_state(self):
  self.run_client('recover');d=self.root/'ms0:/PSP/GAME/Demo';d.mkdir();(d/'user.txt').write_text('keep')
  self.assertNotEqual(self.run_client('install',ok=False).returncode,0);self.assertEqual((d/'user.txt').read_text(),'keep')
  # Parked under .bak, the way the shell does it on the user's yes, the install goes through and the .bak stays.
  d.rename(str(d)+'.bak');self.run_client('install');self.assertEqual((d/'EBOOT.PBP').read_bytes(),b'new package');self.assertEqual((self.root/'ms0:/PSP/GAME/Demo.bak/user.txt').read_text(),'keep')
  self.run_client('remove');self.assertEqual((self.root/'ms0:/PSP/GAME/Demo.bak/user.txt').read_text(),'keep');shutil.rmtree(str(d)+'.bak')
  d.mkdir();(d/'user.txt').write_text('keep')
  shutil.rmtree(d);(self.root/'ms0:/PSP/PSPDX/INSTALLED/io.github.test.demo.state.json').write_text('{bad')
  self.assertNotEqual(self.run_client('install',ok=False).returncode,0)
 def test_bad_packages(self):
  for entries in [{'a/EBOOT.PBP':b'a','b/EBOOT.PBP':b'b'},{'NOT_EBOOT.PBP':b'a'},{'EBOOT.PBP':b'a','../escape':b'b'},{'EBOOT.PBP':b'a','same':b'a','same/child':b'b'},{'EBOOT.PBP':b'a','DATA':b'a','data':b'b'}]:
   with self.subTest(entries=entries):
    self.zip('new.zip',entries);self.assertNotEqual(self.run_client('install',ok=False).returncode,0);self.assertFalse((self.root/'ms0:/PSP/GAME/Demo').exists())
  self.zip('new.zip',{'EBOOT.PBP':b'a'});self.assertNotEqual(self.run_client('install',ok=False,BAD_HASH=1).returncode,0)
 def test_long_paths_can_be_removed_again(self):
  # rm_rf later names every file under PSP/GAME/<32 chars>.old/, and an update beside itself with .pspdx-old, so unpack refuses what either could not name.
  d='D'*32;self.write('manifest.json',dict(SPEC,installdir='PSP/GAME/'+d))
  self.zip('new.zip',{'EBOOT.PBP':b'a','f'*199:b'x'});self.assertNotEqual(self.run_client('install',ok=False).returncode,0);self.assertFalse((self.root/'ms0:/PSP/GAME'/d).exists())
  self.zip('new.zip',{'EBOOT.PBP':b'a','f'*198:b'x','a/'*40+'deep':b'y'});self.run_client('install');self.run_client('install',VERSION=3);self.run_client('remove')
  self.assertFalse((self.root/'ms0:/PSP/GAME'/d).exists());self.assertFalse((self.root/'ms0:/PSP/GAME'/(d+'.old')).exists());self.assertFalse((self.root/'ms0:/PSP/PSPDX/TMP/transaction.json').exists())
 def test_discard_clears_what_recovery_cannot(self):
  self.run_client('install',VERSION=1)
  tmp=self.root/'ms0:/PSP/PSPDX/TMP';(tmp/'transaction.json').write_text('{"id":"io.github.test.demo","dir":"Demo","phase":"wat"}');(tmp/'download.zip').write_bytes(b'x')
  stage=self.root/'ms0:/PSP/GAME/.pspdx-stage';stage.mkdir();(stage/'EBOOT.PBP').write_bytes(b'half')
  old=self.root/'ms0:/PSP/GAME/Demo.old';old.mkdir();(old/'save').write_text('keep')
  self.assertNotEqual(self.run_client('install',ok=False).returncode,0)
  r=self.run_client('discard');self.assertIn('Demo.old',r.stdout)
  self.assertEqual(sorted(p.name for p in tmp.iterdir()),[]);self.assertFalse(stage.exists());self.assertEqual((old/'save').read_text(),'keep')
  old.rename(str(old)[:-4]+'.bak');self.run_client('install');self.assertEqual(self.state()[ID]['installed']['version'],'2')
  self.run_client('discard')
 def test_recovery_leaves_a_directory_it_did_not_place(self):
  # Only "placed" with the stage gone says PSP/GAME/<dir> came out of the stage; before that word it is someone else's.
  self.run_client('recover');foreign=self.root/'ms0:/PSP/GAME/Foreign';foreign.mkdir();(foreign/'save.dat').write_text('precious')
  journal=lambda phase:dict(id='io.github.x.y',dir='Foreign',prior='',phase=phase,op='install',old_state={})
  self.write('ms0:/PSP/PSPDX/TMP/transaction.json',journal('ready'));self.run_client('recover')
  self.assertEqual((foreign/'save.dat').read_text(),'precious');self.assertFalse((self.root/'ms0:/PSP/PSPDX/TMP/transaction.json').exists())
  stage=self.root/'ms0:/PSP/GAME/.pspdx-stage';stage.mkdir();(stage/'EBOOT.PBP').write_bytes(b'half')
  self.write('ms0:/PSP/PSPDX/TMP/transaction.json',journal('placed'));self.run_client('recover')
  self.assertEqual((foreign/'save.dat').read_text(),'precious');self.assertFalse(stage.exists())
  self.write('ms0:/PSP/PSPDX/TMP/transaction.json',journal('placed'));self.run_client('recover');self.assertFalse(foreign.exists())
 def test_recovery_sweeps_what_a_cut_write_left(self):
  self.run_client('install');installed=self.root/'ms0:/PSP/PSPDX/INSTALLED';tmp=self.root/'ms0:/PSP/PSPDX/TMP';pspdx=self.root/'ms0:/PSP/PSPDX'
  for p in [installed/f'{ID}.state.json.new',installed/f'{ID}.state.json.bak',installed/f'{ID}.pspdx.new',tmp/'transaction.json.new',pspdx/'sources.txt.new']:p.write_text('stale')
  (installed/'io.github.test.other.state.json.bak').write_text(json.dumps(self.second_record()));(installed/'io.github.test.other.state.json').unlink()
  before=self.state()[ID];self.run_client('recover')
  self.assertEqual(sorted(p.name for p in installed.iterdir()),[f'{ID}.pspdx',f'{ID}.state.json','io.github.test.other.state.json']);self.assertEqual(self.state()[ID],before)
  self.assertEqual([p.name for p in tmp.iterdir()],[]);self.assertFalse((pspdx/'sources.txt.new').exists())
 def test_sources_file_keeps_its_shape(self):
  self.fixtures();path=self.root/'ms0:/PSP/PSPDX/sources.txt';(self.root/'requests.log').write_text('')
  path.write_bytes('\ufeff# mine\nhttps://example.com/catalog.json'.encode());self.run_client('add','test/demo')
  self.assertEqual(path.read_text(),'\ufeff# mine\nhttps://example.com/catalog.json\nhttps://github.com/test/demo\n')
  self.run_client('fetch');self.assertIn('https://example.com/catalog.json',(self.root/'requests.log').read_text())
  path.write_text('# a\nhttps://example.com/'+'x'*300+'\n');self.assertIn('too long',self.run_client('fetch',ok=False,VERBOSE=1).stderr)
  path.write_text('#'*20000);self.assertIn('over',self.run_client('fetch',ok=False,VERBOSE=1).stderr);self.assertEqual(len(path.read_text()),20000)
 def test_records_claiming_one_name_can_still_be_removed(self):
  # FAT is case-blind: a second record with PSP/GAME/demo refuses an install under Demo, but never a removal.
  self.run_client('install',VERSION=1);other=self.second_record();other['installed']['installdir']='PSP/GAME/demo'
  self.write('ms0:/PSP/PSPDX/INSTALLED/io.github.test.other.state.json',other)
  self.assertNotEqual(self.run_client('install',ok=False).returncode,0)
  self.run_client('remove');self.assertEqual(self.state(),{'io.github.test.other':other});self.assertFalse((self.root/'ms0:/PSP/GAME/Demo').exists())
 def test_pinned_record_still_updates_from_the_catalog(self):
  self.fixtures();record=self.state()[ID];record['source']=SPEC['source']+'@v1';self.write(f'ms0:/PSP/PSPDX/INSTALLED/{ID}.state.json',record)
  self.assertIn(ID+' 2 1',self.run_client('fetch').stdout);self.assertEqual(self.state()[ID]['latest']['version'],'2')
  self.run_client('install');self.assertEqual(self.state()[ID]['installed']['version'],'2')
 def test_recovery_does_not_restore_a_manifest_begin_could_not_read(self):
  self.run_client('install',VERSION=1)
  self.write('ms0:/PSP/PSPDX/TMP/transaction.json',dict(id=ID,dir='Demo',prior='Demo',phase='ready',op='install',old_state=self.state(),old_manifest='x'*100000))
  self.assertIn('not restored',self.run_client('recover',VERBOSE=1).stderr);self.assertFalse((self.root/f'ms0:/PSP/PSPDX/INSTALLED/{ID}.pspdx').exists())
  self.run_client('install');self.assertEqual(self.state()[ID]['installed']['version'],'2')
 def test_catalog_answers_that_must_not_cost_the_cache(self):
  self.fixtures();self.run_client('fetch');cache=next((self.root/'ms0:/PSP/PSPDX/CACHE/catalogs').glob('*.json'));saved=cache.read_bytes()
  catalog=json.loads((self.root/'catalog.json').read_text())
  self.write('catalog.json',dict(catalog,apps=[]));self.assertIn('names no apps',self.run_client('fetch',VERBOSE=1).stderr);self.assertEqual(cache.read_bytes(),saved)
  self.write('catalog.json',dict(catalog,pad='x'*4200000));r=self.run_client('fetch',VERBOSE=1);self.assertIn('larger than',r.stderr);self.assertNotIn('unreachable',r.stderr);self.assertEqual(cache.read_bytes(),saved)
  cache.unlink();r=self.run_client('fetch',VERBOSE=1);self.assertIn('catalog too large',r.stderr);self.assertNotIn('unreachable',r.stderr)
  first=catalog['apps'][0];other=dict(first,id='io.github.test.other',source='https://github.com/test/other',releases=[dict(first['releases'][0],url='https://github.com/test/other/releases/download/v2/download.zip')]);self.write('catalog.json',dict(catalog,apps=[catalog['apps'][0],other]))
  r=self.run_client('fetch',VERBOSE=1);self.assertIn('wants PSP/GAME/Demo, which %s has; not listed'%ID,r.stderr);self.assertIn(ID,r.stdout);self.assertNotIn('io.github.test.other',r.stdout)
 def test_ef0(self):
  self.run_client('install',DEVICE='ef0:/PSP/GAME/PSPDX/EBOOT.PBP');self.assertTrue((self.root/'ef0:/PSP/PSPDX/INSTALLED/io.github.test.demo.state.json').exists());self.assertFalse((self.root/'ms0:/PSP/PSPDX').exists())
 def test_power_cuts(self):
  # First install, update, move and remove: cut after each mutating syscall.
  self.zip('new.zip',{'EBOOT.PBP':b'old package','data.txt':b'old data'})
  self.run_client('install',VERSION=1)
  original=self.root/'baseline';shutil.copytree(self.root/'ms0:',original)
  self.zip('new.zip',{'EBOOT.PBP':b'new package','data.txt':b'new data'})
  for op in ['install','move','remove','fresh']:
   if op=='move':self.write('manifest.json',dict(SPEC,installdir='PSP/GAME/Moved'))
   else:self.write('manifest.json',SPEC)
   for fault in range(1,130):
    shutil.rmtree(self.root/'ms0:');shutil.copytree(original,self.root/'ms0:')
    if op=='fresh':shutil.rmtree(self.root/'ms0:/PSP')
    command='remove' if op=='remove' else 'install'
    r=self.run_client(command,fault,ok=False)
    self.assertIn(r.returncode,[0,77],(op,fault,r.stdout,r.stderr))
    self.run_client('recover')
    state=self.state()
    if ID in state:
     version=state[ID]['installed']['version'];self.assertIn(version,['1','2'])
     target=state[ID]['installed']['installdir'];self.assertTrue((self.root/'ms0:'/target/'EBOOT.PBP').exists(),(op,fault,state))
     self.assertEqual((self.root/'ms0:'/target/'EBOOT.PBP').read_bytes(),b'old package' if version=='1' else b'new package',(op,fault,version))
     saved=json.loads((self.root/f'ms0:/PSP/PSPDX/INSTALLED/{ID}.pspdx').read_text());self.assertEqual(saved['installdir'],target,(op,fault))
    else:self.assertFalse((self.root/'ms0:/PSP/GAME/Demo').exists(),(op,fault))
    self.assertFalse((self.root/'ms0:/PSP/PSPDX/TMP/transaction.json').exists(),(op,fault))
    if r.returncode==0:break
   else:self.fail('fault sweep did not reach completion: '+op)
 def test_write_failure_and_foreign_backup(self):
  self.run_client('install',VERSION=1)
  before=self.state()
  self.assertNotEqual(self.run_client('install',ok=False,WRITE_FAIL=1).returncode,0)
  self.run_client('recover');self.assertEqual(before,self.state())
  foreign=self.root/'ms0:/PSP/GAME/Unrelated.old';foreign.mkdir();(foreign/'keep').write_text('safe')
  self.run_client('recover');self.assertEqual((foreign/'keep').read_text(),'safe')
 def test_corrupt_catalog_preserves_cache(self):
  self.fixtures();self.run_client('fetch')
  (self.root/'catalog.json').write_text('{broken')
  self.run_client('fetch')
  self.assertIn(ID,self.run_client('fetch',OFFLINE=1).stdout)
 def test_storage_paths_are_registered(self):
  import re
  app=pathlib.Path(__file__).resolve().parents[2]/'app'
  registered=set(re.findall(r'\{"([^"\n]*)",', (app/'util/storage_paths.inc').read_text()))
  for path in app.rglob('*.c'):
   if 'lib' in path.relative_to(app).parts:continue
   for name in re.findall(r'storage_path\("([^"\n]*)"\)',path.read_text()):self.assertIn(name,registered,str(path))
 def test_embedded_manifest_matches_root(self):
  import re
  root=pathlib.Path(__file__).resolve().parents[2]
  header=(root/'app/util/self_manifest.h').read_text()
  literal=re.search(r'#define PSPDX_SELF_MANIFEST (.*)',header).group(1)
  self.assertEqual(json.loads(json.loads(literal)),json.loads((root/'.pspdx').read_text()))
 def test_mock_catalog_is_what_the_client_reads(self):
  # dev/mock-catalog writes the desk's catalog and records; the same parser that reads a published one reads them here, so the two cannot drift apart.
  import importlib.machinery,importlib.util
  path=pathlib.Path(__file__).resolve().parents[2]/'dev/mock-catalog'
  loader=importlib.machinery.SourceFileLoader('mock_catalog',str(path));spec=importlib.util.spec_from_loader('mock_catalog',loader);mock=importlib.util.module_from_spec(spec);loader.exec_module(mock)
  for name in ('icon.png','shot.png','film.pmf'):(self.root/name).write_bytes(name.encode())
  mock.assets=lambda work:({'icon':[str(self.root/'icon.png')],'screenshot':[str(self.root/'shot.png')],'video':[str(self.root/'film.pmf')]},str(self.root/'new.zip'))
  work=self.root/'work';mock.make(str(work),str(self.root/'ms0:'),'https://example.com/')
  catalog=json.loads((work/'mock-site/catalog.json').read_text())
  # The host build has no loophole for a loopback package URL; the address is the one thing rewritten before the parser sees it.
  for app in catalog['apps']:app['releases'][0]['url']='%s/releases/download/%s/download.zip'%(app['source'],app['releases'][0]['tag'])
  self.write('catalog.json',catalog);(self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('https://example.com/catalog.json\n')
  r=self.run_client('fetch',VERBOSE=1);rows=[line.split() for line in r.stdout.splitlines()]
  self.assertEqual(len(rows),mock.COUNT,r.stderr);self.assertTrue(all(row[0].startswith(mock.ID_PREFIX) and row[2]=='1' for row in rows))
  states=[int(row[3]) for row in rows];third=mock.COUNT//3
  self.assertEqual((states.count(1),states.count(2),states.count(3)),(mock.COUNT-2*third,third,third),r.stdout)
  self.assertTrue(any(app.get('media',{}).get('video','').endswith('.pmf') for app in catalog['apps']))
 def fixtures(self):
  self.run_client('install',VERSION=1)
  size=(self.root/'new.zip').stat().st_size
  app=dict(id=ID,name='Demo',author='test',tags=['demo'],source=SPEC['source'],installdir=SPEC['installdir'],releases=[dict(tag='v2',published_at='1970-01-01T00:00:02Z',size=size,url='https://github.com/test/demo/releases/download/v2/download.zip')])
  self.write('catalog.json',dict(schema='https://chriopter.github.io/pspdx/schema/catalog-v1.json',generated_at=NOW(),apps=[app]));self.write('release.json',dict(tag_name='v3',published_at='2026-09-12T00:00:00Z',assets=[dict(name='download.zip',size=size,browser_download_url='https://github.com/test/demo/releases/download/v2/download.zip')]))
  (self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('https://example.com/catalog.json\n')
 def test_view_tabs_come_and_go(self):
  # One app, installed with a newer one published: stick, Homebrew, the UMD, which has no rows, and the gear; the basket's tab appears with the first package set aside and goes with it, and its going is what the caller is told.
  self.fixtures();r=self.run_client('view')
  self.assertEqual(r.stdout.splitlines(),['tabs 4: -2 0 -4 -3','tab 0 kind 0 rows 5 first -4 plan 0 0','tab -4 kind 4 rows 0 first -1 plan 0 0','tab -3 kind 3 rows 6 first -1000 plan 0 0','tab -2 kind 1 rows 2 first -2 plan 1 1','basket 1 kept 1 tabs 5 moved 0','basket tab rows 2 first -2 index 0 row 1','emptied kept 0 kind 0 tabs 4 moved 1'],r.stderr)
 def test_catalog_offline_fallback(self):
  self.fixtures();r=self.run_client('fetch');self.assertIn(ID+' 2 1',r.stdout)
  r=self.run_client('fetch',CATALOG_DOWN=1);self.assertIn(ID+' 3 1',r.stdout)
  r=self.run_client('fetch',OFFLINE=1);self.assertIn(ID+' 3 0',r.stdout)
  requests=(self.root/'requests.log').read_text();self.assertNotIn('ICON0',requests);self.assertNotIn('PIC1',requests)
 def test_catalog_base_uses_json_then_text_fallback(self):
  self.fixtures()
  base='https://example.com/pspdx/'
  (self.root/'ms0:/PSP/PSPDX/sources.txt').write_text(base+'\n')
  (self.root/'catalog.txt').write_text(SPEC['source']+'\n')
  (self.root/'requests.log').write_text('')
  self.assertIn(ID+' 2 1',self.run_client('fetch').stdout)
  self.assertEqual((self.root/'requests.log').read_text().splitlines(),[base+'catalog.json'])
  self.assertIn(ID+' 3 1',self.run_client('fetch',CATALOG_DOWN=1).stdout)
  requests=(self.root/'requests.log').read_text().splitlines()
  self.assertIn(base+'catalog.txt',requests)
  self.assertIn('https://raw.githubusercontent.com/test/demo/HEAD/.pspdx',requests)
  self.assertIn('https://api.github.com/repos/test/demo/releases/latest',requests)
 def test_default_catalog_source_and_text_only_site(self):
  self.fixtures()
  source=self.root/'ms0:/PSP/PSPDX/sources.txt'
  source.unlink()
  (self.root/'catalog.txt').write_text(SPEC['source']+'\n')
  self.assertIn(ID+' 3 1',self.run_client('fetch',CATALOG_DOWN=1).stdout)
  self.assertEqual(source.read_text().splitlines()[1:],PRESETS)
  self.assertIn('https://chriopter.github.io/pspdx-catalog/catalog.txt',
                (self.root/'requests.log').read_text())
 def plant_presets(self,text):
  # What a build's own list would be: the program carries it, so a test says another through the harness, not through a file on the stick.
  (self.root/'presets-of-a-newer-build.txt').write_text(text)
 def listed(self):return [l for l in (self.root/'ms0:/PSP/PSPDX/sources.txt').read_text().splitlines() if l and not l.startswith('#')]
 def seen_presets(self):return [l for l in (self.root/'ms0:/PSP/PSPDX/presets.seen').read_text().splitlines() if l and not l.startswith('#')]
 def test_presets_arrive_once_and_stay_removed(self):
  # A fresh stick gets the shipped list in its order, and the next start changes nothing.
  self.run_client('presets');self.assertEqual(self.listed(),PRESETS);self.assertEqual(self.seen_presets(),PRESETS)
  self.run_client('presets');self.assertEqual(self.listed(),PRESETS)
  # The user takes the second out: it stays out.
  path=self.root/'ms0:/PSP/PSPDX/sources.txt';path.write_text('# mine\n'+PRESETS[0]+'\n')
  self.run_client('presets');self.assertEqual(path.read_text(),'# mine\n'+PRESETS[0]+'\n')
  # A newer release brings a third: it arrives once, after what is there.
  third='https://example.com/third/';self.plant_presets(''.join(p+'\n' for p in PRESETS+[third]))
  self.run_client('presets');self.assertEqual(self.listed(),[PRESETS[0],third]);self.assertEqual(self.seen_presets(),PRESETS+[third])
  self.run_client('presets');self.assertEqual(self.listed(),[PRESETS[0],third])
 def test_presets_on_a_stick_that_has_the_first(self):
  # The first already there, spelt another way: only the second is appended, and both count as offered.
  path=self.root/'ms0:/PSP/PSPDX/sources.txt';path.parent.mkdir(parents=True,exist_ok=True)
  path.write_text('# mine\nhttps://Chriopter.github.io/pspdx-catalog\nhttps://example.com/catalog.json\n')
  self.run_client('presets');self.run_client('presets')
  self.assertEqual(path.read_text(),'# mine\nhttps://Chriopter.github.io/pspdx-catalog\nhttps://example.com/catalog.json\n'+PRESETS[1]+'\n')
  self.assertEqual(self.seen_presets(),PRESETS)
 def test_presets_of_the_program_or_with_bad_lines(self):
  path=self.root/'ms0:/PSP/PSPDX/sources.txt';path.parent.mkdir(parents=True,exist_ok=True);path.write_text('https://example.com/catalog.json\n')
  r=self.run_client('presets',VERBOSE=1)
  self.assertEqual(self.listed(),['https://example.com/catalog.json']+PRESETS)
  # Lines that are not sources are said and passed over; the good one still lands.
  path.write_text('https://example.com/catalog.json\n');(self.root/'ms0:/PSP/PSPDX/presets.seen').unlink()
  self.plant_presets('not a url\nhttp://example.com/plain/\nhttps://example.com/with space/\nhttps://'+'x'*300+'\n  https://example.com/good/  # a comment\n')
  r=self.run_client('presets',VERBOSE=1);self.assertEqual(r.stderr.count('skipped'),4,r.stderr)
  self.assertEqual(self.listed(),['https://example.com/catalog.json','https://example.com/good/'])
  # A file that names nothing at all is as good as missing.
  path.write_text('https://example.com/catalog.json\n');(self.root/'ms0:/PSP/PSPDX/presets.seen').unlink()
  self.plant_presets('\x01\x02 garbage\n');self.run_client('presets')
  self.assertEqual(self.listed(),['https://example.com/catalog.json']+PRESETS)
 def test_a_preset_that_does_not_answer_is_any_unreachable_source(self):
  self.fixtures();(self.root/'ms0:/PSP/PSPDX/sources.txt').write_text(''.join(p+'\n' for p in PRESETS))
  r=self.run_client('fetch',DOWN_HOST='pspdev.github.io');self.assertIn(ID,r.stdout)
  requests=(self.root/'requests.log').read_text();self.assertIn(PRESETS[1]+'catalog.json',requests);self.assertIn(PRESETS[1]+'catalog.txt',requests)
  lines=self.run_client('reach',DOWN_HOST='pspdev.github.io').stdout.splitlines()
  self.assertIn(PRESETS[0]+' ok',lines);self.assertIn(PRESETS[1]+' unreachable',lines)
 def test_a_source_that_does_not_load_is_marked_and_the_rest_still_load(self):
  # The second source does not answer: its apps are missing, the first's are there, and only the second is marked. The next fetch starts over.
  self.fixtures();down='https://down.example.org/pspdx/'
  (self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('https://example.com/catalog.json\n'+down+'\n')
  r=self.run_client('reach',DOWN_HOST='down.example.org',THEN_UP=1);first,again=r.stdout.split('--\n')
  self.assertEqual(first.splitlines(),[ID,'https://example.com/catalog.json ok',down+' unreachable'])
  self.assertEqual(again.splitlines(),[ID,'https://example.com/catalog.json ok',down+' ok'])
 def test_manage_sources_says_how_each_source_loaded(self):
  # Sources: Add Source on top, then each source with its status line, and on the right its URL, a sentence, its kind, its apps and when it loaded. A catalog that answers only from its saved copy says so; one that does not answer has no apps and no time.
  self.fixtures();down='https://down.example.org/pspdx/'
  (self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('https://example.com/catalog.json\n'+down+'\n')
  lines=self.run_client('manage',DOWN_HOST='down.example.org').stdout.splitlines()
  self.assertEqual(lines[0],'Add Source |  |  | Adds a catalog site, a catalog.json or a text list.')
  self.assertEqual(lines[1],'Direct Install |  |  | Installs an app from GitHub or from the INBOX folder.')
  self.assertEqual(lines[2],'example.com | catalog.json, 1 app | https://example.com/catalog.json | Catalog file. Deleting it does not delete installed apps.')
  self.assertEqual(lines[3:5],['  Kind: catalog.json','  Apps: 1'])
  self.assertRegex(lines[5],r'^  Loaded: \d{4}-\d\d-\d\d \d\d:\d\d UTC$')
  self.assertEqual(lines[6:],['down.example.org | Catalog site, unreachable | '+down+' | Could not be loaded. Its apps are not displayed. Deleting it does not delete installed apps.','  Kind: Catalog site','up from the top: 3'])
  lines=self.run_client('manage',CATALOG_DOWN=1,DOWN_HOST='down.example.org').stdout.splitlines()
  self.assertEqual(lines[2:5],['example.com | catalog.json, offline copy, 1 app | https://example.com/catalog.json | Could not be loaded. The saved copy is displayed. Deleting it does not delete installed apps.','  Kind: catalog.json','  Apps: 1'])
  self.assertEqual(lines[5],'  Loaded: saved copy')
  (self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('https://github.com/test/demo\n')
  self.assertEqual(self.run_client('manage').stdout.splitlines()[2].split(' | ')[:2],['test/demo','Repository, 1 app'])
 def test_the_program_carries_its_presets_and_reads_none_from_the_stick(self):
  import re
  root=pathlib.Path(__file__).resolve().parents[2]
  header=(root/'app/util/self_presets.h').read_text().split('#define',1)[1]
  embedded=''.join(re.findall(r'"((?:[^"\\]|\\.)*)"',header)).replace('\\n','\n').splitlines()
  self.assertEqual(embedded,PRESETS);self.assertFalse((root/'app/presets.txt').exists())
  # A presets.txt an older release left beside the EBOOT is not read: what it names is not offered, and it stays where it is.
  old=self.root/'ms0:/PSP/GAME/PSPDX/presets.txt';old.parent.mkdir(parents=True,exist_ok=True);old.write_text('https://example.com/old/\n')
  self.run_client('presets');self.assertEqual(self.listed(),PRESETS);self.assertEqual(old.read_text(),'https://example.com/old/\n')
 def test_the_program_carries_the_font_of_its_assets(self):
  # gui/font_data.h is app/assets/font.pgf byte for byte (dev/embed font writes it): the font the program falls back on is the one the notice is about.
  import re
  root=pathlib.Path(__file__).resolve().parents[2];header=(root/'app/gui/font_data.h').read_text()
  data=bytes(int(x,16) for x in re.findall(r'0x([0-9a-f]{2})',header.split('{',1)[1]))
  self.assertEqual(data,(root/'app/assets/font.pgf').read_bytes());self.assertIn('font_data[%d]'%len(data),header)
 def test_unknown_catalog_schema_uses_original_source(self):
  # Another version of PSPDX's catalog schema is not read, and the app comes from its repository. Anything else -- none, a relative path to a copy, another site, a word, a number -- is v1, and said.
  self.fixtures()
  catalog=json.loads((self.root/'catalog.json').read_text())
  for schema in ['https://chriopter.github.io/pspdx/schema/catalog-v2.json','https://chriopter.github.io/pspdx/schema/catalog-v10.json']:
   with self.subTest(schema=schema):
    self.write('catalog.json',dict(catalog,schema=schema));r=self.run_client('fetch',VERBOSE=1);self.assertIn(ID+' 3 ',r.stdout);self.assertIn('another version of the catalog schema; not read',r.stderr)
  for schema in ['schemas/catalog.schema.json','https://example.com/other-format','https://chriopter.github.io/pspdx/schema/catalog-v1.json.bak','',None,7]:
   with self.subTest(schema=schema):
    self.write('catalog.json',dict(catalog,schema=schema));r=self.run_client('fetch',VERBOSE=1);self.assertIn(ID+' 2 1',r.stdout);self.assertIn('read as v1',r.stderr)
  del catalog['schema'];self.write('catalog.json',catalog);r=self.run_client('fetch',VERBOSE=1);self.assertIn(ID+' 2 1',r.stdout);self.assertNotIn('read as v1',r.stderr)
 def test_sharkwouters_catalog_reads_as_v1(self):
  # dev/testdata/wijsman-catalog-2026-09-15.json is https://wijsman.de/psp-homebrew-database/catalog.json as served on 2026-09-15, byte for byte:
  # a schema by a relative path to his own copy of it, ids of its own that are not catalog ids, tags that are not GitHub's (v1.1 for 1.1, 0.0.3 for 0.0.3-psp),
  # PanelPop a GitHub pre-release, Laser Kombat's source with a slash at its end.
  root=pathlib.Path(__file__).resolve().parents[2]
  catalog=json.loads((root/'dev/testdata/wijsman-catalog-2026-09-15.json').read_text());self.assertEqual(catalog['schema'],'schemas/catalog.schema.json')
  self.assertEqual(catalog['apps'][0]['source'],'https://github.com/sharkwouter/laserkombat/')
  # Stamped now, as fixtures() does: a day-old list is asked about at the origin, which is another test.
  self.write('catalog.json',dict(catalog,generated_at=NOW()))
  base=PRESETS[1];(self.root/'ms0:/PSP/PSPDX').mkdir(parents=True);(self.root/'ms0:/PSP/PSPDX/sources.txt').write_text(base+'\n');(self.root/'requests.log').write_text('')
  ids=['io.github.sharkwouter.laserkombat','io.github.sharkwouter.oceanpop','io.github.sharkwouter.panelpop']
  r=self.run_client('fetch',VERBOSE=1)
  # The preset is the site's base, and the base is where catalog.json is asked for, and nothing else.
  self.assertEqual((self.root/'requests.log').read_text().splitlines(),[base+'catalog.json'])
  self.assertEqual([l.split() for l in r.stdout.splitlines()],[[ids[0],'1.1','1','1','0'],[ids[1],'2.0','1','1','0'],[ids[2],'0.0.3','1','1','0']],r.stderr)
  self.assertIn('"laser_kombat" is not an id this stick can use; listed as '+ids[0],r.stderr)
  for app_id,app in zip(ids,catalog['apps']):
   with self.subTest(app=app_id):self.assertEqual(self.run_client('sha',app_id).stdout.split(),[app['releases'][0]['sha256'],app['releases'][0]['url']])
  self.assertEqual(self.run_client('media',ids[0]).stdout.splitlines()[:2],[base+'icons/laser_kombat.png',base+'screenshots/laser_kombat.png'])
  # Installed, the hash decides and the version string does not: the same zip under another tag is current, another zip under the same tag is an update.
  for app,version,sha in [(catalog['apps'][1],'2.0-psp',catalog['apps'][1]['releases'][0]['sha256']),(catalog['apps'][2],'0.0.3','11'*32)]:
   self.write('ms0:/PSP/PSPDX/INSTALLED/io.github.sharkwouter.%s.state.json'%app['id'],dict(source=app['source'],installed=dict(installdir='PSP/GAME/'+app['name'],version=version,published_at=7,sha256=sha)))
  r=self.run_client('fetch',VERBOSE=1);self.assertEqual({l.split()[0]:l.split()[3] for l in r.stdout.splitlines()},{ids[0]:'1',ids[1]:'2',ids[2]:'3'},r.stderr)
  # The slash at the end of a source is the same repository: a record written without it is the same app, one row, current by its hash, and asked at the origin it is still that one.
  laser=catalog['apps'][0];self.write('ms0:/PSP/PSPDX/INSTALLED/%s.state.json'%ids[0],dict(source='https://github.com/sharkwouter/laserkombat',installed=dict(installdir='PSP/GAME/laserkombat',version='1.1',published_at=7,sha256=laser['releases'][0]['sha256'])))
  r=self.run_client('fetch',VERBOSE=1);rows=[l.split() for l in r.stdout.splitlines()];self.assertEqual([row[0] for row in rows],ids,r.stderr);self.assertEqual(rows[0][3],'2',r.stderr)
  self.write('catalog.json',dict(catalog,generated_at=NOW(),apps=[dict(laser,id='other.name.laser')]));self.assertEqual([l.split()[0] for l in self.run_client('fetch').stdout.splitlines()][0],ids[0])
  (self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('https://github.com/sharkwouter/laserkombat/\n');self.write('manifest.json',dict(SPEC,source=laser['source'],installdir='PSP/GAME/laserkombat'))
  self.write('release.json',dict(tag_name='v1.1',published_at='2026-09-12T00:00:00Z',assets=[dict(name='LaserKombat-PSP.zip',size=10,browser_download_url='https://github.com/sharkwouter/laserkombat/releases/download/v1.1/LaserKombat-PSP.zip')]))
  r=self.run_client('fetch',FORCE=1,VERBOSE=1);self.assertEqual([l.split()[0] for l in r.stdout.splitlines()],[ids[0]],r.stderr);self.assertEqual(sorted(self.state()),ids)
 def test_invalid_catalog_release_date_uses_original_source(self):
  self.fixtures()
  catalog=json.loads((self.root/'catalog.json').read_text())
  catalog['apps'][0]['releases'][0]['published_at']='2026-09-12T99:00:00Z'
  self.write('catalog.json',catalog)
  self.assertIn(ID+' 3 1',self.run_client('fetch').stdout)
 def test_force_and_missing_catalog(self):
  self.fixtures();r=self.run_client('fetch',FORCE=1);self.assertIn(ID+' 3 1',r.stdout)
  (self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('');r=self.run_client('fetch');self.assertIn(ID+' 3 0',r.stdout)
 def test_old_update_check_field_is_read_and_ignored(self):
  self.fixtures()
  state=self.state()[ID];state['update_check']='source'
  self.write('ms0:/PSP/PSPDX/INSTALLED/'+ID+'.state.json',state)
  self.assertIn(ID+' 2 1',self.run_client('fetch').stdout)
 def test_sources_added_only_when_valid(self):
  self.fixtures()
  base='https://example.com/other/'
  self.run_client('add',base)
  self.assertIn(base,(self.root/'ms0:/PSP/PSPDX/sources.txt').read_text())
  self.run_client('add','test/demo')
  path=self.root/'ms0:/PSP/PSPDX/sources.txt';before=path.read_text()
  self.assertIn(SPEC['source'],before)
  self.assertNotEqual(self.run_client('add','https://example.com/broken.json',ok=False).returncode,0)
  self.assertEqual(path.read_text(),before)
  (self.root/'catalog.txt').write_text(SPEC['source']+'\n')
  self.run_client('add','https://example.com/text-only/',CATALOG_DOWN=1)
  self.assertIn('https://example.com/text-only/',path.read_text())
  before=path.read_text()
  self.assertNotEqual(self.run_client('add','test/demo',ok=False,OFFLINE=1).returncode,0)
  self.assertEqual(path.read_text(),before)
 def test_inbox_success_removes_only_successful_files(self):
  self.fixtures()
  self.write('ms0:/PSP/PSPDX/INBOX/one.pspdx',SPEC)
  self.write('ms0:/PSP/PSPDX/INBOX/two.pspdx',SPEC)
  self.write('ms0:/PSP/PSPDX/INBOX/broken.pspdx',{'source':'broken'})
  self.run_client('inboxinstall')
  inbox=self.root/'ms0:/PSP/PSPDX/INBOX'
  self.assertEqual([p.name for p in inbox.iterdir()],['broken.pspdx'])
 def test_inbox(self):
  self.fixtures();self.write('ms0:/PSP/PSPDX/INBOX/one.pspdx',SPEC);self.write('ms0:/PSP/PSPDX/INBOX/two.pspdx',SPEC)
  self.assertEqual(self.run_client('inbox').stdout.strip(),'1')
  self.write('ms0:/PSP/PSPDX/INBOX/two.pspdx',dict(SPEC,name='Other'));self.assertEqual(self.run_client('inbox').stdout.strip(),'0')
 def parse(self,spec,ok=True):
  (self.root/'p.json').write_text(json.dumps(spec,ensure_ascii=False),encoding='utf-8')
  r=self.run_client('parse','p.json',ok=False)
  self.assertEqual(r.returncode==0,ok,(spec,r.stdout,r.stderr));return r.stdout.strip()
 def test_manifest_v1_rules(self):
  base=dict(schema=SCHEMA,source='https://github.com/test/demo',name='Demo')
  # The three required fields are a file; the folder and the id come out of the repository.
  self.assertEqual(self.parse(base),'PSP/GAME/demo|homebrew|io.github.test.demo|')
  self.assertEqual(self.parse(dict(base,source='https://github.com/test/'+'r'*40+'.git/')),'PSP/GAME/'+'r'*32+'|homebrew|io.github.test.'+'r'*40+'|')
  self.assertEqual(self.parse(dict(base,tags=['game','Jeu de rôle'],type='plugin')),'|plugin|io.github.test.demo|game,Jeu de rôle')
  for good in [dict(base,author='é'*60),dict(base,license='l'*60),dict(base,license='Public domain, see README'),dict(base,tags=['c'*24]),dict(base,tags=['🎮'*24]),dict(base,tags=[]),dict(base,tags=['t%d'%i for i in range(8)]),
               dict(base,description='d'*2400+'\n'*100),dict(base,description='é'*2500),dict(base,listed_by='https://wijsman.de/psp-homebrew-database/'),dict(base,listed_by='http://wijsman.de/'),dict(base,listed_by=''),dict(base,listed_by=7),dict(base,type='iso'),dict(base,type='homebrew',installdir='PSP/GAME/Demo'),dict(base,summary='s'*60),dict(base,name='é'*39),dict(base,category='game'),dict(base,version='2.0',notes={'any':'thing'})]:
   with self.subTest(good=good):self.parse(good)
  for bad in [dict(base,author='a'*61),dict(base,license='l'*61),dict(base,tags=['c'*25]),dict(base,tags=['']),dict(base,tags='game'),dict(base,tags=['game','game']),dict(base,tags=['t%d'%i for i in range(9)]),dict(base,tags=[1]),dict(base,tags=['a\nb']),
              dict(base,description='d'*2501),dict(base,description='a\tb'),dict(base,description='a\r\nb'),dict(base,name='a\nb'),dict(base,summary='a\nb'),dict(base,author='a\tb'),dict(base,license='a\rb'),
              dict(base,type='theme'),dict(base,type=''),dict(base,type='Plugin'),dict(base,type='plugin',installdir='PSP/GAME/Demo'),dict(base,type='iso',installdir='PSP/GAME/Demo'),
              dict(base,source='http://github.com/test/demo'),dict(base,source='http://example.com/demo'),dict(base,source='https://github.com/test'),dict(base,source='https://github.com/test/demo/issues'),dict(base,source='https://'),dict(base,source='https://github.com/test/.pspdx-stage')]:
   with self.subTest(bad=bad):self.parse(bad,ok=False)
 def test_manifest_from_outside_github(self):
  # Any https source; the id is the source's host and the name, the folder the name. An old listed_by is read past.
  mirror=dict(schema=SCHEMA,source='https://www.Archive.org/details/psp-blocks',name='PSP Blocks!')
  self.assertEqual(self.parse(mirror),'PSP/GAME/PSPBlocks|homebrew|org.archive.pspblocks|')
  self.assertEqual(self.parse(dict(mirror,installdir='PSP/GAME/Blocks',tags=['game','demo'])),'PSP/GAME/Blocks|homebrew|org.archive.pspblocks|game,demo')
  self.assertEqual(self.parse(dict(mirror,source='https://user@lists.example.co.uk:8443?x')),'PSP/GAME/PSPBlocks|homebrew|uk.co.example.lists.pspblocks|')
  self.assertEqual(self.parse(dict(mirror,listed_by='https://wijsman.de/psp-homebrew-database/')),'PSP/GAME/PSPBlocks|homebrew|org.archive.pspblocks|')
  for bad in [dict(mirror,name='★ ★'),dict(mirror,source='https://-/'),dict(mirror,name='.pspdx-stage'),dict(mirror,source='http://archive.org/details/psp-blocks'),
              dict(mirror,source='https://owner.github.io/blocks/'),dict(mirror,source='https://GitHub.IO./')]:
   with self.subTest(bad=bad):self.parse(bad,ok=False)
 def test_a_category_is_one_word_of_the_file(self):
  # 1 to 24 characters and no control character, or the file is refused; a file saved from a catalog entry keeps it.
  for category in ('game','Rundenbasierte Strategie','\U0001f3ae'*24):
   with self.subTest(category=category):self.parse(dict(SPEC,category=category))
  for category in ('c'*25,'','one\ntwo',['game'],7):
   with self.subTest(category=category):self.parse(dict(SPEC,category=category),ok=False)
  catalog=self.from_the_entry();catalog['apps'][0].update(category='emulator',tags=['retro']);self.write('catalog.json',catalog)
  self.assertEqual(self.run_client('get',ID).stdout.strip(),'0');self.assertEqual((self.saved()['category'],self.saved()['tags']),('emulator',['retro']))
 def test_an_id_outside_github_is_the_source_host_and_the_name(self):
  # Outside GitHub the host of the source makes the id: lower case, no one logging in, no port, no www., labels backwards, then the name.
  made=lambda source:self.run_client('ids','x',source,'App').stdout.split('\n')[1]
  for url,app_id in [('https://wijsman.de/psp-homebrew-database/','de.wijsman.app'),('https://www.Wijsman.DE/list','de.wijsman.app'),
                     ('https://user@Lists.Example.co.uk:8443?x','uk.co.example.lists.app'),('https://example.org','org.example.app'),('https://wwwx.org/','org.wwwx.app')]:
   with self.subTest(url=url):self.assertEqual(made(url),app_id)
  for url in ['http://wijsman.de/','https://','https://www./','https://user@:8443/','https://'+'h'*300,'']:
   with self.subTest(url=url):self.assertEqual(made(url),'-')
 def test_install_without_installdir_takes_the_repository_name(self):
  self.write('manifest.json',{k:v for k,v in SPEC.items() if k!='installdir'})
  self.run_client('install');self.assertEqual((self.root/'ms0:/PSP/GAME/demo/EBOOT.PBP').read_bytes(),b'new package')
  installed=self.state()[ID]['installed'];self.assertEqual(installed['installdir'],'PSP/GAME/demo')
  self.assertEqual(installed['sha256'],hashlib.sha256((self.root/'new.zip').read_bytes()).hexdigest())
 # ---- plugins: the folder of the .prx under seplugins/<its name>/, and PLUGINS.TXT only ever written in place
 PRX='ms0:/seplugins/usbnet/usbnet.prx';OLDPRX='ms0:/seplugins/usbnet.prx';OWN=b'always, ms0:/seplugins/usbnet/usbnet.prx, on ';OLDOWN=b'always, ms0:/seplugins/usbnet.prx, on ';USER=b'# my plugins\r\ngame, ms0:/seplugins/cheat.prx, on\r\nxmb, clock.prx, on\r\n'
 def plugin_setup(self,files=None,dir='seplugins',lst=None):
  plug={k:v for k,v in dict(SPEC,type='plugin').items() if k!='installdir'};self.write('manifest.json',plug)
  self.zip('new.zip',files or self.ONE)
  d=self.root/'ms0:'/dir
  if lst is not None:d.mkdir(exist_ok=True);(d/'PLUGINS.TXT').write_bytes(lst)
  return d
 ONE={'usbnet.prx':b'plugin one','LICENSE':b'MIT','docs/readme.txt':b'read me','sub/other.prx':b'not at the top'}
 @staticmethod
 def inside(files=None,name='usbnet'):
  # A plugin's folder as tree() shows it under seplugins/: the folder, every folder in it and every file.
  out={name:None}
  for path,data in (ClientTests.ONE if files is None else files).items():
   parts=path.split('/')
   for i in range(1,len(parts)):out[name+'/'+'/'.join(parts[:i])]=None
   out[name+'/'+path]=data
  return out
 def old_layout(self,prx=b'plugin one',on=True,lst=None,id=None,name='usbnet.prx',version='1'):
  # A stick as PSPDX 1.1 left it: the one file seplugins/<name>, its record, and its own line where it was turned on.
  id=id or ID;d=self.root/'ms0:/seplugins';d.mkdir(exist_ok=True);(d/name).write_bytes(prx);line=b'always, ms0:/seplugins/'+name.encode()+b', on '
  if on:(d/'PLUGINS.TXT').write_bytes((self.USER if lst is None else lst)+line+b'\r\n')
  elif lst is not None:(d/'PLUGINS.TXT').write_bytes(lst)
  spec=json.loads((self.root/'manifest.json').read_text());source=spec['source']
  rec=dict(added_from=source,source=source,installed=dict(version=version,published_at=int(version),installdir='seplugins/'+name,device='ms0:',sha256='ab'*32,plugin_sha256=hashlib.sha256(prx).hexdigest(),**({'plugin_line':'ms0:/seplugins/'+name} if on else {})))
  self.write(f'ms0:/PSP/PSPDX/INSTALLED/{id}.state.json',rec);self.write(f'ms0:/PSP/PSPDX/INSTALLED/{id}.pspdx',spec)
  return d
 def tree(self,d):
  # Every file under a folder, by its bytes: what must not change is compared whole.
  return {str(p.relative_to(d)):(p.read_bytes() if p.is_file() else None) for p in sorted(d.rglob('*'))} if d.exists() else None
 @staticmethod
 def ark_lines(t):
  # ARK's readLine: any byte below a space ends a line; a NUL ends the list. (line, start) each.
  out=[];i=0
  while i<len(t) and t[i]:
   j=i
   while j<len(t) and t[j]>=0x20:j+=1
   out.append((t[i:j],i))
   if j<len(t) and not t[j]:break
   i=j+1
  return out
 @staticmethod
 def ark(line):
  # ARK's processLine, written again from core/systemctrl/src/plugin.c: (run level, path as ARK resolves it, on) or None.
  l=line.strip(b' ')
  if not l or l[:2]==b'//' or l[:1] in (b';',b'#'):return None
  a=l.find(b',');b=l.find(b',',a+1) if a>=0 else -1
  if b<0:return None
  i=b+1
  while i<len(l) and l[i:i+1]!=b',' and l[i:i+2]!=b'//' and l[i:i+1] not in (b';',b'#'):i+=1
  path=l[a+1:b].strip(b' ');word=l[b+1:i].strip(b' ')
  full=path if b':' in path else b'ms0:/SEPLUGINS/'+path
  return l[:a].strip(b' ').lower(),full.lower(),(word.lower() in (b'true',b'on',b'enabled') or word==b'1')
 def ark_state(self,t,path=None):
  # What ARK makes of a list for one path: -1 unnamed, else what its last line says.
  state=-1
  for line,_ in self.ark_lines(t):
   p=self.ark(line)
   if p and p[1]==(path or self.PRX).encode().lower():state=int(p[2])
  return state
 def own_lines(self,t):
  # PSPDX's own line by its bytes, found without the client: (start, end, where its last field begins).
  import re
  head=b'always, '+self.PRX.encode()+b','
  return [(at,at+len(line),at+len(head)) for line,at in self.ark_lines(t) if line.startswith(head) and 3<=len(line)-len(head)<=8 and re.fullmatch(rb' *(on|off) *',line[len(head):])]
 def plist(self,op,path=None):
  r=self.run_client('pluginlist',op,'list.txt',path or self.PRX).stdout.split();return int(r[0]),int(r[1])
 def test_turning_on_again_takes_the_line_that_was_blanked(self):
  # Delete leaves spaces where PSPDX's line was; the next turning on writes the line over them, the same bytes as before, and the list is no longer than it was. Only a line of nothing but spaces with just that room is taken: a shorter one, a much longer one, one with a tab, one that is the list's unfinished end, and any other byte stay as they are, and the line goes after the end.
  f=self.root/'list.txt';own=self.OWN;head=b'; mine\r\ngame, ms0:/seplugins/other.prx, on\r\n';tail=b'umd, ms0:/seplugins/pad.prx, off\r\n'
  f.write_bytes(head+own+b'\r\n'+tail);first=f.read_bytes()
  for _ in range(3):
   self.assertEqual(self.plist('blank'),(1,-1));blank=f.read_bytes();self.assertEqual(blank,head+b' '*len(own)+b'\r\n'+tail)
   self.assertEqual(self.plist('add'),(0,1));self.assertEqual(f.read_bytes(),first);self.assertEqual(self.ark_state(first),1);self.assertEqual(len(self.own_lines(first)),1)
   self.assertEqual(self.plist('off'),(1,0));self.assertEqual(self.plist('on'),(1,1));self.assertEqual(f.read_bytes(),first)
  # Up to four spaces more than the line needs are still PSPDX's own line afterwards.
  f.write_bytes(head+b' '*(len(own)+4)+b'\n'+tail);self.assertEqual(self.plist('add'),(0,1));self.assertEqual(f.read_bytes(),head+own+b'    \n'+tail);self.assertEqual(self.plist('off'),(1,0));self.assertEqual(self.plist('blank'),(1,-1))
  for gap in (b' '*(len(own)-1)+b'\n',b' '*(len(own)+5)+b'\n',b' '*(len(own)-1)+b'\t\n',b' '*(len(own)-1)+b';\n',b'\n'):
   with self.subTest(gap=gap):
    f.write_bytes(head+gap+tail);self.assertEqual(self.plist('add'),(0,1));self.assertEqual(f.read_bytes(),head+gap+tail+own+b'\r\n')
  f.write_bytes(head+b' '*len(own));self.assertEqual(self.plist('add'),(0,1));self.assertEqual(f.read_bytes(),head+b' '*len(own)+b'\r\n'+own+b'\r\n')
 def test_plugin_list_is_written_in_place_or_after_its_end(self):
  # The three writes there are: a line after the list's end, the on or off of PSPDX's own line over itself, spaces over that line. The list never shrinks, no other byte changes, and ARK reads the result as meant.
  f=self.root/'list.txt';own=self.OWN
  for base,eol,gap in ((b'',b'\n',b''),(b'umd, ms0:/seplugins/other.prx, on\n',b'\n',b''),(b'umd, ms0:/seplugins/other.prx, on',b'\n',b'\n'),(self.USER,b'\r\n',b''),(self.USER[:-2],b'\r\n',b'\r\n'),
                       (b'; a comment\n\n\n',b'\n',b''),(b'x\ry\r',b'\n',b''),(b'a\tb\t',b'\n',b'\n'),(b'   ',b'\n',b'\n')):
   with self.subTest(base=base):
    f.write_bytes(base)
    for op in ('on','off','blank'):self.assertEqual(self.plist(op),(-1,-1));self.assertEqual(f.read_bytes(),base)
    self.assertEqual(self.plist('add'),(0,1));added=f.read_bytes();self.assertEqual(added,base+gap+own+eol);self.assertEqual(self.ark_state(added),1)
    at=len(base+gap);self.assertEqual(self.plist('on'),(0,1));self.assertEqual(f.read_bytes(),added)
    self.assertEqual(self.plist('off'),(1,0));off=f.read_bytes();self.assertEqual(off,base+gap+own[:-3]+b'off'+eol);self.assertEqual(self.ark_state(off),0)
    self.assertEqual(self.plist('off'),(0,0));self.assertEqual(self.plist('on'),(1,1));self.assertEqual(f.read_bytes(),added)
    self.assertEqual(self.plist('blank'),(1,-1));blank=f.read_bytes();self.assertEqual(blank,base+gap+b' '*len(own)+eol);self.assertEqual(self.ark_state(blank),-1)
    self.assertEqual([self.ark(l) for l,_ in self.ark_lines(blank)][:len(self.ark_lines(base))],[self.ark(l) for l,_ in self.ark_lines(base)])
    # Turned on again later, the line takes the place of its own spaces, and the list is as it was when it was first added.
    self.assertEqual(self.plist('add'),(0,1));self.assertEqual(f.read_bytes(),added)
  # What ARK's two managers make of the line when they write the list out is still PSPDX's, and is switched in the room it has.
  head=b'always, ms0:/seplugins/usbnet/usbnet.prx,'
  for before,op,after in ((head+b' on\n',b'off',head+b'off\n'),(head+b'off\n',b'on',head+b' on\n'),(head+b' off\n',b'on',head+b' on \n'),(head+b' on',b'off',head+b'off'),(head+b'  on   \r\nx',b'off',head+b' off   \r\nx')):
   f.write_bytes(b'# c\n'+before);self.assertEqual(self.plist(op.decode()),(1,int(op==b'on')));self.assertEqual(f.read_bytes(),b'# c\n'+after)
   self.assertEqual(self.plist('blank')[0],1);self.assertEqual(f.read_bytes(),b'# c\n'+b' '*len(after.split(b'\r')[0].split(b'\n')[0])+after[len(after.split(b'\r')[0].split(b'\n')[0]):])
  # Somebody's line for the same file is not PSPDX's, whatever it says: another run level, another spelling of the path or the word, a comment after it, a space before it, a field more, a longer run of spaces, and two that both look like PSPDX's.
  for text in (b'game, ms0:/seplugins/usbnet/usbnet.prx, on',b'always, MS0:/SEPLUGINS/usbnet/usbnet.prx, on',b'always, usbnet/usbnet.prx, on',b'Always, ms0:/seplugins/usbnet/usbnet.prx, on',b'always,ms0:/seplugins/usbnet/usbnet.prx, on',b'always, ms0:/seplugins/usbnet/usbnet.prx, 1',
               b'always, ms0:/seplugins/usbnet/usbnet.prx, ON',b'always, ms0:/seplugins/usbnet/usbnet.prx, on // mine',b'always, ms0:/seplugins/usbnet/usbnet.prx, on, 5',b' always, ms0:/seplugins/usbnet/usbnet.prx, on',b'always, ms0:/seplugins/usbnet/usbnet.prx,on',
               b'always, ms0:/seplugins/usbnet/usbnet.prx,        on',b'# always, ms0:/seplugins/usbnet/usbnet.prx, on',b'always, ms0:/seplugins/usbnet/usbnet.prx, on \nalways, ms0:/seplugins/usbnet/usbnet.prx, off\n',b'always, ms0:/seplugins/usbnet/usbnet.prx, on\0\n'):
   for op in ('on','off','blank'):
    f.write_bytes(text);self.assertEqual(self.plist(op)[0],-1,(text,op));self.assertEqual(f.read_bytes(),text)
  # The state is ARK's: the last line that names the file, in any of its spellings.
  for text,state in ((b'game, usbnet/usbnet.prx, on\numd, MS0:/SEPLUGINS/USBNET/USBNET.PRX, 0\n',0),(b'umd, usbnet/usbnet.prx, off\nalways, ms0:/seplugins/usbnet/usbnet.prx, Enabled // x',1),(b'# game, usbnet/usbnet.prx, on\n',-1),(b'game,\tusbnet.prx, on',-1),(b'x\0game, usbnet/usbnet.prx, on',-1),(b'game, ef0:/seplugins/usbnet/usbnet.prx, on',-1)):
   f.write_bytes(text);self.assertEqual(self.plist('state')[1],state,text);self.assertEqual(self.ark_state(text),state,text)
  # A path a line cannot hold is not written at all.
  f.write_bytes(b'');self.assertEqual(self.plist('add','ms0:/seplugins/a,b.prx')[0],-1);self.assertEqual(self.plist('add','ms0:/seplugins/a b.prx')[0],-1);self.assertEqual(f.read_bytes(),b'')
 def test_plugin_list_fuzz_against_arks_reader(self):
  # Random lists, each write held against ARK's reader written again here: a write never changes the length but by the line added, never a byte outside PSPDX's one own line, and ARK reads every other line as it did.
  rng=random.Random(7);f=self.root/'list.txt';P=self.PRX.encode()
  names=[P,P,P.upper(),b'usbnet/usbnet.prx',b'Usbnet/Usbnet.PRX',b'ms0:/seplugins/usbnet/myusbnet.prx',P+b'.bak',b'ef0:/seplugins/usbnet/usbnet.prx',b'ms0:/plugins/usbnet.prx',b'ms0:/seplugins/usbnet//usbnet.prx',b'other.prx',b'ms0:/seplugins/caf\xc3\xa9.prx']
  levels=[b'always',b'always',b'game',b'umd, ',b'',b'xmb',b'ULUS12345',b'ms0:/PSP/GAME/x/EBOOT.PBP',b'# c',b'// c',b'; c']
  words=[b'on',b'off',b'on',b'off',b'1',b'0',b'true',b'ON',b'Enabled',b'',b'on // c',b'off#x',b'on;y',b'on, 5',b'yes',b'on x']
  eols=[b'\n',b'\r\n',b'\r',b'\t',b'\n\n',b'\x0b',b'\x1f',b'']
  def line():
   k=rng.random()
   if k<0.15:return rng.choice([b'',b'   ',b'# comment, a, b',b'//x',b'junk',b'a,b',b'\xff\xfe junk \x80',b'x'*rng.choice([10,1100,3000])])
   if k<0.35:return b'always, '+P+b','+b' '*rng.choice([0,1,1,2])+rng.choice([b'on',b'off'])+b' '*rng.choice([0,0,1,3])
   sp=lambda:b' '*rng.choice([0,0,1,1,3])
   return sp()+rng.choice(levels)+sp()+b','+sp()+rng.choice(names)+sp()+b','+sp()+rng.choice(words)+sp()
  switched=added=0
  for it in range(350):
   t=b''.join(line()+rng.choice(eols) for _ in range(rng.randint(0,7)));own=self.own_lines(t);before=[self.ark(l) for l,_ in self.ark_lines(t)]
   for op in ('on','off','blank','add'):
    f.write_bytes(t);rc,state=self.plist(op);o=f.read_bytes();self.assertEqual(state,self.ark_state(o),(t,op,o))
    if op=='add':
     self.assertEqual(rc,0);self.assertTrue(o.startswith(t) and o[len(t):] in (self.OWN+e2 if not g else g+self.OWN+e2 for e2 in (b'\n',b'\r\n') for g in (b'',e2)),(t,o))
     after=[self.ark(l) for l,_ in self.ark_lines(o)];self.assertEqual([x for x in after if x][:len([x for x in before if x])],[x for x in before if x],(t,o))
     self.assertEqual([x for x in after if x][-1],(b'always',P.lower(),True),(t,o));self.assertEqual(len([x for x in after if x]),len([x for x in before if x])+1,(t,o));added+=1
     continue
    if len(own)!=1:self.assertEqual((rc,o),(-1,t),(t,op,o));continue
    start,end,field=own[0];self.assertEqual(len(o),len(t));self.assertEqual((o[:field if op!='blank' else start],o[end:]),(t[:field if op!='blank' else start],t[end:]),(t,op,o))
    if op=='blank':self.assertEqual(o[start:end],b' '*(end-start))
    else:self.assertEqual(self.ark(o[start:end]),(b'always',P.lower(),op=='on'),(t,op,o));self.assertEqual(len(self.own_lines(o)),1,(t,op,o));switched+=1
    self.assertEqual([self.ark(l) for l,a in self.ark_lines(o) if a!=start],[self.ark(l) for l,a in self.ark_lines(t) if a!=start],(t,op,o))
  self.assertGreater(switched,60);self.assertGreater(added,300)
 def test_plugin_is_installed_off_and_turned_on_by_a_line_of_its_own(self):
  # Installing is the plugin's folder and nothing else: the list is not made and not touched. Turning the plugin on puts one line after the list's end; off and on again write over its last field; an update is the files the release ships; deleting writes spaces over the line and takes the files and the folder, and the list is as long as it was.
  for folder,name,base in (('seplugins','PLUGINS.TXT',None),('seplugins','PLUGINS.TXT',b'# plugins\ngame, ms0:/seplugins/other.prx, on\n'),('SEPLUGINS','plugins.txt',self.USER),('SePlugins','Plugins.txt',b'game, ms0:/seplugins/other.prx, on')):
   with self.subTest(base=base,folder=folder):
    shutil.rmtree(self.root/'ms0:');(self.root/'ms0:').mkdir();d=self.plugin_setup(dir=folder);lst=d/name;eol=b'\r\n' if base and b'\r\n' in base else b'\n'
    if base is not None:d.mkdir();lst.write_bytes(base);(d/'other.prx').write_bytes(b'somebody else')
    r=self.run_client('install',VERSION=1);self.assertIn('plugin: usbnet.prx off',r.stderr)
    self.assertEqual(self.tree(d),dict(([(name,base),('other.prx',b'somebody else')] if base is not None else [])+list(self.inside().items())))
    self.assertEqual([p.name for p in (self.root/'ms0:').iterdir() if p.name.lower()=='seplugins'],[folder])
    installed=self.state()[ID]['installed'];self.assertEqual((installed['installdir'],installed['device'],installed['version']),('seplugins/usbnet/usbnet.prx','ms0:','1'))
    self.assertEqual(installed['plugin_sha256'],hashlib.sha256(b'plugin one').hexdigest());self.assertNotIn('plugin_line',installed);self.assertEqual(installed['plugin_files'],{k:hashlib.sha256(v).hexdigest() for k,v in self.ONE.items()})
    self.assertEqual(installed['sha256'],hashlib.sha256((self.root/'new.zip').read_bytes()).hexdigest())
    self.assertEqual(list((self.root/'ms0:/PSP/GAME').iterdir()),[]);self.assertEqual(json.loads((self.root/f'ms0:/PSP/PSPDX/INSTALLED/{ID}.pspdx').read_text())['type'],'plugin')
    self.assertNotEqual(self.run_client('installed-path',ID,ok=False).returncode,0);self.assertEqual(self.run_client('plugin',ID).stdout.strip(),'0')
    # Turned off while it is off: nothing is written, and no list is made for it.
    self.assertEqual(self.run_client('plugin',ID,'off').stdout.strip(),'0');self.assertEqual(lst.read_bytes() if base is not None else lst.exists(),base if base is not None else False)
    self.assertEqual(self.run_client('plugin',ID,'on').stdout.strip(),'1');old=base or b'';gap=eol if old and old[-1:] not in b'\r\n' else b'';on=old+gap+self.OWN+eol;self.assertEqual(lst.read_bytes(),on)
    self.assertEqual(self.state()[ID]['installed']['plugin_line'],self.PRX);self.assertEqual(self.ark_state(on),1)
    self.assertEqual(self.run_client('plugin',ID,'off').stdout.strip(),'0');off=old+gap+self.OWN[:-3]+b'off'+eol;self.assertEqual(lst.read_bytes(),off);self.assertEqual(self.run_client('plugin',ID).stdout.strip(),'0')
    # An update is the files the release ships, each by its hash, and what an earlier release left stays, in the folder and in the record; the line is not its business, and a plugin turned off stays off.
    self.zip('new.zip',{'usbnet.prx':b'plugin two, longer','README':b'x'});r=self.run_client('install');self.assertIn('plugin: usbnet.prx off',r.stderr)
    two=dict(self.ONE,**{'usbnet.prx':b'plugin two, longer','README':b'x'});self.assertEqual(self.tree(d),dict(([(name,off),('other.prx',b'somebody else')] if base is not None else [(name,off)])+list(self.inside(two).items())));installed=self.state()[ID]['installed'];self.assertEqual(installed['plugin_files'],{k:hashlib.sha256(v).hexdigest() for k,v in two.items()})
    self.assertEqual((installed['version'],installed['plugin_sha256'],installed['plugin_line']),('2',hashlib.sha256(b'plugin two, longer').hexdigest(),self.PRX))
    self.assertEqual(self.run_client('plugin',ID,'on').stdout.strip(),'1');self.assertEqual(lst.read_bytes(),on)
    r=self.run_client('install',VERSION=3);self.assertIn('plugin: usbnet.prx on',r.stderr);self.assertEqual(lst.read_bytes(),on)
    self.assertEqual(self.run_client('uninstall',ID).stdout.strip(),'0');gone=old+gap+b' '*len(self.OWN)+eol;self.assertEqual(lst.read_bytes(),gone);self.assertNotIn(ID,self.state())
    self.assertEqual(self.tree(d),dict([(name,gone)]+([('other.prx',b'somebody else')] if base is not None else [])));self.assertEqual(self.ark_state(gone),-1)
    self.assertFalse((self.root/f'ms0:/PSP/PSPDX/INSTALLED/{ID}.pspdx').exists());self.assertFalse((self.root/'ms0:/PSP/PSPDX/TMP/transaction.json').exists())
  # A plugin never turned on leaves no trace in the list, and none of a list where there was none.
  shutil.rmtree(self.root/'ms0:');(self.root/'ms0:').mkdir();d=self.plugin_setup();self.run_client('install');self.assertEqual(self.run_client('uninstall',ID).stdout.strip(),'0');self.assertEqual(self.tree(d),{})
 def test_plugin_lines_somebody_else_wrote_are_never_written(self):
  # A line for the file from before the install, whatever it says, is its writer's: no line is added beside it, turning the plugin on or off leaves the list alone and comes back with what the list says, and deleting the plugin leaves it too and says so. (The reviewer's S4a and S4b.)
  for base in (b'# mine\r\numd, ms0:/seplugins/usbnet/usbnet.prx, off // only for UMD, keep off for now\r\nxmb, clock.prx, on\r\n',b'always, ms0:/seplugins/usbnet/usbnet.prx, on\n',b'always, ms0:/seplugins/usbnet/usbnet.prx, on \n',
               b'umd, ms0:/seplugins/usbnet/usbnet.prx, on\npops, usbnet/usbnet.prx, off\nxmb, USBNET/USBNET.PRX, 0\n',b'game, MS0:/SEPLUGINS/Usbnet/Usbnet.prx, off'):
   with self.subTest(base=base):
    shutil.rmtree(self.root/'ms0:');(self.root/'ms0:').mkdir();d=self.plugin_setup(lst=base);lst=d/'PLUGINS.TXT';state=self.ark_state(base)
    r=self.run_client('install');self.assertIn('plugin: usbnet.prx '+('on' if state==1 else 'off'),r.stderr);self.assertEqual(lst.read_bytes(),base)
    for want in ('on','off','on'):self.assertEqual(self.run_client('plugin',ID,want).stdout.strip(),str(state));self.assertEqual(lst.read_bytes(),base)
    self.assertNotIn('plugin_line',self.state()[ID]['installed'])
    self.assertEqual(self.run_client('uninstall',ID).stdout.strip(),'1');self.assertEqual(self.tree(d),{'PLUGINS.TXT':base})
  # Lines written beside PSPDX's own after it was turned on: its own is still switched and taken out, in place, and theirs stay; what the list then says of the plugin is what comes back.
  d=self.plugin_setup(lst=self.USER);lst=d/'PLUGINS.TXT';self.run_client('install');self.run_client('plugin',ID,'on');theirs=b'game, usbnet/usbnet.prx, off // mine\r\n';lst.write_bytes(lst.read_bytes()+theirs)
  self.assertEqual(self.run_client('plugin',ID,'on').stdout.strip(),'0');self.assertEqual(lst.read_bytes(),self.USER+self.OWN+b'\r\n'+theirs)
  self.assertEqual(self.run_client('plugin',ID,'off').stdout.strip(),'0');self.assertEqual(lst.read_bytes(),self.USER+self.OWN[:-3]+b'off\r\n'+theirs)
  self.assertEqual(self.run_client('uninstall',ID).stdout.strip(),'1');self.assertEqual(lst.read_bytes(),self.USER+b' '*len(self.OWN)+b'\r\n'+theirs)
  # Its own line taken out or changed by hand, or written twice: none is PSPDX's any more. With no line left it is added again on turning on; with one that names the file, nothing is written.
  for hand,after_on in ((self.USER,self.USER+self.OWN+b'\r\n'),(self.USER+b'always, ms0:/seplugins/usbnet/usbnet.prx, 1\r\n',None),(self.USER+(self.OWN+b'\r\n')*2,None),(b'',self.OWN+b'\n')):
   shutil.rmtree(self.root/'ms0:');(self.root/'ms0:').mkdir();d=self.plugin_setup(lst=self.USER);lst=d/'PLUGINS.TXT';self.run_client('install');self.run_client('plugin',ID,'on');lst.write_bytes(hand)
   self.assertEqual(self.run_client('plugin',ID,'off').stdout.strip(),str(max(self.ark_state(hand),0)));self.assertEqual(lst.read_bytes(),hand)
   self.assertEqual(self.run_client('plugin',ID,'on').stdout.strip(),'1');self.assertEqual(lst.read_bytes(),after_on or hand)
   lst.write_bytes(hand);self.run_client('uninstall',ID);self.assertEqual(lst.read_bytes(),hand);self.assertEqual(self.tree(d),{'PLUGINS.TXT':hand})
 def test_plugin_list_survives_arks_managers_and_the_app_still_knows_its_line(self):
  # ARK's XMB/recovery manager and arkMenu's each write the whole list out again in their own way. Rewritten by either, with the plugin on or off, PSPDX's line is still its own and is switched and taken out in the room it has, the list never a byte longer or shorter.
  def xmbctrl(t):
   out=b''
   for line,_ in self.ark_lines(t):
    p=self.ark(line);l=line.strip(b' ')
    if p:a=l.find(b',');b=l.find(b',',a+1);out+=l[:a].strip(b' ')+b', '+l[a+1:b].strip(b' ')+b', '+(b'on' if p[2] else b'off')+b'\n'
    else:out+=line+b'\n'
   return out
  def arkmenu(t):
   out=b''
   for line in t.split(b'\n')[:-1] if t.endswith(b'\n') else t.split(b'\n'):
    k=line.rfind(b',')
    if line[:1] in (b'#',b';') or line[:2]==b'//' or k<0:out+=line+b'\n'
    else:out+=line[:k]+b', '+(b'on' if (line[k+1:].split() or [b''])[0] in (b'on',b'1',b'enabled',b'true') else b'off')+b'\n'
   return out
  for rewrite in (xmbctrl,arkmenu):
   for first in ('on','off'):
    with self.subTest(manager=rewrite.__name__,first=first):
     shutil.rmtree(self.root/'ms0:');(self.root/'ms0:').mkdir();d=self.plugin_setup(lst=self.USER);lst=d/'PLUGINS.TXT';self.run_client('install');self.run_client('plugin',ID,'on');self.run_client('plugin',ID,first)
     t=rewrite(lst.read_bytes());lst.write_bytes(t);self.assertEqual(self.ark_state(t),int(first=='on'));self.assertEqual(len(self.own_lines(t)),1,t);start,end,field=self.own_lines(t)[0]
     for want in ('off','on','off','on') if first=='on' else ('on','off','on'):
      self.assertEqual(self.run_client('plugin',ID,want).stdout.strip(),str(int(want=='on')));o=lst.read_bytes()
      self.assertEqual((len(o),o[:field],o[end:]),(len(t),t[:field],t[end:]));self.assertEqual(self.ark_state(o),int(want=='on'));self.assertEqual(self.ark_state(rewrite(o)),int(want=='on'))
     self.assertEqual(self.run_client('uninstall',ID).stdout.strip(),'0');self.assertEqual(lst.read_bytes(),t[:start]+b' '*(end-start)+t[end:]);self.assertEqual(self.tree(d),{'PLUGINS.TXT':t[:start]+b' '*(end-start)+t[end:]})
 def test_plugin_file_is_the_apps_only_by_its_hash(self):
  # A .prx copied over the installed one by hand is its owner's build, and goes on loading: an update is refused and changes nothing, not a file, not the line, not the record; a delete forgets the record and leaves the folder and the line that loads it, and says so.
  mine=b'MY OWN BUILD, copied by hand over USB'
  shutil.rmtree(self.root/'ms0:');(self.root/'ms0:').mkdir();d=self.plugin_setup(lst=self.USER);self.run_client('install',VERSION=1);self.run_client('plugin',ID,'on');(d/'usbnet/usbnet.prx').write_bytes(mine)
  before=self.tree(d);record=self.state()[ID];self.zip('new.zip',{'usbnet.prx':b'plugin two','LICENSE':b'GPL'})
  r=self.run_client('install',ok=False);self.assertIn('why: its .prx was changed by hand',r.stderr);self.assertEqual(self.tree(d),before);self.assertEqual(self.state()[ID],record)
  self.assertEqual(self.run_client('plugin',ID).stdout.strip(),'1')
  self.assertEqual(self.run_client('uninstall',ID).stdout.strip(),'2');self.assertEqual(self.tree(d),before);self.assertNotIn(ID,self.state());self.assertEqual(self.ark_state(before['PLUGINS.TXT']),1)
  # And then it is a folder PSPDX has no record of, in the way of the plugin like any other.
  r=self.run_client('install',ok=False);self.assertIn("is in the way",r.stderr);self.assertEqual(self.tree(d),before)
  # A record from which the hash has gone vouches for no main file; one deleted by hand is installed afresh, the line left as it is, and deleted with its line.
  shutil.rmtree(self.root/'ms0:');(self.root/'ms0:').mkdir();d=self.plugin_setup(lst=self.USER);self.run_client('install',VERSION=1);p=self.root/f'ms0:/PSP/PSPDX/INSTALLED/{ID}.state.json'
  rec=json.loads(p.read_text());del rec['installed']['plugin_sha256'];p.write_text(json.dumps(rec));self.assertEqual(self.run_client('uninstall',ID).stdout.strip(),'2');self.assertEqual(self.tree(d),dict(self.inside(),**{'PLUGINS.TXT':self.USER}))
  shutil.rmtree(d/'usbnet');self.run_client('install',VERSION=1);self.run_client('plugin',ID,'on');on=(d/'PLUGINS.TXT').read_bytes();(d/'usbnet/usbnet.prx').unlink()
  self.zip('new.zip',{'usbnet.prx':b'plugin two'});self.run_client('install');self.assertEqual(self.tree(d),dict(self.inside(dict(self.ONE,**{'usbnet.prx':b'plugin two'})),**{'PLUGINS.TXT':on}))
  (d/'usbnet/usbnet.prx').unlink();r=self.run_client('uninstall',ID,VERBOSE=1);self.assertIn('already gone',r.stderr);self.assertEqual(self.tree(d),{'PLUGINS.TXT':on.replace(self.OWN,b' '*len(self.OWN))})
  # The whole folder deleted by hand: installed afresh into one made again, and the record's line still its own.
  self.zip('new.zip',self.ONE);self.run_client('install',VERSION=1);self.run_client('plugin',ID,'on');on=(d/'PLUGINS.TXT').read_bytes();shutil.rmtree(d/'usbnet');self.run_client('install');self.assertEqual(self.tree(d),dict(self.inside(),**{'PLUGINS.TXT':on}));self.assertEqual(self.run_client('uninstall',ID).stdout.strip(),'0')
 def test_plugin_folder_keeps_what_is_not_the_apps(self):
  # Every file in the folder is PSPDX's only by the hash its record has of it. One the user put there, and one of the plugin's own the user changed -- its settings -- is theirs: an update writes neither, though the release ships a file of that name, and replaces the ones that are still as installed; a delete takes those and leaves theirs, and the folder with them, and says so.
  files={'usbnet.prx':b'plugin one','usbnet.ini':b'port=1\n','lang/en.txt':b'english','lang/de.txt':b'deutsch'}
  d=self.plugin_setup(files,lst=self.USER);self.run_client('install',VERSION=1);self.run_client('plugin',ID,'on');on=(d/'PLUGINS.TXT').read_bytes();f=d/'usbnet'
  (f/'usbnet.ini').write_bytes(b'port=7 ; mine\n');(f/'notes.txt').write_bytes(b'my notes');(f/'lang/fr.txt').write_bytes(b'my own translation');(f/'saves').mkdir();(f/'saves/a.sav').write_bytes(b's')
  two={'usbnet.prx':b'plugin two','usbnet.ini':b'port=2\n','lang/en.txt':b'english, better','notes.txt':b'release notes','new/extra.bin':b'x'}
  self.zip('new.zip',two);r=self.run_client('install',VERBOSE=1);self.assertIn('usbnet.ini is not a file PSPDX installed; left',r.stderr);self.assertIn('notes.txt is not a file PSPDX installed; left',r.stderr)
  want={'usbnet.prx':b'plugin two','usbnet.ini':b'port=7 ; mine\n','lang/en.txt':b'english, better','lang/de.txt':b'deutsch','lang/fr.txt':b'my own translation','notes.txt':b'my notes','saves/a.sav':b's','new/extra.bin':b'x'}
  self.assertEqual(self.tree(d),dict(self.inside(want),**{'PLUGINS.TXT':on}))
  listed=self.state()[ID]['installed']['plugin_files'];sha=lambda b:hashlib.sha256(b).hexdigest()
  self.assertEqual(listed,{'usbnet.prx':sha(b'plugin two'),'usbnet.ini':sha(b'port=1\n'),'lang/en.txt':sha(b'english, better'),'lang/de.txt':sha(b'deutsch'),'new/extra.bin':sha(b'x')})
  # Put back as it was installed, the file is PSPDX's again and the next update replaces it.
  (f/'usbnet.ini').write_bytes(b'port=1\n');self.run_client('install',VERSION=3);self.assertEqual((f/'usbnet.ini').read_bytes(),b'port=2\n');(f/'usbnet.ini').write_bytes(b'port=9\n')
  self.assertEqual(self.run_client('uninstall',ID).stdout.strip(),'4')
  self.assertEqual(self.tree(d),dict(self.inside({'usbnet.ini':b'port=9\n','lang/fr.txt':b'my own translation','notes.txt':b'my notes','saves/a.sav':b's'}),**{'PLUGINS.TXT':on.replace(self.OWN,b' '*len(self.OWN))}));self.assertNotIn(ID,self.state())
  # What is left is a folder no record names: in the way of a new install, and untouched by it.
  before=self.tree(d);r=self.run_client('install',ok=False);self.assertIn("is in the way",r.stderr);self.assertEqual(self.tree(d),before)
  # A read-only file of the plugin's own stops an update and a delete before anything is written.
  shutil.rmtree(self.root/'ms0:');(self.root/'ms0:').mkdir();d=self.plugin_setup(files,lst=self.USER);self.run_client('install',VERSION=1);(d/'usbnet/lang/en.txt').chmod(0o444);before=self.tree(d);self.zip('new.zip',two)
  r=self.run_client('install',ok=False);self.assertIn('why: one of its files is read-only',r.stderr);r=self.run_client('uninstall',ID,ok=False);self.assertIn('why: one of its files is read-only',r.stderr);self.assertEqual(self.tree(d),before);self.assertEqual(self.state()[ID]['installed']['version'],'1')
 def test_plugin_record_owns_a_line_only_while_the_list_has_it(self):
  # The second review's O3 and R3: the record says PSPDX owns a line only once that line has been read back from the list, and stops saying so when the list no longer has it. A turn-on that is refused, fails or is cut leaves no claim behind, so a line typed later with the very same bytes is its writer's.
  typed=self.USER+b'always, ms0:/seplugins/usbnet/usbnet.prx, on\r\n# the line above is MINE, typed by hand\r\n'
  def claim():return self.state()[ID]['installed'].get('plugin_line'),self.state()[ID]['installed'].get('plugin_write')
  def fresh():
   shutil.rmtree(self.root/'ms0:');(self.root/'ms0:').mkdir();d=self.plugin_setup(lst=self.USER);self.run_client('install');return d/'PLUGINS.TXT'
  def theirs(lst):
   lst.write_bytes(typed);self.assertEqual(self.run_client('plugin',ID,'off').stdout.strip(),'1');self.assertEqual(lst.read_bytes(),typed)
   self.assertEqual(self.run_client('uninstall',ID).stdout.strip(),'1');self.assertEqual(lst.read_bytes(),typed)
  lst=fresh();lst.chmod(0o444);r=self.run_client('plugin',ID,'on',ok=False);self.assertIn('read-only',r.stderr);lst.chmod(0o644);self.assertEqual(claim(),(None,None));self.assertEqual(lst.read_bytes(),self.USER);theirs(lst)
  lst=fresh();self.run_client('plugin',ID,'on',ok=False,WRITE_FAIL=1);self.assertEqual(lst.read_bytes(),self.USER);self.run_client('recover');self.assertEqual(claim(),(None,None));theirs(lst)
  cuts=0
  for fault in range(1,40):
   lst=fresh();r=self.run_client('plugin',ID,'on',ok=False,FAULT=fault)
   if r.returncode==0:break
   self.run_client('recover');text=lst.read_bytes();self.assertIn(text,(self.USER,self.USER+self.OWN+b'\r\n'));self.assertEqual(claim(),(self.PRX if text!=self.USER else None,None),fault);cuts+=1
   if text==self.USER:theirs(lst)
  self.assertGreater(cuts,3)
  # Its line taken out by hand while it was on: the claim goes at the next start, or with the next thing asked of the list.
  lst=fresh();self.run_client('plugin',ID,'on');lst.write_bytes(self.USER);self.run_client('recover');self.assertEqual(claim(),(None,None));theirs(lst)
  # A delete that takes the line out and then stops: the plugin is still installed, off, and owns no line.
  lst=fresh();self.run_client('plugin',ID,'on');on=lst.read_bytes();(self.root/'ms0:/PSP/PSPDX/TMP').mkdir(parents=True,exist_ok=True)
  r=self.run_client('uninstall',ID,ok=False,RO_MATCH=f'{ID}.pspdx');self.run_client('recover')
  if ID in self.state():self.assertEqual(lst.read_bytes(),on.replace(self.OWN,b' '*len(self.OWN)));self.assertEqual(claim(),(None,None));self.assertEqual(self.run_client('plugin',ID).stdout.strip(),'0')
 def test_plugin_write_cut_short_is_finished_at_the_next_start(self):
  # The second review's T1 to T3: a power cut inside the one write leaves some of its bytes on the stick. The record holds what the write was, and the next start -- or the next thing asked of the list -- writes the rest in the same place: a line put after the end gets its line end, a switch its whole word, a line being taken out all its spaces. Every byte outside that line stays; what the app then shows is what the list says.
  shutil.rmtree(self.root/'ms0:');(self.root/'ms0:').mkdir();d=self.plugin_setup(lst=self.USER);lst=d/'PLUGINS.TXT';self.run_client('install');s1=self.root/'s1';shutil.copytree(self.root/'ms0:',s1)
  self.run_client('plugin',ID,'on');s2=self.root/'s2';shutil.copytree(self.root/'ms0:',s2);on=self.USER+self.OWN+b'\r\n';off=on.replace(b'on \r\n',b'off\r\n');blank=on.replace(self.OWN,b' '*len(self.OWN))
  # Installed and off with the spaces of a line taken out before: turning on writes over them, and a cut inside that write is finished the same way.
  s3=self.root/'s3';shutil.copytree(s1,s3);(s3/'seplugins/PLUGINS.TXT').write_bytes(blank)
  def torn(start,command,k,old):
   for fault in range(1,60):
    shutil.rmtree(self.root/'ms0:');shutil.copytree(start,self.root/'ms0:');r=self.run_client(*command,ok=False,FAULT=fault,TORN=k)
    if lst.read_bytes()!=old:self.assertEqual(r.returncode,77);return lst.read_bytes()
    self.assertEqual(r.returncode,77)
  for start,command,old,new,after in ((s1,('plugin',ID,'on'),self.USER,on,'1'),(s2,('plugin',ID,'off'),on,off,'0'),(s2,('uninstall',ID),on,blank,'0'),(s3,('plugin',ID,'on'),blank,on,'1')):
   for k in range(1,len(new)-len(old) if len(new)>len(old) else len(self.OWN)):
    for repair in ('start','switch'):
     with self.subTest(command=command,k=k,repair=repair):
      cut=torn(start,command,k,old)
      if cut==new:continue
      if repair=='start':self.run_client('recover')
      else:self.run_client('plugin',ID,'on' if after=='1' else 'off')
      self.assertEqual(lst.read_bytes(),new);installed=self.state()[ID]['installed'];self.assertNotIn('plugin_write',installed)
      self.assertEqual(installed.get('plugin_line'),None if new==blank else self.PRX);self.assertEqual(self.run_client('plugin',ID).stdout.strip(),after);self.assertEqual(self.ark_state(new),int(after) if new!=blank else -1)
      # And from there everything goes on as if nothing had happened.
      if new==blank:self.assertEqual(self.run_client('plugin',ID,'on').stdout.strip(),'1');self.assertEqual(lst.read_bytes(),on)
      self.assertEqual(self.run_client('uninstall',ID).stdout.strip(),'0');self.assertEqual(self.ark_state(lst.read_bytes()),-1);self.assertEqual(len(self.own_lines(lst.read_bytes())),0)
  # A torn line that something was put after before PSPDX ran again cannot be finished where it is: nothing is written, the next thing asked says the list has changed, once, and then a line of PSPDX's own goes after the end as ever.
  cut=torn(s1,('plugin',ID,'on'),30,self.USER);ark=cut+b'ps1, new.prx, on\n\n';lst.write_bytes(ark);self.run_client('recover');self.assertEqual(lst.read_bytes(),ark)
  r=self.run_client('plugin',ID,'on',ok=False);self.assertIn('why: PLUGINS.TXT has changed',r.stderr);self.assertEqual(lst.read_bytes(),ark);self.assertNotIn('plugin_write',self.state()[ID]['installed'])
  self.assertEqual(self.run_client('plugin',ID,'on').stdout.strip(),'1');self.assertEqual(lst.read_bytes(),ark+self.OWN+b'\n');self.assertEqual(self.state()[ID]['installed']['plugin_line'],self.PRX)
  # The same for bytes at the write's place that are neither the old ones nor the new: somebody's, and left.
  cut=torn(s2,('plugin',ID,'off'),3,on);hand=cut.replace(b', of \r\n',b', xx \r\n');self.assertNotEqual(hand,cut);lst.write_bytes(hand);self.run_client('recover');self.assertEqual(lst.read_bytes(),hand)
  r=self.run_client('plugin',ID,'off',ok=False);self.assertIn('has changed',r.stderr);self.assertEqual(lst.read_bytes(),hand);self.assertEqual(self.state()[ID]['installed'].get('plugin_line'),self.PRX)
  self.assertEqual(self.run_client('plugin',ID,'off').stdout.strip(),'0');self.assertEqual(lst.read_bytes(),hand);self.assertNotIn('plugin_line',self.state()[ID]['installed'])
 def test_plugin_never_touches_files_that_only_look_like_its_own(self):
  # The reviewer's S1, S2 and S5: files and folders under the names a swap would use, of the list, of the plugin's folder or of the one file it was in PSPDX 1.1 -- a usbnet.prx somebody put there too -- are somebody's unless a journal says PSPDX made them. Starting, installing, switching, updating and deleting neither remove nor rename them, and no list is ever made out of one.
  def strays(d,dirs=False):
   for n in ('PLUGINS.TXT.pspdx-old','PLUGINS.TXT.pspdx-new','PLUGINS.TXT.bak','PLUGINS.TXT.new','usbnet.prx','usbnet.prx.pspdx-old','usbnet.prx.pspdx-new','usbnet.pspdx-new','usbnet.pspdx-old','usbnet.old','.pspdx-stage'):
    if dirs:(d/n).mkdir();(d/n/'keep.txt').write_bytes(b'k')
    else:(d/n).write_bytes(b'my own '+n.encode())
  for dirs in (False,True):
   for has_list in (True,False):
    with self.subTest(dirs=dirs,has_list=has_list):
     shutil.rmtree(self.root/'ms0:');(self.root/'ms0:').mkdir();d=self.plugin_setup(lst=self.USER if has_list else None);d.mkdir(exist_ok=True);strays(d,dirs);(self.root/'ef0:/seplugins').mkdir(exist_ok=True);(self.root/'ef0:/seplugins/PLUGINS.TXT.pspdx-new').write_bytes(b'ef0 draft')
     before=self.tree(d);self.run_client('recover');self.assertEqual(self.tree(d),before);self.assertEqual(self.tree(self.root/'ef0:/seplugins'),{'PLUGINS.TXT.pspdx-new':b'ef0 draft'})
     self.run_client('install',VERSION=1);self.assertEqual(self.tree(d),dict(before,**self.inside()));self.assertEqual(self.run_client('plugin',ID,'on').stdout.strip(),'1')
     lst=(self.USER if has_list else b'')+self.OWN+(b'\r\n' if has_list else b'\n');self.assertEqual(self.tree(d),dict(before,**dict(self.inside(),**{'PLUGINS.TXT':lst})))
     self.run_client('plugin',ID,'off');self.run_client('recover');self.zip('new.zip',{'usbnet.prx':b'plugin two'});self.run_client('install');self.run_client('uninstall',ID)
     self.assertEqual(self.tree(d),dict(before,**{'PLUGINS.TXT':lst.replace(self.OWN,b' '*len(self.OWN))}))
  # Inside the plugin's own folder: an update is refused while a name it would write under is taken, and says which; a delete needs neither name and goes through, leaving the file and the folder it is in. Nothing of them changes.
  for stray in ('usbnet.prx.pspdx-old','usbnet.prx.pspdx-new','USBNET.PRX.PSPDX-OLD','docs/readme.txt.pspdx-new'):
   shutil.rmtree(self.root/'ms0:');(self.root/'ms0:').mkdir();d=self.plugin_setup(lst=self.USER);self.run_client('install',VERSION=1);self.run_client('plugin',ID,'on');(d/'usbnet'/stray).write_bytes(b'STALE OLDER COPY');on=(d/'PLUGINS.TXT').read_bytes()
   before=self.tree(d);self.zip('new.zip',dict(self.ONE,**{'usbnet.prx':b'plugin two','docs/readme.txt':b'read me again'}));r=self.run_client('install',ok=False);self.assertIn('an old copy of it is in the way',r.stderr);self.assertEqual(self.tree(d),before);self.assertEqual(self.state()[ID]['installed']['version'],'1')
   # S5a: a delete that stops at the list (a NUL in it) leaves the plugin as it is, and never puts the stale copy in its place.
   (d/'PLUGINS.TXT').write_bytes(on+b'\0');r=self.run_client('uninstall',ID,ok=False);self.assertIn('why: PLUGINS.TXT is not plain text',r.stderr);self.run_client('recover')
   self.assertEqual(self.tree(d),dict(before,**{'PLUGINS.TXT':on+b'\0'}));self.assertEqual(self.state()[ID]['installed']['version'],'1')
   (d/'PLUGINS.TXT').write_bytes(on);self.assertEqual(self.run_client('uninstall',ID).stdout.strip(),'4');self.assertEqual(self.tree(d),dict(self.inside({stray:b'STALE OLDER COPY'}),**{'PLUGINS.TXT':on.replace(self.OWN,b' '*len(self.OWN))}))
 def test_plugin_list_that_is_not_to_be_written_is_refused_whole(self):
  # The reviewer's S6, and what the stick marks: a list too large, with a NUL in it, read-only, or that cannot be told to be one is not written at all, and the reason is given. Installing needs no list and goes through.
  big=self.USER+b''.join(b'game, ms0:/seplugins/p%04d.prx, on\r\n'%i for i in range(2200))
  for bad,why in ((big,'too large'),(self.USER+b'\0x\r\n','not plain text'),((b'# pad\n'*20000)[:65536-20],'too large')):
   with self.subTest(why=why,size=len(bad)):
    shutil.rmtree(self.root/'ms0:');(self.root/'ms0:').mkdir();d=self.plugin_setup(lst=bad);lst=d/'PLUGINS.TXT'
    self.run_client('install');r=self.run_client('plugin',ID,'on',ok=False);self.assertIn('why: PLUGINS.TXT is '+why,r.stderr);self.assertEqual(lst.read_bytes(),bad);self.assertNotIn('plugin_line',self.state()[ID]['installed'])
    self.assertEqual(self.run_client('plugin',ID).stdout.strip(),'0');self.run_client('uninstall',ID);self.assertEqual(self.tree(d),{'PLUGINS.TXT':bad})
  # The largest list that is read is still written to in place, and never grown past what is read.
  d=self.plugin_setup(lst=self.USER);lst=d/'PLUGINS.TXT';self.run_client('install');self.run_client('plugin',ID,'on');on=lst.read_bytes();full=on+(b'# pad\n'*20000)[:65536-len(on)];lst.write_bytes(full)
  self.assertEqual(self.run_client('plugin',ID,'off').stdout.strip(),'0');self.assertEqual(lst.read_bytes(),full.replace(self.OWN,self.OWN[:-3]+b'off'));self.run_client('uninstall',ID);self.assertEqual(len(lst.read_bytes()),65536)
  # Read-only, by the stick's own mark: the list is not written, the plugin's files neither replaced nor deleted.
  d=self.plugin_setup(lst=self.USER);lst=d/'PLUGINS.TXT';self.run_client('install',VERSION=1);lst.chmod(0o444)
  r=self.run_client('plugin',ID,'on',ok=False);self.assertIn('why: PLUGINS.TXT is read-only',r.stderr);self.assertEqual(lst.read_bytes(),self.USER)
  lst.chmod(0o644);self.run_client('plugin',ID,'on');on=lst.read_bytes();lst.chmod(0o444)
  for want in ('off',):r=self.run_client('plugin',ID,want,ok=False);self.assertIn('read-only',r.stderr);self.assertEqual(lst.read_bytes(),on)
  r=self.run_client('uninstall',ID,ok=False);self.assertIn('why: PLUGINS.TXT is read-only',r.stderr);self.assertEqual(self.tree(d),dict(self.inside(),**{'PLUGINS.TXT':on}));self.assertIn(ID,self.state())
  lst.chmod(0o644);(d/'usbnet/usbnet.prx').chmod(0o444);self.zip('new.zip',{'usbnet.prx':b'plugin two'})
  r=self.run_client('install',ok=False);self.assertIn('why: one of its files is read-only',r.stderr);r=self.run_client('uninstall',ID,ok=False);self.assertIn('why: one of its files is read-only',r.stderr)
  self.assertEqual(self.tree(d),dict(self.inside(),**{'PLUGINS.TXT':on}));self.assertEqual(self.state()[ID]['installed']['version'],'1')
  # PLUGINS.TXT a folder, or there twice in two spellings where a stick can hold that: not a list to write to.
  for make in ((lambda d:(d/'PLUGINS.TXT').mkdir()),(lambda d:[(d/n).write_bytes(self.USER) for n in ('PLUGINS.TXT','plugins.txt')])):
   shutil.rmtree(self.root/'ms0:');(self.root/'ms0:').mkdir();d=self.plugin_setup();d.mkdir();make(d);self.run_client('install');before=self.tree(d)
   self.assertNotEqual(self.run_client('plugin',ID,'on',ok=False).returncode,0);self.assertEqual(self.tree(d),before)
 def test_plugin_records_and_zips_cannot_aim_at_another_file(self):
  # The reviewer's S7 and S8. A record edited by hand to name another file under seplugins/ deletes nothing: that file's hash is not the record's, and the record's line is not that file's. A zip whose .prx is not a plain name of its own installs nothing.
  for where in ('seplugins/cheat.prx','seplugins/../PSP/x.prx','seplugins/sub/cheat.prx','seplugins/PLUGINS.TXT','ms0:/seplugins/cheat.prx','seplugins/cheat.prx.','seplugins/cheat.prx ','seplugins\\cheat.prx','seplugins/ef0:x.prx','SEPLUGINS/cheat.prx','seplugins/.prx','seplugins/..prx'):
   with self.subTest(where=where):
    shutil.rmtree(self.root/'ms0:');(self.root/'ms0:').mkdir();d=self.plugin_setup(lst=self.USER);(d/'cheat.prx').write_bytes(b'c');self.run_client('install');self.run_client('plugin',ID,'on');before=self.tree(d)
    p=self.root/f'ms0:/PSP/PSPDX/INSTALLED/{ID}.state.json';rec=json.loads(p.read_text());rec['installed']['installdir']=where;p.write_text(json.dumps(rec))
    for command in (('plugin',ID,'off'),('uninstall',ID),('install',),('recover',)):self.run_client(*command,ok=False);self.assertEqual(self.tree(d),before,(where,command))
  # The same for the line: a record that claims another plugin's line as PSPDX's is believed about none.
  shutil.rmtree(self.root/'ms0:');(self.root/'ms0:').mkdir();d=self.plugin_setup(lst=b'always, ms0:/seplugins/cheat.prx, on \n');(d/'cheat.prx').write_bytes(b'c');self.run_client('install');before=self.tree(d)
  p=self.root/f'ms0:/PSP/PSPDX/INSTALLED/{ID}.state.json';rec=json.loads(p.read_text());rec['installed']['plugin_line']='ms0:/seplugins/cheat.prx';p.write_text(json.dumps(rec))
  self.run_client('plugin',ID,'off',ok=False);self.assertEqual(self.tree(d),before);self.run_client('uninstall',ID);self.assertEqual(self.tree(d),{'PLUGINS.TXT':before['PLUGINS.TXT'],'cheat.prx':b'c'})
  # A journal that names another file, at any phase and for either operation, removes and renames nothing either: not that file, and not a file beside it that only has the name of a copy.
  for phase in ('prepared','staging','placed','committed'):
   for op in ('install','remove'):
    shutil.rmtree(self.root/'ms0:');(self.root/'ms0:').mkdir();d=self.plugin_setup(lst=self.USER)
    for n in ('cheat.prx','cheat.prx.pspdx-old','cheat.prx.pspdx-new'):(d/n).write_bytes(b'c '+n.encode())
    self.run_client('install');before=self.tree(d);sha=hashlib.sha256(b'plugin one').hexdigest()
    self.write('ms0:/PSP/PSPDX/TMP/transaction.json',dict(id=ID,device='ms0:',op=op,phase=phase,plugin='cheat.prx',old_state={},sha=sha,was=sha,size=10));self.run_client('recover')
    self.assertEqual(self.tree(d),before,(phase,op))
  for name,ok in (('PLUGINS.TXT',0),('plugins.txt.prx',0),('PLUGINS.TXT.pspdx-new.prx',0),('x.pspdx-old.prx',0),('PLUGINS.TXT.x.prx',1),('../x.prx',0),('a:b.prx',0),('x.prx.',0),('x.prx ',0),('..prx',0),('a,b.prx',0),('my plugin.prx',0),('x'*28+'.prx',1),('x'*29+'.prx',0),('.prx',0),('-.prx',1)):
   with self.subTest(name=name):
    shutil.rmtree(self.root/'ms0:');(self.root/'ms0:').mkdir();d=self.plugin_setup({name:b'x','EBOOT.PBP':b'e'},lst=self.USER);r=self.run_client('install',ok=False)
    self.assertEqual(self.tree(d),dict({'PLUGINS.TXT':self.USER},**(self.inside({name:b'x','EBOOT.PBP':b'e'},name[:-4]) if ok else {})),r.stderr);self.assertEqual(r.returncode,0 if ok else 1);self.assertFalse((self.root/'ms0:/PSP/x.prx').exists())
  # A record that names another plugin's folder, to the letter of the rule, owns no file in it: every file is the record's only by its hash, and the folder goes only when empty.
  shutil.rmtree(self.root/'ms0:');(self.root/'ms0:').mkdir();d=self.plugin_setup(lst=self.USER);(d/'cheat').mkdir();(d/'cheat/cheat.prx').write_bytes(b'c');(d/'cheat/LICENSE').write_bytes(b'MIT');(d/'cheat/usbnet.prx').write_bytes(b'plugin one')
  self.run_client('install');self.run_client('plugin',ID,'on');before=self.tree(d);p=self.root/f'ms0:/PSP/PSPDX/INSTALLED/{ID}.state.json';rec=json.loads(p.read_text());rec['installed']['installdir']='seplugins/cheat/cheat.prx';p.write_text(json.dumps(rec))
  for command in (('plugin',ID,'off'),('install',),('recover',),('uninstall',ID)):self.run_client(*command,ok=False);self.assertEqual(self.tree(d),before,command)
  # A record cannot list a path that leaves its folder, and a journal that does is not acted on.
  for path in ('../cheat.prx','/x','a/../../b','sub/','x.pspdx-old','a\\b','c:d',''):
   shutil.rmtree(self.root/'ms0:');(self.root/'ms0:').mkdir();d=self.plugin_setup(lst=self.USER);(d/'cheat.prx').write_bytes(b'c');self.run_client('install');before=self.tree(d);sha=hashlib.sha256(b'c').hexdigest()
   rec=json.loads(p.read_text());rec['installed']['plugin_files'][path]=sha;p.write_text(json.dumps(rec));self.run_client('uninstall',ID,ok=False);self.run_client('recover');self.assertEqual(self.tree(d),before,path)
   rec['installed']['plugin_files'].pop(path);p.write_text(json.dumps(rec))
   for phase in ('staging','placed','committed'):
    for op in ('install','remove'):
     self.write('ms0:/PSP/PSPDX/TMP/transaction.json',dict(id=ID,device='ms0:',op=op,phase=phase,plugin='usbnet.prx',dir='usbnet',old_state={},files=[dict(p=path,sha=sha,was=sha,size=1)],made=[]));self.run_client('recover');self.assertEqual(self.tree(d),before,(path,phase,op))
     (self.root/'ms0:/PSP/PSPDX/TMP/transaction.json').unlink()
  # A journal of the folder's kind that names files it has the wrong hash of, or folders with something in them, at any phase and for either operation, removes and renames nothing.
  for phase in ('prepared','staging','placed','committed'):
   for op in ('install','remove'):
    shutil.rmtree(self.root/'ms0:');(self.root/'ms0:').mkdir();d=self.plugin_setup(lst=self.USER);(d/'cheat').mkdir()
    for n in ('cheat.prx','cheat.prx.pspdx-old','cheat.prx.pspdx-new','sub/keep.txt'):(d/'cheat'/n).parent.mkdir(exist_ok=True);(d/'cheat'/n).write_bytes(b'c '+n.encode())
    self.run_client('install');before=self.tree(d);sha=hashlib.sha256(b'plugin one').hexdigest()
    self.write('ms0:/PSP/PSPDX/TMP/transaction.json',dict(id=ID,device='ms0:',op=op,phase=phase,plugin='cheat.prx',dir='cheat',old_state={},files=[dict(p='cheat.prx',sha=sha,was=sha,size=10),dict(p='sub/keep.txt',sha=sha,was=sha,size=10)],made=['','sub']));self.run_client('recover')
    self.assertEqual(self.tree(d),before,(phase,op))
 def test_plugin_is_the_folder_of_the_prx_nearest_the_top(self):
  # The rule a homebrew's package is found by, for a .prx: the folder of the one nearest the top is the package, whole, and goes to seplugins/<that .prx's name without .prx>/. One .prx in that folder is the one the firmware loads; of several, the one the .pspdx names in "plugin", and without that word, or with one that names no file there, nothing is installed and the reason is plain. What lies beside the folder in the zip stays in the zip.
  def go(files,ok=True,**spec):
   shutil.rmtree(self.root/'ms0:');(self.root/'ms0:').mkdir();d=self.plugin_setup(files,lst=self.USER)
   if spec:self.write('manifest.json',dict(json.loads((self.root/'manifest.json').read_text()),**spec))
   r=self.run_client('install',ok=False,VERBOSE=1);self.assertEqual(r.returncode,0 if ok else 1,r.stderr)
   if not ok:self.assertEqual(self.tree(d),{'PLUGINS.TXT':self.USER});self.assertEqual(self.state(),{});self.assertFalse((self.root/'ms0:/PSP/PSPDX/TMP/transaction.json').exists())
   return d,r.stderr
  # In a folder of the zip, with its settings beside it and a folder below: all of it, and nothing from beside or above.
  d,_=go({'README.md':b'r','EBOOT.PBP':b'e','CXMB/cxmb.prx':b'main','CXMB/cxmb.ini':b'theme=1','CXMB/themes/a.ctf':b'a','CXMB/themes/deep/extra.prx':b'deeper','src/main.c':b'c','other/deep/second.prx':b'no nearer'})
  self.assertEqual(self.tree(d),dict(self.inside({'cxmb.prx':b'main','cxmb.ini':b'theme=1','themes/a.ctf':b'a','themes/deep/extra.prx':b'deeper'},'cxmb'),**{'PLUGINS.TXT':self.USER}))
  self.assertEqual(self.state()[ID]['installed']['installdir'],'seplugins/cxmb/cxmb.prx');self.assertEqual(self.run_client('plugin',ID,'on').stdout.strip(),'1');self.assertEqual((d/'PLUGINS.TXT').read_bytes(),self.USER+b'always, ms0:/seplugins/cxmb/cxmb.prx, on \r\n')
  self.assertEqual(self.run_client('uninstall',ID).stdout.strip(),'0');self.assertEqual(sorted(self.tree(d)),['PLUGINS.TXT'])
  # At the top of the zip the zip is the package; a Windows zip's backslashes are slashes; a Mac's shadows are never files.
  d,_=go({'x.prx':b'main','LICENSE':b'MIT','docs/a.txt':b'a','__MACOSX/._x.prx':b'shadow','__MACOSX/docs/._a.txt':b's'});self.assertEqual(self.tree(d),dict(self.inside({'x.prx':b'main','LICENSE':b'MIT','docs/a.txt':b'a'},'x'),**{'PLUGINS.TXT':self.USER}))
  (self.root/'new.zip').write_bytes(raw_zip({b'pack\\y.prx':b'main',b'pack\\data\\y.ini':b'i',b'readme.txt':b'r'}));(self.root/'ms0:/seplugins/x').exists() and shutil.rmtree(self.root/'ms0:/seplugins/x');(self.root/f'ms0:/PSP/PSPDX/INSTALLED/{ID}.state.json').unlink()
  self.run_client('install');self.assertEqual(self.tree(d),dict(self.inside({'y.prx':b'main','data/y.ini':b'i'},'y'),**{'PLUGINS.TXT':self.USER}))
  # Several .prx in the folder: the .pspdx names the one, by its file name alone and without regard to case; the others are files like any.
  three={'P/kernel.prx':b'k','P/user.prx':b'u','P/Main.PRX':b'm','P/cfg.ini':b'c'}
  _,said=go(three,ok=False);self.assertIn('why: its .pspdx must name a .prx',said)
  _,said=go(three,ok=False,plugin='loader.prx');self.assertIn('why: the .prx it names is missing',said)
  d,_=go(three,plugin='main.prx');self.assertEqual(self.tree(d),dict(self.inside({'kernel.prx':b'k','user.prx':b'u','Main.PRX':b'm','cfg.ini':b'c'},'Main'),**{'PLUGINS.TXT':self.USER}));self.assertEqual(self.state()[ID]['installed']['installdir'],'seplugins/Main/Main.PRX')
  self.assertEqual(json.loads((self.root/f'ms0:/PSP/PSPDX/INSTALLED/{ID}.pspdx').read_text())['plugin'],'main.prx')
  # One .prx, and a .pspdx that names it, or another: the name is held to all the same.
  d,_=go({'a.prx':b'a','b.txt':b'b'},plugin='a.prx');self.assertIn('a/a.prx',self.tree(d));_,said=go({'a.prx':b'a'},ok=False,plugin='b.prx');self.assertIn('the .prx it names is missing',said)
  # A .prx as near the top in another folder is another package, and none is guessed at; none at all is no plugin.
  _,said=go({'psp1000/a.prx':b'a','pspgo/a.prx':b'b','README':b'r'},ok=False);self.assertIn('why: its zip has .prx in two folders',said)
  _,said=go({'psp1000/a.prx':b'a','pspgo/a.prx':b'b'},ok=False,plugin='a.prx');self.assertIn('in two folders',said)
  d,_=go({'top.prx':b't','psp1000/a.prx':b'a','pspgo/a.prx':b'b'});self.assertEqual(self.tree(d),dict(self.inside({'top.prx':b't','psp1000/a.prx':b'a','pspgo/a.prx':b'b'},'top'),**{'PLUGINS.TXT':self.USER}))
  _,said=go({'EBOOT.PBP':b'x','readme.txt':b'r'},ok=False);self.assertIn('why: its zip holds no .prx',said)
  # A name the folder or a line cannot have, two files the stick could not tell apart, a path that leaves the zip anywhere in it, a name PSPDX keeps for its copies, and more files than a plugin is.
  for files,why in (({'my plugin.prx':b'x'},'an unusable name'),({'p/a.prx':b'x','p/Readme':b'1','p/README':b'2'},'cannot be unpacked safely'),({'a.prx':b'x','../b.txt':b'y'},'cannot be unpacked safely'),({'a.prx':b'x','b.txt.pspdx-new':b'y'},'cannot be unpacked safely'),
                    ({'a.prx':b'x','d/'+'n'*95:b'y'},'cannot be unpacked safely'),(dict({'a.prx':b'x'},**{'f%02d.bin'%i:b'y' for i in range(64)}),'holds too many files')):
   _,said=go(files,ok=False);self.assertIn(why,said,sorted(files)[:3])
  d,_=go(dict({'a.prx':b'x'},**{'f%02d.bin'%i:b'y' for i in range(63)}));self.assertEqual(len(self.state()[ID]['installed']['plugin_files']),64);self.assertEqual(self.run_client('uninstall',ID).stdout.strip(),'0');self.assertEqual(sorted(self.tree(d)),['PLUGINS.TXT'])
  # The longest name there is: the line still fits the list, is switched and taken out.
  long='x'*28+'.prx';d,_=go({long:b'x'});self.assertEqual(self.run_client('plugin',ID,'on').stdout.strip(),'1');line=('always, ms0:/seplugins/%s/%s, on \r\n'%(long[:-4],long)).encode();self.assertEqual((d/'PLUGINS.TXT').read_bytes(),self.USER+line)
  self.assertEqual(self.run_client('plugin',ID,'off').stdout.strip(),'0');self.assertEqual(self.run_client('uninstall',ID).stdout.strip(),'0');self.assertEqual(self.tree(d),{'PLUGINS.TXT':self.USER+b' '*(len(line)-2)+b'\r\n'})
  # "plugin" is a plugin's word alone, and a file name alone.
  for spec in (dict(SPEC,plugin='a.prx'),dict(SPEC,type='plugin',plugin='a.prx'),dict({k:v for k,v in SPEC.items() if k!='installdir'},type='plugin',plugin='sub/a.prx'),dict({k:v for k,v in SPEC.items() if k!='installdir'},type='plugin',plugin='a.txt'),dict({k:v for k,v in SPEC.items() if k!='installdir'},type='plugin',plugin=7)):
   self.write('bad.json',spec);self.assertNotEqual(self.run_client('parse','bad.json',ok=False).returncode,0,spec)
 def test_plugin_never_takes_a_folder_it_did_not_install(self):
  # A folder of that name somebody put there is not adopted and nothing is written into it, and nothing is made beside it: no list, no copy. The same for a file of the folder's name. An installed plugin keeps its file's name, an app that is a folder under PSP/GAME does not become a plugin, and another app's plugin is that app's.
  d=self.plugin_setup();(d/'usbnet').mkdir(parents=True);(d/'usbnet/usbnet.prx').write_bytes(b'mine');(d/'usbnet/usbnet.ini').write_bytes(b'my settings');before=self.tree(d)
  r=self.run_client('install',ok=False,VERBOSE=1);self.assertIn("is in the way",r.stderr);self.assertEqual(self.tree(d),before);self.assertEqual(self.state(),{})
  self.assertFalse((self.root/'ms0:/PSP/PSPDX/TMP/transaction.json').exists());self.assertFalse((self.root/'ms0:/PSP/PSPDX/TMP/download.zip').exists())
  # Empty, under another spelling, or a file: somebody's all the same.
  for make in ((lambda:(d/'usbnet').mkdir()),(lambda:(d/'USBNET').mkdir()),(lambda:(d/'Usbnet').write_bytes(b'a file of mine'))):
   shutil.rmtree(d);d.mkdir();make();before=self.tree(d);self.assertNotEqual(self.run_client('install',ok=False).returncode,0);self.assertEqual(self.tree(d),before);self.assertEqual(self.state(),{})
  # Refused before the folder is made: a zip that is no plugin leaves no seplugins behind.
  shutil.rmtree(d);self.plugin_setup({'EBOOT.PBP':b'x'});self.run_client('install',ok=False);self.assertFalse(d.exists())
  self.plugin_setup({'Usbnet.PRX':b'one'});self.run_client('install',VERSION=1);self.assertEqual(self.tree(d),self.inside({'Usbnet.PRX':b'one'},'Usbnet'))
  self.plugin_setup({'usbnet2.prx':b'two'});r=self.run_client('install',ok=False);self.assertIn('has another name now',r.stderr);self.assertEqual(self.tree(d),self.inside({'Usbnet.PRX':b'one'},'Usbnet'))
  # Spelled another way by a later release, it is still the file on the stick, under the name it has there.
  self.plugin_setup({'USBNET.prx':b'two'});self.run_client('install');self.assertEqual(self.tree(d),self.inside({'Usbnet.PRX':b'two'},'Usbnet'));self.assertEqual(self.state()[ID]['installed']['installdir'],'seplugins/Usbnet/Usbnet.PRX')
  self.write('manifest.json',SPEC);self.zip('new.zip',{'EBOOT.PBP':b'app'});r=self.run_client('install',ok=False,VERBOSE=1);self.assertIn('delete it first',r.stderr);self.assertFalse((self.root/'ms0:/PSP/GAME/Demo').exists())
  record=json.loads(json.dumps(self.state()[ID]));record['source']='https://github.com/test/other';self.write('ms0:/PSP/PSPDX/INSTALLED/io.github.test.other.state.json',record)
  self.run_client('uninstall',ID);self.plugin_setup({'usbnet.prx':b'x'});r=self.run_client('install',ok=False);self.assertIn("is in the way",r.stderr);self.assertEqual(self.tree(d),{})
 def test_plugin_goes_to_the_device_chosen_and_stays_there(self):
  # On a PSP Go the plugin and its line are on the device the install chose, ef0: named in the line, and an update and the switch keep to it.
  self.plugin_setup();self.run_client('install',INSTALL_DEVICE='ef0:',VERSION=1);d=self.root/'ef0:/seplugins';line=b'always, ef0:/seplugins/usbnet/usbnet.prx, on \n'
  self.assertEqual(self.tree(d),self.inside());self.assertEqual(self.state()[ID]['installed']['device'],'ef0:');self.assertFalse((self.root/'ms0:/seplugins').exists())
  self.assertEqual(self.run_client('plugin',ID,'on').stdout.strip(),'1');self.assertEqual((d/'PLUGINS.TXT').read_bytes(),line);self.assertEqual(self.state()[ID]['installed']['plugin_line'],'ef0:/seplugins/usbnet/usbnet.prx')
  self.zip('new.zip',{'usbnet.prx':b'plugin two'});self.run_client('install',INSTALL_DEVICE='ms0:');self.assertEqual((d/'usbnet/usbnet.prx').read_bytes(),b'plugin two');self.assertFalse((self.root/'ms0:/seplugins').exists())
  self.assertEqual(self.run_client('plugin',ID,'off').stdout.strip(),'0');self.assertEqual((d/'PLUGINS.TXT').read_bytes(),line.replace(b'on ',b'off'))
  self.run_client('uninstall',ID);self.assertEqual(self.tree(d),{'PLUGINS.TXT':b' '*(len(line)-1)+b'\n'})
 def test_plugin_power_cuts_and_failures(self):
  # The reviewer's fault sweeps, over the folder: every plugin operation cut after each mutating call, a write cut after any number of its bytes, each call failing in turn, and the stick full at any point. At every moment, before the next start and after it: the list is there, as long as it was or longer by the one line, each byte the one it was or the one meant for its place; nobody else's file is touched, beside the folder or in it; and after the next start the plugin is whole in one version or gone, every file of it the one its record has the hash of, with nothing of PSPDX's left beside them.
  base=b'# mine\r\ngame, other.prx, on';on=base+b'\r\n'+self.OWN+b'\r\n';off=on.replace(b'on \r\n',b'off\r\n');blank=on.replace(self.OWN,b' '*len(self.OWN))
  d=self.plugin_setup(lst=base);lst=d/'PLUGINS.TXT';theirs={'other.prx':b'somebody else','PLUGINS.TXT.pspdx-old':b'my own old copy','PLUGINS.TXT.bak':b'my backup','usbnet.prx':b'a build of my own','usbnet.pspdx-old':b'mine too'}
  for n,b in theirs.items():(d/n).write_bytes(b)
  mine={'usbnet/mine.txt':b'my notes','usbnet/docs/own.txt':b'my own doc'}
  TWO={'usbnet.prx':b'plugin two','LICENSE':b'MIT, second','docs/readme.txt':b'read me again','new/deep/added.bin':b'added'};both=dict(self.ONE,**TWO);sha=lambda b:hashlib.sha256(b).hexdigest()
  snap={}
  def keep(name):snap[name]=self.root/name;shutil.copytree(self.root/'ms0:',snap[name])
  keep('empty');self.run_client('install',VERSION=1)
  for n,b in mine.items():(d/n).write_bytes(b)
  keep('installed');self.run_client('plugin',ID,'on');keep('on');self.run_client('plugin',ID,'off');keep('off');self.zip('new.zip',TWO)
  # op: the stick it starts from, the command, the list before and the list it is on its way to.
  ops={'fresh':('empty',('install',),base,base),'update':('off',('install',),off,off),'turn on':('installed',('plugin',ID,'on'),base,on),'off':('on',('plugin',ID,'off'),on,off),'on':('off',('plugin',ID,'on'),off,on),'remove':('on',('uninstall',ID),on,blank),'remove unlisted':('installed',('uninstall',ID),base,base)}
  def check(tag,start,old,new,settled):
   files=self.tree(d);text=files.get('PLUGINS.TXT');self.assertIsNotNone(text,tag);self.assertTrue(len(old)<=len(text)<=len(new),(tag,text))
   self.assertTrue(all(c in (new[i],old[i] if i<len(old) else new[i]) for i,c in enumerate(text)),(tag,text))
   for n,b in theirs.items():self.assertEqual(files.get(n),b,(tag,n))
   if start!='empty':
    for n,b in mine.items():self.assertEqual(files.get(n),b,(tag,n))
   state=self.state();inside={n[7:]:b for n,b in files.items() if n.startswith('usbnet/') and b is not None and n not in mine}
   if settled:
    self.assertIn(text,(old,new),tag);own=len(self.own_lines(text));self.assertFalse((self.root/'ms0:/PSP/PSPDX/TMP/transaction.json').exists(),tag)
    if ID in state:
     installed=state[ID]['installed'];self.assertEqual((installed.get('plugin_line'),installed.get('plugin_write')),(self.PRX if own else None,None),tag)
     want=TWO if start=='empty' else self.ONE if installed['version']=='1' else both;self.assertEqual(inside,want,tag);self.assertEqual(installed['plugin_files'],{n:sha(b) for n,b in want.items()},tag);self.assertEqual(installed['plugin_sha256'],sha(want['usbnet.prx']),tag)
     self.assertEqual(self.run_client('plugin',ID).stdout.strip(),str(max(self.ark_state(text),0)),tag)
    else:
     # Gone: nothing of the plugin, and the folder only where the user's files are in it.
     self.assertEqual({n:b for n,b in files.items() if n=='usbnet' or n.startswith('usbnet/')},{} if start=='empty' else dict({'usbnet':None,'usbnet/docs':None},**mine),tag)
   else:
    for n,b in inside.items():
     n=n[:-10] if n.endswith(('.pspdx-new','.pspdx-old')) else n;self.assertIn(n,both,tag)
     if not b in (self.ONE.get(n),TWO.get(n)):self.assertTrue(TWO[n].startswith(b),(tag,n,b))
  for mode in ('cut','torn','fail','full'):
   for op,(start,command,old,new) in ops.items():
    if mode=='torn' and old==new:continue
    steps=range(1,400) if mode in ('cut','fail') else range(0,len(new)-len(old)+len(self.OWN)+2) if mode=='torn' else range(0,3000,41)
    for k in steps:
     shutil.rmtree(self.root/'ms0:');shutil.copytree(snap[start],self.root/'ms0:');tag=(mode,op,k)
     if mode=='torn':
      # The cut falls on the list's one write, wherever in the operation that is, and k of its bytes are down.
      for fault in range(1,60):
       shutil.rmtree(self.root/'ms0:');shutil.copytree(snap[start],self.root/'ms0:');r=self.run_client(*command,ok=False,FAULT=fault,TORN=k)
       if (d/'PLUGINS.TXT').read_bytes()!=old or r.returncode==0:break
      self.assertEqual(r.returncode,77,tag)
     else:
      env={'cut':{'FAULT':k},'fail':{'FAILAT':k},'full':{'STICK_BYTES':sum(p.stat().st_size for p in (self.root/'ms0:').rglob('*') if p.is_file())+k}}[mode]
      r=self.run_client(*command,ok=False,**env);self.assertIn(r.returncode,[0,1,77],(tag,r.stderr))
     check(tag+('before the next start',),start,old,new,False);self.run_client('recover');check(tag+('after it',),start,old,new,True)
     if mode in ('cut','fail') and r.returncode==0 and k>3:break
    else:self.assertNotIn(mode,('cut','fail'),'fault sweep did not reach completion: '+op)
 def test_plugin_of_the_old_layout_moves_into_its_folder_at_its_next_update(self):
  # PSPDX 1.1 installed a plugin as the one file seplugins/<name>.prx. Such a record is turned on and off and deleted as it was. Its next update or reinstall puts the release into the folder; then, in place, the folder's own line goes after the list's end where the old one said on, spaces go over the old one, and the old file goes -- only as the file the record has the hash of. Nothing else in the list or under seplugins/ changes.
  oldline=self.OLDOWN+b'\r\n';moved=self.USER+b' '*len(self.OLDOWN)+b'\r\n'
  def fresh(**k):
   shutil.rmtree(self.root/'ms0:');(self.root/'ms0:').mkdir();d=self.plugin_setup();self.old_layout(**k);(d/'other.prx').write_bytes(b'somebody else');return d
  # Turned on: on afterwards, by the folder's line.
  d=fresh();self.assertEqual(self.run_client('plugin',ID).stdout.strip(),'1');r=self.run_client('install');self.assertIn('plugin: usbnet.prx on',r.stderr)
  self.assertEqual(self.tree(d),dict(self.inside(),**{'PLUGINS.TXT':moved+self.OWN+b'\r\n','other.prx':b'somebody else'}));installed=self.state()[ID]['installed']
  self.assertEqual((installed['installdir'],installed['plugin_line'],installed['version'],installed.get('plugin_old')),('seplugins/usbnet/usbnet.prx',self.PRX,'2',None));self.assertEqual(self.ark_state(moved+self.OWN+b'\r\n'),1);self.assertEqual(self.ark_state(moved+self.OWN+b'\r\n',self.OLDPRX),-1)
  # And from there it is a plugin in a folder like any: off, on, updated, deleted.
  self.assertEqual(self.run_client('plugin',ID,'off').stdout.strip(),'0');self.assertEqual(self.run_client('plugin',ID,'on').stdout.strip(),'1');self.assertEqual(self.run_client('uninstall',ID).stdout.strip(),'0')
  self.assertEqual(self.tree(d),{'PLUGINS.TXT':moved+b' '*len(self.OWN)+b'\r\n','other.prx':b'somebody else'})
  # Until the update the old layout is switched and deleted where it is, by this version too.
  d=fresh();self.assertEqual(self.run_client('plugin',ID,'off').stdout.strip(),'0');off=self.USER+self.OLDOWN[:-3]+b'off\r\n';self.assertEqual(self.tree(d),{'PLUGINS.TXT':off,'usbnet.prx':b'plugin one','other.prx':b'somebody else'})
  self.assertEqual(self.run_client('plugin',ID,'on').stdout.strip(),'1');self.assertEqual((d/'PLUGINS.TXT').read_bytes(),self.USER+oldline);self.run_client('recover');self.assertEqual(self.state()[ID]['installed']['installdir'],'seplugins/usbnet.prx')
  self.assertEqual(self.run_client('uninstall',ID).stdout.strip(),'0');self.assertEqual(self.tree(d),{'PLUGINS.TXT':moved,'other.prx':b'somebody else'});self.assertNotIn(ID,self.state())
  # Turned off: the old line goes, none is added, and the plugin is off.
  d=fresh();self.run_client('plugin',ID,'off');r=self.run_client('install');self.assertIn('plugin: usbnet.prx off',r.stderr);self.assertEqual(self.tree(d),dict(self.inside(),**{'PLUGINS.TXT':moved,'other.prx':b'somebody else'}));self.assertNotIn('plugin_line',self.state()[ID]['installed']);self.assertNotIn('plugin_old',self.state()[ID]['installed'])
  # Never turned on: no list is made or touched.
  d=fresh(on=False);self.run_client('install');self.assertEqual(self.tree(d),dict(self.inside(),**{'other.prx':b'somebody else'}));self.assertNotIn('plugin_old',self.state()[ID]['installed'])
  d=fresh(on=False,lst=self.USER);self.run_client('install');self.assertEqual(self.tree(d),dict(self.inside(),**{'PLUGINS.TXT':self.USER,'other.prx':b'somebody else'}))
  # The old .prx changed by hand is its owner's build: the update is refused and nothing moves, as in 1.1; a delete leaves it and its line.
  d=fresh();(d/'usbnet.prx').write_bytes(b'MY OWN BUILD');before=self.tree(d);record=self.state()[ID];r=self.run_client('install',ok=False);self.assertIn('why: its .prx was changed by hand',r.stderr);self.assertEqual(self.tree(d),before);self.assertEqual(self.state()[ID],record)
  self.assertEqual(self.run_client('uninstall',ID).stdout.strip(),'2');self.assertEqual(self.tree(d),before)
  # Deleted by hand: there is nothing to remove, and the rest moves.
  d=fresh();(d/'usbnet.prx').unlink();self.run_client('install');self.assertEqual(self.tree(d),dict(self.inside(),**{'PLUGINS.TXT':moved+self.OWN+b'\r\n','other.prx':b'somebody else'}))
  # A seplugins/usbnet/ somebody made is in the way: refused, and everything as it was.
  d=fresh();(d/'usbnet').mkdir();(d/'usbnet/mine.txt').write_bytes(b'mine');before=self.tree(d);record=self.state()[ID];r=self.run_client('install',ok=False);self.assertIn("is in the way",r.stderr);self.assertEqual(self.tree(d),before);self.assertEqual(self.state()[ID],record)
  self.assertEqual(self.run_client('plugin',ID,'off').stdout.strip(),'0')
  # A line somebody else wrote for the old file goes on loading it: PSPDX's own is moved, theirs stays, and so does the file it names.
  for theirs in (b'game, usbnet.prx, on\r\n',b'umd, ms0:/seplugins/usbnet.prx, off // mine\r\n'):
   d=fresh(lst=self.USER+theirs);self.run_client('install');self.assertEqual(self.tree(d),dict(self.inside(),**{'PLUGINS.TXT':self.USER+theirs+b' '*len(self.OLDOWN)+b'\r\n'+self.OWN+b'\r\n','other.prx':b'somebody else','usbnet.prx':b'plugin one'}));self.assertNotIn('plugin_old',self.state()[ID]['installed'])
  # The record's line no longer PSPDX's own in the list (changed by hand): not touched, none added; the file it names stays.
  d=fresh();hand=self.USER+b'always, ms0:/seplugins/usbnet.prx, on // keep\r\n';(d/'PLUGINS.TXT').write_bytes(hand);self.run_client('install');self.assertEqual(self.tree(d),dict(self.inside(),**{'PLUGINS.TXT':hand,'other.prx':b'somebody else','usbnet.prx':b'plugin one'}));self.assertNotIn('plugin_line',self.state()[ID]['installed'])
  # A list that cannot be written when the update comes: the folder is in, the old file and its line go on as they are, and the move is finished at the first start that can write it.
  d=fresh();(d/'PLUGINS.TXT').chmod(0o444);self.run_client('install');self.assertEqual(self.tree(d),dict(self.inside(),**{'PLUGINS.TXT':self.USER+oldline,'other.prx':b'somebody else','usbnet.prx':b'plugin one'}));self.assertEqual(self.state()[ID]['installed']['plugin_old'],dict(file='usbnet.prx',sha256=hashlib.sha256(b'plugin one').hexdigest(),line=self.OLDPRX))
  self.run_client('recover');self.assertEqual((d/'usbnet.prx').read_bytes(),b'plugin one');(d/'PLUGINS.TXT').chmod(0o644);self.run_client('recover')
  self.assertEqual(self.tree(d),dict(self.inside(),**{'PLUGINS.TXT':moved+self.OWN+b'\r\n','other.prx':b'somebody else'}));self.assertNotIn('plugin_old',self.state()[ID]['installed']);self.assertEqual(self.run_client('plugin',ID).stdout.strip(),'1')
  # The same asked of a switch or a delete instead of a start.
  for command,after in ((('plugin',ID,'off'),moved+self.OWN[:-3]+b'off\r\n'),(('uninstall',ID),moved+b' '*len(self.OWN)+b'\r\n')):
   d=fresh();(d/'PLUGINS.TXT').chmod(0o444);self.run_client('install');(d/'PLUGINS.TXT').chmod(0o644);self.run_client(*command);self.assertEqual((d/'PLUGINS.TXT').read_bytes(),after);self.assertFalse((d/'usbnet.prx').exists())
  # A power cut or a failing call anywhere in the move, and a write to the list cut after any number of its bytes: after the next start the stick holds the old layout whole or the new one whole, and at every moment the list is the old one, or the old one with the new line after it, or that with spaces over the old line -- never a byte else.
  after=moved+self.OWN+b'\r\n';both=self.USER+oldline+self.OWN+b'\r\n';old_tree={'PLUGINS.TXT':self.USER+oldline,'other.prx':b'somebody else','usbnet.prx':b'plugin one'};new_tree=dict(self.inside(),**{'PLUGINS.TXT':after,'other.prx':b'somebody else'})
  start=self.root/'start';fresh();shutil.copytree(self.root/'ms0:',start);seen=set()
  for mode,steps in (('FAULT',range(1,400)),('FAILAT',range(1,400))):
   for k in steps:
    shutil.rmtree(self.root/'ms0:');shutil.copytree(start,self.root/'ms0:');r=self.run_client('install',ok=False,**{mode:k});self.assertIn(r.returncode,[0,1,77],(mode,k,r.stderr))
    text=(d/'PLUGINS.TXT').read_bytes();self.assertTrue(text in (self.USER+oldline,both,after) or (text.startswith(self.USER+oldline) and both.startswith(text)),(mode,k,text));self.assertEqual((d/'other.prx').read_bytes(),b'somebody else')
    self.run_client('recover');tree=self.tree(d);self.assertIn(tree,(old_tree,new_tree),(mode,k));seen.add(tree==new_tree);installed=self.state()[ID]['installed'];self.assertNotIn('plugin_write',installed)
    self.assertEqual((installed['installdir'],installed.get('plugin_line'),installed.get('plugin_old')),('seplugins/usbnet/usbnet.prx',self.PRX,None) if tree==new_tree else ('seplugins/usbnet.prx',self.OLDPRX,None),(mode,k))
    if r.returncode==0 and k>3:break
   else:self.fail('fault sweep did not reach completion')
  self.assertEqual(seen,{True,False})
  for torn in range(1,len(self.OWN)+2,6):
   for fault in range(1,120):
    shutil.rmtree(self.root/'ms0:');shutil.copytree(start,self.root/'ms0:');r=self.run_client('install',ok=False,FAULT=fault,TORN=torn);text=(d/'PLUGINS.TXT').read_bytes()
    if r.returncode==0:break
    self.assertEqual(len([1 for a,b in zip(text,both) if a not in (b,32)]),0,(torn,fault,text));self.assertLessEqual(len(text),len(both))
    self.run_client('recover');self.assertIn(self.tree(d),(old_tree,new_tree),(torn,fault))
 def test_plugin_texts_fit_their_bands(self):
  # Every line a plugin puts on the screen, measured the way gui/font.c measures it -- the font's own advances, as intraFont adds them up -- against the room its band gives it. The font is the one the release carries, as wide as the console's ltn8; the fallback face, ltn0, runs 1.46 times as wide, and the lines have to fit in that too. The status line is one line of 448 px at FONT_META; a question's title one of 440 at FONT_TITLE, and its line is broken at 400 into at most three, the last of which takes what is left.
  import re,sys;sys.path.insert(0,str(pathlib.Path(__file__).parent));import pgf
  app=pathlib.Path(__file__).resolve().parents[2]/'app';font=pgf.Font(app/'assets/font.pgf');wide=1.46
  text=dict(re.findall(r'^#define (T_\w+)\s+"((?:[^"\\]|\\.)*)"',(app/'text.h').read_text(),re.M))
  name,version='pspkit-usbnet','0.1.3'
  def fill(t,*a):
   it=iter(a);return re.sub(r'%(?:\.\d+)?l?[sdu]',lambda m:next(it),t)
  def lines(shown):
   # gui/shell.c's break_lines, in the wide face: how many lines of 400, and how wide the last.
   out=[''];words=shown.split(' ')
   for w in words:
    if out[-1] and font.width(out[-1]+' '+w,1.2)*wide>400 and len(out)<3:out.append(w)
    else:out[-1]=(out[-1]+' '+w).strip()
   return out
  status=[('T_PLUGIN_ON',()),('T_PLUGIN_OFF',()),('T_PLUGIN_UPDATED',(name,version)),('T_PLUGIN_REMOVED',(name,)),('T_PLUGIN_REMOVED_LINES',()),('T_PLUGIN_LEFT',()),('T_PLUGIN_KEPT',()),('T_PLUGIN_NOT_OURS',()),('T_PLUGIN_FAILED',())]
  # A reason in each line that can give it -- an install's, a delete's, a switch's -- and in the 64 bytes a reason is kept in.
  for key in text:
   if key.startswith('T_WHY_') and ('PRX' in key or 'LIST' in key):
    said=('T_PLUGIN_FAILED_WHY','T_PLUGIN_NOT_DELETED') if 'LIST' in key else ('T_PLUGIN_NOT_INSTALLED','T_PLUGIN_NOT_DELETED') if 'READONLY' in key else ('T_PLUGIN_NOT_INSTALLED',)
    # One reason names what is in the way, seplugins/<name>: measured with a name as long as the cable plugin's; a longer one wraps in the band the refusal stands in.
    why=text[key].replace('%s','usbnet') if key=='T_WHY_PRX_THERE' else text[key]
    self.assertNotIn('%',why);self.assertLess(len(why),64,key);status+=[(line,(why,)) for line in said]
  self.assertGreater(len(status),25)
  for key,args in status:
   shown=fill(text[key],*args);self.assertLessEqual(font.width(shown)*wide,448,shown);self.assertLess(len(shown.encode()),96,shown)
  def heads(shown):
   # The question's own lines of 440, three at most, broken as gui/shell.c breaks them, in the wide face.
   out=[''];
   for w in shown.split(' '):
    if out[-1] and font.width(out[-1]+' '+w,1.2)*wide>440 and len(out)<3:out.append(w)
    else:out[-1]=(out[-1]+' '+w).strip()
   return out
  # Every question that carries a name, with the longest a catalog may give -- 40 characters -- and the folder's 32: in the band's lines, two in the console's face and three in the wider one. The start question ran over both edges with such a name.
  # (One word wider than the band by itself cannot be broken, and is cut at the band's edge by the clipped print: not measured here.)
  longest=('Remote Joystick Bridge for the USB Port','Grand Winter Mountain Rally Championship','Worldwide Homebrew Warehouse Manager MMX')
  self.assertTrue(all(39<=len(n)<=40 for n in longest))
  for key,names in (('T_RUN_ASK',longest),('T_REMOVE_ASK',longest),('T_PLUGIN_INSTALLED_ASK',longest+(name,)),('T_DIR_EXISTS_ASK',('Remote_Joystick_Bridge_USB_Port1',)),('T_PLUGIN_ON_ASK',('',)),('T_PLUGIN_OFF_ASK',('',)),('T_CABLE_CONNECT_ASK',('',)),('T_CABLE_GATEWAY_ASK',('',)),('T_CABLE_SEARCH_ASK',('',)),('T_CABLE_FOUND_ASK',('',)),('T_CABLE_NONE',('',)),('T_CABLE_LOOKING',('',)),('T_RESTART_ASK',('',)),('T_INBOX_ASK',('64','s')),('T_ALL_ASK_INSTALL',('4096','s')),('T_ALL_ASK_UPDATE',('4096','s'))):
   for n in names:
    shown=fill(text[key],*([n] if isinstance(n,str) and key not in ('T_ALL_ASK_INSTALL','T_ALL_ASK_UPDATE','T_INBOX_ASK') else names)) if '%' in text[key] else text[key]
    broken=heads(shown);self.assertLessEqual(len(broken),3,shown);self.assertTrue(all(font.width(l,1.2)*wide<=440 for l in broken),broken);self.assertLessEqual(sum(font.width(l,1.2) for l in broken),2*440*0.9,shown);self.assertLess(len(shown.encode()),200,shown)
  for key in ('T_CABLE_OFF','T_CABLE_NOT_LOADED'):self.assertLessEqual(font.width(text[key])*wide,448,key);self.assertLess(len(text[key]),64,key)
  url=text['T_CABLE_GATEWAY_URL'];text['T_CABLE_GATEWAY_LINE']='Download it at '+url;self.assertIn('"Download it at " T_CABLE_GATEWAY_URL',(app/'text.h').read_text());self.assertEqual(url,'github.com/chriopter/pspkit-usbnet')
  # Not found, the same words after what to do about it; and the footer's two words there no wider than Yes and No are in the longest of the band's faces.
  text['T_CABLE_NONE_LINE']='Start it on your PC. '+text['T_CABLE_GATEWAY_LINE'];self.assertIn('"Start it on your PC. " T_CABLE_GATEWAY_LINE',(app/'text.h').read_text());self.assertLessEqual(font.width(text['T_CABLE_RETRY']+text['T_HINT_CANCEL'],1.2)*wide,200);self.assertNotIn('T_CABLE_SURE_ASK',text)
  # The connection's menu, and its row under the gear with the icon at its end: the word in the room the icon leaves (the row's 164 less the icon's 30 and 2 between, with a tenth to spare for the console's own font), its note in the lines a note has; the switch Options had is gone.
  self.assertLessEqual(font.width(text['T_CABLE_USB']),font.width(text['T_STORAGE_INTERNAL']));self.assertLessEqual(font.width(text['T_SET_CABLE'],1.2)*1.1,164-30-2);self.assertLessEqual(len(text['T_NOTE_CABLE']),len(text['T_SYS_FILL_NOTE'])+25);self.assertNotIn('T_SYS_CABLE',text);self.assertNotIn('T_CABLE_ON',text)
  # A line breaks at 400 and is drawn centred, unclipped, in a band 480 wide: a word that cannot be broken -- the gateway's address -- may stand alone on its line up to the title's 440.
  for key in ('T_PLUGIN_TURN_ON_LINE','T_PLUGIN_ON_LINE','T_PLUGIN_OFF_LINE','T_CABLE_CONNECT_LINE','T_CABLE_GATEWAY_LINE','T_CABLE_NONE_LINE','T_CABLE_GO_ON_LINE'):
   broken=lines(text[key]);self.assertLessEqual(len(broken),3,broken);self.assertTrue(all(font.width(l,1.2)*wide<=(440 if ' ' not in l else 400) for l in broken),broken);self.assertLess(len(text[key]),200)
  # The address is whole and on one line in either face.
  for key in ('T_CABLE_GATEWAY_LINE','T_CABLE_NONE_LINE'):self.assertTrue(any(url in l.split() for l in lines(text[key])),lines(text[key]))
  # The menu's row, in the 32 bytes it is kept in; and the card's line, no longer than the one it stands in for.
  for key in ('T_MENU_PLUGIN_ON','T_MENU_PLUGIN_OFF'):self.assertLessEqual(font.width(text[key]),font.width(text['T_MENU_REBUILD']),key)
  self.assertLess(font.width(fill(text['T_PANEL_PLUGIN_OFF'],version)),font.width(fill(text['T_PANEL_REBUILD'],version,'1.2 MB')))
  # The measure itself: a word is as wide as its letters, at any size, and no letter is nothing or a whole band.
  self.assertTrue(11*4<font.width('PLUGINS.TXT')<11*12);self.assertEqual(font.width('ab',1.2),1.2*(font.width('a')+font.width('b')))
 # ---- the cable: pspkit-usbnet's plugin, carried in the program
 CABLE='io.github.chriopter.pspkitusbnet'
 def cable_setup(self,tag='v0.1.3',prx=b'usbnet module',note=True):
  # What the program carries of the plugin (the harness reads it from carried/, see host/carried.c), and the release's zip as a catalog lists it.
  d=self.root/'carried';d.mkdir(parents=True,exist_ok=True);self.zip('usbnet-psp.zip',{'usbnet.prx':prx,'LICENSE':b'MIT'});zipsha=hashlib.sha256((self.root/'usbnet-psp.zip').read_bytes()).hexdigest()
  if prx is not None:(d/'usbnet.prx').write_bytes(prx)
  if note:(d/'usbnet.txt').write_text('tag=%s\nzip=%s\n'%(tag,zipsha))
  return zipsha
 def cable(self,*args,**env):
  r=self.run_client('cable',*args,**env);return r.stdout.rstrip('\n')
 def test_cable_is_asked_about_once_and_only_where_it_can_be_offered(self):
  # How to connect is a question only at a start that has never had the answer, with the plugin's copy and the release it is of carried in the program, a kernel to load it and no cable plugin there already. Nothing is loaded or installed by the start itself.
  note=self.root/'loaded.txt';self.cable_setup();d=self.root/'ms0:/seplugins'
  self.assertEqual(self.cable(0,KERNEL=1,LOADED_NOTE=note),'1 0 0');self.assertFalse(note.exists());self.assertFalse(d.exists())
  # No kernel to ask -- an emulator: no question, and nothing done. Answered already, either way: no question.
  self.assertEqual(self.cable(0),'0 0 0');self.assertEqual(self.cable(1,KERNEL=1,LOADED_NOTE=note),'0 0 0');self.assertFalse(note.exists())
  # The copy or the note of its release missing, or the note not one: no question.
  for prx,text in ((None,None),(b'x',''),(b'x','tag=v1\n'),(b'x','zip=%s\n'%('a'*64)),(b'x','tag=v1\nzip=xyz\n')):
   shutil.rmtree(self.root/'carried');g=self.root/'carried';g.mkdir()
   if prx:(g/'usbnet.prx').write_bytes(prx)
   if text is not None:(g/'usbnet.txt').write_text(text)
   self.assertEqual(self.cable(0,KERNEL=1),'0 0 0',(prx,text))
  # Loaded by the firmware from a copy somebody put there by hand: it is there, and nothing is asked.
  self.cable_setup();self.assertEqual(self.cable(0,KERNEL=1,LOADED=1),'0 1 1')
  # The cable chosen: installed, turned on, loaded at once from the installed copy; and from then on a start asks nothing and loads nothing, on or off.
  self.assertEqual(self.cable(0,'use',KERNEL=1,LOADED_NOTE=note),'0 1 ');self.assertEqual(note.read_text(),'ms0:/seplugins/usbnet/usbnet.prx\n')
  self.assertEqual(self.tree(d),dict(self.inside({'usbnet.prx':b'usbnet module'}),**{'PLUGINS.TXT':b'always, ms0:/seplugins/usbnet/usbnet.prx, on \n'}));note.unlink()
  for choice in (0,1,2,3):self.assertEqual(self.cable(choice,KERNEL=1,LOADED_NOTE=note),'0 1 0')
  self.run_client('plugin',self.CABLE,'off')
  for choice in (0,2):self.assertEqual(self.cable(choice,KERNEL=1,LOADED_NOTE=note),'0 1 0')
  self.assertFalse(note.exists())
  # Chosen again under Options with the plugin installed and off: turned on and loaded, nothing installed anew.
  before=self.state()[self.CABLE];self.assertEqual(self.cable(1,'use',KERNEL=1,LOADED_NOTE=note),'0 1 ');self.assertEqual(self.run_client('plugin',self.CABLE).stdout.strip(),'1')
  self.assertEqual(self.state()[self.CABLE]['installed']['plugin_sha256'],before['installed']['plugin_sha256'])
 def test_cable_looks_for_the_gateway_before_it_asks(self):
  # The cable chosen and its module loaded, the plugin is asked whether the gateway answers and what is put to the user follows from that. Printed: module ready, what the look found (0 there, 1 not, 2 not said), what is asked (0 nothing, 1 to connect, 2 to look again, 3 whether it runs).
  self.cable_setup();self.assertEqual(self.cable(0,'use',KERNEL=1),'0 1 ')
  # Until the cable has connected once: found, the connect question; not found, Retry or Cancel, and found after a retry; a plugin from before it could look -- or none of its answers understood -- the old question whether the gateway runs.
  self.assertEqual(self.cable(2,'look',KERNEL=1,LOADED=1,PROBE=1),'1 0 1');self.assertEqual(self.cable(2,'look',KERNEL=1,LOADED=1,PROBE=0),'1 1 2');self.assertEqual(self.cable(2,'look',KERNEL=1,LOADED=1),'1 2 3')
  # Once it has: nothing is asked, whatever the look says; nor of Wi-Fi, nor before an answer.
  for choice in (0,1,3):
   for probe in ({'PROBE':1},{'PROBE':0},{}):self.assertEqual(self.cable(choice,'look',KERNEL=1,LOADED=1,**probe)[-1],'0',(choice,probe))
  # No module, nothing to ask: the look is not said, not "not found".
  shutil.rmtree(self.root/'ms0:/seplugins');shutil.rmtree(self.root/'ms0:/PSP/PSPDX/INSTALLED');self.assertEqual(self.cable(2,'look',KERNEL=1,LOAD_FAILS=1,PROBE=1),'0 2 3')
  # Played at the desk, PSPDX.CABLE's first character is the gateway.
  flag=self.root/'ms0:/PSP/PSPDX/DEBUG/PSPDX.CABLE';flag.parent.mkdir(parents=True,exist_ok=True)
  for said,want in ((b'1','1 0 1'),(b'0','1 1 2'),(b'','1 2 3'),(b'yes\n','1 2 3')):
   flag.write_bytes(said);self.assertEqual(self.cable(2,'look'),want,said)
 def test_cable_installs_the_carried_copy_as_the_store_would(self):
  # The carried copy goes in through the plugin installer: the record is the one an install from the catalog of the same release writes -- the catalog's id, the tag for a version, the zip's hash -- so the same release in a catalog is current and a newer one an update, which then installs over it like any.
  zipsha=self.cable_setup();d=self.root/'ms0:/seplugins';(self.root/'manifest.json').unlink();self.assertEqual(self.cable(0,'use',KERNEL=1),'0 1 ')
  rec=self.state()[self.CABLE];installed=rec['installed'];self.assertEqual(rec['source'],'https://github.com/chriopter/pspkit-usbnet')
  self.assertEqual({k:installed[k] for k in ('version','installdir','device','sha256','plugin_sha256','plugin_line')},dict(version='0.1.3',installdir='seplugins/usbnet/usbnet.prx',device='ms0:',sha256=zipsha,plugin_sha256=hashlib.sha256(b'usbnet module').hexdigest(),plugin_line='ms0:/seplugins/usbnet/usbnet.prx'))
  saved=json.loads((self.root/f'ms0:/PSP/PSPDX/INSTALLED/{self.CABLE}.pspdx').read_text());self.assertEqual((saved['type'],saved['source'],saved['schema']),('plugin',rec['source'],SCHEMA))
  self.assertFalse((self.root/'ms0:/PSP/PSPDX/TMP/transaction.json').exists())
  app=dict(id=self.CABLE,name='pspkit-usbnet',type='plugin',category='plugin',author='chriopter',source='https://github.com/chriopter/pspkit-usbnet',releases=[dict(tag='v0.1.3',published_at='2026-10-06T08:38:00Z',size=(self.root/'usbnet-psp.zip').stat().st_size,sha256=zipsha,url='https://github.com/chriopter/pspkit-usbnet/releases/download/v0.1.3/download.zip')])
  def rows(app):
   self.write('catalog.json',dict(schema='https://chriopter.github.io/pspdx/schema/catalog-v1.json',generated_at=NOW(),apps=[app]));(self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('https://example.com/catalog.json\n')
   return {row.split()[0]:row.split() for row in self.run_client('fetch').stdout.splitlines()}[self.CABLE]
  self.assertEqual(rows(app)[1:],['0.1.3','1','2','0'])
  # The same catalog's entry installed by the store on another stick writes the same record, but for the line, which the store leaves to be asked about.
  other=self.root/'other';shutil.copytree(self.root/'ms0:',other);shutil.rmtree(self.root/'ms0:/seplugins');(self.root/f'ms0:/PSP/PSPDX/INSTALLED/{self.CABLE}.state.json').unlink();(self.root/f'ms0:/PSP/PSPDX/INSTALLED/{self.CABLE}.pspdx').unlink()
  self.assertEqual(self.run_client('get',self.CABLE,ZIP_FILE=self.root/'usbnet-psp.zip').stdout.strip(),'0');store=self.state()[self.CABLE]['installed']
  self.assertEqual({k:store[k] for k in ('version','installdir','device','sha256','plugin_sha256')},{k:installed[k] for k in ('version','installdir','device','sha256','plugin_sha256')});self.assertEqual(installed['plugin_files'],{'usbnet.prx':installed['plugin_sha256']});self.assertEqual(store['plugin_files'],dict(installed['plugin_files'],LICENSE=hashlib.sha256(b'MIT').hexdigest()))
  shutil.rmtree(self.root/'ms0:');shutil.copytree(other,self.root/'ms0:')
  # A newer release in the catalog: an update, and the store installs it over the carried copy, with the files the carried copy had none of, the line and its on left as they are.
  self.zip('usbnet-psp.zip',{'usbnet.prx':b'usbnet module, newer','LICENSE':b'MIT'});newer=dict(app,releases=[dict(app['releases'][0],tag='v0.1.4',published_at='2026-10-07T08:00:00Z',size=(self.root/'usbnet-psp.zip').stat().st_size,sha256=hashlib.sha256((self.root/'usbnet-psp.zip').read_bytes()).hexdigest(),url='https://github.com/chriopter/pspkit-usbnet/releases/download/v0.1.4/download.zip')])
  self.assertEqual(rows(newer)[1:],['0.1.4','1','3','0']);self.assertEqual(self.run_client('get',self.CABLE,ZIP_FILE=self.root/'usbnet-psp.zip').stdout.strip(),'0')
  self.assertEqual(self.tree(d),dict(self.inside({'usbnet.prx':b'usbnet module, newer','LICENSE':b'MIT'}),**{'PLUGINS.TXT':b'always, ms0:/seplugins/usbnet/usbnet.prx, on \n'}));self.assertEqual(self.state()[self.CABLE]['installed']['version'],'0.1.4');self.assertEqual(rows(newer)[3],'2')
  # On a PSP Go started from the internal storage the plugin goes there, and its line names ef0:.
  shutil.rmtree(self.root/'ms0:');(self.root/'ms0:').mkdir();(self.root/'ef0:/PSP/GAME/PSPDX').mkdir(parents=True);g=self.root/'carried';(g/'usbnet.prx').write_bytes(b'usbnet module');(g/'usbnet.txt').write_text('tag=0.1.3\nzip=%s\n'%zipsha)
  boot='ef0:/PSP/GAME/PSPDX/EBOOT.PBP';self.assertEqual(self.cable(0,'use',KERNEL=1,DEVICE=boot),'0 1 ');self.assertEqual(self.tree(self.root/'ef0:/seplugins'),dict(self.inside({'usbnet.prx':b'usbnet module'}),**{'PLUGINS.TXT':b'always, ef0:/seplugins/usbnet/usbnet.prx, on \n'}));self.assertFalse((self.root/'ms0:/seplugins').exists())
 def test_a_plugin_copy_beside_the_eboot_is_no_longer_the_carried_one(self):
  # Releases up to 1.1.4 put usbnet.prx and usbnet.txt beside the EBOOT. They are not looked at: a program that carries none asks nothing with them there, one that carries its own installs its own, and the old files stay as they are.
  g=self.root/'ms0:/PSP/GAME/PSPDX';g.mkdir(parents=True);(g/'usbnet.prx').write_bytes(b'an older build');(g/'usbnet.txt').write_text('tag=v0.0.1\nzip=%s\n'%('a'*64))
  self.assertEqual(self.cable(0,KERNEL=1),'0 0 0')
  zipsha=self.cable_setup(tag='v0.1.3',prx=b'usbnet module');self.assertEqual(self.cable(0,KERNEL=1),'1 0 0')
  self.assertEqual(self.cable(0,'use',KERNEL=1),'0 1 ');installed=self.state()[self.CABLE]['installed']
  self.assertEqual((installed['version'],installed['sha256']),('0.1.3',zipsha));self.assertEqual((self.root/'ms0:/seplugins/usbnet/usbnet.prx').read_bytes(),b'usbnet module')
  self.assertEqual((g/'usbnet.prx').read_bytes(),b'an older build');self.assertTrue((g/'usbnet.txt').exists())
 def test_cable_refused_says_why_and_still_serves_the_session(self):
  # Whatever the plugin installer refuses it refuses here: nothing is written into a seplugins/usbnet/ somebody put there, a list that is not to be written is not written. The reason is one line, the carried copy is loaded for the session all the same, and a later start with the cable chosen loads it again, nothing else.
  note=self.root/'loaded.txt';self.cable_setup();d=self.root/'ms0:/seplugins';d.mkdir();(d/'usbnet').mkdir();(d/'usbnet/usbnet.prx').write_bytes(b'mine');(d/'PLUGINS.TXT').write_bytes(self.USER);before=self.tree(d)
  self.assertEqual(self.cable(0,'use',KERNEL=1,LOADED_NOTE=note),"0 1 Not installed: seplugins/usbnet is in the way.");self.assertEqual(self.tree(d),before);self.assertEqual(self.state(),{})
  self.assertEqual(note.read_text(),'ms0:/PSP/PSPDX/CACHE/usbnet.prx\n');note.unlink()
  self.assertEqual(self.cable(2,KERNEL=1,LOADED_NOTE=note),'0 1 1');self.assertEqual(note.read_text(),'ms0:/PSP/PSPDX/CACHE/usbnet.prx\n');note.unlink()
  self.assertEqual(self.cable(1,KERNEL=1,LOADED_NOTE=note),'0 0 0');self.assertFalse(note.exists());self.assertEqual(self.tree(d),before)
  # Installed, but the list cannot be written: the plugin is in and off, the line says why, the module is loaded.
  shutil.rmtree(d/'usbnet');(d/'PLUGINS.TXT').chmod(0o444);self.assertEqual(self.cable(0,'use',KERNEL=1,LOADED_NOTE=note),'0 1 Not changed: PLUGINS.TXT is read-only.')
  self.assertEqual(self.tree(d),dict(before,**self.inside({'usbnet.prx':b'usbnet module'})));self.assertEqual(self.run_client('plugin',self.CABLE).stdout.strip(),'0');self.assertEqual(note.read_text(),'ms0:/seplugins/usbnet/usbnet.prx\n')
  # A line of somebody's own that keeps it off is theirs: said, not changed.
  (d/'PLUGINS.TXT').chmod(0o644);hand=self.USER+b'game, usbnet/usbnet.prx, off\r\n';(d/'PLUGINS.TXT').write_bytes(hand);self.assertEqual(self.cable(0,'use',KERNEL=1),'0 1 Not changed: other lines name it.');self.assertEqual((d/'PLUGINS.TXT').read_bytes(),hand)
  # The module will not load: that is what comes back, with the plugin installed and on for the next start.
  shutil.rmtree(self.root/'ms0:/seplugins');shutil.rmtree(self.root/'ms0:/PSP/PSPDX/INSTALLED');self.assertEqual(self.cable(0,'use',KERNEL=1,LOAD_FAILS=1),'-1 0 ');self.assertEqual(self.run_client('plugin',self.CABLE).stdout.strip(),'1')
  # A power cut anywhere in it leaves, after the next start, the plugin whole or not there, and nothing else under seplugins/.
  for fault in range(1,200):
   shutil.rmtree(self.root/'ms0:');(self.root/'ms0:').mkdir();self.cable_setup();d.mkdir();(d/'PLUGINS.TXT').write_bytes(self.USER)
   r=self.run_client('cable',0,'use',ok=False,KERNEL=1,FAULT=fault);self.run_client('recover');files=self.tree(d);text=files.pop('PLUGINS.TXT');self.assertIn(text,(self.USER,self.USER+self.OWN+b'\r\n'),fault)
   self.assertEqual(files,self.inside({'usbnet.prx':b'usbnet module'}) if self.CABLE in self.state() else {},fault)
   if r.returncode==0:break
  else:self.fail('fault sweep did not reach completion')
  self.assertGreater(fault,20)
 def test_a_plugin_is_one_by_its_type_and_shown_as_one_by_its_category(self):
  # What an entry is decides its category: a plugin stands under "plugin" whatever category it names or leaves out, on its page and in the store's rows, and nowhere else; a homebrew that carries the tag "plugin", or even writes it for a category, is a homebrew and stands where its own category puts it, or nowhere.
  self.fixtures();catalog=json.loads((self.root/'catalog.json').read_text());first=catalog['apps'][0]
  def app(n,**fields):
   source='https://github.com/test/app%d'%n;a=dict(first,id='io.github.test.app%d'%n,name='App %d'%n,source=source,installdir='PSP/GAME/App%d'%n,releases=[dict(first['releases'][0],url=source+'/releases/download/v2/download.zip')],**fields)
   if fields.get('type')=='plugin':a.pop('installdir')
   a.pop('category',None) if 'category' not in fields else None;return a
  apps=[app(0,type='plugin',tags=['plugin']),app(1,type='plugin',tags=['tool'],category='game'),app(2,type='plugin',tags=[],category='Plugin'),app(3,tags=['plugin']),app(4,tags=['plugin'],category='app'),app(5,tags=['x'],category='plugin'),app(6,tags=['plugin','game'])]
  self.write('catalog.json',dict(catalog,apps=apps));(self.root/'manifest.json').unlink()
  got=dict(line.split() for line in self.run_client('categories').stdout.splitlines())
  self.assertEqual([got['io.github.test.app%d'%n] for n in range(7)],['plugin','plugin','plugin','-','app','-','-'])
  listed=lambda word:sorted(self.run_client('listing',word).stdout.split())
  self.assertEqual(listed('plugin'),['io.github.test.app0','io.github.test.app1','io.github.test.app2']);self.assertEqual(listed('app'),['io.github.test.app4'])
  self.assertNotIn('io.github.test.app1',listed('game'))
  # One row for them among the categories, by the one word, however the entries spell theirs.
  r=self.run_client('groups').stdout;self.assertEqual(r.count('0 plugin 3'),1,r);self.assertNotIn('Plugin',r)
 def test_plugins_are_listed_and_installed(self):
  # A plugin in a catalog is a row that installs like any other, from its entry where the repository has no .pspdx; an ISO is listed and not installed.
  self.fixtures();catalog=json.loads((self.root/'catalog.json').read_text());first=catalog['apps'][0]
  plugin={k:v for k,v in dict(first,id='io.github.test.plug',name='Plug',type='plugin',category='plugin',tags=['plugin'],source='https://github.com/test/plug',releases=[dict(first['releases'][0],url='https://github.com/test/plug/releases/download/v2/download.zip')]).items() if k!='installdir'}
  iso=dict(plugin,id='io.github.test.disc',name='Disc',type='iso',source='https://github.com/test/disc',releases=[dict(first['releases'][0],url='https://github.com/test/disc/releases/download/v2/download.zip')])
  mirror={k:v for k,v in dict(first,id='de.wijsman.blocks',name='Blocks',source='https://archive.org/details/psp-blocks',releases=[dict(first['releases'][0],url='https://archive.org/download/psp-blocks/blocks.zip')]).items() if k!='installdir'}
  self.write('catalog.json',dict(catalog,apps=[first,plugin,iso,mirror]))
  r=self.run_client('fetch',VERBOSE=1);rows={row.split()[0]:row.split() for row in r.stdout.splitlines()}
  # A mirror installs from its entry, and so does a plugin; an ISO, from anywhere, is listed and not installed.
  self.assertEqual((rows[ID][4],rows['io.github.test.plug'][4],rows['io.github.test.disc'][4],rows['de.wijsman.blocks'][4]),('0','0','1','0'),r.stdout)
  self.assertEqual(rows['io.github.test.plug'][3],'1');self.assertIn('io.github.test.plug',self.run_client('listing','plugin').stdout.split())
  r=self.run_client('prepare','io.github.test.disc',ok=False,VERBOSE=1);self.assertEqual(r.stdout.strip(),'-1');self.assertIn('cannot be installed yet',r.stderr)
  (self.root/'manifest.json').unlink();self.zip('plug.zip',{'plug.prx':b'a plugin','LICENSE':b'MIT'});size=(self.root/'plug.zip').stat().st_size
  plugin['releases'][0].update(size=size,sha256=hashlib.sha256((self.root/'plug.zip').read_bytes()).hexdigest());self.write('catalog.json',dict(catalog,apps=[first,plugin]))
  r=self.run_client('get','io.github.test.plug',VERBOSE=1,ZIP_FILE=self.root/'plug.zip');self.assertEqual(r.stdout.strip(),'0',r.stderr)
  dir=self.root/'ms0:/seplugins';self.assertEqual(self.tree(dir),self.inside({'plug.prx':b'a plugin','LICENSE':b'MIT'},'plug'))
  saved=json.loads((self.root/'ms0:/PSP/PSPDX/INSTALLED/io.github.test.plug.pspdx').read_text());self.assertEqual((saved['type'],saved.get('installdir')),('plugin',None))
  # Installed and current in the list; a newer zip in the catalog is an update like any other, and without the catalog the row comes back out of its record.
  rows={row.split()[0]:row.split() for row in self.run_client('fetch').stdout.splitlines()};self.assertEqual(rows['io.github.test.plug'][3:],['2','0'])
  plugin['releases'][0].update(tag='v3',published_at='1970-01-01T00:00:03Z',sha256='1'*64);self.write('catalog.json',dict(catalog,apps=[first,plugin]))
  rows={row.split()[0]:row.split() for row in self.run_client('fetch').stdout.splitlines()};self.assertEqual(rows['io.github.test.plug'][3:],['3','0'])
  self.write('catalog.json',dict(catalog,apps=[first]));rows={row.split()[0]:row.split() for row in self.run_client('fetch',OFFLINE=1).stdout.splitlines()};self.assertEqual(rows['io.github.test.plug'][4],'0')
  # An entry that names the .prx to load, for a repository with no .pspdx: the word goes into the file the install checks and saves, and decides between two.
  self.run_client('uninstall','io.github.test.plug');self.zip('plug.zip',{'plug.prx':b'a plugin','kernel.prx':b'k'});named=dict(plugin,plugin='kernel.prx');named['releases']=[dict(plugin['releases'][0],size=(self.root/'plug.zip').stat().st_size,sha256=hashlib.sha256((self.root/'plug.zip').read_bytes()).hexdigest())]
  self.write('catalog.json',dict(catalog,apps=[first,named]));self.assertEqual(self.run_client('get','io.github.test.plug',ZIP_FILE=self.root/'plug.zip').stdout.strip(),'0');self.assertEqual(self.tree(dir),self.inside({'plug.prx':b'a plugin','kernel.prx':b'k'},'kernel'))
  self.assertEqual(json.loads((self.root/'ms0:/PSP/PSPDX/INSTALLED/io.github.test.plug.pspdx').read_text())['plugin'],'kernel.prx')
  self.run_client('uninstall','io.github.test.plug');self.write('catalog.json',dict(catalog,apps=[first,{k:v for k,v in named.items() if k!='plugin'}]));r=self.run_client('get','io.github.test.plug',ok=False,ZIP_FILE=self.root/'plug.zip');self.assertIn('must name a .prx',r.stderr);self.assertEqual(self.tree(dir),{})
  # A plugin carrying an installdir is not listed at all; a mirror the list calls a word is listed as its source's host and name make it, and one it gives an id keeps that id.
  self.write('catalog.json',dict(catalog,apps=[first,dict(plugin,installdir='PSP/GAME/Plug'),dict(mirror,id='blocks')]))
  self.assertEqual([row.split()[0] for row in self.run_client('fetch').stdout.splitlines()],[ID,'org.archive.blocks'])
  self.write('catalog.json',dict(catalog,apps=[first,dict(mirror,id='de.wijsman.other')]))
  self.assertEqual([row.split()[0] for row in self.run_client('fetch').stdout.splitlines()],[ID,'de.wijsman.other'])
  # The origin path skips a .pspdx whose source is not GitHub; INBOX takes a plugin and keeps an ISO for a later version.
  (self.root/'catalog.txt').write_text(SPEC['source']+'\n');(self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('https://example.com/pspdx/\n')
  self.write('manifest.json',dict(SPEC,source='https://archive.org/details/psp-blocks'))
  r=self.run_client('fetch',CATALOG_DOWN=1,FORCE=1,VERBOSE=1);self.assertIn('outside GitHub',r.stderr)
  plug={k:v for k,v in dict(SPEC,type='iso',source='https://github.com/test/disc').items() if k!='installdir'}
  self.write('ms0:/PSP/PSPDX/INBOX/disc.pspdx',plug);r=self.run_client('inbox',VERBOSE=1);self.assertEqual(r.stdout.strip(),'0');self.assertIn('cannot install yet; kept',r.stderr)
 def test_a_catalog_names_its_entries_as_it_likes(self):
  # A catalog's id that is one is kept; a word, a path, nothing or too much gives way to the id the source makes, and only that names a file.
  self.fixtures();catalog=json.loads((self.root/'catalog.json').read_text());first=catalog['apps'][0];(self.root/'manifest.json').unlink()
  names=['oceanpop','laser_kombat','../../x','','x'*500,None,'Io.Github.Test.App6','io.github.test.app7.',7,'a'*80+'.'+'b'*79,'com.example.laser','io.github.test.squat']
  release=lambda source:[dict(first['releases'][0],url=source+'/releases/download/v2/download.zip')]
  apps=[]
  for i,name in enumerate(names):
   source='https://github.com/test/app%d'%i;app=dict(first,source=source,name='App %d'%i,installdir='PSP/GAME/App%d'%i,releases=release(source))
   if name is None:del app['id']
   else:app['id']=name
   apps.append(app)
  # The demo the stick keeps as ID stays ID whatever the list calls it; a second entry making an id already listed loses to the first; an id held on the stick by another source is not taken.
  evil='https://github.com/test/evil'
  catalog['apps']=[dict(first,id='com.example.demo'),*apps,dict(apps[1],id='laser',name='Again',installdir='PSP/GAME/Again'),dict(apps[0],id=ID,source=evil,installdir='PSP/GAME/Evil',releases=release(evil))]
  self.write('catalog.json',catalog)
  expected=[ID,*['io.github.test.app%d'%i for i in range(10)],'com.example.laser','io.github.test.app11','io.github.test.evil']
  r=self.run_client('fetch',VERBOSE=1);self.assertEqual([row.split()[0] for row in r.stdout.splitlines()],expected,r.stderr)
  self.assertIn('"oceanpop" is not an id this stick can use; listed as io.github.test.app0',r.stderr)
  self.assertIn('"%s..." is not an id'%('x'*40),r.stderr);self.assertNotIn('laser_kombat" is not',self.run_client('fetch').stderr)
  # The saved catalog is read by the same rule.
  self.assertEqual([row.split()[0] for row in self.run_client('fetch',CATALOG_DOWN=1,ok=False).stdout.splitlines()],expected)
  for app_id in expected[1:]:
   with self.subTest(app_id=app_id):self.assertEqual(self.run_client('get',app_id,VERBOSE=1).stdout.strip(),'0')
  self.assertEqual(sorted(p.name for p in (self.root/'ms0:/PSP/PSPDX/INSTALLED').iterdir()),sorted(f'{i}.{kind}' for i in expected for kind in ('pspdx','state.json')))
  self.assertEqual(sorted(p.name for p in (self.root/'ms0:/PSP/GAME').iterdir()),sorted(['Demo','Evil']+['App%d'%i for i in range(12)]))
  # Kept under the catalog's id, the app is still the stick's with no catalog to say so.
  row=next(l.split() for l in self.run_client('fetch',CATALOG_DOWN=1,FORCE=1,ok=False).stdout.splitlines() if l.startswith('com.example.laser '));self.assertEqual(row[3],'2',row)
  self.assertEqual(sorted(p.name for p in self.root.iterdir() if p.name not in ('ms0:','ef0:')),['catalog.json','gzip.log','new.zip','release.json','requests.log'])
  self.assertEqual([p for p in self.root.rglob('*') if any(w in str(p) for w in ('oceanpop','laser_kombat','x'*20,'Io.Github')) or p.name.split('.')[0] in ('laser','x')],[])
 def test_an_update_is_another_zip_not_a_later_date(self):
  self.fixtures();self.run_client('install',VERSION=100000)
  same=hashlib.sha256((self.root/'new.zip').read_bytes()).hexdigest();self.assertEqual(self.state()[ID]['installed']['sha256'],same)
  catalog=json.loads((self.root/'catalog.json').read_text());release=catalog['apps'][0]['releases'][0]
  for published,sha,state in (('2026-09-12T00:00:00Z',same,'2'),('1970-01-02','0'*63+'1','3'),('1970-01-02',same,'2')):
   with self.subTest(published=published,sha=sha):
    release.update(published_at=published,sha256=sha);self.write('catalog.json',catalog)
    row=next(line.split() for line in self.run_client('fetch').stdout.splitlines() if line.startswith(ID+' '));self.assertEqual(row[3],state,row)
 def from_the_entry(self):
  # The fixtures' one app as its catalog entry says it, and its repository's own .pspdx gone: GitHub answers 404.
  self.fixtures();catalog=json.loads((self.root/'catalog.json').read_text())
  catalog['apps'][0].update(summary='From the list',description='Two lines.\nFrom the list.')
  self.write('catalog.json',catalog);(self.root/'manifest.json').unlink();(self.root/'requests.log').write_text('')
  return catalog
 def saved(self,app_id=ID):return json.loads((self.root/f'ms0:/PSP/PSPDX/INSTALLED/{app_id}.pspdx').read_text())
 def test_a_github_app_whose_repository_has_no_pspdx_installs_and_updates_from_its_entry(self):
  catalog=self.from_the_entry();r=self.run_client('get',ID,VERBOSE=1);self.assertEqual(r.stdout.strip(),'0',r.stderr)
  self.assertEqual((self.root/'ms0:/PSP/GAME/Demo/EBOOT.PBP').read_bytes(),b'new package');self.assertIn('installed from the catalog entry',r.stderr)
  # The repository was asked once, and what is saved is the entry's word.
  self.assertEqual((self.root/'requests.log').read_text().splitlines().count('https://raw.githubusercontent.com/test/demo/HEAD/.pspdx'),1)
  self.assertEqual(self.saved(),dict(schema=SCHEMA,source=SPEC['source'],name='Demo',tags=['demo'],installdir='PSP/GAME/Demo',author='test',summary='From the list',description='Two lines.\nFrom the list.'))
  record=self.state()[ID];self.assertEqual((record['installed']['version'],record['latest']['checked_from']),('2','https://example.com/catalog.json'))
  # Another zip in the entry is an update. A live catalog answers without GitHub; a forced check asks the repository for its .pspdx, hears 404,
  # and the live entry stands. With only the saved catalog the release is asked of GitHub's API by the saved file, as for any app.
  catalog['apps'][0]['releases'][0]['sha256']='0'*63+'1';self.write('catalog.json',catalog);pspdx='https://raw.githubusercontent.com/test/demo/HEAD/.pspdx'
  api='https://api.github.com/repos/test/demo/releases/latest'
  github=lambda:[x for x in (self.root/'requests.log').read_text().splitlines() if 'github' in x]
  for env,asked in (({},[]),(dict(FORCE=1),[pspdx]),(dict(FORCE=1,CATALOG_DOWN=1),[pspdx,api]),(dict(CATALOG_DOWN=1),[])):
   with self.subTest(env=env):
    (self.root/'requests.log').write_text('');r=self.run_client('fetch',VERBOSE=1,**env)
    row=next(l.split() for l in r.stdout.splitlines() if l.startswith(ID+' '));self.assertEqual(row[3],'3',(row,r.stderr))
    # What GitHub answered counts as asked: the check after it, with only the saved catalog, waits the six hours.
    self.assertEqual(github(),asked,r.stderr)
  self.assertEqual(self.state()[ID]['latest']['checked_from'],SPEC['source'])
  # Once the repository has a .pspdx of its own, that is what the check hears, and the release is asked for.
  self.write('manifest.json',dict(SPEC,summary='From the repository'));(self.root/'requests.log').write_text('')
  r=self.run_client('fetch',FORCE=1,CATALOG_DOWN=1,VERBOSE=1);self.assertIn('origin: test/demo .pspdx: Demo',r.stderr);self.assertIn(pspdx,github())
  self.assertTrue(any('api.github.com' in x for x in github()),github())
 def test_the_repositorys_own_pspdx_wins(self):
  self.from_the_entry();self.write('manifest.json',SPEC)
  r=self.run_client('get',ID,VERBOSE=1);self.assertEqual(r.stdout.strip(),'0',r.stderr);self.assertEqual(self.saved(),SPEC);self.assertNotIn('installed from the catalog entry',r.stderr)
 def test_an_entry_that_still_names_listed_by_installs_as_any_other(self):
  # A catalog written before listed_by went: the field is read past, doubled or not, and not saved.
  catalog=self.from_the_entry();(self.root/'catalog.json').write_text(json.dumps(catalog).replace('"name": "Demo"','"name": "Demo", "listed_by": "https://lists.example.org/psp/", "listed_by": 7'))
  r=self.run_client('get',ID,VERBOSE=1);self.assertEqual(r.stdout.strip(),'0',r.stderr);self.assertNotIn('listed_by',self.saved())
 def test_an_entry_from_outside_github_installs_from_the_entry(self):
  self.fixtures();catalog=json.loads((self.root/'catalog.json').read_text());first=catalog['apps'][0]
  package=(self.root/'new.zip').read_bytes();blocks='de.wijsman.blocks'
  mirror=dict(id=blocks,name='Blocks',source='https://archive.org/details/psp-blocks',installdir='PSP/GAME/Blocks',
              releases=[dict(tag='v1',published_at='2026-01-02T00:00:00Z',size=len(package),sha256=hashlib.sha256(package).hexdigest(),url='https://archive.org/download/psp-blocks/download.zip')])
  self.write('catalog.json',dict(catalog,apps=[first,mirror]));(self.root/'requests.log').write_text('')
  r=self.run_client('get',blocks,VERBOSE=1);self.assertEqual(r.stdout.strip(),'0',r.stderr)
  self.assertEqual((self.root/'ms0:/PSP/GAME/Blocks/EBOOT.PBP').read_bytes(),b'new package')
  self.assertEqual((self.state()[blocks]['source'],self.saved(blocks)['source']),(mirror['source'],mirror['source']))
  self.assertEqual([x for x in (self.root/'requests.log').read_text().splitlines() if 'archive.org' in x],[mirror['releases'][0]['url']])
  # With no catalog at all it is still on the stick, from the file it was saved with, and nothing is asked about it.
  shutil.rmtree(self.root/'ms0:/PSP/PSPDX/CACHE');(self.root/'requests.log').write_text('')
  self.assertIn(blocks+' 1 0',self.run_client('fetch',CATALOG_DOWN=1,FORCE=1,ok=False).stdout)
  self.assertEqual([x for x in (self.root/'requests.log').read_text().splitlines() if 'archive.org' in x or 'blocks' in x],[])
  # A download from outside GitHub is held to the catalog's hash, and without one there is nothing to hold it to.
  del mirror['releases'][0]['sha256'];self.write('catalog.json',dict(catalog,apps=[first,mirror]))
  r=self.run_client('get',blocks,ok=False,VERBOSE=1);self.assertNotEqual(r.returncode,0);self.assertIn('SHA-256',r.stderr)
 def test_old_pspdx_lines_and_manifest_urls_are_passed_over(self):
  # A stick from when a list's .pspdx could stand in: the line is skipped and said, the record's field read past and left.
  self.fixtures();record=self.state()[ID];record['manifest_url']='https://lists.example.org/psp/demo.pspdx';self.write(f'ms0:/PSP/PSPDX/INSTALLED/{ID}.state.json',record)
  (self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('https://lists.example.org/psp/demo.pspdx\nhttps://example.com/catalog.json\n');(self.root/'requests.log').write_text('')
  r=self.run_client('fetch',VERBOSE=1);self.assertIn(ID+' 2 1',r.stdout);self.assertIn('no source any more; skipped',r.stderr)
  self.assertEqual(self.state()[ID]['manifest_url'],record['manifest_url'])
  # In a list the same.
  (self.root/'catalog.txt').write_text('https://lists.example.org/psp/demo.pspdx\n'+SPEC['source']+'\n');(self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('https://example.com/pspdx/\n')
  r=self.run_client('fetch',CATALOG_DOWN=1,VERBOSE=1);self.assertIn(ID+' 3 1',r.stdout);self.assertIn('a list no longer names; skipped',r.stderr)
  self.assertNotIn('demo.pspdx',(self.root/'requests.log').read_text())
  # And none can be added.
  self.assertNotEqual(self.run_client('add','https://lists.example.org/psp/demo.pspdx',ok=False).returncode,0)
 def wrap(self,text,width,max_bytes=255,most=100):
  (self.root/'text.txt').write_bytes(text.encode());r=self.run_client('wrap',width,max_bytes,most,'text.txt')
  return [json.loads(line) for line in r.stdout.splitlines()]
 def test_text_is_wrapped_once_by_width_newline_and_character(self):
  # The measure on the host is one unit a character, so a width is a count of characters.
  self.assertEqual(self.wrap('one two three four',9),['one two','three','four'])
  self.assertEqual(self.wrap('one two three',13),['one two three'])
  # A newline always ends a line, an empty line stays, and the spaces after a newline are the next line's.
  self.assertEqual(self.wrap('first\n\n  indented\nlast\n',20),['first','','  indented','last'])
  # The spaces a line broke at go with it, however many.
  self.assertEqual(self.wrap('aaa     bbb',5),['aaa','bbb'])
  # A word wider than the line is cut between characters, never inside one, and a line holds one at least.
  self.assertEqual(self.wrap('ééééééé ü',3),['ééé','ééé','é ü'])
  self.assertEqual(self.wrap('abc',0),['a','b','c'])
  self.assertEqual(self.wrap('日本語のテキスト',4),['日本語の','テキスト'])
  # Bytes are capped as well as width, and never mid-character either: four two-byte letters are eight bytes.
  self.assertEqual(self.wrap('éééé éé',100,max_bytes=8),['éééé','éé'])
  self.assertEqual(self.wrap('ééééé',100,max_bytes=5),['éé','éé','é'])
  # No more lines than there is room for, and nothing at all from nothing.
  self.assertEqual(self.wrap('a\nb\nc\nd',10,most=2),['a','b']);self.assertEqual(self.wrap('',10),[])
  # A whole description at its longest comes back whole: every character on some line, in order.
  text=('Ein Absatz über Käse, Brötchen und ß. '*40+'\n\n')*2+'x'*300
  lines=self.wrap(text,48,most=3000);self.assertTrue(all(len(l)<=48 for l in lines))
  self.assertEqual(''.join(lines).replace(' ',''),text.replace(' ','').replace('\n',''))
 def wrap_bytes(self,raw,width,max_bytes):
  (self.root/'text.txt').write_bytes(raw)
  e=dict(os.environ,ASAN_OPTIONS='detect_leaks=0')
  r=subprocess.run([BIN,'wrap',str(width),str(max_bytes),'4096','text.txt'],cwd=self.root,env=e,capture_output=True)
  self.assertNotIn(b'AddressSanitizer',r.stderr);self.assertNotIn(b'runtime error:',r.stderr);self.assertEqual(r.returncode,0)
  return [l[1:-1].replace(b'\\\\',b'\\').replace(b'\\"',b'"') for l in r.stdout.split(b'\n') if l]
 def test_text_that_is_not_utf8_still_breaks_within_the_byte_cap(self):
  # A run of continuation bytes is as many characters of one byte, not one character of all of them: no line past its cap.
  raw=b'a'+b'\x80'*600+b' \xe2\x80\x94\xc3\xa9 \xf0\x9f\x98\x80'+b'\xbf'*300+b'\xe2\x80'
  lines=self.wrap_bytes(raw,1000,255)
  self.assertTrue(all(0<len(l)<=255 for l in lines),[len(l) for l in lines])
  self.assertEqual(b''.join(lines).replace(b' ',b''),raw.replace(b' ',b''))
  # A whole letter is never cut, however small the cap: the four bytes of one stay one line.
  lines=self.wrap_bytes('—é😀'.encode(),1000,2)
  self.assertEqual(lines,['—'.encode(),'é'.encode(),'😀'.encode()])
 def test_one_tab_holds_everything_published(self):
  # No tab per category: whatever an entry is tagged and whatever category or type it names, it stands in Homebrew, the one tab for what is published, a plugin too; the tabs stay the stick, Homebrew, the empty UMD and the gear. Homebrew opens with the three category rows the console has words for, before the package, whatever the entry names.
  self.fixtures();catalog=json.loads((self.root/'catalog.json').read_text());app=catalog['apps'][0]
  for tags,kind,category in ((['Jeu','games','Game'],None,None),([],None,None),(None,None,None),(['game','demo','emulator'],None,None),(['plugin'],None,None),(['game'],'plugin',None),
                             (['puzzle'],None,'game'),(['game','demo'],None,'emulator'),(['game'],None,'Puzzle'),(['game'],None,'Game'),(['demo'],'plugin','app')):
   with self.subTest(tags=tags,kind=kind,category=category):
    a={k:v for k,v in app.items() if k!='tags'}
    if tags is not None:a['tags']=tags
    if category is not None:a['category']=category
    if kind:a['type']=kind;a.pop('installdir')
    self.write('catalog.json',dict(catalog,apps=[a]))
    # The store opens with its three ways to browse it by -- category, tag, source -- whatever the entry names, the package below them.
    self.assertEqual(self.run_client('view').stdout.splitlines()[:3],['tabs 4: -2 0 -4 -3','tab 0 kind 0 rows 5 first -4 plan 0 0','tab -4 kind 4 rows 0 first -1 plan 0 0'])
 def test_the_store_is_browsed_by_category_tag_and_source(self):
  # Three rows above the packages, one a way to browse by, with how many rows it lists: the categories, the tags at least two shown apps carry (rare, one, has no chip), the most carried first, spelled as first met, "unreleased" never, nor a contest's tag with its year in it (a model number after a dash is no year), and the sources by the name their catalog gives or their URL makes. Back out of a row lands on it, back out of a way on the way; a tag or a source narrows the list to its apps.
  self.fixtures();catalog=json.loads((self.root/'catalog.json').read_text());app=catalog['apps'][0]
  def entry(n,tags,category='game'):
   return dict(app,id=f'app_{n}',name=f'App {n}',tags=tags,category=category,source=f'https://github.com/test/app{n}',installdir=f'PSP/GAME/App{n}',
               releases=[dict(app['releases'][0],url=f'https://github.com/test/app{n}/releases/download/v2/download.zip')])
  first=[entry(1,['puzzle','Racing','NEO Spring Compo 2007','Homebrew Idol (2008)','PSP-2000+ only']),entry(2,['Puzzle','unreleased']),entry(3,['puzzle','racing','neo spring compo 2007','Homebrew Idol (2008)','Scenery Beta 2009/2010','PSP-2000+ only']),entry(4,['racing','unreleased']),entry(5,['puzzle','racing','unreleased']),entry(6,['unreleased'],'demo'),entry(7,['rare','rare2'])]
  self.write('catalog.json',dict(catalog,apps=[app]+first))
  self.write('second.json',dict(catalog,name='  PSPDX Community ',apps=[entry(8,['racing']),entry(1,['puzzle'])]))
  (self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('https://example.com/catalog.json\nhttps://example.com/second.json\n')
  (self.root/'map.txt').write_text('https://example.com/second.json %s\n'%(self.root/'second.json'))
  r=self.run_client('groups',URL_MAP=self.root/'map.txt')
  self.assertEqual(r.stdout.splitlines(),['Browse by Category 4','Browse by Tag 3','Browse by Source 2','then 1',
                                          '> Browse by Category','0 game 4','0 demo 0','0 app 0','0 emulator 0','apps 0 back 3','root 1',
                                          '> Browse by Tag','1 Racing 3','1 PSP-2000+ only 2','1 puzzle 2','apps 2 back 2','root 2',
                                          '> Browse by Source','2 example.com 4','2 PSPDX Community 1','apps 1 back 1','root 3'],r.stderr)
  # Shown, the unreleased count, puzzle has its row, and the switch itself is never a tag row.
  r=self.run_client('groups',URL_MAP=self.root/'map.txt',UNRELEASED=1)
  self.assertEqual(r.stdout.splitlines()[4:19],['> Browse by Category','0 game 7','0 demo 1','0 app 0','0 emulator 0','apps 0 back 3','root 1',
                                                '> Browse by Tag','1 Racing 5','1 puzzle 4','1 PSP-2000+ only 2','apps 2 back 2','root 2','> Browse by Source','2 example.com 8'],r.stderr)
  ids=lambda *a:[i.rsplit('.',1)[-1] for i in self.run_client('listing',*a,URL_MAP=self.root/'map.txt').stdout.split()]
  self.assertEqual(sorted(ids('Racing')),['app1','app3','app8'])
  self.assertEqual(ids('PSPDX Community'),['app8'])
 def test_an_installed_app_refreshed_from_its_repository_stays_in_its_source(self):
  # A catalog a day old is asked about at each installed app's repository, and the answer takes the entry's place: the row keeps the source that listed it, or Browse by Source would show the source without its installed apps.
  self.fixtures();catalog=json.loads((self.root/'catalog.json').read_text());self.write('manifest.json',SPEC)
  self.write('catalog.json',dict(catalog,generated_at='2020-01-01T00:00:00Z'))
  r=self.run_client('groups');self.assertIn('2 example.com 1',r.stdout.splitlines(),r.stdout+r.stderr)
 def test_a_release_the_catalog_dated_by_its_day_is_not_an_update_at_its_repository(self):
  # Installed from a catalog that gave the release its day alone; the catalog is a day old, so the repository is asked, and GitHub publishes the same release at noon of that day with no hash for its asset: the same version on the same day is no update. Another version, or a later day, is.
  self.fixtures();catalog=json.loads((self.root/'catalog.json').read_text());self.write('catalog.json',dict(catalog,generated_at='2020-01-01T00:00:00Z'))
  record=self.state()[ID];record['installed'].update(version='3',published_at=1789171200);record.pop('latest',None)
  def state(published,tag='v3'):
   self.write(f'ms0:/PSP/PSPDX/INSTALLED/{ID}.state.json',record)
   release=json.loads((self.root/'release.json').read_text());self.write('release.json',dict(release,tag_name=tag,published_at=published))
   return self.row(self.run_client('fetch',FORCE=1).stdout)
  same=state('2026-09-12T12:00:11Z');later=state('2026-09-13T00:00:01Z');other=state('2026-09-12T12:00:11Z','v4')
  self.assertEqual((same[1],later[1],other[1]),('3','3','4'),(same,later,other))
  self.assertEqual(len({tuple(later[2:]),tuple(other[2:])}),1,(later,other));self.assertNotEqual(same[2:],later[2:],(same,later))
 def test_a_clock_behind_the_catalog_does_not_hide_updates(self):
  # The console's clock says a time before the catalog was made (here the catalog is stamped a year ahead, which is the same thing): its age is unknown, so the installed app is asked about at its repository as under a catalog a day old, and the newer release there is an update.
  self.fixtures();catalog=json.loads((self.root/'catalog.json').read_text());self.write('manifest.json',SPEC)
  ahead=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime(time.time()+365*86400))
  fresh=self.row(self.run_client('fetch').stdout);(self.root/'requests.log').write_text('')
  self.write('catalog.json',dict(catalog,generated_at=ahead));behind=self.row(self.run_client('fetch').stdout)
  self.assertIn('api.github.com',(self.root/'requests.log').read_text());self.assertEqual((fresh[1],behind[1]),('2','3'),(fresh,behind))
 def test_the_tag_cloud_lays_out_and_walks(self):
  # Chips fill a line of 100 with 6 between them and each line is centred; left and right go along the order, onto the next line at the end; up and down go to the chip on the next line whose middle is nearest, and nowhere past the edges. A chip wider than the room stands alone.
  r=self.run_client('cloud',100,40,40,30,60,20,150,10)
  self.assertEqual(r.stdout.splitlines(),['lines 5','0 7  0 1 0 2','0 53  0 2 1 3','1 2  1 3 0 4','1 38  2 4 1 4','2 40  3 5 3 5','3 0  4 6 4 6','4 45  5 6 5 6'])
 def test_a_source_is_named_by_its_url(self):
  urls=['https://pspdev.github.io/homebrew/','https://chriopter.github.io/pspdx-catalog/','https://chriopter.github.io/pspdx-catalog/catalog.json',
        'https://someone.github.io/','https://github.com/owner/repo@v1.2','https://raw.githubusercontent.com/owner/lists/main/psp.txt',
        'https://www.example.com/catalog.json','https://user@files.example.net:8443/psp/apps/list.txt','nonsense']
  self.assertEqual(self.run_client('sourcename',*urls).stdout.splitlines(),['pspdev','chriopter','chriopter','someone','owner / repo','owner / lists','example.com','files.example.net / psp','nonsense'])
 def gzip_catalog(self,data=None,cut=0,flip=False):
  raw=(self.root/'catalog.json').read_bytes() if data is None else json.dumps(data).encode()
  packed=bytearray(gzip.compress(raw))
  if flip:packed[len(packed)//2]^=0xff
  (self.root/'catalog.json.gz').write_bytes(bytes(packed[:len(packed)-cut]))
 def saved_catalog(self):return next((self.root/'ms0:/PSP/PSPDX/CACHE/catalogs').glob('*.json'))
 def test_catalog_arrives_gzipped_and_reads_as_plain(self):
  # Pages sends the catalog gzipped to a client that asks, and only the catalog asks: the fake ends the run on a ZIP asked for gzipped.
  self.fixtures();plain=self.run_client('fetch').stdout;self.saved_catalog().unlink()
  self.gzip_catalog();r=self.run_client('fetch',VERBOSE=1)
  self.assertEqual(r.stdout,plain);self.assertIn('bytes gzipped',r.stderr)
  self.assertEqual(self.saved_catalog().read_bytes(),(self.root/'catalog.json').read_bytes())
  self.assertEqual(set((self.root/'gzip.log').read_text().splitlines()),{'https://example.com/catalog.json'})
  self.write('ms0:/PSP/PSPDX/INBOX/one.pspdx',SPEC);self.run_client('inboxinstall');self.assertIn(ID,self.state())
  self.assertIn('download.zip',(self.root/'requests.log').read_text())
  self.assertEqual(set((self.root/'gzip.log').read_text().splitlines()),{'https://example.com/catalog.json'})
 def test_gzip_that_does_not_inflate_is_a_failed_fetch(self):
  self.fixtures();self.run_client('fetch');saved=self.saved_catalog().read_bytes()
  for why,env,kw in [('corrupt gzip',{},dict(flip=True)),('gzip cut short',{},dict(cut=5)),('no header announced',dict(GZIP_UNNAMED=1),{})]:
   self.gzip_catalog(**kw);r=self.run_client('fetch',VERBOSE=1,**env)
   self.assertIn(why,r.stderr);self.assertIn(ID,r.stdout);self.assertEqual(self.saved_catalog().read_bytes(),saved)
  catalog=json.loads((self.root/'catalog.json').read_text());self.gzip_catalog(dict(catalog,pad='x'*4200000))
  r=self.run_client('fetch',VERBOSE=1);self.assertIn('larger than',r.stderr);self.assertNotIn('unreachable',r.stderr);self.assertEqual(self.saved_catalog().read_bytes(),saved)
  # The room is counted in inflated text, to the byte: RESPONSE_MAX fits, one more does not.
  room=4*1024*1024;base=len(json.dumps(dict(catalog,pad='')))
  for extra,fits in [(0,True),(1,False)]:
   self.gzip_catalog(dict(catalog,pad='x'*(room-base+extra)));r=self.run_client('fetch',VERBOSE=1)
   self.assertEqual('larger than' not in r.stderr,fits,r.stderr[-300:]);self.assertEqual('bytes gzipped' in r.stderr,fits)
 # --- what a catalog entry is held to (QA review B) ---
 def catalog_app(self):return json.loads((self.root/'catalog.json').read_text())
 def row(self,stdout,app_id=ID):return next((l.split(' ') for l in stdout.splitlines() if l.startswith(app_id+' ')),None)
 def test_gzip_waits_for_its_first_byte(self):
  # A piece of nothing before the first byte decides nothing: the gzip after it still inflates.
  self.fixtures();plain=self.run_client('fetch').stdout;self.saved_catalog().unlink()
  self.gzip_catalog();r=self.run_client('fetch',VERBOSE=1,EMPTY_FIRST_PIECE=1)
  self.assertEqual(r.stdout,plain,r.stderr);self.assertIn('bytes gzipped',r.stderr)
  (self.root/'catalog.json.gz').unlink();self.assertEqual(self.run_client('fetch',EMPTY_FIRST_PIECE=1).stdout,plain)
 def test_a_release_the_catalog_gets_wrong_leaves_the_app_to_its_repository(self):
  # A hash that is there and is no string, a day no calendar has, releases that are no list: the entry goes, and the repository answers (3) instead of the catalog (2).
  self.fixtures();catalog=self.catalog_app();release=catalog['apps'][0]['releases'][0]
  for change,version in [(dict(sha256=None),'3'),(dict(sha256=12345),'3'),(dict(published_at='2023-02-31'),'3'),(dict(published_at='2023-02-29T12:00:00Z'),'3'),
                         (dict(published_at='2023-04-31'),'3'),(dict(published_at='2100-02-29'),'3'),(dict(published_at='2024-02-29'),'2'),(dict(published_at='2000-02-29T23:59:59Z'),'2'),(dict(tag='v'),'v')]:
   with self.subTest(change=change):
    self.write('catalog.json',dict(catalog,apps=[dict(catalog['apps'][0],releases=[dict(release,**change)])]))
    self.assertEqual(self.row(self.run_client('fetch').stdout)[1],version)
  self.write('catalog.json',dict(catalog,apps=[dict(catalog['apps'][0],releases={'latest':release})]));self.assertEqual(self.row(self.run_client('fetch').stdout)[1],'3')
  # A list stamped on a day that does not exist is no catalog.
  self.write('catalog.json',dict(catalog,generated_at='2023-02-30T00:00:00Z'));self.assertEqual(self.row(self.run_client('fetch').stdout)[1],'3')
  # A tag that is only a "v" is its own version at the origin too.
  self.write('catalog.json',catalog);release_json=json.loads((self.root/'release.json').read_text());self.write('release.json',dict(release_json,tag_name='v'))
  self.assertEqual(self.row(self.run_client('fetch',FORCE=1).stdout)[1],'v')
 def mirror(self,name='Blocks',**change):
  package=(self.root/'new.zip').read_bytes();plain=''.join(c for c in name if c.isalnum())
  app=dict(name=name,source='https://archive.org/details/psp-'+plain.lower(),installdir='PSP/GAME/'+plain[:32],
           releases=[dict(tag='v1',published_at='2026-01-02T00:00:00Z',size=len(package),sha256=hashlib.sha256(package).hexdigest(),url='https://archive.org/download/psp-blocks/download.zip')])
  app.update(change);return app
 def test_a_release_address_of_512_characters_fits(self):
  self.fixtures();catalog=self.catalog_app();first=catalog['apps'][0]
  for length,listed in [(512,True),(513,False)]:
   with self.subTest(length=length):
    url='https://archive.org/download/'+'u'*(length-len('https://archive.org/download/')-len('/download.zip'))+'/download.zip';self.assertEqual(len(url),length)
    mirror=self.mirror();mirror['releases'][0]['url']=url;self.write('catalog.json',dict(catalog,apps=[first,mirror]))
    self.assertEqual(self.row(self.run_client('fetch').stdout,'org.archive.blocks') is not None,listed)
  mirror['releases'][0]['url']=url[:len('https://archive.org/download/')]+url[len('https://archive.org/download/')+1:];self.assertEqual(len(mirror['releases'][0]['url']),512)
  self.write('catalog.json',dict(catalog,apps=[first,mirror]));r=self.run_client('get','org.archive.blocks',VERBOSE=1);self.assertEqual(r.stdout.strip(),'0',r.stderr)
 def test_an_entry_is_held_to_the_pspdx_rules(self):
  # Outside GitHub and on it, an entry that could never make a v1 .pspdx is not listed as an app that installs, and the log says which field.
  # A source under github.io would make an id that is a repository's, and is no app either.
  self.fixtures();catalog=self.catalog_app();first=catalog['apps'][0]
  bad=[self.mirror('Summary',summary='s'*61),self.mirror('N'*41),self.mirror('Tagged',tags=['t'*25]),self.mirror('Twice',tags=['game','game']),self.mirror('Tab',author='tab\there'),
       self.mirror('Empty',category=''),self.mirror('Wide',category='c'*25),self.mirror('Long',description='d'*2501),self.mirror('Null',license=None),self.mirror('Many',tags=['t%d'%i for i in range(9)]),self.mirror('Pages',source='https://owner.github.io/pages/')]
  github=dict(first,id='io.github.test.other',source='https://github.com/test/other',installdir='PSP/GAME/Other',summary='s'*61,
              releases=[dict(first['releases'][0],url='https://github.com/test/other/releases/download/v2/download.zip')])
  good=self.mirror('Fine',summary='s'*60,description='d'*2500,tags=['t%d'%i for i in range(8)],category='game')
  self.write('catalog.json',dict(catalog,apps=[first,*bad,github,good]))
  r=self.run_client('fetch',VERBOSE=1);ids=[l.split()[0] for l in r.stdout.splitlines()]
  # Text longer than the file allows is shortened and a tag that breaks the rules left out: those apps stay.
  self.assertEqual(ids,[ID,'org.archive.summary','org.archive.'+'n'*39,'org.archive.tagged','org.archive.twice','org.archive.many','io.github.test.other','org.archive.fine'],r.stderr)
  # What is not a matter of length still is not an app.
  for field in ('author','category','description','license'):self.assertIn('breaks the .pspdx rules (%s); dropped'%field,r.stderr)
  for field in ('summary','name','tags'):self.assertNotIn('breaks the .pspdx rules (%s)'%field,r.stderr)
  self.assertIn('which only a GitHub repository has; refused',r.stderr);self.assertIn('Pages is from outside GitHub and its source\'s host and name make no id; dropped',r.stderr)
  self.assertEqual(self.run_client('get','org.archive.fine',VERBOSE=1).stdout.strip(),'0')
 def test_a_catalog_names_an_app_without_a_repository(self):
  # An entry with no source is the catalog's to name: source <catalog>#<its id>, id catalog.<host backwards>.<the id made safe>, installed from the entry and held to its hash.
  self.fixtures();catalog=self.catalog_app();first=catalog['apps'][0]
  package=(self.root/'new.zip').read_bytes()
  def listed(given,name,**change):
   app=dict(id=given,name=name,category='application',tags=['NEO Spring Coding Compo 2009','game'],summary='s'*80,
            releases=[dict(tag='1.0',published_at='2009-05-01',size=len(package),sha256=hashlib.sha256(package).hexdigest(),url='https://archive.org/download/x/download.zip')])
   app.update(change);return app
  under,dash,long_=listed('psp_doom','PSP Doom'),listed('psp-doom','PSP Doom Two'),listed('_'*60,'Long One')
  none=listed(None,'No Id');del none['id']
  self.write('catalog.json',dict(catalog,apps=[first,under,dash,long_,none]))
  r=self.run_client('fetch',VERBOSE=1);ids=[l.split()[0] for l in r.stdout.splitlines()]
  self.assertEqual(ids[:3],[ID,'catalog.com.example.pspz5fdoom','catalog.com.example.pspz2ddoom'],r.stderr)
  self.assertEqual(len(ids),4,r.stderr);self.assertRegex(ids[3],r'^catalog\.com\.example\.(z5f){13}z\.[0-9a-f]{12}$')
  self.assertNotIn('is not an id this stick can use',r.stderr)
  # Installed, it is the same app on the next start: the .pspdx kept on the stick names the catalog and makes the same id.
  app='catalog.com.example.pspz5fdoom'
  self.assertEqual(self.run_client('get',app,VERBOSE=1).stdout.strip(),'0')
  saved=json.loads((self.root/f'ms0:/PSP/PSPDX/INSTALLED/{app}.pspdx').read_text())
  self.assertEqual(saved['source'],'https://example.com/catalog.json#psp_doom');self.assertEqual(saved['tags'],['game'])
  self.assertEqual(len(saved['summary']),60);self.assertTrue(saved['summary'].endswith('\u2026'))
  self.assertEqual(self.row(self.run_client('fetch').stdout,app)[3],'2')
  # A package that does not hash to what the entry says is not installed.
  self.zip('new.zip',{'EBOOT.PBP':b'other','data.txt':b'other'})
  self.assertNotEqual(self.run_client('get','catalog.com.example.pspz2ddoom',ok=False).stdout.strip(),'0')
 def test_a_failed_install_says_why(self):
  # The status line names the cause instead of a number: a server that answers 500, and a zip with two EBOOT.PBP.
  self.fixtures();catalog=self.catalog_app()
  def listed():
   package=(self.root/'new.zip').read_bytes()
   return dict(catalog,apps=[dict(id='two',name='Two',category='game',summary='s',releases=[dict(tag='1',published_at='2020-01-01',size=len(package),sha256=hashlib.sha256(package).hexdigest(),url='https://archive.org/download/two/download.zip')])])
  self.write('catalog.json',listed());(self.root/'map.txt').write_text('archive.org/download/two - 500\n')
  r=self.run_client('get','catalog.com.example.two',ok=False,URL_MAP=self.root/'map.txt')
  self.assertIn('why: the server answered 500',r.stderr)
  self.zip('new.zip',{'a/EBOOT.PBP':b'a','b/EBOOT.PBP':b'b'});self.write('catalog.json',listed())
  r=self.run_client('get','catalog.com.example.two',ok=False)
  self.assertIn('why: its zip holds several EBOOT.PBP side by side',r.stderr)
 def test_the_topmost_eboot_is_the_package(self):
  # Built for today's firmware at the top, the 1.50 kernel's launchers under it: the top folder installs whole, what is beside it stays out, and a Mac's __MACOSX shadow is never read or written.
  self.fixtures();catalog=self.catalog_app()
  self.zip('new.zip',{'Game/EBOOT.PBP':b'new','Game/data.bin':b'd','Game/Game%/EBOOT.PBP':b'k1','Game/Game/EBOOT.PBP':b'k2','Other/EBOOT.PBP/x':b'','Extra/150/EBOOT.PBP':b'o','__MACOSX/Game/._EBOOT.PBP':b'm','__MACOSX/EBOOT.PBP':b'm'})
  package=(self.root/'new.zip').read_bytes()
  self.write('catalog.json',dict(catalog,apps=[dict(id='deep',name='Deep',category='game',summary='s',releases=[dict(tag='1',published_at='2020-01-01',size=len(package),sha256=hashlib.sha256(package).hexdigest(),url='https://archive.org/download/deep/download.zip')])]))
  r=self.run_client('get','catalog.com.example.deep',VERBOSE=1)
  self.assertEqual(r.stdout.strip(),'0',r.stderr[-500:]);self.assertIn('zip: 4 EBOOT.PBP; Game/ goes to PSP/GAME/Deep, 2 under it, 1 left out',r.stderr)
  game=self.root/'ms0:/PSP/GAME/Deep'
  self.assertEqual((game/'EBOOT.PBP').read_bytes(),b'new');self.assertTrue((game/'Game%/EBOOT.PBP').exists());self.assertTrue((game/'data.bin').exists())
  self.assertFalse((self.root/'ms0:/PSP/GAME/Extra').exists());self.assertFalse(list(self.root.rglob('__MACOSX')))
  # With the package at the very top of the zip, the shadow is beside it and still left out.
  self.zip('new.zip',{'EBOOT.PBP':b'top','old%/EBOOT.PBP':b'k','__MACOSX/._EBOOT.PBP':b'm'});package=(self.root/'new.zip').read_bytes()
  self.write('catalog.json',dict(catalog,apps=[dict(id='flat',name='Flat',category='game',summary='s',releases=[dict(tag='1',published_at='2020-01-01',size=len(package),sha256=hashlib.sha256(package).hexdigest(),url='https://archive.org/download/flat/download.zip')])]))
  r=self.run_client('get','catalog.com.example.flat',VERBOSE=1)
  self.assertEqual(r.stdout.strip(),'0',r.stderr[-500:]);self.assertIn('the top goes to PSP/GAME/Flat, 1 under it, 0 left out',r.stderr)
  self.assertFalse(list(self.root.rglob('__MACOSX')))
 def test_names_in_a_code_page_are_read_as_one(self):
  # A zip made on a Japanese system names files in Shift-JIS, one made on an old Western system in CP437, neither flagged UTF-8: both go on the stick under their UTF-8 names, and the install says so.
  self.fixtures();catalog=self.catalog_app()
  sjis='\u30b7\u30b9\u30c6\u30e0'.encode('shift_jis')
  z=self.root/'new.zip';z.write_bytes(raw_zip({b'EBOOT.PBP':b'a',b'RTP/'+sjis+b'.bmp':b'b',b'img/fontgro\xe1.png':b'c',b'img/Men\x81.png':b'd',b'Am\x82liorations.txt':b'e'}))
  package=z.read_bytes()
  self.write('catalog.json',dict(catalog,apps=[dict(id='cp',name='Code Page',category='game',summary='s',releases=[dict(tag='1',published_at='2020-01-01',size=len(package),sha256=hashlib.sha256(package).hexdigest(),url='https://archive.org/download/cp/download.zip')])]))
  r=self.run_client('get','catalog.com.example.cp',VERBOSE=1)
  self.assertEqual(r.stdout.strip(),'0',r.stderr[-600:]);self.assertIn('zip: 4 names in a code page, 1 of them Shift-JIS',r.stderr)
  game=self.root/'ms0:/PSP/GAME/CodePage'
  self.assertTrue((game/'RTP/\u30b7\u30b9\u30c6\u30e0.bmp').exists(),sorted(p.name for p in game.rglob('*')))
  self.assertTrue((game/'img/fontgro\u00df.png').exists());self.assertTrue((game/'img/Men\u00fc.png').exists())
  # é and a letter read as Shift-JIS too, as a full-width letter; that is not Japanese, and CP437 is taken.
  self.assertTrue((game/'Am\u00e9liorations.txt').exists())
 def test_an_archive_org_mirror_that_answers_500_is_gone_around(self):
  # archive.org redirects to a mirror that answers 500; the item's metadata names its storage servers, and the zip comes from the first that has it. A server outside archive.org is never asked.
  self.fixtures();catalog=self.catalog_app();package=(self.root/'new.zip').read_bytes()
  self.write('catalog.json',dict(catalog,apps=[dict(id='fb',name='Fall Back',category='game',summary='s',releases=[dict(tag='1',published_at='2020-01-01',size=len(package),sha256=hashlib.sha256(package).hexdigest(),url='https://archive.org/download/fb.-7z/Fall%20Back.zip')])]))
  self.write('meta.json',dict(d1='ia601.us.archive.org',d2='ia801.us.archive.org',dir='/9/items/fb.-7z',files=[dict(name='Fall Back.zip')]))
  (self.root/'map.txt').write_text('archive.org/download/fb.-7z - 500\narchive.org/metadata/fb.-7z meta.json 200\nia601.us.archive.org - 500\nia801.us.archive.org/9/items/fb.-7z/Fall%20Back.zip new.zip 200\n')
  r=self.run_client('get','catalog.com.example.fb',VERBOSE=1,URL_MAP=self.root/'map.txt')
  self.assertEqual(r.stdout.strip(),'0',r.stderr[-800:]);self.assertIn('metadata named 2 storage servers',r.stderr)
  self.assertIn('https://ia801.us.archive.org/9/items/fb.-7z/Fall%20Back.zip',(self.root/'requests.log').read_text())
  self.write('meta.json',dict(d1='evil.example.com',dir='/9/items/fb.-7z'))
  for f in (self.root/'ms0:/PSP/PSPDX/INSTALLED').glob('catalog.com.example.fb*'):f.unlink()
  shutil.rmtree(self.root/'ms0:/PSP/GAME/FallBack',ignore_errors=True)
  r=self.run_client('get','catalog.com.example.fb',VERBOSE=1,ok=False,URL_MAP=self.root/'map.txt')
  self.assertIn('metadata named 0 storage servers',r.stderr);self.assertIn('why: the server answered 500',r.stderr)
  self.assertNotIn('evil.example.com',(self.root/'requests.log').read_text())
 def test_pictures_are_kept_in_one_pack(self):
  # Stills go into CACHE/media.pak with an index beside it, not a file each; they come back from the pack without the network, a record the pack does not hold (a cut write) is ignored. An icon's PNG is not kept at all: the list keeps icons shrunk, as thumbnails.
  (self.root/'a.png').write_bytes(b'SHOT-A'+b'x'*100);(self.root/'b.png').write_bytes(b'SHOT-B'+b'y'*200);(self.root/'i.png').write_bytes(b'ICON-I'+b'z'*50)
  (self.root/'map.txt').write_text('example.com/a.png a.png 200\nexample.com/b.png b.png 200\nexample.com/i.png i.png 200\n')
  r=self.run_client('asset','shot','app.a','https://example.com/a.png',URL_MAP=self.root/'map.txt',VERBOSE=1)
  self.assertEqual(r.stdout.split()[0],'106');self.assertIn('fetched',r.stderr)
  self.run_client('asset','shot','app.b','https://example.com/b.png',URL_MAP=self.root/'map.txt')
  self.assertEqual(self.run_client('asset','icon','app.i','https://example.com/i.png',URL_MAP=self.root/'map.txt').stdout.split()[0],'56')
  cache=self.root/'ms0:/PSP/PSPDX/CACHE'
  self.assertEqual((cache/'media.pak').stat().st_size,106+206);self.assertEqual((cache/'media.idx').stat().st_size,32)
  self.assertFalse(list((cache/'media').glob('*')) if (cache/'media').exists() else [])
  # A new run, the network gone: both stills from the pack, the icon nowhere.
  self.assertEqual(self.run_client('asset','shot','app.b','https://example.com/b.png',OFFLINE=1).stdout.split(),['206','SHOT-Byyyyyyyyyy'])
  self.assertEqual(self.run_client('asset','icon','app.i','https://example.com/i.png',OFFLINE=1,ok=False).returncode,1)
  # A cut write: the index names bytes the pack never got. That record is dropped, the one before it stands.
  with open(cache/'media.pak','r+b') as f:f.truncate(150)
  self.assertEqual(self.run_client('asset','shot','app.a','https://example.com/a.png',OFFLINE=1).stdout.split()[0],'106')
  self.assertEqual(self.run_client('asset','shot','app.b','https://example.com/b.png',OFFLINE=1,ok=False).returncode,1)
 def test_a_catalog_of_thousands_lists_every_app_newest_first(self):
  # Past two thousand entries, every one is a row, held in memory, and the store lists the newest release first; one date shared keeps the catalog's order, and no date at all stands last.
  self.fixtures();catalog=self.catalog_app();package=(self.root/'new.zip').read_bytes();rng=random.Random(7)
  def app(n,day):
   return dict(id=f'app_{n}',name=f'App {n}',category='game',tags=['game'],summary='s'*40,
               releases=[dict(tag='1.0',published_at=day,size=len(package),sha256=hashlib.sha256(package).hexdigest(),url=f'https://archive.org/download/a{n}/download.zip')])
  days=[f'{2005+rng.randrange(20)}-{1+rng.randrange(12):02d}-{1+rng.randrange(28):02d}' for _ in range(3000)]
  apps=[app(n,d) for n,d in enumerate(days)]
  self.write('catalog.json',dict(catalog,apps=apps))
  r=self.run_client('fetch',VERBOSE=1);self.assertEqual(len(r.stdout.splitlines()),3001,r.stderr[-400:])
  listed=[i for i in self.run_client('listing').stdout.split() if i!=ID]
  want=[f'catalog.com.example.appz5f{n}' for n in sorted(range(3000),key=lambda n:(-int(days[n].replace('-','')),n))]
  self.assertEqual(listed,want)
  # The same list comes back from the saved copy, with the network gone.
  self.assertEqual([i for i in self.run_client('listing',OFFLINE=1).stdout.split() if i!=ID],want)
 def test_a_catalog_past_what_is_held_keeps_the_first(self):
  # More entries than there are places: MAX_APPS rows, the installed one among them, are listed and the fetch still counts as done.
  self.fixtures();catalog=self.catalog_app();package=(self.root/'new.zip').read_bytes()
  apps=[dict(id=f'x{n}',name=f'X {n}',category='game',summary='s',releases=[dict(tag='1',published_at='2020-01-01',size=len(package),sha256=hashlib.sha256(package).hexdigest(),url=f'https://archive.org/download/x{n}/download.zip')]) for n in range(4200)]
  self.write('catalog.json',dict(catalog,apps=apps));r=self.run_client('fetch',VERBOSE=1)
  self.assertEqual(len(r.stdout.splitlines()),4096,r.stderr[-400:])
 def test_an_id_as_long_as_github_makes_one(self):
  # GitHub allows an owner of 39 characters and a repository of 100: the id is 150, and names the files on the stick whole.
  owner,repo='o'*39,'r'*100;source='https://github.com/%s/%s'%(owner,repo);long_id='io.github.%s.%s'%(owner,repo);self.assertEqual(len(long_id),150)
  self.assertEqual(self.run_client('ids',source).stdout.split(),[long_id,source]);self.assertEqual(self.run_client('ids',source+'r').stdout.split(),['-','-'])
  self.fixtures();catalog=self.catalog_app();first=catalog['apps'][0]
  app=dict(first,id=long_id,source=source,installdir='PSP/GAME/Long',releases=[dict(first['releases'][0],url=source+'/releases/download/v2/download.zip')])
  self.write('catalog.json',dict(catalog,apps=[first,app]));self.write('manifest.json',dict(SPEC,source=source,installdir='PSP/GAME/Long'))
  self.assertIsNotNone(self.row(self.run_client('fetch').stdout,long_id))
  r=self.run_client('get',long_id,VERBOSE=1);self.assertEqual(r.stdout.strip(),'0',r.stderr)
  self.assertTrue((self.root/f'ms0:/PSP/PSPDX/INSTALLED/{long_id}.state.json').exists());self.assertTrue((self.root/f'ms0:/PSP/PSPDX/INSTALLED/{long_id}.pspdx').exists())
  self.assertEqual(self.row(self.run_client('fetch').stdout,long_id)[3],'2')
  # Asked at the repository, with no catalog, it is the same app.
  (self.root/'ms0:/PSP/PSPDX/sources.txt').write_text(source+'\n');self.write('release.json',dict(tag_name='v3',published_at='2026-09-12T00:00:00Z',assets=[dict(name='download.zip',size=(self.root/'new.zip').stat().st_size,browser_download_url=source+'/releases/download/v3/download.zip')]))
  self.assertEqual(self.row(self.run_client('fetch',FORCE=1).stdout,long_id)[1],'3')
 def test_a_catalog_that_is_not_taken_does_not_date_the_list(self):
  # A first source that is no catalog, stamped years ago, does not make the one after it look stale and send the apps to their repositories.
  self.fixtures();(self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('https://example.com/second/catalog.json\nhttps://example.com/catalog.json\n')
  self.write('second.json',dict(schema='https://chriopter.github.io/pspdx/schema/catalog-v1.json',generated_at='2020-01-01T00:00:00Z',apps=[dict(name='Nothing')]))
  (self.root/'requests.log').write_text('');r=self.run_client('fetch',SECOND_CATALOG='second.json')
  self.assertEqual(self.row(r.stdout)[1],'2');self.assertNotIn('raw.githubusercontent.com',(self.root/'requests.log').read_text())
 def test_media_addresses_resolve_the_way_a_browser_reads_them(self):
  self.fixtures();catalog=self.catalog_app();app=catalog['apps'][0]
  for media,expected in [(dict(icon='/root/icon.png',screenshots=['//cdn.example.org/shot.png'],video='HTTPS://Host.example/film.pmf',sound='media/SND0.AT3'),
                          ['https://example.com/root/icon.png','https://cdn.example.org/shot.png','HTTPS://Host.example/film.pmf','https://example.com/media/SND0.AT3']),
                         (dict(icon='a'*235,screenshots={'first':'shot.png'},sound='a'*236),['https://example.com/'+'a'*235,'','',''])]:
   with self.subTest(media=media):
    self.write('catalog.json',dict(catalog,apps=[dict(app,media=media)]));self.assertEqual(self.run_client('media',ID).stdout.split('\n')[:4],expected)
 def test_an_entry_that_names_a_field_twice_is_not_taken(self):
  # cJSON reads the first of two, most readers the last: the entry says two things and is left to the repository. A field no one reads may be doubled.
  self.fixtures();text=(self.root/'catalog.json').read_text();url='"url": "https://github.com/test/demo/releases/download/v2/download.zip"'
  for doubled,version in [(url+', "url": "https://github.com/test/demo/releases/download/v9/other.zip"','3'),(url+', "note": 1, "note": 2','2')]:
   with self.subTest(doubled=doubled):(self.root/'catalog.json').write_text(text.replace(url,doubled));self.assertEqual(self.row(self.run_client('fetch').stdout)[1],version)
  (self.root/'catalog.json').write_text(text.replace('"name": "Demo"','"name": "Demo", "source": "https://github.com/test/evil"'));self.assertEqual(self.row(self.run_client('fetch').stdout)[1],'3')
 def plant_record(self,app_id,source,installdir,pspdx=None):
  self.write(f'ms0:/PSP/PSPDX/INSTALLED/{app_id}.state.json',dict(source=source,installed=dict(installdir=installdir,version='1',published_at=7)))
  if pspdx:self.write(f'ms0:/PSP/PSPDX/INSTALLED/{app_id}.pspdx',pspdx)
 def test_an_installed_app_is_one_row(self):
  # Installed from a mirror the list has since moved away from: the list's row stands, and the stick's record adds no second one under the same id.
  (self.root/'ms0:/PSP/PSPDX').mkdir(parents=True);(self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('https://example.com/catalog.json\n')
  self.write('catalog.json',dict(schema='https://chriopter.github.io/pspdx/schema/catalog-v1.json',generated_at=NOW(),apps=[self.mirror(source='https://archive.org/details/psp-blocks-v2')]))
  self.plant_record('org.archive.blocks','https://archive.org/details/psp-blocks','PSP/GAME/Blocks',dict(schema=SCHEMA,source='https://archive.org/details/psp-blocks',name='Blocks',installdir='PSP/GAME/Blocks'))
  r=self.run_client('fetch',VERBOSE=1);self.assertEqual([l.split()[0] for l in r.stdout.splitlines()],['org.archive.blocks'],r.stderr);self.assertIn('not listed twice',r.stderr)
 def other_thing(self):
  # A catalog entry of another app that wants PSP/GAME/Demo.
  sha='ab'*32;return dict(schema='https://chriopter.github.io/pspdx/schema/catalog-v1.json',generated_at=NOW(),apps=[dict(id='io.github.other.thing',name='Thing',source='https://github.com/other/thing',installdir='PSP/GAME/Demo',
       releases=[dict(tag='v1',published_at='2026-01-02T00:00:00Z',size=1000,sha256=sha,url='https://github.com/other/thing/releases/download/v1/x.zip')])])
 def test_an_app_asked_at_its_repository_leaves_a_taken_folder_alone(self):
  (self.root/'ms0:/PSP/PSPDX').mkdir(parents=True);(self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('https://example.com/catalog.json\nhttps://github.com/test/demo\n')
  self.write('catalog.json',self.other_thing());self.write('release.json',dict(tag_name='v3',published_at='2026-09-12T00:00:00Z',assets=[dict(name='download.zip',size=10,browser_download_url='https://github.com/test/demo/releases/download/v3/download.zip')]))
  r=self.run_client('fetch',VERBOSE=1);self.assertEqual([l.split()[0] for l in r.stdout.splitlines()],['io.github.other.thing'],r.stderr)
  self.assertIn('origin: io.github.test.demo wants PSP/GAME/Demo, which io.github.other.thing has; not listed',r.stderr)
  self.assertIn("status: Demo not listed: PSP/GAME/Demo is another app's.",r.stderr)
 def test_an_installed_app_keeps_its_folder(self):
  # Installed, and a catalog lists another app under its folder before it: the installed app stays listed, from its saved file or its repository alike, and the other entry is the one said on the status line.
  (self.root/'ms0:/PSP/PSPDX').mkdir(parents=True);(self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('https://example.com/catalog.json\n')
  self.write('catalog.json',self.other_thing());(self.root/'manifest.json').unlink();self.plant_record(ID,SPEC['source'],'PSP/GAME/Demo',SPEC)
  for repository in (False,True):
   with self.subTest(repository=repository):
    if repository:self.write('manifest.json',SPEC);self.write('release.json',dict(tag_name='v3',published_at='2026-09-12T00:00:00Z',assets=[dict(name='download.zip',size=10,browser_download_url='https://github.com/test/demo/releases/download/v3/download.zip')]))
    r=self.run_client('fetch',VERBOSE=1,FORCE=1);self.assertEqual([l.split()[0] for l in r.stdout.splitlines()],[ID],r.stderr)
    self.assertIn('catalog: io.github.other.thing wants PSP/GAME/Demo, which %s has; not listed'%ID,r.stderr);self.assertIn("status: Thing not listed: PSP/GAME/Demo is another app's.",r.stderr)
 def test_an_installed_app_listed_after_another_in_its_folder_is_still_usable(self):
  # The catalog names the other app first and the installed demo second: the demo is listed, updates from its own source and is removed; the other entry is not listed.
  self.fixtures();catalog=self.catalog_app();thing=self.other_thing()['apps'][0]
  self.write('catalog.json',dict(catalog,apps=[thing,catalog['apps'][0]]))
  r=self.run_client('fetch',VERBOSE=1);self.assertEqual([l.split()[0] for l in r.stdout.splitlines()],[ID],r.stderr);self.assertEqual(self.row(r.stdout)[1],'2')
  self.assertIn('catalog: io.github.other.thing wants PSP/GAME/Demo, which %s has; not listed'%ID,r.stderr);self.assertIn("status: Thing not listed: PSP/GAME/Demo is another app's.",r.stderr)
  r=self.run_client('get',ID,VERBOSE=1);self.assertEqual(r.stdout.strip(),'0',r.stderr);self.assertEqual(self.state()[ID]['installed']['version'],'2')
  self.run_client('remove');self.assertNotIn(ID,self.state());self.assertFalse((self.root/'ms0:/PSP/GAME/Demo').exists())
  # Removed, it holds nothing: the other app is listed again.
  self.assertEqual([l.split()[0] for l in self.run_client('fetch').stdout.splitlines()],['io.github.other.thing'])
 def test_between_two_apps_not_installed_the_first_listed_keeps_the_folder(self):
  (self.root/'ms0:/PSP/PSPDX').mkdir(parents=True);(self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('https://example.com/catalog.json\n')
  catalog=self.other_thing();thing=catalog['apps'][0]
  second=dict(thing,id='io.github.other.second',name='Second',source='https://github.com/other/second',releases=[dict(thing['releases'][0],url='https://github.com/other/second/releases/download/v1/x.zip')])
  self.write('catalog.json',dict(catalog,apps=[thing,second]))
  r=self.run_client('fetch',VERBOSE=1);self.assertEqual([l.split()[0] for l in r.stdout.splitlines()],['io.github.other.thing'],r.stderr)
  self.assertIn('catalog: io.github.other.second wants PSP/GAME/Demo, which io.github.other.thing has; not listed',r.stderr);self.assertIn("status: Second not listed: PSP/GAME/Demo is another app's.",r.stderr)
  self.write('catalog.json',dict(catalog,apps=[second,thing]))
  r=self.run_client('fetch',VERBOSE=1);self.assertEqual([l.split()[0] for l in r.stdout.splitlines()],['io.github.other.second'],r.stderr);self.assertIn("status: Thing not listed",r.stderr)
 def test_the_folder_line_gives_way_in_the_name_only(self):
  self.assertEqual(self.run_client('folderline','Demo','Demo').stdout,"Demo not listed: PSP/GAME/Demo is another app's.\n")
  line=self.run_client('folderline','Ä'*60,'D'*32).stdout.rstrip('\n');self.assertLessEqual(len(line.encode()),95)
  self.assertTrue(line.startswith('Ä'*11) and not line.startswith('Ä'*12),line);self.assertTrue(line.endswith(" not listed: PSP/GAME/%s is another app's."%('D'*32)),line)
 # --- what the stick and a list are held to (QA review C) ---
 def test_a_long_run_of_zeros_at_the_buffer_edge_unpacks(self):
  # zlib can still hold output after the last compressed byte when the 64 KB buffer fills mid-match.
  for size,level in [(327683,6),(327683,9),(393217,1)]:
   with self.subTest(size=size,level=level):
    with zipfile.ZipFile(self.root/'new.zip','w',compression=zipfile.ZIP_DEFLATED,compresslevel=level) as z:z.writestr('EBOOT.PBP',bytes(size))
    r=self.run_client('install',VERBOSE=1);self.assertEqual((self.root/'ms0:/PSP/GAME/Demo/EBOOT.PBP').read_bytes(),bytes(size),r.stderr);self.run_client('remove')
 def test_a_record_that_names_a_field_twice_is_damaged(self):
  # A second "installed" would come to the front after an update, and a removal would then take another app's folder.
  self.run_client('install',VERSION=1);record=self.root/f'ms0:/PSP/PSPDX/INSTALLED/{ID}.state.json';text=record.read_text()
  other=self.root/'ms0:/PSP/GAME/Other';other.mkdir();(other/'save.dat').write_text('precious')
  for key,value in [('installed','{"version":"0","published_at":0,"installdir":"PSP/GAME/Other"}'),('source','"https://github.com/test/demo"')]:
   with self.subTest(key=key):
    doubled=text[:-1]+',"%s":%s}'%(key,value);record.write_text(doubled)
    self.assertNotEqual(self.run_client('install',ok=False,VERSION=2).returncode,0);self.assertNotEqual(self.run_client('remove',ok=False).returncode,0)
    self.assertEqual(record.read_text(),doubled);self.assertEqual((other/'save.dat').read_text(),'precious');self.assertTrue((self.root/'ms0:/PSP/GAME/Demo/EBOOT.PBP').exists())
  record.write_text(text.replace('"latest":{','"latest":{"size":1,'));self.assertNotEqual(self.run_client('install',ok=False,VERSION=2).returncode,0)
 def test_a_record_field_too_long_for_its_place_is_refused_not_cut(self):
  self.fixtures();self.run_client('fetch');record=self.state()[ID];self.assertEqual(self.run_client('latest',ID).stdout.split()[0],'2')
  for field,value in [('version','€'*86),('download_url','https://github.com/test/demo/releases/download/v2/'+'a'*480+'/download.zip'),('checked_from','https://'+'c'*300)]:
   with self.subTest(field=field):
    self.write(f'ms0:/PSP/PSPDX/INSTALLED/{ID}.state.json',dict(record,latest=dict(record['latest'],**{field:value})));self.assertEqual(self.run_client('latest',ID,ok=False).stdout.strip(),'-1')
  # A source past what a record keeps makes the records unreadable, so nothing is written over them.
  self.write(f'ms0:/PSP/PSPDX/INSTALLED/{ID}.state.json',record);self.plant_record('de.example.long','https://archive.org/'+'x'*1100,'PSP/GAME/Long')
  r=self.run_client('install',ok=False,VERBOSE=1);self.assertNotEqual(r.returncode,0);self.assertIn('writes blocked',r.stderr)
 def test_every_source_that_loads_can_be_deleted(self):
  path=self.root/'ms0:/PSP/PSPDX/sources.txt';path.parent.mkdir(parents=True)
  for text,url,rest in [('﻿https://example.com/catalog.json\nhttps://github.com/o/r\n','https://example.com/catalog.json','https://github.com/o/r\n'),
                        ('https://a.example/  # '+'c'*300+'\nhttps://github.com/o/r\n','https://a.example/','https://github.com/o/r\n'),
                        # A file pasted together from two: the mark inside does not keep a second copy loading once the first goes.
                        ('https://b.example/\n﻿https://b.example/\nhttps://github.com/o/r\n','https://b.example/','https://github.com/o/r\n')]:
   with self.subTest(url=url):
    path.write_bytes(text.encode());self.assertTrue(self.run_client('reach').stdout.splitlines()[0].startswith(url+' '))
    self.assertEqual(self.run_client('drop',url).stdout.strip(),'1');self.assertEqual(path.read_text(),rest)
 def test_a_list_past_what_is_read_is_cut_at_a_line(self):
  # 64 KB of a catalog.txt are read; the line the edge falls in is not read as a shorter repository.
  head='https://github.com/owner/repo';pad=64*1024-1-len(head);lines=''
  while len(lines)+1000<=pad-2:lines+='#'+'x'*998+'\n'
  lines+='#'*(pad-len(lines)-1)+'\n';(self.root/'catalog.txt').write_text(lines+'https://github.com/owner/repository-with-a-long-name\nhttps://github.com/owner/other\n')
  (self.root/'ms0:/PSP/PSPDX').mkdir(parents=True);(self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('https://example.com/pspdx/\n')
  r=self.run_client('fetch',ok=False,CATALOG_DOWN=1,VERBOSE=1);self.assertIn('longer than 64 KB',r.stderr)
  self.assertNotIn('/owner/repo/',(self.root/'requests.log').read_text())
  # A cache line too long for its place is not cut into another address either.
  (self.root/'catalog.txt').write_text('cache https://example.com/'+'x'*300+'/catalog.json\nhttps://github.com/test/demo\n');(self.root/'requests.log').write_text('')
  (self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('https://example.com/list/catalog.txt\n');self.run_client('fetch',ok=False,CATALOG_DOWN=1)
  self.assertNotIn('xxx',(self.root/'requests.log').read_text().replace('catalog.txt',''))
 def test_the_inbox_summary_is_never_cut_inside_a_character(self):
  self.fixtures();catalog=self.catalog_app();first=catalog['apps'][0]
  for count,name,summary in [(2,'€'*30+'ab',' …'),(3,'A'*40,'A'*40+', '+'A'*40+' …'),(1,'€'*31,'€'*31)]:
   with self.subTest(count=count,name=name):
    shutil.rmtree(self.root/'ms0:/PSP/PSPDX/INBOX',ignore_errors=True);apps=[first]
    for i in range(count):
     source='https://github.com/test/in%d'%i;apps.append(dict(first,id='io.github.test.in%d'%i,source=source,installdir='PSP/GAME/In%d'%i,releases=[dict(first['releases'][0],url=source+'/releases/download/v2/download.zip')]))
     self.write('ms0:/PSP/PSPDX/INBOX/in%d.pspdx'%i,dict(SPEC,source=source,name=name,installdir='PSP/GAME/In%d'%i))
    self.write('catalog.json',dict(catalog,apps=apps));r=subprocess.run([BIN,'inbox'],cwd=self.root,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0',FETCH_FIRST='1'),capture_output=True)
    self.assertEqual(r.stdout.strip(),str(count).encode());self.assertIn(('summary: '+summary+'\n').encode(),r.stderr)
 def test_a_folder_never_ends_in_a_dot(self):
  # FAT drops a trailing dot: PSP/GAME/Demo. is Demo, and ... the folder above.
  for folder in ('Demo.','...','.'):
   with self.subTest(folder=folder):self.parse(dict(SPEC,installdir='PSP/GAME/'+folder),ok=False)
  self.assertEqual(self.parse(dict(SPEC,installdir='PSP/GAME/.Demo.x')).split('|')[0],'PSP/GAME/.Demo.x')
  mirror=dict(schema=SCHEMA,source='https://archive.org/details/tetris',name='Tetris Inc.')
  self.assertEqual(self.parse(mirror).split('|')[0],'PSP/GAME/TetrisInc')
  self.assertEqual(self.parse({k:v for k,v in SPEC.items() if k!='installdir'}|dict(source='https://github.com/test/demo.')).split('|')[0],'PSP/GAME/demo')
 def test_a_journal_whose_manifest_is_not_text_is_left_alone(self):
  self.run_client('install',VERSION=1);saved=self.root/f'ms0:/PSP/PSPDX/INSTALLED/{ID}.pspdx';before=saved.read_text()
  journal=self.root/'ms0:/PSP/PSPDX/TMP/transaction.json';self.write('ms0:/PSP/PSPDX/TMP/transaction.json',dict(id=ID,dir='Demo',prior='Demo',op='install',phase='ready',old_state=self.state(),old_manifest=5))
  r=self.run_client('recover',VERBOSE=1);self.assertIn('not text',r.stderr);self.assertEqual(saved.read_text(),before);self.assertTrue(journal.exists())
 def test_a_full_presets_seen_never_brings_a_deleted_source_back(self):
  path=self.root/'ms0:/PSP/PSPDX/sources.txt';path.parent.mkdir(parents=True);path.write_text('# mine\n')
  (self.root/'ms0:/PSP/PSPDX/presets.seen').write_text(''.join('https://example.com/seen%d/\n'%i for i in range(64)))
  self.plant_presets('https://example.com/new/\n');r=self.run_client('presets',VERBOSE=1);self.assertIn('presets.seen is full',r.stderr)
  path.write_text('# mine\n');self.run_client('presets');self.assertEqual(path.read_text(),'# mine\n')
 def test_a_source_as_long_as_one_that_loads_can_be_added(self):
  self.fixtures();(self.root/'catalog.txt').write_text(SPEC['source']+'\n');path=self.root/'ms0:/PSP/PSPDX/sources.txt';path.write_text('')
  address=lambda length:'https://example.com/'+'x'*(length-len('https://example.com/')-len('/catalog.txt'))+'/catalog.txt'
  for length,ok in [(255,True),(256,False)]:
   with self.subTest(length=length):
    url=address(length);self.assertEqual(len(url),length)
    self.assertEqual(self.run_client('add',url,ok=False).returncode==0,ok);self.assertEqual(url in path.read_text().splitlines(),ok)
  self.assertIn(address(255)+' ok',self.run_client('reach').stdout.splitlines())
 # --- what a .pspdx and an id are held to (QA review A) ---
 def raw_parse(self,data,ok):
  (self.root/'raw.json').write_bytes(data);r=self.run_client('parse','raw.json',ok=False)
  self.assertEqual(r.returncode==0,ok,(data[-60:],r.stderr))
 def test_a_pspdx_is_json_to_the_letter(self):
  base=json.dumps(SPEC).encode();body=base[:-1]
  for tail,ok in [(b',"description":"\\\\u0000"}',True),(b',"description":"\\u0000"}',False),(b',"x":"\\\\\\u0000"}',False),(b',"description":"a\\nb"}',True),
                  (b',"description":"a\nb"}',False),(b',"x":"a\tb"}',False),(b',\x0b"x":1}',False),(b'}\x0b',False),(b'}\x0c',False),(b'}\r\n\t ',True),(b',"x":"\xff"}',False),(b',"x":"\xed\xa0\x80"}',False)]+\
                 [(b',"x":%s}'%n,False) for n in (b'01',b'-01',b'00',b'1.',b'1.e5',b'+1',b'.5',b'-',b'1e',b'0x10')]+[(b',"x":%s}'%n,True) for n in (b'0',b'-0',b'1',b'-1.5',b'1e5',b'1E+5',b'0.5e-3',b'[1,2]')]:
   with self.subTest(tail=tail):self.raw_parse(body+tail,ok)
  self.raw_parse(b'\xef\xbb\xbf'+base,True);self.raw_parse(json.dumps(SPEC,indent=2).replace('\n','\r\n').encode(),True)
 def test_an_id_is_made_whole_or_not_at_all(self):
  ids=lambda *a:self.run_client('ids',*a).stdout.split('\n')
  for url,made in [('https://github.com/-/_',['-','-']),('https://github.com/owner/-',['-','-']),('https://github.com/o/a.git.git',['-','-']),('https://github.com/o/a.git',['io.github.o.a','https://github.com/o/a']),('https://github.com/O-1/R_2',['io.github.o1.r2','https://github.com/O-1/R_2'])]:
   with self.subTest(url=url):self.assertEqual(ids(url)[0].split(),made)
  for source,name,made in [('https://例え.jp/','App','-'),('https://テスト.jp/','App','-'),('https://a..b.org/','App','-'),('https://example.org./','App','org.example.app'),('https://evil.example\\@good.example/','App','example.evil.app'),
                           ('https://[2001:db8::1]:8443/list','App','2001db81.app'),('https://owner.github.io/list/','App','-'),('https://github.io/','App','-'),('https://github.io.example/','App','example.io.github.app')]:
   with self.subTest(source=source):self.assertEqual(ids('https://github.com/o/r',source,name)[1],made)
  self.assertIn('which only a GitHub repository has',self.run_client('ids','x','https://owner.github.io/','App',VERBOSE=1).stderr)
  # A file whose source is at <owner>.github.io makes no id, since it would be a repository's.
  self.parse(dict(schema=SCHEMA,source='https://owner.github.io/list/',name='App'),ok=False)
 # --- a release the .pspdx pins ---
 def release_json(self,tag,*names,**extra):
  size=(self.root/'new.zip').stat().st_size
  return dict(tag_name=tag,published_at='2026-09-1%dT00:00:00Z'%(len(tag)%10),assets=[dict(name=n,size=size,browser_download_url='https://github.com/test/demo/releases/download/%s/%s'%(tag,n)) for n in names],**extra)
 def forget_latest(self):
  # What the last check heard stays in the record and lists the app; a case that is about what the repository says now starts without it.
  record=self.state()[ID];record.pop('latest',None);self.write(f'ms0:/PSP/PSPDX/INSTALLED/{ID}.state.json',record)
 def direct(self,spec):
  # The demo installed at version 1, and asked at its repository with no catalog.
  self.run_client('install',VERSION=1);self.write('manifest.json',spec);(self.root/'ms0:/PSP/PSPDX/sources.txt').write_text(SPEC['source']+'\n');(self.root/'requests.log').write_text('')
 def test_a_pinned_release_is_the_one_asked_for(self):
  # The tag the file pins, a prerelease or not, and nothing newer is looked for; an unknown field inside release is passed over.
  self.direct(dict(SPEC,release=dict(tag='v2',note='kept for later')))
  self.write('release.json',self.release_json('v3','download.zip'));self.write('release-tag.json',self.release_json('v2','download.zip',prerelease=True))
  self.assertEqual(self.run_client('release',ID).stdout.split(),['2','https://github.com/test/demo/releases/download/v2/download.zip','1'])
  requests=(self.root/'requests.log').read_text();self.assertIn('https://api.github.com/repos/test/demo/releases/tags/v2\n',requests);self.assertNotIn('/releases/latest',requests)
  # Installed at 1 and pinned to 2, forced or not: a direct check never calls it an update.
  for env in ({},dict(FORCE=1)):
   with self.subTest(env=env):self.assertEqual(self.row(self.run_client('fetch',**env).stdout)[3],'2')
  # A tag is any text: it is escaped for the path.
  self.write('manifest.json',dict(SPEC,release=dict(tag='v 1/β')));(self.root/'requests.log').write_text('');self.run_client('fetch',ok=False)
  self.assertIn('/releases/tags/v%201%2F%CE%B2\n',(self.root/'requests.log').read_text())
  # Without the pin it is the latest again, and that is an update.
  self.write('manifest.json',SPEC);self.assertEqual(self.row(self.run_client('fetch',FORCE=1).stdout)[1:4],['3','1','3'])
 def test_a_pin_holds_between_checks_that_do_not_ask(self):
  # Installed before the repository pinned: once a check has heard the pin, one that does not ask again knows it too.
  self.direct(dict(SPEC,release=dict(tag='v2')));self.write('release.json',self.release_json('v3','download.zip'));self.write('release-tag.json',self.release_json('v2','download.zip'))
  self.assertEqual(self.row(self.run_client('fetch').stdout)[3],'2');self.assertTrue(self.state()[ID]['latest']['pinned'])
  (self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('');self.assertEqual(self.row(self.run_client('fetch').stdout)[1:4],['2','0','2'])
  # Unpinned again, the newest is an update, asked or not.
  (self.root/'ms0:/PSP/PSPDX/sources.txt').write_text(SPEC['source']+'\n');self.write('manifest.json',SPEC)
  self.assertEqual(self.row(self.run_client('fetch').stdout)[3],'3');self.assertNotIn('pinned',self.state()[ID]['latest']);(self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('')
  self.assertEqual(self.row(self.run_client('fetch').stdout)[3],'3')
 def test_an_inbox_file_that_pins_installs_that_release_or_nothing(self):
  # The catalog offers v2: a file pinning v1 waits in INBOX, one pinning v2 is queued.
  self.fixtures()
  for tag,queued in [('v1','0'),('v2','1'),('2','0')]:
   with self.subTest(tag=tag):
    self.write('ms0:/PSP/PSPDX/INBOX/one.pspdx',dict(SPEC,release=dict(tag=tag)));r=self.run_client('inbox',VERBOSE=1,FETCH_FIRST=1)
    self.assertEqual(r.stdout.strip(),queued,r.stderr);self.assertEqual('which its source does not offer; kept' in r.stderr,queued=='0')
 def test_an_inbox_pin_for_an_installed_app_nobody_lists_asks_github_for_that_tag(self):
  # Installed at 1 and heard at its repository; no list names it now, so its row knows no tag: GitHub is asked once, for the pinned tag, and what it answers must be the pin exactly.
  def installed_and_unlisted():
   self.direct(SPEC);self.write('release.json',self.release_json('v3','download.zip'));self.write('release-tag.json',self.release_json('v2','download.zip'))
   self.run_client('fetch');(self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('');(self.root/'requests.log').write_text('')
  api=lambda:[l for l in (self.root/'requests.log').read_text().splitlines() if 'api.github.com' in l]
  (self.root/'map').write_text('releases/tags/v9 - 404\n');installed_and_unlisted()
  for tag,queued in [('v2','1'),('2','0'),('v9','0')]:
   with self.subTest(tag=tag):
    self.write('ms0:/PSP/PSPDX/INBOX/one.pspdx',dict(SPEC,release=dict(tag=tag)));(self.root/'requests.log').write_text('')
    r=self.run_client('inbox',VERBOSE=1,FETCH_FIRST=1,URL_MAP=self.root/'map');self.assertEqual(r.stdout.strip(),queued,r.stderr)
    self.assertEqual(api(),['https://api.github.com/repos/test/demo/releases/tags/'+tag],r.stderr)
    self.assertEqual('which its source does not offer; kept' in r.stderr,queued=='0')
  # It installs that release: the repository's own .pspdx wins where there is one, the file where GitHub says there is none.
  mine=dict(SPEC,summary='Mine',release=dict(tag='v2'))
  for own in (True,False):
   with self.subTest(own=own):
    installed_and_unlisted()
    if not own:(self.root/'manifest.json').unlink()
    inbox=self.root/'ms0:/PSP/PSPDX/INBOX/one.pspdx';self.write('ms0:/PSP/PSPDX/INBOX/one.pspdx',mine)
    r=self.run_client('inboxinstall',VERBOSE=1,FETCH_FIRST=1);self.assertFalse(inbox.exists(),r.stderr)
    self.assertEqual(self.state()[ID]['installed']['version'],'2');self.assertEqual(self.saved(),SPEC if own else mine)
    self.assertEqual(api(),['https://api.github.com/repos/test/demo/releases/tags/v2'],r.stderr)
 def test_a_record_keeps_its_folder_from_a_saved_catalog(self):
  # A catalog that is only saved lists another app in Demo and the installed demo in Other; the record has Demo, so the other app is the one left out, and the record's word moves the demo's row to Demo.
  self.fixtures();catalog=self.catalog_app();first=catalog['apps'][0]
  a=dict(first,id='io.github.test.a',name='Aaa',source='https://github.com/test/a',installdir='PSP/GAME/Demo',releases=[dict(first['releases'][0],url='https://github.com/test/a/releases/download/v2/download.zip')])
  self.write('catalog.json',dict(catalog,apps=[a,dict(first,installdir='PSP/GAME/Other')]));self.run_client('fetch')
  # Offline nothing is asked at the repository: only the record's word can move the row.
  (self.root/'requests.log').write_text('');r=self.run_client('fetch',VERBOSE=1,OFFLINE=1);self.assertEqual((self.root/'requests.log').read_text(),'')
  self.assertEqual([l.split()[0] for l in r.stdout.splitlines()],[ID],r.stderr)
  self.assertIn('catalog: io.github.test.a wants PSP/GAME/Demo, which %s has; not listed'%ID,r.stderr);self.assertIn("status: Aaa not listed: PSP/GAME/Demo is another app's.",r.stderr)
 def test_a_pinned_release_names_one_of_its_assets(self):
  base='https://github.com/test/demo/releases/download/v2/'
  self.write('release-tag.json',self.release_json('v2','game-psp-download.zip','other-download.zip','notes.txt'))
  self.direct(dict(SPEC,release=dict(tag='v2',url=base+'other-download.zip',published_at='2026-09-12')))
  self.assertEqual(self.run_client('release',ID).stdout.split()[1],base+'other-download.zip')
  self.write('manifest.json',dict(SPEC,release=dict(tag='v2',url=base+'missing.zip')));self.forget_latest();r=self.run_client('release',ID,ok=False,VERBOSE=1)
  self.assertNotIn('releases/download',r.stdout);self.assertIn('the release.url of its .pspdx is not an asset of the release',r.stderr)
  # Without a url the zip rule picks, as for any release.
  self.write('manifest.json',dict(SPEC,release=dict(tag='v2')));self.assertEqual(self.run_client('release',ID).stdout.split()[1],base+'game-psp-download.zip')
 def test_the_zip_is_the_only_one_or_the_only_one_for_psp(self):
  self.direct(SPEC)
  for names,chosen in [(['download.zip','readme.txt'],'download.zip'),(['vita-download.zip','PSP-download.zip'],'PSP-download.zip'),(['a-psp-download.zip','b-Psp-download.zip'],None),
                       (['one-download.zip','two-download.zip'],None),(['readme.txt'],None)]:
   with self.subTest(names=names):
    self.write('release.json',self.release_json('v3',*names));self.forget_latest();r=self.run_client('release',ID,ok=False,VERBOSE=1)
    if chosen:self.assertEqual(r.stdout.split()[1],'https://github.com/test/demo/releases/download/v3/'+chosen,r.stderr)
    else:self.assertNotIn('releases/download',r.stdout);self.assertIn('no .zip on the release' if len(names)==1 else 'not exactly one with psp in its name',r.stderr)
 def test_a_release_in_the_file_is_held_to_its_rules(self):
  for release in (dict(tag='v1'),dict(tag='x'*64,url='https://github.com/test/demo/releases/download/v1/a.zip',published_at='2026-09-12T08:29:23Z',later={'any':1}),dict(tag='β'*64,published_at='2024-02-29')):
   with self.subTest(release=release):self.parse(dict(SPEC,release=release))
  for release in ('v1',dict(),dict(tag=''),dict(tag='x'*65),dict(tag=1),dict(tag='a\tb'),dict(tag='v1',url='http://example.com/a.zip'),dict(tag='v1',url='https://'+'u'*505),
                  dict(tag='v1',url='https://example.com/ä.zip'),dict(tag='v1',url=7),dict(tag='v1',published_at='2023-02-31'),dict(tag='v1',published_at=1)):
   with self.subTest(release=release):self.parse(dict(SPEC,release=release),ok=False)
  self.parse(dict(SPEC,release=dict(tag='v1',url='https://'+'u'*504)))
  # Outside GitHub nothing says where the tag's zip is or when it came out but the file.
  mirror=dict(schema=SCHEMA,source='https://archive.org/details/psp-blocks',name='Blocks')
  self.parse(dict(mirror,release=dict(tag='v1',url='https://archive.org/download/psp-blocks/blocks.zip',published_at='2024-12-20')))
  for release in (dict(tag='v1'),dict(tag='v1',url='https://archive.org/download/psp-blocks/blocks.zip'),dict(tag='v1',published_at='2024-12-20')):
   with self.subTest(outside=release):self.parse(dict(mirror,release=release),ok=False)
  (self.root/'raw.json').write_text(json.dumps(SPEC)[:-1]+', "release": {"tag": "v1", "tag": "v2"}}');self.assertNotEqual(self.run_client('parse','raw.json',ok=False).returncode,0)
 # --- the small things before 0.7 ---
 def test_a_catalog_that_names_no_schema_reads_past_what_it_does_not_know(self):
  # No schema and fields no version reads, top-level and in an entry, a listed_by among them: read as v1, gzipped or not (an update, since the entry names no hash and a later date). A schema that is a list, an object or true names no other version, and is v1 too.
  self.fixtures();catalog=self.catalog_app();del catalog['schema']
  catalog.update(maintainer='someone',listed_by='https://lists.example.org/psp/');catalog['apps'][0].update(listed_by='https://lists.example.org/psp/',homepage={'any':1})
  self.write('catalog.json',catalog);self.assertEqual(self.row(self.run_client('fetch').stdout)[1:4],['2','1','3'])
  self.gzip_catalog();self.assertEqual(self.row(self.run_client('fetch').stdout)[1:4],['2','1','3']);(self.root/'catalog.json.gz').unlink()
  for schema in ([],{},True):
   with self.subTest(schema=schema):self.write('catalog.json',dict(catalog,schema=schema));self.assertEqual(self.row(self.run_client('fetch').stdout)[1:4],['2','1','3'])
 def test_a_catalog_id_is_kept_up_to_159_bytes_and_when_it_is_one(self):
  self.fixtures();catalog=self.catalog_app();first=catalog['apps'][0]
  longest='com.example.'+'a'*(159-len('com.example.'));self.assertEqual(len(longest),159)
  for given,listed in [(longest,longest),(longest+'a','org.archive.blocks'),('de.wijsman.blocks','de.wijsman.blocks'),('com..example','org.archive.blocks'),('.com.example','org.archive.blocks'),
                       ('com.example.','org.archive.blocks'),('Com.example','org.archive.blocks'),('com.exa_mple','org.archive.blocks'),('comexample','org.archive.blocks'),
                       ('io.github.someone.blocks','org.archive.blocks'),('com.example.blöcks','org.archive.blocks')]:
   with self.subTest(given=given):
    self.write('catalog.json',dict(catalog,apps=[first,self.mirror(id=given)]));r=self.run_client('fetch',VERBOSE=1)
    self.assertEqual([l.split()[0] for l in r.stdout.splitlines()],[ID,listed],r.stderr)
  # The longest names the files on the stick whole.
  self.write('catalog.json',dict(catalog,apps=[first,self.mirror(id=longest)]))
  r=self.run_client('get',longest,VERBOSE=1);self.assertEqual(r.stdout.strip(),'0',r.stderr)
  self.assertTrue((self.root/f'ms0:/PSP/PSPDX/INSTALLED/{longest}.state.json').exists());self.assertEqual(self.row(self.run_client('fetch').stdout,longest)[3],'2')
 def test_a_release_date_at_the_edges_of_what_a_stick_counts(self):
  # Seconds since 1970 in 32 bits: the first second after the epoch to the last one 2106 has. The epoch itself is a record's "no date", and is not taken.
  self.fixtures();catalog=self.catalog_app();release=catalog['apps'][0]['releases'][0]
  for published,version in [('1969-12-31T23:59:59Z','3'),('1970-01-01T00:00:00Z','3'),('1970-01-01','3'),('1970-01-01T00:00:01Z','2'),('1970-01-02','2'),
                            ('2106-02-07T06:28:15Z','2'),('2106-02-07T06:28:16Z','3'),('2106-02-08','3'),('1970-01-01T00:00:01','3'),('1970-1-01','3'),('0000-01-01','3')]:
   with self.subTest(published=published):
    self.forget_latest();self.write('catalog.json',dict(catalog,apps=[dict(catalog['apps'][0],releases=[dict(release,published_at=published)])]));self.assertEqual(self.row(self.run_client('fetch').stdout)[1],version)
 def test_an_empty_catalog_is_a_catalog_of_nothing(self):
  (self.root/'ms0:/PSP/PSPDX').mkdir(parents=True);(self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('https://example.com/catalog.json\n')
  for catalog in (dict(schema='https://chriopter.github.io/pspdx/schema/catalog-v1.json',generated_at=NOW(),apps=[]),dict(generated_at=NOW(),apps=[]),dict(apps=[]),{},[]):
   with self.subTest(catalog=catalog):self.write('catalog.json',catalog);r=self.run_client('fetch',ok=False,VERBOSE=1);self.assertEqual(r.stdout,'',r.stderr)
  (self.root/'catalog.json').write_bytes(b'');self.assertEqual(self.run_client('fetch',ok=False).stdout,'')
  # Installed, the app stays on the list with a catalog that names nothing.
  self.fixtures();self.write('catalog.json',dict(generated_at=NOW(),apps=[]));self.assertIsNotNone(self.row(self.run_client('fetch',ok=False).stdout))
 def test_text_from_an_entry_in_any_language_is_listed_and_saved_whole(self):
  catalog=self.from_the_entry();name='Überschall 東京 🎮';description=('🎮'*2499)+'\n'
  catalog['apps'][0].update(name=name,summary='Schnell — 速い',author='Zoë',description=description);self.write('catalog.json',catalog)
  r=self.run_client('get',ID,VERBOSE=1);self.assertEqual(r.stdout.strip(),'0',r.stderr)
  self.assertEqual((self.saved()['name'],self.saved()['summary'],self.saved()['author'],self.saved()['description']),(name,'Schnell — 速い','Zoë',description))
  catalog['apps'][0].update(description='🎮'*2501);self.write('catalog.json',catalog);self.assertIn('breaks the .pspdx rules (description)',self.run_client('fetch',VERBOSE=1).stderr)
 def test_a_release_outside_github_from_the_list_to_an_update(self):
  # A list, a source and a zip on three hosts, none of them GitHub: the id is the source's, the hash is the gate, another hash is the update, and GitHub is never asked.
  (self.root/'ms0:/PSP/PSPDX').mkdir(parents=True);(self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('https://lists.example.com/psp/catalog.json\n');(self.root/'requests.log').write_text('')
  package=(self.root/'new.zip').read_bytes();sha=hashlib.sha256(package).hexdigest();blocks='org.example.psp.blocks'
  entry=dict(name='Blocks',source='https://psp.example.org/apps/blocks',installdir='PSP/GAME/Blocks',summary='Falling blocks',
             releases=[dict(tag='v1',published_at='2026-01-02T00:00:00Z',size=len(package),sha256=sha,url='https://files.example.net/blocks/v1/download.zip')])
  listing=lambda **change:self.write('catalog.json',dict(generated_at=NOW(),apps=[dict(entry,**change)]))
  listing();r=self.run_client('fetch',VERBOSE=1);self.assertEqual([l.split() for l in r.stdout.splitlines()],[[blocks,'1','1','1','0']],r.stderr)
  r=self.run_client('get',blocks,VERBOSE=1);self.assertEqual(r.stdout.strip(),'0',r.stderr);self.assertEqual((self.root/'ms0:/PSP/GAME/Blocks/EBOOT.PBP').read_bytes(),b'new package')
  self.assertEqual((self.state()[blocks]['source'],self.state()[blocks]['installed']['sha256'],self.saved(blocks)['source']),(entry['source'],sha,entry['source']))
  for env in ({},dict(FORCE=1)):
   with self.subTest(env=env):self.assertEqual(self.row(self.run_client('fetch',**env).stdout,blocks)[3],'2')
  # Another tag over the same zip is not an update; another hash under the same tag is, and a download that does not hash to it is refused and changes nothing.
  listing(releases=[dict(entry['releases'][0],tag='v1.0.1')]);self.assertEqual(self.row(self.run_client('fetch').stdout,blocks)[3],'2')
  other=hashlib.sha256(b'other').hexdigest();listing(releases=[dict(entry['releases'][0],sha256=other)]);self.assertEqual(self.row(self.run_client('fetch').stdout,blocks)[3],'3')
  r=self.run_client('get',blocks,ok=False,VERBOSE=1);self.assertNotEqual(r.stdout.strip(),'0');self.assertIn('sha256 MISMATCH',r.stderr)
  self.assertEqual(self.state()[blocks]['installed']['sha256'],sha);self.assertEqual((self.root/'ms0:/PSP/GAME/Blocks/EBOOT.PBP').read_bytes(),b'new package')
  # The real next zip, hashed right, installs over it and is current.
  self.zip('new.zip',{'EBOOT.PBP':b'v2 package'});package=(self.root/'new.zip').read_bytes();sha2=hashlib.sha256(package).hexdigest()
  listing(releases=[dict(tag='v2',published_at='2026-02-02T00:00:00Z',size=len(package),sha256=sha2,url='https://files.example.net/blocks/v2/download.zip')])
  self.assertEqual(self.row(self.run_client('fetch').stdout,blocks)[1:4],['2','1','3'])
  r=self.run_client('get',blocks,VERBOSE=1);self.assertEqual(r.stdout.strip(),'0',r.stderr);self.assertEqual((self.root/'ms0:/PSP/GAME/Blocks/EBOOT.PBP').read_bytes(),b'v2 package')
  self.assertEqual((self.state()[blocks]['installed']['version'],self.row(self.run_client('fetch',FORCE=1).stdout,blocks)[3]),('2','2'))
  # With no list answering it is still the stick's, and still nobody at GitHub is asked.
  self.assertIsNotNone(self.row(self.run_client('fetch',CATALOG_DOWN=1,FORCE=1,ok=False).stdout,blocks))
  requests=set((self.root/'requests.log').read_text().splitlines());self.assertEqual([x for x in requests if 'github' in x],[])
  self.assertEqual(requests,{'https://lists.example.com/psp/catalog.json','https://files.example.net/blocks/v1/download.zip','https://files.example.net/blocks/v2/download.zip'})
  # Not installed: a zip over http, a source under github.io and an id claiming GitHub are each not what they say.
  for gone in ('INSTALLED','CACHE'):shutil.rmtree(self.root/'ms0:/PSP/PSPDX'/gone)
  shutil.rmtree(self.root/'ms0:/PSP/GAME')
  listing(releases=[dict(entry['releases'][0],url='http://files.example.net/blocks/v1/download.zip')]);r=self.run_client('fetch',ok=False,VERBOSE=1);self.assertEqual(r.stdout,'')
  self.assertIn('Blocks is from outside GitHub and its release cannot be installed as it stands; dropped',r.stderr)
  listing(source='https://owner.github.io/blocks/');r=self.run_client('fetch',ok=False,VERBOSE=1);self.assertEqual(r.stdout,'');self.assertIn('which only a GitHub repository has; refused',r.stderr)
  listing(id='io.github.owner.blocks');self.assertEqual(self.run_client('fetch').stdout.split()[0],blocks)
 def test_a_pspdx_from_outside_github_that_pins_its_release(self):
  # The file is read with its url and date; INBOX keeps it for a version that installs from outside GitHub, and never deletes it.
  mirror=dict(schema=SCHEMA,source='https://psp.example.org/apps/blocks',name='Blocks',release=dict(tag='v1',url='https://files.example.net/blocks/v1/download.zip',published_at='2026-01-02T00:00:00Z'))
  self.assertEqual(self.parse(mirror),'PSP/GAME/Blocks|homebrew|org.example.psp.blocks|')
  for release in (dict(tag='v1'),dict(tag='v1',url=mirror['release']['url']),dict(tag='v1',published_at='2026-01-02'),dict(tag='v1',url='http://files.example.net/a.zip',published_at='2026-01-02')):
   with self.subTest(release=release):self.parse(dict(mirror,release=release),ok=False)
  self.fixtures();self.write('ms0:/PSP/PSPDX/INBOX/blocks.pspdx',mirror)
  r=self.run_client('inbox',VERBOSE=1,FETCH_FIRST=1);self.assertEqual(r.stdout.strip(),'0',r.stderr);self.assertIn('from outside GitHub',r.stderr)
  self.run_client('inboxinstall');self.assertTrue((self.root/'ms0:/PSP/PSPDX/INBOX/blocks.pspdx').exists());self.assertNotIn('org.example.psp.blocks',self.state())
 def test_a_stick_at_0_4_to_0_7_sees_0_8_0_as_an_update_of_itself(self):
  # PSPDX's own record comes from its first start: rev 0 and the version built in. The same version is the release and notes its rev, a build past its tag too; any other is an update, 0.8 and 0.8.00 included; once noted, another zip is one too.
  (self.root/'ms0:/PSP/PSPDX').mkdir(parents=True);(self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('https://example.com/catalog.json\n')
  self_id='io.github.chriopter.pspdxapp';source='https://github.com/chriopter/pspdx-app'
  release=lambda tag,day,sha:dict(tag=tag,published_at='2026-09-%02dT08:51:07Z'%day,size=3000000,sha256=sha,url=source+'/releases/download/%s/pspdx.zip'%tag)
  entry=lambda *releases:dict(generated_at=NOW(),apps=[dict(id=self_id,name='PSPDX',author='chriopter',source=source,installdir='PSP/GAME/PSPDX',category='app',releases=list(releases))])
  def record(version,rev,sha=None,app_id=self_id,source=source):
   installed=dict(installdir='PSP/GAME/PSPDX',version=version,published_at=rev,**({'sha256':sha} if sha else {}))
   self.write(f'ms0:/PSP/PSPDX/INSTALLED/{app_id}.state.json',dict(source=source,installed=installed))
  (old,new)=('62'*32,'7a'*32);catalog=lambda:self.write('catalog.json',entry(release('v0.8.0',16,new),release('v0.4',14,old)))
  for version,state in [('0.4','3'),('0.5','3'),('0.6','3'),('0.7','3'),('0.7-3-gabc1234','3'),('0.8','3'),('0.8.0','2'),('0.8.0-3-gabc1234','2'),('0.8.00','3'),('dev','3')]:
   with self.subTest(version=version):
    record(version,0);catalog();self.assertEqual(self.row(self.run_client('fetch').stdout,self_id)[1:4],['0.8.0','1',state])
    self.assertEqual(self.state()[self_id]['installed']['published_at']!=0,state=='2')
  # A stick that noted its release's rev and hash: the zip of 0.8.0 is another than 0.4's to 0.7's, and 0.8.0's own is not.
  for version,sha,state in [('0.4',old,'3'),('0.5',old,'3'),('0.6',old,'3'),('0.7',old,'3'),('0.8.0',new,'2')]:
   with self.subTest(noted=version):
    record(version,1757926267,sha);catalog();self.assertEqual(self.row(self.run_client('fetch').stdout,self_id)[1:4],['0.8.0','1',state])
  # A 0.6 that has already noted its release's rev without a hash: 0.8.0 is an update, 0.6 itself is not.
  record('0.6',0);self.write('catalog.json',entry(release('v0.6',14,old)));self.assertEqual(self.row(self.run_client('fetch').stdout,self_id)[3],'2');self.assertNotEqual(self.state()[self_id]['installed']['published_at'],0)
  catalog();self.assertEqual(self.row(self.run_client('fetch').stdout,self_id)[1:4],['0.8.0','1','3'])
  # 0.4 and 0.5 kept their record as io.github.chriopter.pspdx: retired at start, and 0.8.0 is PSPDX's one row again.
  (self.root/f'ms0:/PSP/PSPDX/INSTALLED/{self_id}.state.json').unlink();self.game('PSPDX').mkdir(parents=True)
  legacy='io.github.chriopter.pspdx'
  for version in ('0.4','0.5'):
   with self.subTest(legacy=version):
    record(version,1757926267,old,app_id=legacy,source='https://github.com/chriopter/pspdx');catalog()
    self.assertEqual(self.run_client('retire').stdout.strip(),'1');self.assertNotIn(legacy,self.state())
    rows=self.run_client('fetch').stdout.splitlines();self.assertEqual([l.split()[0] for l in rows],[self_id]);self.assertEqual(self.row('\n'.join(rows),self_id)[1:3],['0.8.0','1'])
 # --- installs and removals the megatest broke (0.7) ---
 def game(self,*parts):return self.root.joinpath('ms0:/PSP/GAME',*parts)
 def journal_path(self):return self.root/'ms0:/PSP/PSPDX/TMP/transaction.json'
 def ours_left(self,folder):return sorted(str(p.relative_to(folder)) for p in folder.rglob('*') if p.name.endswith(('.pspdx-new','.pspdx-old')))
 def test_cancel_download_and_unpack_preserve_install_and_allow_retry(self):
  for device in ('ms0:', 'ef0:'):
   for existing in (False, True):
    for phase in ('download', 'unpack'):
     for after in (1, 2, 3):
      with self.subTest(device=device,existing=existing,phase=phase,after=after):
       for dev in ('ms0:', 'ef0:'):
        shutil.rmtree(self.root/dev);(self.root/dev).mkdir()
       self.zip('new.zip',{'EBOOT.PBP':b'old executable','data.txt':b'old data'})
       game=self.root/device/'PSP/GAME/Demo'
       if existing:
        self.run_client('install',INSTALL_DEVICE=device,VERSION=1)
        (game/'save.dat').write_bytes(b'user save\x00unchanged')
       before=self.state()
       self.zip('new.zip',{'EBOOT.PBP':os.urandom(20000),'data.txt':b'replacement','sub/new.dat':b'new'})
       r=self.run_client('install',INSTALL_DEVICE=device,CANCEL_PHASE=phase,CANCEL_AFTER=after,ok=False)
       self.assertEqual(r.stdout.split()[0],'-9',r.stderr)
       self.assertEqual(self.state(),before)
       self.assertFalse(self.journal_path().exists())
       self.assertFalse((self.root/'ms0:/PSP/PSPDX/TMP/download.zip').exists())
       self.assertFalse((game.parent/'.pspdx-stage').exists())
       if existing:
        self.assertEqual((game/'EBOOT.PBP').read_bytes(),b'old executable')
        self.assertEqual((game/'data.txt').read_bytes(),b'old data')
        self.assertEqual((game/'save.dat').read_bytes(),b'user save\x00unchanged')
        self.assertFalse((game/'sub/new.dat').exists())
        self.assertEqual(self.ours_left(game),[])
       else:self.assertFalse(game.exists())
       self.run_client('install',INSTALL_DEVICE=device)
       self.assertEqual(self.state()[ID]['installed']['version'],'2')
       if existing:self.assertEqual((game/'save.dat').read_bytes(),b'user save\x00unchanged')
       self.run_client('remove');self.assertFalse(game.exists())
 def test_interrupted_download_keeps_old_files_and_can_retry(self):
  for device in ('ms0:', 'ef0:'):
   for existing in (False, True):
    for after in (1, 8192, 16384):
     with self.subTest(device=device,existing=existing,after=after):
      for dev in ('ms0:', 'ef0:'):
       shutil.rmtree(self.root/dev);(self.root/dev).mkdir()
      self.zip('new.zip',{'EBOOT.PBP':b'old executable','data.txt':b'old data'})
      game=self.root/device/'PSP/GAME/Demo'
      if existing:
       self.run_client('install',INSTALL_DEVICE=device,VERSION=1)
       (game/'save.dat').write_bytes(b'my save')
      before=self.state()
      self.zip('new.zip',{'EBOOT.PBP':os.urandom(20000),'data.txt':b'updated'})
      r=self.run_client('install',INSTALL_DEVICE=device,NET_DROP_AFTER=after,ok=False)
      self.assertNotEqual(r.returncode,0)
      self.assertNotEqual(r.stdout.split()[0],'-9')  # failure, not user cancellation
      self.assertEqual(self.state(),before)
      if existing:
       self.assertEqual((game/'EBOOT.PBP').read_bytes(),b'old executable')
       self.assertEqual((game/'data.txt').read_bytes(),b'old data')
       self.assertEqual((game/'save.dat').read_bytes(),b'my save')
      else:self.assertFalse(game.exists())
      self.assertFalse(self.journal_path().exists())
      self.assertFalse((self.root/'ms0:/PSP/PSPDX/TMP/download.zip').exists())
      self.run_client('install',INSTALL_DEVICE=device)
      self.assertEqual((game/'data.txt').read_bytes(),b'updated')
      self.run_client('remove');self.assertFalse(game.exists())
 def test_repeated_install_reinstall_remove_leaves_no_transactions(self):
  for cycle in range(30):
   device='ef0:' if cycle%2 else 'ms0:'
   game=self.root/device/'PSP/GAME/Demo'
   payload=('cycle %d'%cycle).encode()
   self.zip('new.zip',{'EBOOT.PBP':payload,'nested/data.bin':payload*50})
   self.run_client('install',INSTALL_DEVICE=device)
   (game/'save.dat').write_bytes(payload+b' save')
   self.run_client('install',INSTALL_DEVICE=device)
   self.assertEqual((game/'EBOOT.PBP').read_bytes(),payload)
   self.assertEqual((game/'save.dat').read_bytes(),payload+b' save')
   self.run_client('remove');self.run_client('recover')
   self.assertFalse(game.exists());self.assertNotIn(ID,self.state())
   self.assertFalse(self.journal_path().exists())
   self.assertEqual(self.ours_left(self.root),[])
   for dev in ('ms0:', 'ef0:'):
    self.assertFalse((self.root/dev/'PSP/GAME/.pspdx-stage').exists())
    self.assertFalse((self.root/dev/'PSP/GAME/Demo.old').exists())
   self.assertFalse((self.root/'ms0:/PSP/PSPDX/TMP/download.zip').exists())
 def test_an_app_whose_folder_was_deleted_by_hand_can_be_installed_and_removed(self):
  # The record says installed, the folder is gone: neither a reinstall nor a removal may be stuck on it.
  self.run_client('install');d=self.game('Demo');shutil.rmtree(d)
  r=self.run_client('install',VERBOSE=1);self.assertEqual((d/'EBOOT.PBP').read_bytes(),b'new package');self.assertEqual(self.state()[ID]['installed']['installdir'],'PSP/GAME/Demo');self.assertIn('is gone; installed as new',r.stderr)
  shutil.rmtree(d);self.run_client('remove');self.assertNotIn(ID,self.state());self.assertFalse((self.root/f'ms0:/PSP/PSPDX/INSTALLED/{ID}.pspdx').exists())
  self.assertFalse(self.journal_path().exists());self.assertFalse(self.game('.pspdx-stage').exists())
  # Cut anywhere in a reinstall over the missing folder, recovery leaves either no folder or the whole new one.
  self.run_client('install',VERSION=1);shutil.rmtree(d);base=self.root/'base';shutil.copytree(self.root/'ms0:',base)
  for fault in range(1,120):
   shutil.rmtree(self.root/'ms0:');shutil.copytree(base,self.root/'ms0:')
   r=self.run_client('install',fault,ok=False);self.run_client('recover');version=self.state()[ID]['installed']['version']
   self.assertEqual((d/'EBOOT.PBP').read_bytes() if d.exists() else None,b'new package' if version=='2' else None,(fault,version))
   self.assertFalse(self.journal_path().exists(),fault)
   if r.returncode==0:break
  else:self.fail('the sweep did not reach the end')
 def test_a_finished_transaction_is_not_called_put_back(self):
  for command in ('install','remove'):
   with self.subTest(command=command):self.assertNotIn('put back',self.run_client(command,VERBOSE=1).stderr)
  self.run_client('install');self.write('ms0:/PSP/PSPDX/TMP/transaction.json',dict(id=ID,dir='Demo',prior='Demo',phase='ready',op='install',old_state=self.state(),old_manifest=json.dumps(SPEC)))
  self.assertIn('an unfinished transaction was put back',self.run_client('recover',VERBOSE=1).stderr)
 def test_recovery_leaves_a_folder_the_records_give_another_app(self):
  # A journal for a new id that names an installed app's folder, with a snapshot that leaves that app out, and one that names the folder PSPDX runs from: recovery touches neither and keeps the journal.
  self.run_client('install');game=self.game('Demo');before=self.state()
  pspdx=self.game('PSPDX');pspdx.mkdir();(pspdx/'EBOOT.PBP').write_bytes(b'running')
  for folder,check in (('Demo',lambda:(game/'EBOOT.PBP').read_bytes()==b'new package'),('PSPDX',lambda:(pspdx/'EBOOT.PBP').read_bytes()==b'running')):
   for op in ('install','remove'):
    for phase in ('placed','committed'):
     with self.subTest(folder=folder,op=op,phase=phase):
      if phase=='committed' and folder=='PSPDX':shutil.copytree(pspdx,self.game('PSPDX.old'))
      self.write('ms0:/PSP/PSPDX/TMP/transaction.json',dict(id='io.github.x.y',dir=folder,prior='',op=op,phase=phase,old_state={}))
      r=self.run_client('recover',VERBOSE=1);self.assertTrue(check());self.assertEqual(self.state(),before);self.assertIn('another app',r.stderr)
      self.assertTrue(self.journal_path().exists());self.run_client('discard');self.assertTrue(check())
      if phase=='committed' and folder=='PSPDX':self.assertTrue(self.game('PSPDX.old','EBOOT.PBP').exists());shutil.rmtree(self.game('PSPDX.old'))
 def test_an_update_that_fills_the_stick_leaves_nothing_in_the_way(self):
  # A stick that does not say what is free fills up in the middle of the unpack: recovery makes room before it writes the record back, and the next install goes through.
  self.run_client('install',VERSION=1);before=self.state();used=sum(p.stat().st_size for p in (self.root/'ms0:').rglob('*') if p.is_file())
  self.zip('new.zip',{'EBOOT.PBP':os.urandom(60000),'data.txt':b'new data'})
  r=self.run_client('install',ok=False,DEVCTL_FAIL=1,STICK_BYTES=used+60000+2000,VERBOSE=1);self.assertNotEqual(r.returncode,0)
  self.assertFalse(self.journal_path().exists(),r.stderr);self.assertEqual(self.state(),before);self.assertEqual(self.ours_left(self.game('Demo')),[])
  self.assertEqual((self.game('Demo','EBOOT.PBP')).read_bytes(),b'new package');self.assertFalse((self.root/'ms0:/PSP/PSPDX/TMP/download.zip').exists())
  self.run_client('install');self.assertEqual(len(self.game('Demo','EBOOT.PBP').read_bytes()),60000)
 def test_an_install_that_does_not_fit_says_so_before_it_writes(self):
  self.run_client('recover');used=sum(p.stat().st_size for p in (self.root/'ms0:').rglob('*') if p.is_file())
  # Not even the download fits, and then the download fits and what it unpacks to does not.
  for entries,room in (({'EBOOT.PBP':b'new package'},100),({'EBOOT.PBP':bytes(3000000)},400000)):
   with self.subTest(room=room):
    self.zip('new.zip',entries);r=self.run_client('install',ok=False,STICK_BYTES=used+room,VERBOSE=1)
    code,ops,needed=r.stdout.split();self.assertEqual(code,'-10',r.stderr);self.assertGreater(int(needed),room);self.assertIn('KB needed on the Memory Stick',r.stderr)
    self.assertNotIn(ID,self.state());self.assertFalse(self.game('Demo').exists());self.assertFalse(self.journal_path().exists());self.assertFalse((self.root/'ms0:/PSP/PSPDX/TMP/download.zip').exists())
  self.run_client('install');self.assertEqual(len(self.game('Demo','EBOOT.PBP').read_bytes()),3000000)
 def test_a_backup_that_will_not_go_blocks_no_other_install(self):
  # A read-only file in Demo.old: the removal is done, every other app still installs, only Demo says the folder is in the way, and the next start clears it.
  self.run_client('install');self.write('manifest.json',dict(SPEC,source='https://github.com/test/other',installdir='PSP/GAME/Other'))
  self.run_client('install');self.write('manifest.json',SPEC)
  r=self.run_client('remove',VERBOSE=1,RO_MATCH='Demo.old/');self.assertNotIn(ID,self.state());self.assertFalse(self.journal_path().exists());self.assertIn('tried again at the next start',r.stderr)
  self.assertTrue(self.game('Demo.old').exists());self.write('manifest.json',dict(SPEC,source='https://github.com/test/other',installdir='PSP/GAME/Other'))
  self.run_client('install',VERSION=3,RO_MATCH='Demo.old/');self.assertEqual(self.state()['io.github.test.other']['installed']['version'],'3')
  self.write('manifest.json',SPEC);r=self.run_client('install',ok=False,VERBOSE=1,RO_MATCH='Demo.old/');self.assertIn('Demo.old is in the way',r.stderr);self.assertTrue(self.game('Demo.old').exists())
  r=self.run_client('recover',VERBOSE=1);self.assertFalse(self.game('Demo.old').exists());self.assertIn('cleared',r.stderr);self.assertFalse((self.root/'ms0:/PSP/PSPDX/TMP/cleanup.txt').exists())
  self.run_client('install');self.assertEqual(self.state()[ID]['installed']['version'],'2')
  # A user's own folder that happens to end in .old is nobody's to clear.
  mine=self.game('Mine.old');mine.mkdir();(mine/'keep').write_text('keep');self.run_client('recover');self.assertTrue((mine/'keep').exists())
 def test_pspdx_never_deletes_the_folder_it_runs_from(self):
  # Whatever id a record gives it: the folder the EBOOT was started from is not deleted, and the code says why.
  pspdx=self.game('PSPDX');pspdx.mkdir(parents=True);(pspdx/'EBOOT.PBP').write_bytes(b'running')
  for app_id,source in (('io.github.chriopter.pspdxapp','https://github.com/chriopter/pspdx-app'),('io.github.someone.thing','https://github.com/someone/thing'),('org.example.pspdx','https://example.org/pspdx')):
   with self.subTest(app_id=app_id):
    self.plant_record(app_id,source,'PSP/GAME/pspdx');r=self.run_client('uninstall',app_id,ok=False,VERBOSE=1)
    self.assertEqual(r.stdout.strip(),'-11',r.stderr);self.assertIn('the folder PSPDX runs from',r.stderr);self.assertTrue((pspdx/'EBOOT.PBP').exists());self.assertIn(app_id,self.state())
    (self.root/f'ms0:/PSP/PSPDX/INSTALLED/{app_id}.state.json').unlink()
  # Started from another folder, a record of PSP/GAME/PSPDX is an app like any other.
  self.plant_record('io.github.someone.thing','https://github.com/someone/thing','PSP/GAME/PSPDX')
  self.assertEqual(self.run_client('uninstall','io.github.someone.thing',DEVICE='ms0:/PSP/GAME/PSPDX2/EBOOT.PBP').stdout.strip(),'0');self.assertFalse(pspdx.exists())
 def test_the_record_pspdx_0_5_kept_of_itself_is_retired(self):
  legacy='io.github.chriopter.pspdx';self.game('PSPDX').mkdir(parents=True)
  self.plant_record(legacy,'https://github.com/chriopter/pspdx','PSP/GAME/PSPDX',dict(schema=SCHEMA,source='https://github.com/chriopter/pspdx',name='PSPDX',installdir='PSP/GAME/PSPDX'))
  # Not while a transaction is open, not for another folder than the running one, and never the folder itself.
  self.write('ms0:/PSP/PSPDX/TMP/transaction.json',{'broken':1});self.assertEqual(self.run_client('retire',ok=False).stdout.strip(),'0');self.journal_path().unlink()
  self.assertEqual(self.run_client('retire',DEVICE='ms0:/PSP/GAME/Other/EBOOT.PBP').stdout.strip(),'0');self.assertIn(legacy,self.state())
  self.assertEqual(self.run_client('retire').stdout.strip(),'1');self.assertNotIn(legacy,self.state())
  self.assertFalse((self.root/f'ms0:/PSP/PSPDX/INSTALLED/{legacy}.pspdx').exists());self.assertTrue(self.game('PSPDX').exists())
  self.assertEqual(self.run_client('retire').stdout.strip(),'0')
 def update_fixture(self,moved=False):
  # Version 1 with a file the next one drops, a save and a folder the user added; version 2 with a file of its own.
  self.zip('new.zip',{'EBOOT.PBP':b'old package','data.txt':b'old data','gone.txt':b'only in 1','lib/a.prx':b'old prx'})
  self.run_client('install',VERSION=1);d=self.game('Demo')
  (d/'save.dat').write_text('mine');(d/'SAVES').mkdir();(d/'SAVES/slot1').write_text('slot');(d/'lib/user.cfg').write_text('cfg')
  self.zip('new.zip',{'EBOOT.PBP':b'new package','data.txt':b'new data','lib/a.prx':b'new prx','added/new.txt':b'added'})
  if moved:self.write('manifest.json',dict(SPEC,installdir='PSP/GAME/Moved'))
 def assert_whole(self,where,label):
  # One version or the other in every file it ships, the user's files untouched, and nothing of PSPDX's left beside them.
  state=self.state()[ID];version=state['installed']['version'];folder=self.root/'ms0:'/state['installed']['installdir']
  self.assertEqual(self.ours_left(folder),[],label);self.assertFalse(self.journal_path().exists(),label)
  expect={'1':{'EBOOT.PBP':b'old package','data.txt':b'old data','lib/a.prx':b'old prx'},'2':{'EBOOT.PBP':b'new package','data.txt':b'new data','lib/a.prx':b'new prx','added/new.txt':b'added'}}[version]
  for name,data in expect.items():self.assertEqual((folder/name).read_bytes(),data,(label,version,name))
  if version=='1':self.assertFalse((folder/'added/new.txt').exists(),label)
  self.assertEqual(((folder/'save.dat').read_text(),(folder/'SAVES/slot1').read_text(),(folder/'lib/user.cfg').read_text(),(folder/'gone.txt').read_bytes()),('mine','slot','cfg',b'only in 1'),label)
  self.assertEqual([p.name for p in self.game().iterdir() if p.name not in ('Demo','Moved','PSPDX')],[],label)
  self.assertFalse((self.game('Demo') if folder.name=='Moved' else self.game('Moved')).exists(),label)
  return version
 def test_an_update_keeps_the_files_the_release_does_not_ship(self):
  for moved in (False,True):
   with self.subTest(moved=moved):
    shutil.rmtree(self.root/'ms0:');(self.root/'ms0:').mkdir();self.write('manifest.json',SPEC)
    self.update_fixture(moved);self.run_client('install');self.assertEqual(self.assert_whole(self.root,'done'),'2')
    self.assertEqual(self.state()[ID]['installed']['installdir'],'PSP/GAME/Moved' if moved else 'PSP/GAME/Demo')
    # Delete still takes the whole folder, the user's files with it.
    self.run_client('remove');self.assertFalse(self.game('Moved' if moved else 'Demo').exists())
 def test_power_cuts_and_failures_during_an_update_leave_one_version_and_the_users_files(self):
  for moved in (False,True):
   shutil.rmtree(self.root/'ms0:');(self.root/'ms0:').mkdir();self.write('manifest.json',SPEC)
   self.update_fixture(moved);base=self.root/('base%d'%moved);shutil.rmtree(base,ignore_errors=True);shutil.copytree(self.root/'ms0:',base)
   seen=set()
   for kind in ('cut','fail'):
    for step in range(1,200):
     shutil.rmtree(self.root/'ms0:');shutil.copytree(base,self.root/'ms0:');(self.root/'failat.log').unlink(missing_ok=True)
     r=self.run_client('install',*((step,) if kind=='cut' else ()),ok=False,**({} if kind=='cut' else dict(FAILAT=step)))
     self.assertIn(r.returncode,[0,1,77],(moved,kind,step,r.stderr[-400:]))
     phase=json.loads(self.journal_path().read_text()).get('phase') if self.journal_path().exists() else None
     if kind=='cut' and phase in ('placed','restoring','committed'):
      # The recovery itself cut short, again and again, before one gets through.
      snapshot=self.root/'cut';shutil.rmtree(snapshot,ignore_errors=True);shutil.copytree(self.root/'ms0:',snapshot)
      for again in range(1,80):
       shutil.rmtree(self.root/'ms0:');shutil.copytree(snapshot,self.root/'ms0:')
       if self.run_client('recover',again,ok=False).returncode==0:break
       self.run_client('recover');self.assert_whole(self.root,(moved,kind,step,'recovery cut',again))
      shutil.rmtree(self.root/'ms0:');shutil.copytree(snapshot,self.root/'ms0:')
     self.run_client('recover');seen.add(self.assert_whole(self.root,(moved,kind,step,phase)))
     if r.returncode==0:break
    else:self.fail('the sweep did not reach the end: %s %s'%(moved,kind))
   self.assertEqual(seen,{'1','2'},moved)
 # --- what text and hashes are held to (0.7 megatest) ---
 def test_a_catalog_written_with_crlf_and_tabs_is_read_as_meant(self):
  # sharkwouter's list as a Windows editor would save it: the apps stay, and what they install from is v1.
  root=pathlib.Path(__file__).resolve().parents[2];wouter=json.loads((root/'dev/testdata/wijsman-catalog-2026-09-15.json').read_text())
  for app in wouter['apps']:app['description']=app.get('description','Game').replace('\n','\r\n')+'\r\n\tEnd\rOf text';app['summary']='Fast\tand\r\nfun'
  self.write('catalog.json',dict(wouter,generated_at=NOW()));(self.root/'ms0:/PSP/PSPDX').mkdir(parents=True);(self.root/'ms0:/PSP/PSPDX/sources.txt').write_text(PRESETS[1]+'\n')
  r=self.run_client('fetch',VERBOSE=1);self.assertEqual(len(r.stdout.splitlines()),3,r.stderr);self.assertNotIn('breaks the .pspdx rules',r.stderr)
  catalog=self.from_the_entry();catalog['apps'][0].update(summary='Tab\there\r\n',description='One\r\nTwo\rThree\tfour');self.write('catalog.json',catalog)
  r=self.run_client('get',ID,VERBOSE=1);self.assertEqual(r.stdout.strip(),'0',r.stderr)
  self.assertEqual((self.saved()['summary'],self.saved()['description']),('Tab here ','One\nTwo\nThree four'))
  # The file itself stays strict.
  self.parse(dict(SPEC,description='a\r\nb'),ok=False)
 def test_a_nul_in_text_is_no_text(self):
  # In a catalog entry the entry goes, said; an escaped backslash before the letters is text like any other.
  self.fixtures();catalog=self.catalog_app();first=catalog['apps'][0];other=self.mirror('Other')
  text=json.dumps(dict(catalog,apps=[first,other])).replace('"name": "Other"','"name": "Oth\\u0000er"')
  (self.root/'catalog.json').write_text(text);r=self.run_client('fetch',VERBOSE=1)
  self.assertEqual([l.split()[0] for l in r.stdout.splitlines()],[ID],r.stderr);self.assertIn('holds a NUL or a control character in its text; dropped',r.stderr)
  (self.root/'catalog.json').write_text(json.dumps(dict(catalog,apps=[first,dict(other,description='a \\u0000 b')])));self.assertEqual(len(self.run_client('fetch').stdout.splitlines()),2)
  # In a record the record is damaged, as any other damaged record: nothing is written over it.
  record=self.root/f'ms0:/PSP/PSPDX/INSTALLED/{ID}.state.json';good=record.read_text()
  record.write_text(good.replace('"version":"1"','"version":"1\\u0000x"'));r=self.run_client('install',ok=False,VERBOSE=1)
  self.assertNotEqual(r.returncode,0);self.assertIn('writes blocked',r.stderr);self.assertIn('\\u0000',record.read_text())
 def test_a_sha256_of_zeros_is_no_hash(self):
  # A catalog's release with one is not taken, and the repository answers; a record with one is damaged; GitHub's digest of zeros is none.
  self.fixtures();catalog=self.catalog_app();release=catalog['apps'][0]['releases'][0]
  self.write('catalog.json',dict(catalog,apps=[dict(catalog['apps'][0],releases=[dict(release,sha256='0'*64)])]));self.assertEqual(self.row(self.run_client('fetch').stdout)[1],'3')
  self.write('catalog.json',catalog);self.run_client('fetch');record=self.state()[ID]
  self.write(f'ms0:/PSP/PSPDX/INSTALLED/{ID}.state.json',dict(record,latest=dict(record['latest'],sha256='0'*64)));self.assertEqual(self.run_client('latest',ID,ok=False).stdout.strip(),'-1')
  self.write(f'ms0:/PSP/PSPDX/INSTALLED/{ID}.state.json',dict(record,installed=dict(record['installed'],sha256='0'*64)))
  self.assertIn('writes blocked',self.run_client('install',ok=False,VERBOSE=1).stderr)
  self.write(f'ms0:/PSP/PSPDX/INSTALLED/{ID}.state.json',record);self.write('catalog.json',dict(catalog,apps=[]))
  release_json=json.loads((self.root/'release.json').read_text());release_json['assets'][0]['digest']='sha256:'+'0'*64;self.write('release.json',release_json)
  self.assertEqual(self.run_client('sha',ID,FORCE=1).stdout.split()[0],'-')
 # --- one app, one row, one folder (0.7 megatest) ---
 def rows(self,*args,**env):
  r=self.run_client('rows',*args,ok=False,**env);return {l.split('|')[0]:l.split('|')[1:] for l in r.stdout.splitlines()},r
 def test_an_installed_app_keeps_its_record_id_whatever_a_list_calls_it(self):
  # Installed under a catalog's own id: another id, none, the text list's repository and a mirror's entry all list and update the one record.
  self.fixtures();self.run_client('remove');catalog=self.catalog_app();first=catalog['apps'][0];kept='de.example.demo'
  self.write('catalog.json',dict(catalog,apps=[dict(first,id=kept)]));self.assertEqual(self.run_client('get',kept).stdout.strip(),'0');self.assertEqual(list(self.state()),[kept])
  for change in (dict(id='com.other.demo'),dict(id=None)):
   with self.subTest(change=change):
    app=dict(first,**change)
    if change['id'] is None:del app['id']
    self.write('catalog.json',dict(catalog,apps=[app]));rows,r=self.rows(VERBOSE=1)
    self.assertEqual(list(rows),[kept],r.stderr);self.assertNotEqual(rows[kept][2],'1');self.assertIn('is kept on this stick as '+kept,r.stderr)
  (self.root/'catalog.txt').write_text(SPEC['source']+'\n');(self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('https://example.com/pspdx/\n')
  rows,r=self.rows(CATALOG_DOWN=1);self.assertEqual(list(rows),[kept],r.stderr)
  self.assertEqual(self.run_client('get',kept,CATALOG_DOWN=1).stdout.strip(),'0');self.assertEqual(list(self.state()),[kept])
  # Away from GitHub the same, by the source.
  (self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('https://example.com/catalog.json\n');mirror=self.mirror(id='de.wijsman.blocks')
  self.write('catalog.json',dict(catalog,apps=[mirror]));self.assertEqual(self.run_client('get','de.wijsman.blocks').stdout.strip(),'0')
  del mirror['id'];self.write('catalog.json',dict(catalog,apps=[mirror]));self.assertIn('de.wijsman.blocks',self.rows()[0])
 def test_an_installed_app_keeps_its_folder_when_a_list_names_another(self):
  # A catalog names another folder for the installed demo: the row and the update stay in Demo. Not installed, the repository's own file wins over the entry's folder.
  self.fixtures();catalog=self.catalog_app();catalog['apps'][0]['installdir']='PSP/GAME/Elsewhere';self.write('catalog.json',catalog)
  self.assertEqual(self.rows()[0][ID][1],'Demo');r=self.run_client('get',ID,VERBOSE=1);self.assertEqual(r.stdout.strip(),'0',r.stderr)
  self.assertEqual(self.state()[ID]['installed']['installdir'],'PSP/GAME/Demo');self.assertFalse(self.game('Elsewhere').exists())
  self.run_client('remove');r=self.run_client('get',ID,VERBOSE=1);self.assertEqual(r.stdout.strip(),'0',r.stderr);self.assertTrue(self.game('Demo','EBOOT.PBP').exists());self.assertFalse(self.game('Elsewhere').exists())
 def test_a_repository_listed_under_two_ids_is_one_row(self):
  # Not installed, so no record folds the second into the first: two rows would offer the same app twice, and the second install fail.
  self.fixtures();self.run_client('remove');catalog=self.catalog_app();first=catalog['apps'][0]
  self.write('catalog.json',dict(catalog,apps=[first,dict(first,id='de.example.demo',name='Mirror',installdir='PSP/GAME/Other')]))
  r=self.run_client('fetch',VERBOSE=1);self.assertEqual([l.split()[0] for l in r.stdout.splitlines()],[ID],r.stderr);self.assertIn('listed already; not listed twice',r.stderr)
 def test_a_text_list_that_answered_is_not_unreachable(self):
  # Its repositories listed by a source before it, or every one of them refused at GitHub: the list is there either way.
  self.fixtures();shutil.copy(self.root/'catalog.json',self.root/'second.json');(self.root/'catalog.txt').write_text(SPEC['source']+'\n')
  (self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('https://example.com/second/catalog.json\nhttps://example.com/list/\n')
  self.assertIn('https://example.com/list/ ok',self.run_client('reach',SECOND_CATALOG='second.json',CATALOG_DOWN=1).stdout.splitlines())
  (self.root/'catalog.txt').write_text('https://github.com/test/nothing\n');(self.root/'map').write_text('test/nothing/HEAD/.pspdx -\n')
  (self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('https://example.com/list/\n');shutil.rmtree(self.root/'ms0:/PSP/PSPDX/CACHE')
  self.assertIn('https://example.com/list/ ok',self.run_client('reach',CATALOG_DOWN=1,URL_MAP=self.root/'map').stdout.splitlines())
 def test_the_newest_pspdx_is_kept_and_only_its_author_moves_the_folder(self):
  self.fixtures();(self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('')
  # A forced check hears a new name, and one that does not ask says the same after it.
  self.write('manifest.json',dict(SPEC,name='Demo Two'))
  for env in (dict(FORCE=1),{}):
   with self.subTest(env=env):self.assertEqual(self.rows(**env)[0][ID][:2],['Demo Two','Demo'])
  self.assertEqual(self.saved()['name'],'Demo Two')
  # The user renames the folder, and the record with it: the update goes there, and Demo is not made again.
  record=self.state()[ID];self.assertEqual(record['installed']['pspdx_installdir'],'PSP/GAME/Demo')
  self.game('Demo').rename(self.game('Mine'));(self.game('Mine')/'save.dat').write_text('mine')
  record['installed']['installdir']='PSP/GAME/Mine';self.write(f'ms0:/PSP/PSPDX/INSTALLED/{ID}.state.json',record)
  self.assertEqual(self.rows(FORCE=1)[0][ID][1],'Mine');r=self.run_client('get',ID,VERBOSE=1);self.assertEqual(r.stdout.strip(),'0',r.stderr)
  self.assertEqual(self.state()[ID]['installed']['installdir'],'PSP/GAME/Mine');self.assertFalse(self.game('Demo').exists());self.assertEqual((self.game('Mine')/'save.dat').read_text(),'mine')
  # The author names another folder: then it moves, with what the user keeps in it. A record from before the field was kept is told by its saved file,
  # even once a check has saved the author's newer one.
  record=self.state()[ID];del record['installed']['pspdx_installdir'];self.write(f'ms0:/PSP/PSPDX/INSTALLED/{ID}.state.json',record)
  self.write('manifest.json',dict(SPEC,name='Demo Two',installdir='PSP/GAME/Moved'))
  self.assertEqual(self.rows(FORCE=1)[0][ID][1],'Moved');self.assertEqual(self.saved()['installdir'],'PSP/GAME/Moved');self.assertEqual(self.state()[ID]['installed']['pspdx_installdir'],'PSP/GAME/Demo')
  self.assertEqual(self.rows()[0][ID][1],'Moved');r=self.run_client('get',ID,VERBOSE=1);self.assertEqual(r.stdout.strip(),'0',r.stderr)
  self.assertEqual(self.state()[ID]['installed']['installdir'],'PSP/GAME/Moved');self.assertEqual((self.game('Moved')/'save.dat').read_text(),'mine');self.assertFalse(self.game('Mine').exists())
 # --- what GitHub answers, and when it does not (0.7 megatest) ---
 def test_a_check_stamped_in_the_future_is_due(self):
  self.fixtures();(self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('');self.run_client('fetch',FORCE=1);record=self.state()[ID]
  record['latest']['checked_at']=int(time.time())+400*86400;self.write(f'ms0:/PSP/PSPDX/INSTALLED/{ID}.state.json',record);(self.root/'requests.log').write_text('')
  self.run_client('fetch');self.assertIn('api.github.com',(self.root/'requests.log').read_text())
  (self.root/'requests.log').write_text('');self.run_client('fetch');self.assertNotIn('api.github.com',(self.root/'requests.log').read_text())
 def test_a_pinned_release_url_that_is_no_zip_is_refused_before_the_download(self):
  base='https://github.com/test/demo/releases/download/v2/'
  self.write('release-tag.json',self.release_json('v2','download.zip','notes.txt'))
  self.direct(dict(SPEC,release=dict(tag='v2',url=base+'notes.txt')));self.forget_latest()
  r=self.run_client('release',ID,ok=False,VERBOSE=1);self.assertNotIn('releases/download',r.stdout);self.assertIn('names no .zip',r.stderr)
  self.assertNotIn('notes.txt',(self.root/'requests.log').read_text().replace('releases/tags/v2',''))
 def test_a_text_that_trickles_is_given_up_after_two_minutes(self):
  # The clock moves 200 s a read: the catalog is given up, the saved one stands; a ZIP has no such limit.
  self.fixtures();self.run_client('fetch');r=self.run_client('fetch',VERBOSE=1,CLOCK_STEP_MS=200000)
  self.assertIn('took more than 120 s; given up',r.stderr);self.assertIn(ID,r.stdout)
  self.assertEqual(self.run_client('install',CLOCK_STEP_MS=200000).stdout.split()[0],'0')
 def test_too_large_is_said_for_a_catalog_directory(self):
  # The presets name a catalog by its directory, so catalog.txt is asked for after it, and a second source answers nothing: the status line still says the size was why.
  self.fixtures();catalog=json.loads((self.root/'catalog.json').read_text())
  for f in (self.root/'ms0:/PSP/PSPDX/INSTALLED').glob('*'):f.unlink()
  self.write('catalog.json',dict(catalog,pad='x'*4200000))
  (self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('https://example.com/\nhttps://other.example/catalog.json\n')
  r=self.run_client('fetch',VERBOSE=1,ok=False,DOWN_HOST='other.example');self.assertIn('live catalog too large',r.stderr);self.assertIn('status: too large',r.stderr,r.stderr[-600:])
 def test_github_not_answering_is_no_missing_release_and_waits_six_hours(self):
  self.fixtures();(self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('');(self.root/'map').write_text('api.github.com - 403\n');before=self.state()[ID]['installed']
  r=self.run_client('fetch',FORCE=1,VERBOSE=1,URL_MAP=self.root/'map');self.assertIn('GitHub did not answer',r.stderr);self.assertNotIn('has no release',r.stderr)
  self.assertEqual(self.state()[ID]['installed'],before);self.assertEqual(self.state()[ID]['latest']['checked_from'],SPEC['source'])
  (self.root/'requests.log').write_text('');self.run_client('fetch',URL_MAP=self.root/'map');self.assertNotIn('api.github.com',(self.root/'requests.log').read_text())
  # Direct Install says so, as it says a repository with no release.
  (self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('');r=self.run_client('add','test/demo',ok=False,URL_MAP=self.root/'map');self.assertIn('refused -5',r.stderr)
  (self.root/'map').write_text('api.github.com - 404\n');r=self.run_client('add','test/demo',ok=False,URL_MAP=self.root/'map');self.assertIn('refused -2',r.stderr)
 def test_a_404_from_another_host_is_not_a_missing_pspdx(self):
  # The .pspdx request ends somewhere else after a redirect and hears 404 there: that is no answer about the repository.
  catalog=self.from_the_entry();(self.root/'map').write_text('/HEAD/.pspdx - 404 portal.example.net\n')
  r=self.run_client('get',ID,ok=False,VERBOSE=1,URL_MAP=self.root/'map');self.assertNotEqual(r.stdout.strip(),'0');self.assertIn('did not come (status 404 from portal.example.net)',r.stderr)
  self.assertNotIn('installed from the catalog entry',r.stderr)
  r=self.run_client('fetch',FORCE=1,CATALOG_DOWN=1,VERBOSE=1,URL_MAP=self.root/'map');self.assertNotIn('has no .pspdx',r.stderr);self.assertNotIn('api.github.com',(self.root/'requests.log').read_text())
 def test_a_repository_typed_without_a_pspdx_installs_from_its_release(self):
  self.fixtures();self.run_client('remove');(self.root/'manifest.json').unlink();(self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('')
  r=self.run_client('add','test/demo',VERBOSE=1);self.assertIn('made an app of its own name',r.stderr)
  rows,r=self.rows(VERBOSE=1);self.assertEqual(rows[ID][:2],['demo','demo'],r.stderr)
  r=self.run_client('get',ID,VERBOSE=1);self.assertEqual(r.stdout.strip(),'0',r.stderr);self.assertTrue(self.game('demo','EBOOT.PBP').exists())
  self.assertEqual(self.saved(),dict(schema=SCHEMA,source=SPEC['source'],name='demo'))
  # A list's line is not the user's word: without a .pspdx it stays out.
  (self.root/'catalog.txt').write_text('https://github.com/test/other\n');(self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('https://example.com/list/\n')
  self.assertNotIn('io.github.test.other',self.rows(CATALOG_DOWN=1)[0])
 def test_a_catalog_from_its_saved_copy_is_said_to_be_one(self):
  self.fixtures();self.run_client('fetch')
  self.assertIn('https://example.com/catalog.json offline copy',self.run_client('reach',CATALOG_DOWN=1).stdout.splitlines())
  self.assertIn('https://example.com/catalog.json ok',self.run_client('reach').stdout.splitlines())
 # --- INBOX and Direct Install (0.7 megatest) ---
 def test_an_inbox_file_for_a_repository_without_a_pspdx_is_the_app(self):
  # The repository answers 404: the user's file installs, is kept, and the release is asked at GitHub by it, now and at a check with no catalog.
  self.fixtures();self.run_client('remove');catalog=self.catalog_app();(self.root/'manifest.json').unlink();sources=self.root/'ms0:/PSP/PSPDX/sources.txt';sources.write_text('')
  mine=dict(SPEC,summary='Mine',tags=['mine']);inbox=self.root/'ms0:/PSP/PSPDX/INBOX/demo.pspdx';self.write('ms0:/PSP/PSPDX/INBOX/demo.pspdx',mine)
  r=self.run_client('inbox',VERBOSE=1);self.assertEqual(r.stdout.strip(),'1',r.stderr);self.assertIn('the repository has no .pspdx; installed from your file',r.stderr)
  r=self.run_client('inboxinstall',VERBOSE=1);self.assertEqual(self.saved(),mine,r.stderr);self.assertFalse(inbox.exists());self.assertEqual(self.state()[ID]['installed']['version'],'3')
  (self.root/'requests.log').write_text('');rows,r=self.rows(FORCE=1,VERBOSE=1);self.assertEqual(rows[ID][2],'2',r.stderr);self.assertIn('https://api.github.com/repos/test/demo/releases/latest',(self.root/'requests.log').read_text())
  self.assertEqual(self.saved(),mine)
  # A catalog lists it as well: where the repository has none, the file is still what installs; a .pspdx of its own wins over the file.
  for own,saved in ((None,mine),(SPEC,SPEC)):
   with self.subTest(own=own is not None):
    self.run_client('remove');sources.write_text('https://example.com/catalog.json\n');self.write('catalog.json',catalog);self.write('ms0:/PSP/PSPDX/INBOX/demo.pspdx',mine)
    if own:self.write('manifest.json',own)
    r=self.run_client('inboxinstall',VERBOSE=1,FETCH_FIRST=1);self.assertEqual(self.saved(),saved,r.stderr);self.assertFalse(inbox.exists())
    self.assertIn('installed instead of the file' if own else 'installed from your file',r.stderr)
 def test_inbox_places_go_only_to_files_that_install(self):
  # Seventy files for repositories that are gone, and one that installs: the one is queued, whichever order the stick lists them in.
  self.fixtures();(self.root/'map').write_text('test/gone - 404\n')
  for i in range(70):self.write('ms0:/PSP/PSPDX/INBOX/gone%02d.pspdx'%i,dict(SPEC,source='https://github.com/test/gone%d'%i,installdir='PSP/GAME/Gone%d'%i))
  self.write('ms0:/PSP/PSPDX/INBOX/zz-demo.pspdx',SPEC)
  r=self.run_client('inbox',VERBOSE=1,URL_MAP=self.root/'map');self.assertEqual(r.stdout.strip(),'1',r.stderr[-800:]);self.assertNotIn('capacity',r.stderr)
 # --- what the screen shows (0.7 megatest) ---
 def test_the_same_version_over_another_zip_is_a_new_build(self):
  self.fixtures();catalog=self.catalog_app();release=catalog['apps'][0]['releases'][0]
  for tag,sha,new_build in (('v1','0'*63+'1','1'),('v2','0'*63+'1','0')):
   with self.subTest(tag=tag):self.write('catalog.json',dict(catalog,apps=[dict(catalog['apps'][0],releases=[dict(release,tag=tag,sha256=sha)])]));self.assertEqual(self.rows()[0][ID][2:],['3',new_build])
 def png(self,name,w,h,row,kind=6,interlace=0,cut=0,palette=b''):
  # A PNG of rows the caller makes, 8 bits a sample; cut takes that many bytes off the end of its image data.
  import struct,zlib
  chunk=lambda t,d:struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d))
  body=zlib.compress(b''.join(b'\0'+row(y) for y in range(h)))
  (self.root/name).write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,kind,0,0,interlace))+(chunk(b'PLTE',palette) if palette else b'')+chunk(b'IDAT',body[:len(body)-cut])+chunk(b'IEND',b''))
  return name
 def decode(self,*names):
  if not IMAGE_BIN:self.skipTest('no image decoder built')
  r=subprocess.run([IMAGE_BIN,*names],cwd=self.root,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1'),capture_output=True,text=True)
  self.assertEqual(r.returncode,0,r.stderr[-2000:]);self.assertNotIn('Sanitizer',r.stderr);self.assertNotIn('runtime error:',r.stderr);return r.stdout.splitlines()
 def test_a_picture_larger_than_a_texture_is_scaled_down_to_fit(self):
  halves=lambda w,left,right:lambda y:left*(w//2)+right*(w-w//2)
  cases=[(self.png('screenshot.png',640,480,halves(640,b'\xff\0\0',b'\0\0\xff'),kind=2),'0 256x192 256x256 255,0,0,255 0,0,255,255'),
         (self.png('wide.png',2048,100,halves(2048,b'\0\xff\0\0',b'\x0a\x14\x1e\xff')),'0 256x13 256x16 0,0,0,0 10,20,30,255'),
         (self.png('tall.png',100,2048,lambda y:b'\x80'*100,kind=0),'0 13x256 16x256 128,128,128,255 128,128,128,255'),
         # A transparent pixel does not whiten the red beside it: the colour is weighted by alpha, the alpha averaged.
         (self.png('edge.png',1024,2,lambda y:b'\xff\xff\xff\0\xc8\0\0\xff'*512),'0 256x1 256x1 200,0,0,128 200,0,0,128'),
         (self.png('exact.png',512,512,lambda y:b'\x01\x02\x03\x04'*512),'0 256x256 256x256 1,2,3,4 1,2,3,4'),
         (self.png('palette.png',480,272,lambda y:b'\0'*240+b'\1'*240,kind=3,palette=b'\x11\x22\x33\x44\x55\x66'),'0 256x145 256x256 17,34,51,255 68,85,102,255'),
         (self.png('huge.png',4097,1,lambda y:b'\0\0\0\0'*4097),'-1'),
         (self.png('interlaced.png',600,600,lambda y:b'\0\0\0\0'*600,interlace=1),'-1'),
         # Cut short part way through the rows, scaled or not: nothing it had allocated is left behind.
         (self.png('cut-scaled.png',640,480,lambda y:os.urandom(1920),kind=2,cut=4000),'-1'),
         (self.png('cut.png',480,272,lambda y:os.urandom(1920),cut=4000),'-1')]
  self.assertEqual(self.decode(*[name for name,_ in cases]),[line for _,line in cases])
if __name__=='__main__':unittest.main()

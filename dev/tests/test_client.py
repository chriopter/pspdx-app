"""Run actual client parsers, persistence and installer against a PSP I/O adapter.
The adapter preserves same-directory rename semantics and supports power cuts.
"""
import gzip, hashlib, json, os, pathlib, shutil, subprocess, tempfile, time, unittest, zipfile
BIN=os.environ.get('PSPDX_TEST_BIN','/tmp/pspdx-host-test')
IMAGE_BIN=os.environ.get('PSPDX_IMAGE_BIN')
SCHEMA='https://chriopter.github.io/pspdx/schema/pspdx-v1.json'
ID='io.github.test.demo'
PRESETS=['https://chriopter.github.io/pspdx-catalog/','https://wijsman.de/psp-homebrew-database/']
SPEC=dict(schema=SCHEMA,source='https://github.com/test/demo',name='Demo',tags=['demo'],installdir='PSP/GAME/Demo',author='test',summary='Demo',license='MIT')
# A catalog stamped now: one a day old is asked about at the origin, which is another test.
NOW=lambda:time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())
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
  self.write('catalog.json',dict(catalog,pad='x'*530000));r=self.run_client('fetch',VERBOSE=1);self.assertIn('larger than',r.stderr);self.assertNotIn('unreachable',r.stderr);self.assertEqual(cache.read_bytes(),saved)
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
  # One app, installed with a newer one published: gear, stick, All and its category; the basket's tab appears with the first package set aside and goes with it, and its going is what the caller is told.
  self.fixtures();r=self.run_client('view')
  self.assertEqual(r.stdout.splitlines(),['tabs 4: -3 -2 0 2','tab 0 kind 0 rows 1 first 0 plan 0 0','tab 2 kind 0 rows 1 first 0 plan 0 0','tab -3 kind 3 rows 5 first -100 plan 0 0','tab -2 kind 1 rows 2 first -2 plan 1 1','basket 1 kept 1 tabs 5 moved 0','basket tab rows 2 first -2 index 0 row 1','emptied kept 0 kind 0 tabs 4 moved 1'],r.stderr)
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
  p=self.root/'ms0:/PSP/GAME/PSPDX/presets.txt';p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text)
 def listed(self):return [l for l in (self.root/'ms0:/PSP/PSPDX/sources.txt').read_text().splitlines() if l and not l.startswith('#')]
 def seen_presets(self):return [l for l in (self.root/'ms0:/PSP/PSPDX/presets.seen').read_text().splitlines() if l and not l.startswith('#')]
 def test_presets_arrive_once_and_stay_removed(self):
  # A fresh stick gets the shipped list in its order, and the next start changes nothing.
  root=pathlib.Path(__file__).resolve().parents[2];self.plant_presets((root/'app/presets.txt').read_text())
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
  root=pathlib.Path(__file__).resolve().parents[2];self.plant_presets((root/'app/presets.txt').read_text())
  self.run_client('presets');self.run_client('presets')
  self.assertEqual(path.read_text(),'# mine\nhttps://Chriopter.github.io/pspdx-catalog\nhttps://example.com/catalog.json\n'+PRESETS[1]+'\n')
  self.assertEqual(self.seen_presets(),PRESETS)
 def test_presets_without_the_file_or_with_bad_lines(self):
  path=self.root/'ms0:/PSP/PSPDX/sources.txt';path.parent.mkdir(parents=True,exist_ok=True);path.write_text('https://example.com/catalog.json\n')
  r=self.run_client('presets',VERBOSE=1);self.assertIn('built-in list stands',r.stderr)
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
  r=self.run_client('fetch',DOWN_HOST='wijsman.de');self.assertIn(ID,r.stdout)
  requests=(self.root/'requests.log').read_text();self.assertIn(PRESETS[1]+'catalog.json',requests);self.assertIn(PRESETS[1]+'catalog.txt',requests)
  lines=self.run_client('reach',DOWN_HOST='wijsman.de').stdout.splitlines()
  self.assertIn(PRESETS[0]+' ok',lines);self.assertIn(PRESETS[1]+' unreachable',lines)
 def test_a_source_that_does_not_load_is_marked_and_the_rest_still_load(self):
  # The second source does not answer: its apps are missing, the first's are there, and only the second is marked. The next fetch starts over.
  self.fixtures();down='https://down.example.org/pspdx/'
  (self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('https://example.com/catalog.json\n'+down+'\n')
  r=self.run_client('reach',DOWN_HOST='down.example.org',THEN_UP=1);first,again=r.stdout.split('--\n')
  self.assertEqual(first.splitlines(),[ID,'https://example.com/catalog.json ok',down+' unreachable'])
  self.assertEqual(again.splitlines(),[ID,'https://example.com/catalog.json ok',down+' ok'])
 def test_embedded_presets_match_the_file(self):
  import re
  root=pathlib.Path(__file__).resolve().parents[2]
  header=(root/'app/util/self_presets.h').read_text().split('#define',1)[1]
  embedded=''.join(re.findall(r'"((?:[^"\\]|\\.)*)"',header)).replace('\\n','\n').splitlines()
  shipped=[l.split('#')[0].strip() for l in (root/'app/presets.txt').read_text().splitlines() if l.split('#')[0].strip()]
  self.assertEqual(embedded,shipped);self.assertEqual(shipped,PRESETS)
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
 def test_plugins_are_listed_and_not_installed(self):
  plug={k:v for k,v in dict(SPEC,type='plugin').items() if k!='installdir'};self.write('manifest.json',plug)
  r=self.run_client('install',ok=False,VERBOSE=1);self.assertIn('type plugin cannot be installed yet',r.stderr);self.assertFalse((self.root/'ms0:/PSP/GAME/Demo').exists())
  self.write('manifest.json',SPEC);self.fixtures();catalog=json.loads((self.root/'catalog.json').read_text());first=catalog['apps'][0]
  plugin={k:v for k,v in dict(first,id='io.github.test.plug',name='Plug',type='plugin',tags=['plugin'],source='https://github.com/test/plug',releases=[dict(first['releases'][0],url='https://github.com/test/plug/releases/download/v2/download.zip')]).items() if k!='installdir'}
  mirror={k:v for k,v in dict(first,id='de.wijsman.blocks',name='Blocks',source='https://archive.org/details/psp-blocks',releases=[dict(first['releases'][0],url='https://archive.org/download/psp-blocks/blocks.zip')]).items() if k!='installdir'}
  self.write('catalog.json',dict(catalog,apps=[first,plugin,mirror]))
  r=self.run_client('fetch',VERBOSE=1);rows={row.split()[0]:row.split() for row in r.stdout.splitlines()}
  # A mirror installs from its entry; a plugin, from anywhere, is listed and not installed.
  self.assertEqual((rows[ID][4],rows['io.github.test.plug'][4],rows['de.wijsman.blocks'][4]),('0','1','0'),r.stdout)
  r=self.run_client('prepare','io.github.test.plug',ok=False,VERBOSE=1);self.assertEqual(r.stdout.strip(),'-1');self.assertIn('cannot be installed yet',r.stderr)
  # A plugin carrying an installdir is not listed at all; a mirror the list calls a word is listed as its source's host and name make it, and one it gives an id keeps that id.
  self.write('catalog.json',dict(catalog,apps=[first,dict(plugin,installdir='PSP/GAME/Plug'),dict(mirror,id='blocks')]))
  self.assertEqual([row.split()[0] for row in self.run_client('fetch').stdout.splitlines()],[ID,'org.archive.blocks'])
  self.write('catalog.json',dict(catalog,apps=[first,dict(mirror,id='de.wijsman.other')]))
  self.assertEqual([row.split()[0] for row in self.run_client('fetch').stdout.splitlines()],[ID,'de.wijsman.other'])
  # The origin path skips a .pspdx whose source is not GitHub, and INBOX keeps a plugin for a later version.
  (self.root/'catalog.txt').write_text(SPEC['source']+'\n');(self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('https://example.com/pspdx/\n')
  self.write('manifest.json',dict(SPEC,source='https://archive.org/details/psp-blocks'))
  r=self.run_client('fetch',CATALOG_DOWN=1,FORCE=1,VERBOSE=1);self.assertIn('outside GitHub',r.stderr)
  self.write('ms0:/PSP/PSPDX/INBOX/plug.pspdx',plug);r=self.run_client('inbox',VERBOSE=1);self.assertEqual(r.stdout.strip(),'0');self.assertIn('cannot install yet; kept',r.stderr)
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
 def test_tabs_come_from_known_tags_and_the_plugin_type(self):
  # A category decides the one tab; without one, the tags do; a category no tab has leaves All alone; the plugins are the type's.
  self.fixtures();catalog=json.loads((self.root/'catalog.json').read_text());app=catalog['apps'][0]
  for tags,kind,tabs,category in ((['Jeu','games','Game'],None,'-3 -2 0',None),([],None,'-3 -2 0',None),(None,None,'-3 -2 0',None),(['game','demo','emulator'],None,'-3 -2 0 1 2 4',None),(['plugin'],None,'-3 -2 0',None),(['game'],'plugin','-3 -2 0 1 5',None),
                                  (['puzzle'],None,'-3 -2 0 1','game'),(['game','demo'],None,'-3 -2 0 4','emulator'),(['game'],None,'-3 -2 0','Puzzle'),(['game'],None,'-3 -2 0','Game'),(['demo'],'plugin','-3 -2 0 3 5','app')):
   with self.subTest(tags=tags,kind=kind,category=category):
    a={k:v for k,v in app.items() if k!='tags'}
    if tags is not None:a['tags']=tags
    if category is not None:a['category']=category
    if kind:a['type']=kind;a.pop('installdir')
    self.write('catalog.json',dict(catalog,apps=[a]))
    self.assertEqual(self.run_client('view').stdout.splitlines()[0],'tabs %d: %s'%(len(tabs.split()),tabs))
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
  catalog=json.loads((self.root/'catalog.json').read_text());self.gzip_catalog(dict(catalog,pad='x'*530000))
  r=self.run_client('fetch',VERBOSE=1);self.assertIn('larger than',r.stderr);self.assertNotIn('unreachable',r.stderr);self.assertEqual(self.saved_catalog().read_bytes(),saved)
  # The room is counted in inflated text, to the byte: all of the buffer but its terminator fits, one more does not.
  room=512*1024-1;base=len(json.dumps(dict(catalog,pad='')))
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
  r=self.run_client('fetch',VERBOSE=1);self.assertEqual([l.split()[0] for l in r.stdout.splitlines()],[ID,'org.archive.fine'],r.stderr)
  for field in ('summary','name','tags','author','category','description','license'):self.assertIn('breaks the .pspdx rules (%s); dropped'%field,r.stderr)
  self.assertIn('io.github.test.other breaks the .pspdx rules (summary)',r.stderr)
  self.assertIn('which only a GitHub repository has; refused',r.stderr);self.assertIn('Pages is from outside GitHub and its source\'s host and name make no id; dropped',r.stderr)
  self.assertEqual(self.run_client('get','org.archive.fine',VERBOSE=1).stdout.strip(),'0')
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
 def test_a_stick_at_0_6_sees_0_7_as_an_update_of_itself(self):
  # PSPDX's own record comes from its first start: rev 0 and the version built in. The same version is the release and notes its rev; another is an update; once noted, another zip is one too.
  (self.root/'ms0:/PSP/PSPDX').mkdir(parents=True);(self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('https://example.com/catalog.json\n')
  self_id='io.github.chriopter.pspdxapp';source='https://github.com/chriopter/pspdx-app'
  release=lambda tag,day,sha:dict(tag=tag,published_at='2026-09-%02dT08:51:07Z'%day,size=3000000,sha256=sha,url=source+'/releases/download/%s/pspdx.zip'%tag)
  entry=lambda *releases:dict(generated_at=NOW(),apps=[dict(id=self_id,name='PSPDX',author='chriopter',source=source,installdir='PSP/GAME/PSPDX',category='app',releases=list(releases))])
  record=lambda version,rev:self.write(f'ms0:/PSP/PSPDX/INSTALLED/{self_id}.state.json',dict(source=source,installed=dict(installdir='PSP/GAME/PSPDX',version=version,published_at=rev)))
  (a,b)=('62'*32,'7a'*32)
  for version,state in [('0.6','3'),('0.7','2'),('0.7-3-gabc1234','2'),('0.70','3'),('dev','3')]:
   with self.subTest(version=version):
    record(version,0);self.write('catalog.json',entry(release('v0.7',15,b),release('v0.6',14,a)));self.assertEqual(self.row(self.run_client('fetch').stdout,self_id)[1:4],['0.7','1',state])
    self.assertEqual(self.state()[self_id]['installed']['published_at']!=0,state=='2')
  # A 0.6 that has already noted its release's rev: 0.7 is an update, 0.6 itself is not.
  record('0.6',0);self.write('catalog.json',entry(release('v0.6',14,a)));self.assertEqual(self.row(self.run_client('fetch').stdout,self_id)[3],'2');self.assertNotEqual(self.state()[self_id]['installed']['published_at'],0)
  self.write('catalog.json',entry(release('v0.7',15,b),release('v0.6',14,a)));self.assertEqual(self.row(self.run_client('fetch').stdout,self_id)[1:4],['0.7','1','3'])
 # --- installs and removals the megatest broke (0.7) ---
 def game(self,*parts):return self.root.joinpath('ms0:/PSP/GAME',*parts)
 def journal_path(self):return self.root/'ms0:/PSP/PSPDX/TMP/transaction.json'
 def ours_left(self,folder):return sorted(str(p.relative_to(folder)) for p in folder.rglob('*') if p.name.endswith(('.pspdx-new','.pspdx-old')))
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
  self.write('catalog.json',dict(catalog,pad='x'*530000))
  (self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('https://example.com/\nhttps://other.example/catalog.json\n')
  r=self.run_client('fetch',VERBOSE=1,ok=False,DOWN_HOST='other.example');self.assertIn('live catalog too large',r.stderr);self.assertIn('status: too large',r.stderr,r.stderr[-600:])
 def test_github_not_answering_is_no_missing_release_and_waits_six_hours(self):
  self.fixtures();(self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('');(self.root/'map').write_text('api.github.com - 403\n');before=self.state()[ID]['installed']
  r=self.run_client('fetch',FORCE=1,VERBOSE=1,URL_MAP=self.root/'map');self.assertIn('GitHub did not answer',r.stderr);self.assertNotIn('has no release',r.stderr)
  self.assertEqual(self.state()[ID]['installed'],before);self.assertEqual(self.state()[ID]['latest']['checked_from'],SPEC['source'])
  (self.root/'requests.log').write_text('');self.run_client('fetch',URL_MAP=self.root/'map');self.assertNotIn('api.github.com',(self.root/'requests.log').read_text())
  # Direct install says so, as it says a repository with no release.
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
 # --- INBOX and Direct install (0.7 megatest) ---
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
  cases=[(self.png('screenshot.png',640,480,halves(640,b'\xff\0\0',b'\0\0\xff'),kind=2),'0 512x384 512x512 255,0,0,255 0,0,255,255'),
         (self.png('wide.png',2048,100,halves(2048,b'\0\xff\0\0',b'\x0a\x14\x1e\xff')),'0 512x25 512x32 0,0,0,0 10,20,30,255'),
         (self.png('tall.png',100,2048,lambda y:b'\x80'*100,kind=0),'0 25x512 32x512 128,128,128,255 128,128,128,255'),
         # A transparent pixel does not whiten the red beside it: the colour is weighted by alpha, the alpha averaged.
         (self.png('edge.png',1024,2,lambda y:b'\xff\xff\xff\0\xc8\0\0\xff'*512),'0 512x1 512x1 200,0,0,128 200,0,0,128'),
         (self.png('exact.png',512,512,lambda y:b'\x01\x02\x03\x04'*512),'0 512x512 512x512 1,2,3,4 1,2,3,4'),
         (self.png('palette.png',480,272,lambda y:b'\0'*240+b'\1'*240,kind=3,palette=b'\x11\x22\x33\x44\x55\x66'),'0 480x272 512x512 17,34,51,255 68,85,102,255'),
         (self.png('huge.png',4097,1,lambda y:b'\0\0\0\0'*4097),'-1'),
         (self.png('interlaced.png',600,600,lambda y:b'\0\0\0\0'*600,interlace=1),'-1'),
         # Cut short part way through the rows, scaled or not: nothing it had allocated is left behind.
         (self.png('cut-scaled.png',640,480,lambda y:os.urandom(1920),kind=2,cut=4000),'-1'),
         (self.png('cut.png',480,272,lambda y:os.urandom(1920),cut=4000),'-1')]
  self.assertEqual(self.decode(*[name for name,_ in cases]),[line for _,line in cases])
if __name__=='__main__':unittest.main()

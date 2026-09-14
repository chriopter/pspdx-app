"""Run actual client parsers, persistence and installer against a PSP I/O adapter.
The adapter preserves same-directory rename semantics and supports power cuts.
"""
import gzip, hashlib, json, os, pathlib, shutil, subprocess, tempfile, time, unittest, zipfile
BIN=os.environ.get('PSPDX_TEST_BIN','/tmp/pspdx-host-test')
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
  # rm_rf later names every file under PSP/GAME/<32 chars>.old/, so unpack refuses what removal could not delete.
  d='D'*32;self.write('manifest.json',dict(SPEC,installdir='PSP/GAME/'+d))
  self.zip('new.zip',{'EBOOT.PBP':b'a','f'*205:b'x'});self.assertNotEqual(self.run_client('install',ok=False).returncode,0);self.assertFalse((self.root/'ms0:/PSP/GAME'/d).exists())
  self.zip('new.zip',{'EBOOT.PBP':b'a','f'*204:b'x','a/'*40+'deep':b'y'});self.run_client('install');self.run_client('remove')
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
  r=self.run_client('fetch',VERBOSE=1);self.assertIn('dropped',r.stderr);self.assertIn(ID,r.stdout);self.assertNotIn('io.github.test.other',r.stdout)
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
  self.fixtures()
  catalog=json.loads((self.root/'catalog.json').read_text())
  catalog['schema']='https://example.com/other-format'
  self.write('catalog.json',catalog)
  self.assertIn(ID+' 3 1',self.run_client('fetch').stdout)
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
               dict(base,description='d'*2400+'\n'*100),dict(base,description='é'*2500),dict(base,listed_by='https://wijsman.de/psp-homebrew-database/'),dict(base,type='iso'),dict(base,type='homebrew',installdir='PSP/GAME/Demo'),dict(base,summary='s'*60),dict(base,name='é'*39),dict(base,category='game'),dict(base,version='2.0',notes={'any':'thing'})]:
   with self.subTest(good=good):self.parse(good)
  for bad in [dict(base,author='a'*61),dict(base,license='l'*61),dict(base,tags=['c'*25]),dict(base,tags=['']),dict(base,tags='game'),dict(base,tags=['game','game']),dict(base,tags=['t%d'%i for i in range(9)]),dict(base,tags=[1]),dict(base,tags=['a\nb']),
              dict(base,description='d'*2501),dict(base,description='a\tb'),dict(base,description='a\r\nb'),dict(base,name='a\nb'),dict(base,summary='a\nb'),dict(base,author='a\tb'),dict(base,license='a\rb'),
              dict(base,listed_by='http://wijsman.de/'),dict(base,listed_by='https://'),dict(base,listed_by=''),dict(base,type='theme'),dict(base,type=''),dict(base,type='Plugin'),dict(base,type='plugin',installdir='PSP/GAME/Demo'),dict(base,type='iso',installdir='PSP/GAME/Demo'),
              dict(base,source='http://github.com/test/demo'),dict(base,source='http://example.com/demo'),dict(base,source='https://github.com/test'),dict(base,source='https://github.com/test/demo/issues'),dict(base,source='https://'),dict(base,source='https://github.com/test/.pspdx-stage')]:
   with self.subTest(bad=bad):self.parse(bad,ok=False)
 def test_manifest_from_outside_github(self):
  # Any https source; the id is the vouching list's host and the name, the folder the name.
  mirror=dict(schema=SCHEMA,source='https://archive.org/details/psp-blocks',name='PSP Blocks!',listed_by='https://www.Wijsman.de/psp-homebrew-database/')
  self.assertEqual(self.parse(mirror),'PSP/GAME/PSPBlocks|homebrew|de.wijsman.pspblocks|')
  self.assertEqual(self.parse(dict(mirror,installdir='PSP/GAME/Blocks',tags=['game','demo'])),'PSP/GAME/Blocks|homebrew|de.wijsman.pspblocks|game,demo')
  self.assertEqual(self.parse(dict(mirror,listed_by='https://user@lists.example.co.uk:8443?x')),'PSP/GAME/PSPBlocks|homebrew|uk.co.example.lists.pspblocks|')
  for bad in [{k:v for k,v in mirror.items() if k!='listed_by'},dict(mirror,name='★ ★'),dict(mirror,listed_by='https://-/'),dict(mirror,name='.pspdx-stage'),dict(mirror,source='http://archive.org/details/psp-blocks')]:
   with self.subTest(bad=bad):self.parse(bad,ok=False)
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
  mirror={k:v for k,v in dict(first,id='de.wijsman.blocks',name='Blocks',source='https://archive.org/details/psp-blocks',listed_by='https://wijsman.de/psp-homebrew-database/',releases=[dict(first['releases'][0],url='https://archive.org/download/psp-blocks/blocks.zip')]).items() if k!='installdir'}
  self.write('catalog.json',dict(catalog,apps=[first,plugin,mirror]))
  r=self.run_client('fetch',VERBOSE=1);rows={row.split()[0]:row.split() for row in r.stdout.splitlines()}
  # A mirror a list vouches for installs from its entry; a plugin, from anywhere, is listed and not installed.
  self.assertEqual((rows[ID][4],rows['io.github.test.plug'][4],rows['de.wijsman.blocks'][4]),('0','1','0'),r.stdout)
  r=self.run_client('prepare','io.github.test.plug',ok=False,VERBOSE=1);self.assertEqual(r.stdout.strip(),'-1');self.assertIn('cannot be installed yet',r.stderr)
  # A plugin carrying an installdir, or a mirror whose id is not its list's and name, is not listed at all.
  self.write('catalog.json',dict(catalog,apps=[first,dict(plugin,installdir='PSP/GAME/Plug'),dict(mirror,id='de.wijsman.other')]))
  self.assertEqual([row.split()[0] for row in self.run_client('fetch').stdout.splitlines()],[ID])
  # The origin path skips a .pspdx whose source is not GitHub, and INBOX keeps a plugin for a later version.
  (self.root/'catalog.txt').write_text(SPEC['source']+'\n');(self.root/'ms0:/PSP/PSPDX/sources.txt').write_text('https://example.com/pspdx/\n')
  self.write('manifest.json',dict(SPEC,source='https://archive.org/details/psp-blocks',listed_by='https://wijsman.de/'))
  r=self.run_client('fetch',CATALOG_DOWN=1,FORCE=1,VERBOSE=1);self.assertIn('outside GitHub',r.stderr)
  self.write('ms0:/PSP/PSPDX/INBOX/plug.pspdx',plug);r=self.run_client('inbox',VERBOSE=1);self.assertEqual(r.stdout.strip(),'0');self.assertIn('cannot install yet; kept',r.stderr)
 def test_an_update_is_another_zip_not_a_later_date(self):
  self.fixtures();self.run_client('install',VERSION=100000)
  same=hashlib.sha256((self.root/'new.zip').read_bytes()).hexdigest();self.assertEqual(self.state()[ID]['installed']['sha256'],same)
  catalog=json.loads((self.root/'catalog.json').read_text());release=catalog['apps'][0]['releases'][0]
  for published,sha,state in (('2026-09-12T00:00:00Z',same,'2'),('1970-01-02','0'*63+'1','3'),('1970-01-02',same,'2')):
   with self.subTest(published=published,sha=sha):
    release.update(published_at=published,sha256=sha);self.write('catalog.json',catalog)
    row=next(line.split() for line in self.run_client('fetch').stdout.splitlines() if line.startswith(ID+' '));self.assertEqual(row[3],state,row)
 def vouched(self):
  # The fixtures' one app, vouched for by a list in the catalog, and its repository's own .pspdx gone.
  self.fixtures();catalog=json.loads((self.root/'catalog.json').read_text())
  catalog['apps'][0].update(listed_by='https://lists.example.org/psp/',summary='From the list',description='Two lines.\nFrom the list.')
  self.write('catalog.json',catalog);(self.root/'manifest.json').unlink();(self.root/'requests.log').write_text('')
  return catalog
 def saved(self,app_id=ID):return json.loads((self.root/f'ms0:/PSP/PSPDX/INSTALLED/{app_id}.pspdx').read_text())
 def test_a_vouched_entry_installs_from_the_catalog_and_updates_only_through_one(self):
  catalog=self.vouched();r=self.run_client('get',ID,VERBOSE=1);self.assertEqual(r.stdout.strip(),'0',r.stderr)
  self.assertEqual((self.root/'ms0:/PSP/GAME/Demo/EBOOT.PBP').read_bytes(),b'new package');self.assertIn('vouches for it',r.stderr)
  # The repository was asked once, and what is saved is the entry's word with the list that gives it.
  self.assertEqual((self.root/'requests.log').read_text().splitlines().count('https://raw.githubusercontent.com/test/demo/HEAD/.pspdx'),1)
  self.assertEqual(self.saved(),dict(schema=SCHEMA,source=SPEC['source'],name='Demo',tags=['demo'],installdir='PSP/GAME/Demo',author='test',summary='From the list',description='Two lines.\nFrom the list.',listed_by='https://lists.example.org/psp/'))
  record=self.state()[ID];self.assertEqual((record['installed']['version'],record['latest']['checked_from']),('2','https://example.com/catalog.json'))
  # Another zip in the entry is an update, and no check asks the repository about it, forced or not, with the catalog up or only saved.
  catalog['apps'][0]['releases'][0]['sha256']='0'*63+'1';self.write('catalog.json',catalog);(self.root/'requests.log').write_text('')
  for env in ({},dict(FORCE=1),dict(FORCE=1,CATALOG_DOWN=1)):
   with self.subTest(env=env):
    row=next(l.split() for l in self.run_client('fetch',**env).stdout.splitlines() if l.startswith(ID+' '));self.assertEqual(row[3],'3',row)
  self.assertEqual([x for x in (self.root/'requests.log').read_text().splitlines() if 'github' in x],[])
 def test_the_repositorys_own_pspdx_wins_and_no_vouch_is_no_install(self):
  self.vouched();self.write('manifest.json',SPEC)
  r=self.run_client('get',ID,VERBOSE=1);self.assertEqual(r.stdout.strip(),'0',r.stderr);self.assertEqual(self.saved(),SPEC);self.assertNotIn('vouches for it',r.stderr)
  # Without listed_by, no .pspdx is no install, as it always was.
  catalog=json.loads((self.root/'catalog.json').read_text());del catalog['apps'][0]['listed_by'];self.write('catalog.json',catalog);(self.root/'manifest.json').unlink()
  before=self.state()[ID];r=self.run_client('get',ID,ok=False,VERBOSE=1)
  self.assertNotEqual(r.returncode,0);self.assertIn('no catalog vouches for it',r.stderr);self.assertEqual((self.state()[ID],self.saved()),(before,SPEC))
 def test_a_vouched_entry_from_outside_github_installs_from_the_entry(self):
  self.fixtures();catalog=json.loads((self.root/'catalog.json').read_text());first=catalog['apps'][0]
  package=(self.root/'new.zip').read_bytes();blocks='de.wijsman.blocks'
  mirror=dict(id=blocks,name='Blocks',source='https://archive.org/details/psp-blocks',listed_by='https://wijsman.de/psp-homebrew-database/',installdir='PSP/GAME/Blocks',
              releases=[dict(tag='v1',published_at='2026-01-02T00:00:00Z',size=len(package),sha256=hashlib.sha256(package).hexdigest(),url='https://archive.org/download/psp-blocks/download.zip')])
  self.write('catalog.json',dict(catalog,apps=[first,mirror]));(self.root/'requests.log').write_text('')
  r=self.run_client('get',blocks,VERBOSE=1);self.assertEqual(r.stdout.strip(),'0',r.stderr)
  self.assertEqual((self.root/'ms0:/PSP/GAME/Blocks/EBOOT.PBP').read_bytes(),b'new package')
  self.assertEqual((self.state()[blocks]['source'],self.saved(blocks)['listed_by']),(mirror['source'],mirror['listed_by']))
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
 def test_tabs_come_from_known_tags_and_the_plugin_type(self):
  self.fixtures();catalog=json.loads((self.root/'catalog.json').read_text());app=catalog['apps'][0]
  for tags,kind,tabs in ((['Jeu','games','Game'],None,'-3 -2 0'),([],None,'-3 -2 0'),(None,None,'-3 -2 0'),(['game','demo','emulator'],None,'-3 -2 0 1 2 4'),(['plugin'],None,'-3 -2 0'),(['game'],'plugin','-3 -2 0 1 5')):
   with self.subTest(tags=tags,kind=kind):
    a={k:v for k,v in app.items() if k!='tags'}
    if tags is not None:a['tags']=tags
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
if __name__=='__main__':unittest.main()

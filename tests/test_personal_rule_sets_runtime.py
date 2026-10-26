"""Isolated native runtime regression; no app database, TUN or system proxy.

First generate fixtures with test_singbox --personal-rule-fixtures <directory>.
Run: python test_personal_rule_sets_runtime.py <directory> <sing-box> [openssl] [test_runtime_health]
"""
from pathlib import Path
import json, subprocess, socket, threading, http.server, time, copy, sys, ssl, tempfile
if len(sys.argv)>1 and sys.argv[1]=='--ctest':
 if len(sys.argv)!=6:raise SystemExit('compiler, fixed core, OpenSSL CLI and import test executable are required')
 compiler,engine,openssl,importer=map(lambda p:str(Path(p).resolve()),sys.argv[2:])
 if not all(Path(p).is_file() for p in (compiler,engine,openssl,importer)):
  raise SystemExit('compiler, fixed core, OpenSSL CLI and import test executable must exist')
 with tempfile.TemporaryDirectory(prefix='clashflux-rule-sets-') as fixture:
  subprocess.run([compiler,'--personal-rule-fixtures',fixture],check=True,timeout=20)
  subprocess.run([sys.executable,str(Path(__file__).resolve()),fixture,engine,openssl,importer],check=True,timeout=80)
 raise SystemExit(0)
root=Path(sys.argv[1]); engine=Path(sys.argv[2]); root.mkdir(parents=True,exist_ok=True)
source={'version':5,'rules':[{'domain_suffix':['ads.test']}]}
(root/'rules.json').write_text(json.dumps(source))
subprocess.run([str(engine),'rule-set','compile','--output',str(root/'rules.srs'),str(root/'rules.json')],check=True,capture_output=True)
if len(sys.argv)>4:
 subprocess.run([sys.argv[4],'--rule-set-import',str(root),str(engine)],check=True)
requests=[]
class Resources(http.server.BaseHTTPRequestHandler):
 def do_GET(self):
  requests.append(self.path)
  body=(root/('rules.srs' if self.path.endswith('.srs') else 'rules.json')).read_bytes()
  self.send_response(200); self.send_header('Content-Length',str(len(body))); self.end_headers(); self.wfile.write(body)
 def log_message(self,*args):pass
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Resources)
threading.Thread(target=server.serve_forever,daemon=True).start()
def port():
 with socket.socket() as s:s.bind(('127.0.0.1',0));return s.getsockname()[1]
def recv(s,n):
 b=b''
 while len(b)<n:
  chunk=s.recv(n-len(b))
  if not chunk:break
  b+=chunk
 return b
def connect(proxy,host,target):
 s=socket.create_connection(('127.0.0.1',proxy),timeout=5);s.settimeout(6)
 s.sendall(b'\x05\x01\x00'); assert recv(s,2)==b'\x05\x00'
 h=host.encode(); s.sendall(b'\x05\x01\x00\x03'+bytes([len(h)])+h+target.to_bytes(2,'big'))
 response=recv(s,4)
 if len(response)==4 and response[1]==0:
  if response[3]==1:recv(s,6)
  elif response[3]==4:recv(s,18)
  elif response[3]==3:recv(s,recv(s,1)[0]+2)
  else:raise AssertionError(('invalid SOCKS address',response))
  # Complete the SOCKS handshake before sending application bytes, as real
  # clients do. Pipelining them made the fixture depend on packet boundaries.
  s.sendall(b'GET / HTTP/1.0\r\nHost: '+h+b'\r\n\r\n')
 return s,response
def run_case(label,config,offline=False,blocked='ads.test'):
 work=root/label;work.mkdir(exist_ok=True)
 listen=port();config=copy.deepcopy(config)
 config['inbounds']=[{'type':'mixed','tag':'test','listen':'127.0.0.1','listen_port':listen}]
 config['experimental'].pop('clash_api',None)
 config['experimental']['cache_file']={'enabled':True,'path':str(root/'shared-cache.db' if 'binary' in label else work/'cache.db')}
 config['log']={'level':'debug','timestamp':False}
 config['dns']['servers']=[{'type':'hosts','tag':config['dns']['final'],'predefined':{blocked:['192.0.2.99']}}]
 file=work/'config.json';file.write_text(json.dumps(config))
 check=subprocess.run([str(engine),'check','-c',str(file),'-D',str(work)],capture_output=True,text=True,timeout=15)
 assert check.returncode==0,(label,check.stderr)
 with (work/'runtime.log').open('w') as log:
  p=subprocess.Popen([str(engine),'run','-c',str(file),'-D',str(work)],stdout=log,stderr=subprocess.STDOUT,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
  try:
   for i in range(100):
    if p.poll() is not None:raise AssertionError((label,'early exit', (work/'runtime.log').read_text()))
    try:
     s=socket.create_connection(('127.0.0.1',listen),timeout=.2);s.close();break
    except OSError:time.sleep(.1)
   else:raise AssertionError((label,'not ready'))
   s,r=connect(listen,blocked,80)
   if len(r)==4 and r[1]==0:
    try:assert s.recv(4096)==b'',(label,'blocked domain returned payload')
    except ConnectionResetError:pass
   s.close()
   assert 'rule_set=' in (work/'runtime.log').read_text() and '=> reject' in (work/'runtime.log').read_text(),(label,'no rule-set rejection evidence')
   # A local server proves unrelated traffic still follows DIRECT.
   sink=socket.socket();sink.bind(('127.0.0.1',0));sink.listen();sink.settimeout(6)
   received=[]
   def serve():
    try:
     c,_=sink.accept();c.settimeout(6);received.append(c.recv(4096));c.sendall(b'HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nOK');c.close()
    finally:sink.close()
   worker=threading.Thread(target=serve,daemon=True);worker.start()
   s,r=connect(listen,'127.0.0.1',sink.getsockname()[1]);assert len(r)==4 and r[1]==0,(label,'DIRECT should connect',r)
   response=s.recv(4096);s.close();worker.join(7);assert b'200 OK' in response and received,(label,'direct echo',response)
  finally:
   p.terminate()
   try:p.wait(8)
   except subprocess.TimeoutExpired:p.kill();p.wait()
 return {'case':label,'passed':True,'offline':offline}
inline=json.loads((root/'personal-library-inline.json').read_text())
remote=json.loads((root/'personal-library-remote.json').read_text())
results=[run_case('inline',inline)]
results.append(run_case('global',json.loads((root/'personal-library-global.json').read_text())))
if len(sys.argv)>4:
 results.append(run_case('imported-srs',json.loads((root/'personal-library-imported.json').read_text())))
for fmt,suffix in [('source','json'),('binary','srs')]:
 config=copy.deepcopy(remote);resource=config['route']['rule_set'][0]
 resource['format']=fmt;resource['url']=f'http://127.0.0.1:{server.server_port}/rules.{suffix}'
 resource['tag']='runtime-'+fmt+'-'+str(server.server_port)
 for rule in config['route']['rules']:
  if 'rule_set' in rule:rule['rule_set']=[resource['tag']]
 results.append(run_case('remote-'+fmt,config))
 if fmt=='binary':binary=config
assert '/rules.json' in requests and '/rules.srs' in requests,requests
before=len(requests)
results.append(run_case('same-binary-cache',binary))
assert len(requests)==before,('same identity should keep native cache',requests)
# Explicit refresh uses a new native identity, preserving the previous cache.
# New bytes block a different domain, proving that the old cached set was not reused.
(root/'rules.json').write_text(json.dumps({'version':5,'rules':[{'domain_suffix':['fresh.test']}]}))
subprocess.run([str(engine),'rule-set','compile','--output',str(root/'rules.srs'),str(root/'rules.json')],check=True,capture_output=True)
forced=copy.deepcopy(binary);newtag=forced['route']['rule_set'][0]['tag']+'_fetch_1'
forced['route']['rule_set'][0]['tag']=newtag
for rule in forced['route']['rules']:
 if 'rule_set' in rule:rule['rule_set']=[newtag]
results.append(run_case('forced-binary',forced,blocked='fresh.test'))
assert len(requests)==before+1,('forced refresh did not download once',requests)
# A failed new identity must not overwrite either previously usable snapshot.
(root/'rules.srs').write_bytes(b'not a valid SRS file')
broken=copy.deepcopy(forced);broken_tag=newtag+'_fetch_2'
broken['route']['rule_set'][0]['tag']=broken_tag
for rule in broken['route']['rules']:
 if 'rule_set' in rule:rule['rule_set']=[broken_tag]
broken['experimental']={'cache_file':{'enabled':True,'path':str(root/'shared-cache.db')}}
broken['inbounds']=[{'type':'mixed','listen':'127.0.0.1','listen_port':port()}]
broken_file=root/'failed-refresh.json';broken_file.write_text(json.dumps(broken))
failed=subprocess.run([str(engine),'run','-c',str(broken_file),'-D',str(root)],capture_output=True,text=True,timeout=20,
 creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
failed_log=failed.stdout+failed.stderr;(root/'failed-refresh.log').write_text(failed_log)
assert failed.returncode!=0 and 'invalid sing-box rule-set file' in failed_log,failed_log
assert len(requests)==before+2,requests
results.append({'case':'failed-forced-binary','passed':True,'invalid_content_rejected':True})
server.shutdown();server.server_close();before=len(requests)
results.append(run_case('cached-binary',forced,True,blocked='fresh.test'));assert len(requests)==before
results.append(run_case('rollback-binary-cache',binary,True));assert len(requests)==before
if len(sys.argv)>3:
 # A self-signed endpoint must fail before any HTTP request is received.
 tlsroot=root/'untrusted-tls';tlsroot.mkdir(exist_ok=True)
 cert=tlsroot/'cert.pem';key=tlsroot/'key.pem'
 subprocess.run([sys.argv[3],'req','-x509','-newkey','rsa:2048','-nodes','-days','1',
  '-subj','/CN=localhost','-addext','subjectAltName=IP:127.0.0.1',
  '-keyout',str(key),'-out',str(cert)],check=True,capture_output=True)
 tlsserver=http.server.ThreadingHTTPServer(('127.0.0.1',0),Resources)
 context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);context.load_cert_chain(cert,key)
 tlsserver.socket=context.wrap_socket(tlsserver.socket,server_side=True)
 threading.Thread(target=tlsserver.serve_forever,daemon=True).start()
 try:
  config=copy.deepcopy(binary);resource=config['route']['rule_set'][0]
  resource['tag']='tls-untrusted-'+str(tlsserver.server_port)
  resource['url']=f'https://127.0.0.1:{tlsserver.server_port}/rules.srs'
  for rule in config['route']['rules']:
   if 'rule_set' in rule:rule['rule_set']=[resource['tag']]
  config['inbounds']=[{'type':'mixed','listen':'127.0.0.1','listen_port':port()}]
  config['experimental']={'cache_file':{'enabled':True,'path':str(tlsroot/'cache.db')}}
  file=tlsroot/'config.json';file.write_text(json.dumps(config))
  outcome=subprocess.run([str(engine),'run','-c',str(file),'-D',str(tlsroot)],capture_output=True,text=True,timeout=20,
   creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
  log=outcome.stdout+outcome.stderr;(tlsroot/'runtime.log').write_text(log)
  assert outcome.returncode!=0 and 'certificate' in log.lower() and len(requests)==before,log
  results.append({'case':'untrusted-tls','passed':True,'certificate_rejected':True})
 finally:tlsserver.shutdown();tlsserver.server_close()
(root/'results.json').write_text(json.dumps({'results':results,'downloads':requests},indent=2))
print(json.dumps({'results':results,'downloads':requests},indent=2))

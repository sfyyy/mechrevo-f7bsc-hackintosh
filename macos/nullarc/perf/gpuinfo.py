# usage: python3 gpuinfo.py OUT.txt [chrome flags...] — dump chrome://gpu text via CDP (stdlib only)
import sys, os, json, time, socket, base64, subprocess, urllib.request, shutil
out=sys.argv[1]; flags=sys.argv[2:]; port=9333; prof='/tmp/ct-gpuinfo'
shutil.rmtree(prof, ignore_errors=True)
p=subprocess.Popen(["/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",f"--remote-debugging-port={port}",
  f"--user-data-dir={prof}","--no-first-run","--no-default-browser-check",*flags,"about:blank"],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
time.sleep(6)
tabs=json.load(urllib.request.urlopen(f"http://127.0.0.1:{port}/json"))
ws=[t for t in tabs if t.get('type')=='page'][0]['webSocketDebuggerUrl']
path=ws.split(str(port),1)[1]
s=socket.create_connection(('127.0.0.1',port))
k=base64.b64encode(os.urandom(16)).decode()
s.send(f"GET {path} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {k}\r\nSec-WebSocket-Version: 13\r\n\r\n".encode())
buf=b''
while b'\r\n\r\n' not in buf: buf+=s.recv(4096)
buf=buf.split(b'\r\n\r\n',1)[1]
def send(o):
    d=json.dumps(o).encode(); m=os.urandom(4); h=bytes([0x81]); n=len(d)
    h+= bytes([0x80|n]) if n<126 else bytes([0xFE])+n.to_bytes(2,'big') if n<65536 else bytes([0xFF])+n.to_bytes(8,'big')
    s.send(h+m+bytes(b^m[i%4] for i,b in enumerate(d)))
def recv():
    global buf
    def need(n):
        global buf
        while len(buf)<n: buf+=s.recv(65536)
    need(2); n=buf[1]&127; o=2
    if n==126: need(4); n=int.from_bytes(buf[2:4],'big'); o=4
    elif n==127: need(10); n=int.from_bytes(buf[2:10],'big'); o=10
    need(o+n); d=buf[o:o+n]; buf=buf[o+n:]; return json.loads(d)
js=open(os.path.join(os.path.dirname(os.path.abspath(__file__)),'gpuinfo.js')).read()
send({"id":9,"method":"Page.navigate","params":{"url":"chrome://gpu"}})
while recv().get('id')!=9: pass
time.sleep(4)
send({"id":1,"method":"Runtime.evaluate","params":{"expression":js,"returnByValue":True}})
while True:
    r=recv()
    if r.get('id')==1: break
open(out,'w').write(r['result']['result'].get('value',''))
p.terminate(); time.sleep(1); subprocess.run(["pkill","-f",f"user-data-dir={prof}"])

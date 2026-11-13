#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
"""Differential test of tools/publisher/pyjson against Python's json module.

  python3 pyjson_differential.py <build>/bin/pyjsoncheck [seed]

Generates random documents and malformed inputs, runs the checker on them and
compares every result with json.dumps(json.loads(...)) byte for byte. Use the
Python version the thoughts-server publisher runs on; error wording changes
between Python versions."""
import json, random, os, sys, subprocess, shutil, tempfile
checker = sys.argv[1]
random.seed(int(sys.argv[2]) if len(sys.argv) > 2 else 1)
d = tempfile.mkdtemp(prefix='pyjson-cases-')
chars = ['a','Z','0',' ','"','\\','/','\n','\r','\t','\b','\f','\x00','\x1f','\x7f','é','€','中','\u2028','\ufeff','😀','𝄞','\U0010ffff','\ud7ff','\ue000','\uffff']
def rstr():
    return ''.join(random.choice(chars) for _ in range(random.randint(0,6)))
def rfloat():
    return random.choice([0.0,-0.0,1e-5,1e-4,0.0001234,1e16,1e15,123456789012345678.0,0.1+0.2,1/3,-2.5,5e-324,1.7976931348623157e308,1e22,1e21,123.0,float('nan'),float('inf'),-float('inf'),random.uniform(-1e6,1e6),random.uniform(-1,1)*10**random.randint(-30,30)])
def rint():
    return random.choice([0,-1,1,2**31,2**53+1,2**63-1,-2**63,2**63,2**64+5,-2**70,random.randint(-10**6,10**6)])
def rvalue(depth=0):
    k=random.random()
    if depth>3 or k<0.3:
        return random.choice([None,True,False,rint(),rfloat(),rstr(),rstr()])
    if k<0.65:
        return [rvalue(depth+1) for _ in range(random.randint(0,4))]
    return {rstr(): rvalue(depth+1) for _ in range(random.randint(0,4))}
cases=[]
n=0
for i in range(3000):
    v=rvalue()
    allow = random.random()<0.8
    text=json.dumps(v, ensure_ascii=random.random()<0.5, indent=random.choice([None,2]))
    if random.random()<0.2: text = '  '+text.replace(', ', ' ,\n ')+' \n'
    opts=dict(indent=random.choice([None,None,2,0,4]), sort_keys=random.random()<0.5, ensure_ascii=random.random()<0.6, allow_nan=allow,
              separators=random.choice([None,None,(',',':'),(', ',': '),(',',': ')]), reject_duplicates=False)
    cases.append((text,opts))
bad=['','{','[1,]','{"a":1,}','{"a" 1}','{"a":1 "b":2}','[1 2]','{1:2}','"abc','"\\x"','"\\u12g4"','01','-','1.','.5','tru','nul','[1]x','"\x01"','{"a":1,"a":2}','\ufeff{}','{"a":\n  [1,\n  2,,3]}','"😀\\q"','NaN','-Infinity','1e400','-0','[1e-400]','{"😀":1, "a":}']
for b in bad:
    cases.append((b, dict(indent=None,sort_keys=False,ensure_ascii=True,allow_nan=True,separators=None,reject_duplicates=False)))
rawbad=[b'"a\xc3',b'"\xe2\x82',b'"\xf0\x9f\x98',b'"\xf0\x9f\x98x"',b'"\xe2x"',b'"\xff"',b'["a\xc3"]',b'"\xc3\x28"',b'"\xe0\x80\x80"',b'"\xed\xa0\x80"',b'"\xf4\x90\x80\x80"',b'"\xf0\x9f\x98\x80"',b'"\xc0\xaf"',b'\xef\xbb\xbf{}',b'{"k":"\xe2\x82"}']
for b in rawbad:
    cases.append((b, dict(indent=None,sort_keys=False,ensure_ascii=True,allow_nan=True,separators=None,reject_duplicates=False)))
for idx,(text,opts) in enumerate(cases):
    if isinstance(text, bytes):
        open(f'{d}/c{idx:05d}.json','wb').write(text)
    else:
        open(f'{d}/c{idx:05d}.json','w',encoding='utf-8',errors='surrogatepass').write(text)
    with open(f'{d}/c{idx:05d}.opts','w') as f:
        for k,v in opts.items():
            if k=='separators': f.write(f"separators={'|'.join(v) if v else ''}\n")
            else: f.write(f"{k}={v}\n")
subprocess.run([checker, d], check=True)
mism=0
for idx,(text,opts) in enumerate(cases):
    try:
        raw=open(f'{d}/c{idx:05d}.json','rb').read()
        v=json.loads(raw.decode('utf-8') if isinstance(text, bytes) else open(f'{d}/c{idx:05d}.json',encoding='utf-8',errors='surrogatepass').read())
        o={k:val for k,val in opts.items() if k!='reject_duplicates'}
        exp=json.dumps(v, **o).encode('utf-8', errors='surrogatepass')
    except Exception as e:
        exp=('ERROR: '+str(e)).encode('utf-8', errors='surrogatepass')
    got=open(f'{d}/c{idx:05d}.out','rb').read()
    if got!=exp:
        mism+=1
        if mism<=8: print('MISMATCH', idx, repr(text[:80]), opts, '\n  exp', exp[:160], '\n  got', got[:160])
print('cases', len(cases), 'mismatches', mism)
shutil.rmtree(d, ignore_errors=True)
sys.exit(1 if mism else 0)

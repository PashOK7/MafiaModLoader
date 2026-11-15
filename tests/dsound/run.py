"""Opt-in, audible DirectSound restart regression. Standard library only; no game assets or microphone."""
import argparse
from array import array
import json
import math
from pathlib import Path
import struct
import subprocess

def wave_read(path):
    data=path.read_bytes()
    assert data[:4]==b'RIFF' and data[8:12]==b'WAVE'
    chunks={};at=12
    while at+8<=len(data):
        name,size=struct.unpack_from('<4sI',data,at);at+=8
        chunks[name]=data[at:at+size];at+=size+(size&1)
    fmt=chunks[b'fmt ']
    kind,channels,rate,_,align,bits=struct.unpack_from('<HHIIHH',fmt)
    if kind==0xfffe:kind=struct.unpack_from('<I',fmt,24)[0]
    assert kind==3 and bits==32 and align==channels*4, (kind,bits,align)
    samples=array('f');samples.frombytes(chunks[b'data'])
    return rate,channels,samples

def segment_measure(rate,channels,samples,start,end):
    first=int(start*rate);last=int(end*rate)
    mono=[sum(samples[i*channels:(i+1)*channels])/channels for i in range(first,last)]
    assert mono
    rms=math.sqrt(sum(v*v for v in mono)/len(mono))
    # Distinguish the actual old/new playback rates, rather than trusting GetFrequency.
    def tone_power(hz):
        real=imag=0
        for i,value in enumerate(mono):
            window=.5-.5*math.cos(2*math.pi*i/(len(mono)-1))
            angle=2*math.pi*hz*i/rate
            real+=window*value*math.cos(angle);imag+=window*value*math.sin(angle)
        return real*real+imag*imag
    old=tone_power(311*16000/22050)
    new=tone_power(311*30869/22050)
    return {'dbfs':20*math.log10(max(rms,1e-12)),
            'actual_rate':'new' if new>old else 'old',
            'new_over_old_db':10*math.log10(max(new,1e-30)/max(old,1e-30))}

def analyze(prefix):
    results={}
    for mode in ('deferred','software'):
        base=Path(str(prefix)+'-'+mode)
        receipt=json.loads(base.with_suffix('.json').read_text())
        assert receipt['test']['exit']==receipt['capture_end']['exit']==0,receipt
        start=next(r['start_qpc_100ns'] for r in receipt['capture_start'] if r.get('ready'))
        rate,channels,samples=wave_read(base.with_suffix('.wav'))
        marks=[json.loads(line) for line in receipt['test']['output'].splitlines()]
        measured={}
        for row in marks[:-1]:
            at=(row['qpc_100ns']-start)/1e7
            measured[row['label']]=segment_measure(rate,channels,samples,at+.4,at+2.2)
            if row['label']!='initial':
                assert row['volume']==-411 and row['frequency']==30869,row
        results[mode]=measured
    software=results['software'];reference=software['changed_while_playing']['dbfs']
    for label,row in software.items():
        if label=='initial':continue
        assert row['actual_rate']=='new' and row['new_over_old_db']>30,(label,row)
        assert abs(row['dbfs']-reference)<.1,(label,row,reference)
    deferred=results['deferred']
    reproduced=deferred['restarted']['actual_rate']=='old'
    if reproduced:
        assert abs(deferred['restarted']['dbfs']-deferred['initial']['dbfs'])<.1
        assert deferred['volume_transition']['actual_rate']=='old'
        assert deferred['frequency_transition']['actual_rate']=='new'
    print(json.dumps({'deferred_backend_bug_reproduced':reproduced,
                      'software_restart_preserves_actual_gain_and_rate':True,'measurements':results},indent=2))

def record(exe_dir,prefix,known_background_pid):
    for mode in ('deferred','software'):
        base=Path(str(prefix)+'-'+mode);dest=base.with_suffix('.json');wave=base.with_suffix('.wav')
        assert not dest.exists() and not wave.exists()
        command=[str(exe_dir/'dsound_repro.exe')]+(['--software'] if mode=='software' else [])
        test=subprocess.Popen(command,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        recorder=None;receipt={}
        try:
            ready=json.loads(test.stdout.readline());assert ready['ready'],ready;receipt['ready']=ready
            command=[str(exe_dir/'loopback.exe'),str(wave),'20',str(ready['pid'])]
            if known_background_pid:command.append(str(known_background_pid))
            recorder=subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            receipt['capture_start']=[]
            for line in recorder.stdout:
                row=json.loads(line);receipt['capture_start'].append(row)
                if row.get('ready'):break
            else:raise RuntimeError(recorder.stderr.read())
            test.stdin.write('\n');test.stdin.flush()
            out,err=test.communicate(timeout=25)
            receipt['test']={'exit':test.returncode,'output':out,'error':err}
        finally:
            if test.poll() is None:test.terminate()
            if recorder:
                out,err=recorder.communicate(timeout=25)
                receipt['capture_end']={'exit':recorder.returncode,'output':out,'error':err}
            assert not dest.exists();dest.write_text(json.dumps(receipt,indent=2))

if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--executables',type=Path)
    ap.add_argument('--prefix',type=Path,required=True,help='output basename, or existing capture basename with --analyze-only')
    ap.add_argument('--known-background-pid',type=int)
    ap.add_argument('--analyze-only',action='store_true')
    args=ap.parse_args()
    if not args.analyze_only:
        assert args.executables, '--executables is required for recording'
        args.prefix.parent.mkdir(parents=True,exist_ok=True)
        record(args.executables,args.prefix,args.known_background_pid)
    analyze(args.prefix)

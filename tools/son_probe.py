#!/usr/bin/env python3
"""son_probe — a reference client / protocol tester for sond (sigrok-over-net).

Exercises the full control + data path: HELLO, SCAN, DECODERS, CONFIG, START,
then consumes the data plane until CAPTURE_END and prints a summary.

Usage:
  son_probe.py <host> [port] [--driver demo] [--continuous SECS]
               [--rate HZ] [--samples N] [--decoder id:ch=idx,...:opt=val,...]
"""
import socket, struct, json, sys, time

HDR = struct.Struct('<IBBHII')          # magic,ver,type,flags,stream,length
LOGIC = struct.Struct('<QIIB7x')        # start,count,dropped,unitsize
ANALOG = struct.Struct('<QII')          # start,count,channel
ANNB = struct.Struct('<IHH')            # stack,row,count
ANN = struct.Struct('<QQHH4x')          # start,end,class,n_texts
MAGIC, VER = 0x4b524753, 1
T = dict(HELLO=1, SERVER_INFO=2, SCAN_REQ=3, SCAN_RESULT=4, CONFIG=5, START=6,
         STOP=7, SESSION_META=8, DATA_LOGIC=9, DATA_ANALOG=10, DECODER_INFO=11,
         ANN_BATCH=12, CAPTURE_END=13, FLOW=14, ERROR=15, DECODERS_REQ=16,
         DECODERS_LIST=17)
RT = {v: k for k, v in T.items()}
FLAG_ZSTD = 1
try:
    from compression.zstd import decompress as _zdec       # Python 3.14+
except Exception:
    try:
        import zstandard as _zs
        _zdec = lambda b: _zs.ZstdDecompressor().decompress(b)
    except Exception:
        _zdec = None
WIRE_BYTES = 0


def send(s, typ, payload=b'', flags=0, stream=0):
    s.sendall(HDR.pack(MAGIC, VER, typ, flags, stream, len(payload)) + payload)


def send_json(s, typ, obj):
    send(s, typ, json.dumps(obj).encode())


def recvall(s, n):
    b = b''
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise EOFError('connection closed')
        b += c
    return b


def recv(s):
    global WIRE_BYTES
    magic, ver, typ, flags, stream, length = HDR.unpack(recvall(s, 16))
    if magic != MAGIC:
        raise ValueError('bad magic %08x' % magic)
    p = recvall(s, length) if length else b''
    WIRE_BYTES += 16 + length
    if flags & FLAG_ZSTD:
        if _zdec is None:
            raise RuntimeError('server sent zstd but no python zstd module available')
        p = _zdec(p)
        flags &= ~FLAG_ZSTD
    return typ, flags, stream, p


def recv_type(s, want):
    while True:
        typ, flags, stream, p = recv(s)
        if typ == want:
            return p
        if typ == T['ERROR']:
            raise RuntimeError('server error: ' + p.decode())


def parse_decoder_arg(a):
    # id:ch=idx,ch=idx:opt=val,opt=val
    parts = a.split(':')
    d = {'id': parts[0], 'channels': {}, 'options': {}}
    if len(parts) > 1 and parts[1]:
        for kv in parts[1].split(','):
            k, v = kv.split('='); d['channels'][k] = int(v)
    if len(parts) > 2 and parts[2]:
        for kv in parts[2].split(','):
            k, v = kv.split('=')
            try: d['options'][k] = int(v)
            except ValueError: d['options'][k] = v
    return d


def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)
    host = sys.argv[1]
    port = 5620
    driver = 'demo'; cont = 0; rate = 1000000; samples = 100000; decs = []; save = None
    args = sys.argv[2:]
    i = 0
    while i < len(args):
        a = args[i]
        if a.isdigit(): port = int(a)
        elif a == '--driver': i += 1; driver = args[i]
        elif a == '--continuous': i += 1; cont = float(args[i])
        elif a == '--rate': i += 1; rate = int(args[i])
        elif a == '--samples': i += 1; samples = int(args[i])
        elif a == '--decoder': i += 1; decs.append(parse_decoder_arg(args[i]))
        elif a == '--save': i += 1; save = args[i]
        i += 1

    s = socket.create_connection((host, port), timeout=10)
    s.settimeout(20)
    send_json(s, T['HELLO'], {'client': 'son_probe', 'proto': 1})
    print('SERVER_INFO:', json.loads(recv_type(s, T['SERVER_INFO'])))

    send_json(s, T['SCAN_REQ'], {})
    devs = json.loads(recv_type(s, T['SCAN_RESULT']))['devices']
    print('devices found: %d' % len(devs))
    dev = next((d for d in devs if d['driver'] == driver), None)
    if not dev:
        print('driver %s not found; have: %s' % (driver, [d['driver'] for d in devs])); sys.exit(2)
    logic = [c['index'] for c in dev['channels'] if c['type'] == 'logic']
    print('using %s id=%d, %d logic channels' % (driver, dev['id'], len(logic)))

    send_json(s, T['DECODERS_REQ'], {})
    dl = json.loads(recv_type(s, T['DECODERS_LIST']))['decoders']
    print('decoders available: %d' % len(dl))
    i2s = next((d for d in dl if d['id'] == 'i2s_ex'), None)
    if i2s:
        fmt = next((o for o in i2s['options'] if o['id'] == 'format'), None)
        print('  i2s_ex present; format option values =', fmt['values'] if fmt else '?')
    else:
        print('  WARNING: i2s_ex decoder NOT found')

    cfg = {'device_id': dev['id'], 'samplerate': rate, 'channels': logic,
           'mode': 'continuous' if cont else 'triggered', 'limit_samples': samples,
           'decoders': decs}
    send_json(s, T['CONFIG'], cfg)
    send_json(s, T['START'], {})

    meta = None; logic_samples = 0; logic_bytes = 0; analog = {}; anns = 0; ann_texts = []
    rec = None
    t0 = time.time()
    while True:
        if cont and time.time() - t0 > cont:
            send_json(s, T['STOP'], {})
            cont = 0  # only send once
        typ, flags, stream, p = recv(s)
        if save is not None:
            if typ == T['SESSION_META']: rec = bytearray()
            if rec is not None: rec += HDR.pack(MAGIC, VER, typ, flags, stream, len(p)) + p
        if typ == T['SESSION_META']:
            meta = json.loads(p)
            print('SESSION_META: rate=%d unitsize=%d total=%d channels=%d' %
                  (meta['samplerate'], meta['unitsize'], meta['total_samples'], len(meta['channels'])))
        elif typ == T['DATA_LOGIC']:
            st, cnt, dropped, us = LOGIC.unpack(p[:24])
            logic_samples += cnt; logic_bytes += len(p) - 24
        elif typ == T['DATA_ANALOG']:
            st, cnt, ch = ANALOG.unpack(p[:16])
            analog[ch] = analog.get(ch, 0) + cnt
        elif typ == T['DECODER_INFO']:
            print('DECODER_INFO:', json.loads(p))
        elif typ == T['ANN_BATCH']:
            stack, row, count = ANNB.unpack(p[:8]); off = 8
            for _ in range(count):
                stt, en, cls, nt = ANN.unpack(p[off:off + 24]); off += 24
                texts = []
                for _t in range(nt):
                    (ln,) = struct.unpack('<H', p[off:off + 2]); off += 2
                    texts.append(p[off:off + ln].decode('utf-8', 'replace')); off += ln
                anns += 1
                if len(ann_texts) < 8 and texts:
                    ann_texts.append(texts[0])
        elif typ == T['CAPTURE_END']:
            print('CAPTURE_END:', json.loads(p)); break
        elif typ == T['ERROR']:
            print('ERROR:', json.loads(p)); break

    print('--- summary ---')
    print('logic samples: %d (%d bytes uncompressed)' % (logic_samples, logic_bytes))
    print('bytes on wire: %d (all messages incl. headers)' % WIRE_BYTES)
    if logic_bytes:
        print('logic wire ratio vs uncompressed logic: %.1f%%' % (100.0 * WIRE_BYTES / max(1, logic_bytes)))
    if analog:
        print('analog samples/channel:', analog)
    print('annotations: %d' % anns)
    if ann_texts:
        print('sample annotations:', ann_texts)
    if save is not None and rec:
        with open(save, 'wb') as f:
            f.write(b'SONCAP\x00\x01')
            f.write(rec)
        print('saved capture (%d frame-bytes) to %s' % (len(rec), save))


if __name__ == '__main__':
    main()

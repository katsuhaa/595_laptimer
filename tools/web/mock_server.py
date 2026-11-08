#!/usr/bin/env python3
"""tools/web/mock_server.py - the laptimer's Web API on the Mac / in the container, for the page tests.

Serves a folder laid out like the XTSD (ARCH 5.1: sessions/<id>/, config/, best/, replay/) with the
same routes as firmware/laptimer/src/web (web.cpp):
  GET  /  /maint                      web/index.html (gzip, as the device)
  GET  /report?id=<id>[&save=1]       web/report.html cut at <!--LT_DATA--> + the session's data blocks
                                      (made by `report --data`, the C++ code of core/reportdata, so the
                                      mock embeds exactly as the device does); save=1: attachment
                                      report_<id>.html and the marker .dl_report.html
  GET  /report.html                   the page alone (it then fetches /api/reportdata?id=)
  GET  /api/reportdata?id=<id>        the data blocks alone
  GET  /dl?id=<id>&f=<file>           a session file as attachment; marker .dl_<file> when sent
  GET  /api/status /api/sessions /api/session?id= /api/config
  GET  /config/<name>                 (wifi.json refused: it holds passwords)
  POST /api/delete?id=                (the active session refused)
  POST /upload?name=<config>          multipart; JSON checked like the device (line / column / reason)
  POST /api/maintdone?id=             marks the item done (today, the status ODO and track hours)
  POST /api/odo?km=                   sets the status ODO (0..999999.9 km), like the device
  POST /api/course_from_vbo           multipart VBO head -> a course in courses.json (a stand-in for
                                      the app's lt::CourseDb::fromRaceChronoVbo)
  POST /api/format                    only while "storage_mounted" is false (--unmounted)
  OPTIONS, PROPFIND (Depth 0/1), GET/HEAD of files: read-only WebDAV as the device; writes 403/405
Usage: mock_server.py --root <folder> --report-bin <report> [--web web] [--port 8080] [--active <id>] [--unmounted]
Python 3.9+ standard library only.
"""
import argparse
import datetime
import email.parser
import email.policy
import gzip
import json
import math
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MARKER = b'<!--LT_DATA-->'
CONFIGS = ('courses.json', 'maint.json', 'checklist.json', 'wifi.json')
SESSION_ID = re.compile(r'^\d{8}_\d{6}$')
FILE_NAME = re.compile(r'^[A-Za-z0-9_][A-Za-z0-9._-]{0,62}$')
MB_PER_HOUR = 72e6  # プロジェクト計画 3.4: CSV + VBO + UBX + IMU, about 72 MB per hour


def kind_of(name):
    if name.startswith('.dl_'):
        return 'marker'
    if name.startswith('.'):
        return 'hidden'
    if name == 'meta.json':
        return 'meta'
    for suffix, kind in (('_laps.csv', 'laps'), ('_imu.csv', 'imu'), ('_can.csv', 'can'), ('.csv', 'csv'),
                         ('.vbo', 'vbo'), ('.ubx', 'ubx')):
        if name.endswith(suffix):
            return kind
    return 'other'


def content_type(name):
    if name.endswith('.csv'):
        return 'text/csv; charset=utf-8'
    if name.endswith('.json'):
        return 'application/json; charset=utf-8'
    if name.endswith('.html'):
        return 'text/html; charset=utf-8'
    return 'application/octet-stream'


class JsonCheck:
    """firmware/webcheck JsonCheck in Python: the same reason codes, lines and columns (characters)."""

    def __init__(self, data):
        self.b = data
        self.i = 0
        self.line = 1
        self.col = 1
        self.depth = 0
        self.why = ''
        self.err_line = 0
        self.err_col = 0

    def peek(self):
        return self.b[self.i] if self.i < len(self.b) else -1

    def get(self):
        c = self.peek()
        if c < 0:
            return c
        self.i += 1
        if c == 0x0A:
            self.line += 1
            self.col = 1
        elif (c & 0xC0) != 0x80:
            self.col += 1
        return c

    def fail(self, why):
        if not self.why:
            self.why, self.err_line, self.err_col = why, self.line, self.col
        return False

    def ws(self):
        while self.peek() in (0x20, 0x09, 0x0D, 0x0A):
            self.get()

    def digits(self):
        any_ = False
        while 0x30 <= self.peek() <= 0x39:
            self.get()
            any_ = True
        return any_

    def word(self, w):
        for ch in w.encode():
            if self.peek() != ch:
                return self.fail('eof' if self.peek() < 0 else 'unexpected')
            self.get()
        return True

    def string(self):
        self.get()
        while True:
            c = self.peek()
            if c < 0:
                return self.fail('eof-in-string')
            if c == 0x22:
                self.get()
                return True
            if c < 0x20:
                return self.fail('control-in-string')
            self.get()
            if c != 0x5C:
                continue
            e = self.peek()
            if e == ord('u'):
                self.get()
                for _ in range(4):
                    if self.peek() < 0 or chr(self.peek()) not in '0123456789abcdefABCDEF':
                        return self.fail('bad-escape')
                    self.get()
            elif e > 0 and chr(e) in '"\\/bfnrt':
                self.get()
            else:
                return self.fail('bad-escape')

    def number(self):
        if self.peek() == ord('-'):
            self.get()
        if self.peek() == ord('0'):
            self.get()
        elif not self.digits():
            return self.fail('bad-number')
        if self.peek() == ord('.'):
            self.get()
            if not self.digits():
                return self.fail('bad-number')
        if self.peek() in (ord('e'), ord('E')):
            self.get()
            if self.peek() in (ord('+'), ord('-')):
                self.get()
            if not self.digits():
                return self.fail('bad-number')
        return True

    def value(self):
        self.ws()
        c = self.peek()
        if c < 0:
            return self.fail('eof')
        if c in (ord('{'), ord('[')):
            return self.container(c == ord('{'))
        if c == 0x22:
            return self.string()
        if c == ord('t'):
            return self.word('true')
        if c == ord('f'):
            return self.word('false')
        if c == ord('n'):
            return self.word('null')
        if c == ord('-') or 0x30 <= c <= 0x39:
            return self.number()
        return self.fail('unexpected')

    def container(self, obj):
        self.depth += 1
        if self.depth > 32:
            return self.fail('too-deep')
        close = ord('}') if obj else ord(']')
        self.get()
        self.ws()
        if self.peek() == close:
            self.get()
            self.depth -= 1
            return True
        while True:
            if obj:
                self.ws()
                if self.peek() != 0x22:
                    return self.fail('eof' if self.peek() < 0 else 'expected-key')
                if not self.string():
                    return False
                self.ws()
                if self.peek() != ord(':'):
                    return self.fail('eof' if self.peek() < 0 else 'expected-colon')
                self.get()
            if not self.value():
                return False
            self.ws()
            c = self.peek()
            if c == ord(','):
                self.get()
                continue
            if c == close:
                self.get()
                self.depth -= 1
                return True
            if c < 0:
                return self.fail('eof')
            return self.fail('expected-comma-or-brace' if obj else 'expected-comma-or-bracket')

    def run(self):
        if self.peek() == 0xEF:
            self.get()
            if self.get() != 0xBB or self.get() != 0xBF:
                return self.fail('bad-bom')
            self.col = 1
        if not self.value():
            return False
        self.ws()
        if self.peek() >= 0:
            return self.fail('extra')
        return True


class Card:
    """The XTSD folder and the mock's state."""

    def __init__(self, root, report_bin, web, active, unmounted=False):
        self.root = os.path.abspath(root)
        self.report_bin = os.path.abspath(report_bin)
        self.web = os.path.abspath(web)
        self.active = active or ''
        self.lock = threading.Lock()
        self.started = time.time()
        self.odo_km = 12345.6
        self.track_h = 12.5
        self.mounted = not unmounted

    def path(self, *parts):
        return os.path.join(self.root, *parts)

    def session_ids(self):
        d = self.path('sessions')
        if not os.path.isdir(d):
            return []
        return sorted((n for n in os.listdir(d) if SESSION_ID.match(n) and os.path.isdir(os.path.join(d, n))),
                      reverse=True)

    def session(self, sid):
        d = self.path('sessions', sid)
        names = sorted(os.listdir(d))
        markers = {n[4:] for n in names if n.startswith('.dl_')}
        files, total = [], 0
        for n in names:
            k = kind_of(n)
            if k in ('marker', 'hidden') or not os.path.isfile(os.path.join(d, n)):
                continue
            size = os.path.getsize(os.path.join(d, n))
            total += size
            files.append({'name': n, 'size': size, 'kind': k, 'dl': n in markers})
        meta = None
        try:
            with open(os.path.join(d, 'meta.json'), 'rb') as f:
                raw = f.read()
            if JsonCheck(raw).run():
                meta = json.loads(raw.decode('utf-8'))
        except OSError:
            pass
        downloaded = 'report.html' in markers or any(f['dl'] and f['kind'] in ('csv', 'vbo') for f in files)
        return {'id': sid, 'meta': meta, 'bytes': total, 'downloaded': downloaded, 'report_dl': 'report.html' in markers,
                'active': sid == self.active, 'files': files}

    def status(self):
        used = 0
        for base, _, names in os.walk(self.root):
            for n in names:
                used += os.path.getsize(os.path.join(base, n))
        total = 1_860_000_000  # an XTSD 2 GB shows about 1.86 GB
        free = max(0, total - used)
        return {'fw': 'mock', 'host': 'laptimer', 'ip': '127.0.0.1', 'ssid': 'mock', 'rssi': -50,
                'free_bytes': free, 'total_bytes': total, 'remain_h': round(free / MB_PER_HOUR, 1),
                'low': free < total * 0.2, 'utc_ms': int(time.time() * 1000), 'tz_min': 540, 'active': self.active,
                'odo_km': self.odo_km, 'track_h': self.track_h, 'uptime_s': int(time.time() - self.started),
                'last': '-', 'storage_mounted': self.mounted, 'can_format': True}

    def config_list(self):
        out = []
        for n in CONFIGS + ('settings.json',):
            p = self.path('config', n)
            if os.path.isfile(p):
                st = os.stat(p)
                out.append({'name': n, 'exists': True, 'size': st.st_size, 'mtime': int(st.st_mtime)})
            elif n != 'settings.json':
                out.append({'name': n, 'exists': False, 'size': 0, 'mtime': 0})
        return out

    def data_blocks(self, sid):
        r = subprocess.run([self.report_bin, '--data', self.path('sessions', sid)], capture_output=True, check=False)
        if r.returncode != 0:
            raise RuntimeError(r.stderr.decode('utf-8', 'replace'))
        return r.stdout

    def page(self, name):
        with open(os.path.join(self.web, name), 'rb') as f:
            return f.read()


def multipart_file(headers, body):
    """The first file of a multipart/form-data body: (filename, bytes) or (None, None)."""
    ctype = headers.get('Content-Type', '')
    msg = email.parser.BytesParser(policy=email.policy.HTTP).parsebytes(
        b'Content-Type: ' + ctype.encode('latin-1') + b'\r\n\r\n' + body)
    if not msg.is_multipart():
        return None, None
    for part in msg.iter_parts():
        fn = part.get_filename()
        if fn is not None:
            return fn, part.get_payload(decode=True) or b''
    return None, None


def mock_course_from_vbo(card, text):
    """A stand-in for the app's lt::CourseDb::fromRaceChronoVbo: [laptiming] -> a course entry."""
    lines = text.replace('\r\n', '\n').split('\n')
    name, gates, section = '', [], ''
    for ln in lines:
        s = ln.strip()
        if s.startswith('['):
            section = s
            continue
        if section == '[session data]' and s.startswith('name '):
            name = s[5:].strip()
        if section == '[laptiming]' and (s.startswith('Start') or s.startswith('Split')):
            f = s.split()
            lon_a, lat_a, lon_b, lat_b = (float(x) for x in f[1:5])
            la, lo, lb, lob = lat_a / 60, -lon_a / 60, lat_b / 60, -lon_b / 60
            dy = (la - lb) * 111000.0
            dx = (lo - lob) * 111000.0 * math.cos(math.radians(la))
            hdg = (math.degrees(math.atan2(dx, dy)) + 360.0) % 360.0
            label = s.split('¬')[-1].strip() if '¬' in s else f[0]
            g = {'type': 'sf' if f[0] == 'Start' else 'split', 'lat': round(la, 7), 'lon': round(lo, 7),
                 'heading': round(hdg, 1), 'width': 50 if f[0] == 'Start' else 30}
            if g['type'] == 'split':
                g['name'] = label
            gates.append(g)
    if not any(g['type'] == 'sf' for g in gates):
        return False, '[laptiming] に Start の線がありません'
    p = card.path('config', 'courses.json')
    doc = {'version': 1, 'courses': []}
    if os.path.isfile(p):
        try:
            with open(p, 'rb') as f:
                doc = json.loads(f.read().decode('utf-8'))
        except ValueError:
            pass
    sf = next(g for g in gates if g['type'] == 'sf')
    target = None
    for c in doc.get('courses', []):
        for g in c.get('gates', []):
            if g.get('type') == 'sf' and abs(g['lat'] - sf['lat']) < 0.003 and abs(g['lon'] - sf['lon']) < 0.003:
                target = c
    if target is None:
        target = {'id': 'course%d' % (len(doc.get('courses', [])) + 1), 'name': name or 'コース', 'min_lap_s': 30}
        doc.setdefault('courses', []).append(target)
        verb = '追加'
    else:
        verb = '置き換え'
    target['gates'] = gates
    os.makedirs(card.path('config'), exist_ok=True)
    with open(p, 'w', encoding='utf-8') as f:
        json.dump(doc, f, ensure_ascii=False, indent=2)
    return True, '%s（%s）を%s、線 %d 本' % (target.get('name', ''), target['id'], verb, len(gates))


def dav_entry(href, name, is_dir, size, mtime, quota=None):
    t = datetime.datetime.fromtimestamp(mtime, datetime.timezone.utc)
    http = t.strftime('%a, %d %b %Y %H:%M:%S GMT')
    iso = t.strftime('%Y-%m-%dT%H:%M:%SZ')
    href = urllib.parse.quote(href, safe='/._-~')
    name = name.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;').replace('"', '&quot;')
    s = '<D:response><D:href>%s</D:href><D:propstat><D:prop><D:displayname>%s</D:displayname>' % (href, name)
    if is_dir:
        s += '<D:resourcetype><D:collection/></D:resourcetype>'
        if quota:
            s += '<D:quota-available-bytes>%d</D:quota-available-bytes><D:quota-used-bytes>%d</D:quota-used-bytes>' % quota
    else:
        s += ('<D:resourcetype/><D:getcontentlength>%d</D:getcontentlength><D:getcontenttype>%s</D:getcontenttype>'
              % (size, content_type(name)))
    s += '<D:getlastmodified>%s</D:getlastmodified><D:creationdate>%s</D:creationdate>' % (http, iso)
    return s + '</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>\n'


DAV_TOP = ('sessions', 'best', 'config', 'replay')


def dav_visible(rel_parts):
    if not rel_parts:
        return True
    if rel_parts[0] not in DAV_TOP:
        return False
    if any(p.startswith('.') for p in rel_parts):
        return False
    return not (rel_parts[0] == 'config' and len(rel_parts) > 1 and rel_parts[1] == 'wifi.json')


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    server_version = 'laptimer-mock'
    card = None  # set in main()

    def log_message(self, fmt, *args):
        sys.stderr.write('mock: %s %s\n' % (self.command, fmt % args))

    # ---- helpers
    def reply(self, code, body=b'', ctype='application/json; charset=utf-8', headers=None):
        if isinstance(body, str):
            body = body.encode('utf-8')
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Connection', 'close')
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != 'HEAD':
            self.wfile.write(body)
        self.close_connection = True

    def json(self, obj, code=200):
        self.reply(code, json.dumps(obj, ensure_ascii=False))

    def fail(self, code, error, message):
        self.json({'ok': False, 'error': error, 'message': message}, code)

    def body(self):
        n = int(self.headers.get('Content-Length') or 0)
        return self.rfile.read(n) if n > 0 else b''

    def query(self):
        u = urllib.parse.urlsplit(self.path)
        return urllib.parse.unquote(u.path), {k: v[0] for k, v in urllib.parse.parse_qs(u.query).items()}

    def send_file(self, path, name, attach, mark=None):
        size = os.path.getsize(path)
        self.send_response(200)
        self.send_header('Content-Type', content_type(name))
        self.send_header('Content-Length', str(size))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Connection', 'close')
        if attach:
            self.send_header('Content-Disposition', 'attachment; filename="%s"' % name)
        self.end_headers()
        self.close_connection = True
        if self.command == 'HEAD':
            return
        sent = 0
        with open(path, 'rb') as f:
            while True:
                b = f.read(8192)
                if not b:
                    break
                self.wfile.write(b)
                sent += len(b)
        if mark and sent == size:
            open(mark, 'wb').close()

    # ---- methods
    def do_GET(self):
        path, q = self.query()
        c = self.card
        if path in ('/', '/maint', '/index.html'):
            self.reply(200, gzip.compress(c.page('index.html'), mtime=0), 'text/html; charset=utf-8',
                       {'Content-Encoding': 'gzip'})
        elif path == '/report.html':
            self.reply(200, gzip.compress(c.page('report.html'), mtime=0), 'text/html; charset=utf-8',
                       {'Content-Encoding': 'gzip'})
        elif path in ('/report', '/api/reportdata'):
            sid = q.get('id', '')
            if not SESSION_ID.match(sid) or not os.path.isdir(c.path('sessions', sid)):
                return self.fail(404, 'not-found', 'セッションがありません')
            data = c.data_blocks(sid)
            if path == '/api/reportdata':
                return self.reply(200, data, 'text/html; charset=utf-8')
            page = c.page('report.html')
            if page.count(MARKER) != 1:
                return self.fail(500, 'page', 'report.html の目印がありません')
            head, tail = page.split(MARKER)
            save = q.get('save') == '1'
            headers = {'Content-Disposition': 'attachment; filename="report_%s.html"' % sid} if save else {}
            self.reply(200, head + data + tail, 'text/html; charset=utf-8', headers)
            if save:
                open(c.path('sessions', sid, '.dl_report.html'), 'wb').close()
        elif path == '/dl':
            sid, name = q.get('id', ''), q.get('f', '')
            p = c.path('sessions', sid, name)
            if not SESSION_ID.match(sid) or not FILE_NAME.match(name) or not os.path.isfile(p):
                return self.fail(404, 'not-found', 'ファイルがありません')
            self.send_file(p, name, True, c.path('sessions', sid, '.dl_' + name))
        elif path == '/api/status':
            self.json(c.status())
        elif path == '/api/sessions':
            self.json([c.session(s) for s in c.session_ids()])
        elif path == '/api/session':
            sid = q.get('id', '')
            if not SESSION_ID.match(sid) or not os.path.isdir(c.path('sessions', sid)):
                return self.fail(404, 'not-found', 'セッションがありません')
            self.json(c.session(sid))
        elif path == '/api/config':
            self.json(c.config_list())
        elif path.startswith('/config/') and path.count('/') == 2:
            name = path[8:]
            if name == 'wifi.json':
                return self.fail(403, 'secret', 'wifi.json はパスワードを含むため本機から読み出せません')
            p = c.path('config', name)
            if not FILE_NAME.match(name) or not os.path.isfile(p):
                return self.fail(404, 'not-found', name + ' はまだありません')
            self.send_file(p, name, q.get('save') == '1')
        else:
            self.dav_get()

    def do_HEAD(self):
        self.dav_get()

    def dav_get(self):
        path, _ = self.query()
        parts = [p for p in path.split('/') if p]
        p = self.card.path(*parts)
        if parts and dav_visible(parts) and os.path.isfile(p):
            return self.send_file(p, parts[-1], False)
        self.reply(404, 'not found', 'text/plain; charset=utf-8')

    def do_POST(self):
        path, q = self.query()
        c = self.card
        body = self.body()
        if path == '/api/delete':
            sid = q.get('id', '')
            if not SESSION_ID.match(sid) or not os.path.isdir(c.path('sessions', sid)):
                return self.fail(404, 'not-found', 'セッションがありません')
            if sid == c.active:
                return self.fail(409, 'active', '記録中のセッションは削除できません')
            shutil.rmtree(c.path('sessions', sid))
            return self.json({'ok': True})
        if path == '/api/format':
            if c.mounted:
                return self.fail(409, 'mounted', 'XTSD は使える状態です。フォーマットしません')
            c.mounted = True
            return self.json({'ok': True})
        if path == '/upload':
            name = q.get('name', '')
            if name not in CONFIGS:
                return self.fail(400, 'bad-name', name + ' は受け付けません')
            fn, data = multipart_file(self.headers, body)
            if data is None:
                return self.fail(400, 'no-file', 'ファイルが届いていません')
            if len(data) > 64 * 1024:
                return self.fail(413, 'too-large', '64 KB まで')
            chk = JsonCheck(data)
            if not chk.run():
                return self.json({'ok': False, 'error': 'json', 'name': name, 'size': len(data), 'line': chk.err_line,
                                  'col': chk.err_col, 'why': chk.why})
            os.makedirs(c.path('config'), exist_ok=True)
            with open(c.path('config', name), 'wb') as f:
                f.write(data)
            return self.json({'ok': True, 'name': name, 'size': len(data)})
        if path == '/api/odo':
            t = q.get('km', '')
            try:
                km = float(t) if 0 < len(t) < 16 else -1.0
            except ValueError:
                km = -1.0
            if not (0.0 <= km <= 999999.9) or km != km:
                return self.fail(400, 'bad-km', 'ODO は 0〜999999.9 km の数で指定してください')
            c.odo_km = km
            return self.json({'ok': True})
        if path == '/api/maintdone':
            item = q.get('id', '')
            p = c.path('config', 'maint.json')
            try:
                with open(p, 'rb') as f:
                    doc = json.loads(f.read().decode('utf-8'))
            except (OSError, ValueError):
                return self.fail(404, 'not-found', 'maint.json がありません')
            for it in doc.get('items', []):
                if it.get('id') == item:
                    it['last_date'] = datetime.date.today().isoformat()
                    it['last_odo'] = round(c.odo_km)
                    it['last_track_h'] = c.track_h
                    with open(p, 'w', encoding='utf-8') as f:
                        json.dump(doc, f, ensure_ascii=False, indent=2)
                    return self.json({'ok': True})
            return self.fail(404, 'not-found', item + ' という項目はありません')
        if path == '/api/course_from_vbo':
            fn, data = multipart_file(self.headers, body)
            if data is None:
                return self.fail(400, 'no-file', 'ファイルが届いていません')
            ok, msg = mock_course_from_vbo(c, data.decode('utf-8', 'replace'))
            return self.json({'ok': ok, 'error': '' if ok else 'convert', 'message': msg})
        self.dav_write()

    def do_OPTIONS(self):
        self.reply(200, b'', 'text/plain', {'DAV': '1', 'Allow': 'OPTIONS, GET, HEAD, PROPFIND', 'MS-Author-Via': 'DAV'})

    def do_PROPFIND(self):
        self.body()
        path, _ = self.query()
        parts = [p for p in path.split('/') if p]
        p = self.card.path(*parts)
        if not dav_visible(parts) or not os.path.exists(p):
            return self.reply(404, 'not found', 'text/plain; charset=utf-8')
        depth = self.headers.get('Depth', '1')
        st = self.card.status()
        quota = (st['free_bytes'], st['total_bytes'] - st['free_bytes'])
        href = '/' + '/'.join(parts) + ('/' if os.path.isdir(p) and parts else '')
        if not parts:
            href = '/'
        out = '<?xml version="1.0" encoding="utf-8"?>\n<D:multistatus xmlns:D="DAV:">\n'
        out += dav_entry(href, parts[-1] if parts else 'laptimer', os.path.isdir(p), os.path.getsize(p),
                         int(os.path.getmtime(p)), quota if os.path.isdir(p) else None)
        if os.path.isdir(p) and depth != '0':
            for n in sorted(os.listdir(p)):
                sub = parts + [n]
                q = os.path.join(p, n)
                if not dav_visible(sub):
                    continue
                isd = os.path.isdir(q)
                out += dav_entry(href + n + ('/' if isd else ''), n, isd, 0 if isd else os.path.getsize(q),
                                 int(os.path.getmtime(q)))
        out += '</D:multistatus>\n'
        self.send_response(207, 'Multi-Status')
        b = out.encode('utf-8')
        self.send_header('Content-Type', 'application/xml; charset="utf-8"')
        self.send_header('Content-Length', str(len(b)))
        self.send_header('Connection', 'close')
        self.end_headers()
        self.wfile.write(b)
        self.close_connection = True

    def dav_write(self):
        self.body()
        if self.command in ('LOCK', 'UNLOCK'):
            return self.reply(405, 'read-only', 'text/plain', {'Allow': 'OPTIONS, GET, HEAD, PROPFIND'})
        self.reply(403, 'read-only', 'text/plain')

    do_PUT = do_DELETE = do_MKCOL = do_MOVE = do_COPY = do_PROPPATCH = do_LOCK = do_UNLOCK = do_PATCH = dav_write


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--root', required=True, help='folder laid out like the XTSD')
    ap.add_argument('--report-bin', required=True, help='the host report tool (go.sh report builds it)')
    ap.add_argument('--web', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'web'))
    ap.add_argument('--port', type=int, default=8080)
    ap.add_argument('--active', default='', help='session id treated as being recorded')
    ap.add_argument('--unmounted', action='store_true', help='report the XTSD as not mounted until /api/format')
    a = ap.parse_args()
    Handler.card = Card(a.root, a.report_bin, a.web, a.active, a.unmounted)
    srv = ThreadingHTTPServer(('127.0.0.1', a.port), Handler)
    print('mock laptimer: http://127.0.0.1:%d/  root %s' % (srv.server_address[1], Handler.card.root), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()

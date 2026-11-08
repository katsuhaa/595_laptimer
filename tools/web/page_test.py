#!/usr/bin/env python3
"""tools/web/page_test.py - the iPhone pages tested in headless Chrome against the mock server.

  page_test.py --report-bin <report> --vbo <RaceChrono VBO export> [--rcz <unzipped .rcz dir>]...
               [--out out/web] [--chrome <path>]   (default: /usr/bin/google-chrome or the Mac's Google Chrome)

Makes a card folder (XTSD layout) in a temp directory: sessions converted from the owner's RaceChrono
data with `report --session` (laps from core/lap with RaceChrono's track definition), config files
from data/. Starts tools/web/mock_server.py on it and, in a 390 px wide phone viewport, checks:
  index      session list (count, laps, best), status, config dates
  report     each session: the lap table (lap numbers, lap times, sectors) equals the session's laps
             file; load time (also with the CPU slowed 4x); the crosshair; the map in both colourings
  fetch      /report.html?id= (the page fetching /api/reportdata) shows the same laps
  download   /report?id=&save=1 saves report_<id>.html, marks .dl_report.html; the saved file opened
             from disk shows the same laps; a VBO download is byte-identical and marked
  upload     a good courses.json is stored; a broken one is refused with line / column / reason and
             the stored file is unchanged; a wrong-shaped file is stopped in the page
  maint      an edited maint.json is sent and stored; "実施済み" updates the item; the ODO is set
  course     a RaceChrono VBO -> course (the mock's stand-in for lt::CourseDb)
  delete     the active session cannot be deleted; another one is deleted after the confirm
  format     with --unmounted the format button appears and works behind a confirm
  webdav     OPTIONS / PROPFIND (mock) answer DAV 1 and well-formed XML; PUT is refused
Screenshots: <out>/*.png (390 px wide). Last line: RESULT: PASS or FAIL.
Needs: python playwright (uses the installed Chrome, no browser download).
"""
import argparse
import asyncio
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
import xml.dom.minidom

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
FAILS = []


def check(ok, what):
    print('%s %s' % ('PASS' if ok else 'FAIL', what), flush=True)
    if not ok:
        FAILS.append(what)
    return ok


def lap_text(ms):
    m, rest = divmod(ms, 60000)
    return '%d:%06.3f' % (m, rest / 1000.0)


def sec_text(ms):
    return lap_text(ms) if ms >= 60000 else '%.3f' % (ms / 1000.0)


def read_laps(session_dir):
    for n in os.listdir(session_dir):
        if n.endswith('_laps.csv'):
            laps = []
            with open(os.path.join(session_dir, n), encoding='utf-8') as f:
                for line in f:
                    p = line.strip().split(',')
                    if len(p) < 4 or not p[0].isdigit():
                        continue
                    sec = [int(x) for x in (p[4] if len(p) > 4 else '').split(';') if x]
                    laps.append({'lap': int(p[0]), 'ms': int(p[3]), 'sec': sec})
            return laps
    return []


def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Mock:
    def __init__(self, root, report_bin, extra=()):
        self.port = free_port()
        self.proc = subprocess.Popen([sys.executable, os.path.join(ROOT, 'tools', 'web', 'mock_server.py'), '--root', root,
                                      '--report-bin', report_bin, '--port', str(self.port)] + list(extra),
                                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.proc.stdout.readline()  # "mock laptimer: http://..."
        self.url = 'http://127.0.0.1:%d' % self.port

    def close(self):
        self.proc.terminate()
        self.proc.wait(5)


def request(url, method='GET', headers=None, data=None):
    req = urllib.request.Request(url, method=method, headers=headers or {}, data=data)
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, dict(r.headers), r.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()


async def lap_table(page):
    """Lap rows of the report as shown: [{'lap': 8, 'time': '1:22.534', 'sec': ['22.463', ...]}]"""
    return await page.evaluate('''() => {
      const out = [];
      for (const tr of document.querySelectorAll('#laps tr.lap')) {
        const tds = tr.querySelectorAll('td');
        const s = tr.nextElementSibling;
        const sec = s ? [...s.querySelectorAll('.sg span')].map(e => e.textContent) : [];
        out.push({lap: +tds[0].textContent.replace('L', ''), time: tds[1].textContent, sec: sec});
      }
      return out;
    }''')


def compare_laps(shown, laps):
    if len(shown) != len(laps):
        return False, '%d rows shown, %d laps in the file' % (len(shown), len(laps))
    for s, l in zip(shown, laps):
        want_sec = [sec_text(x) for x in l['sec']]
        if s['lap'] != l['lap'] or s['time'] != lap_text(l['ms']) or (want_sec and s['sec'] != want_sec):
            return False, 'L%d shown %s %s, file %s %s' % (l['lap'], s['time'], s['sec'], lap_text(l['ms']), want_sec)
    return True, '%d laps, times and sectors equal' % len(laps)


async def run(a, card, sessions):
    from playwright.async_api import async_playwright
    out = a.out
    os.makedirs(out, exist_ok=True)
    ids = sorted(sessions, reverse=True)
    active = ids[-1] if len(ids) > 1 else ''
    mock = Mock(card, a.report_bin, ['--active', active] if active else [])
    print('mock server %s, card %s, active session %s' % (mock.url, card, active or '-'), flush=True)
    async with async_playwright() as p:
        browser = await p.chromium.launch(executable_path=a.chrome, args=['--no-sandbox'])
        ctx = await browser.new_context(viewport={'width': 390, 'height': 844}, device_scale_factor=2, is_mobile=True,
                                        has_touch=True, accept_downloads=True, locale='ja-JP')
        page = await ctx.new_page()
        errors = []
        page.on('pageerror', lambda e: errors.append(str(e)))

        # ---- index
        await page.goto(mock.url + '/')
        await page.wait_for_function('window.__ltIndex && window.__ltIndex.ready', timeout=20000)
        cards = await page.evaluate("[...document.querySelectorAll('#sessions .card')].map(c => [c.dataset.id, c.querySelector('.facts').textContent])")
        check([c[0] for c in cards] == ids, 'index: sessions newest first %s' % [c[0] for c in cards])
        for sid, facts in cards:
            laps = read_laps(os.path.join(card, 'sessions', sid))
            best = min(laps, key=lambda l: l['ms']) if laps else None
            check(('%d 周' % len(laps)) in facts and (not best or lap_text(best['ms']) in facts),
                  'index: %s card shows %d laps, BEST %s (%s)' % (sid, len(laps), lap_text(best['ms']) if best else '-', facts))
        check(not await page.locator('#format').is_visible(), 'index: no format button while the XTSD is mounted')
        status = await page.evaluate("document.querySelector('#status').textContent")
        check('未ダウンロード' in status and '残り' in status and 'laptimer.local' in status, 'index: status shows address, remaining hours, undownloaded')
        await page.screenshot(path=os.path.join(out, 'index.png'), full_page=True)
        await page.screenshot(path=os.path.join(out, 'index_top.png'))

        # ---- reports
        for sid in ids:
            laps = read_laps(os.path.join(card, 'sessions', sid))
            t0 = time.time()
            await page.goto(mock.url + '/report?id=' + sid)
            await page.wait_for_function('window.__ltReport', timeout=30000)
            r = await page.evaluate('window.__ltReport')
            wall = (time.time() - t0) * 1000
            check(r.get('ready'), 'report %s: ready (%s)' % (sid, r.get('error', 'ok')))
            ok, why = compare_laps(await lap_table(page), laps)
            check(ok, 'report %s: lap table = laps file: %s' % (sid, why))
            check([l['ms'] for l in r.get('laps', [])] == [l['ms'] for l in laps], 'report %s: page lap data = laps file' % sid)
            print('info report %s: %d rows, page code %d ms after the data arrived, %.0f ms from the request (mock)' % (
                sid, r.get('rows', 0), r.get('ms', 0), wall), flush=True)
            await page.screenshot(path=os.path.join(out, 'report_%s.png' % sid), full_page=True)
            await page.screenshot(path=os.path.join(out, 'report_%s_top.png' % sid))
            # crosshair on the first chart, then the map with it
            cv = page.locator('#charts .cv').first
            await cv.scroll_into_view_if_needed()
            bb = await cv.bounding_box()
            await page.touchscreen.tap(bb['x'] + bb['width'] * 0.55, bb['y'] + 30)
            await page.wait_for_timeout(150)
            ro = await page.evaluate("[...document.querySelectorAll('#charts .ro')].map(e => e.textContent)")
            check(all(x and '最高' not in x for x in ro[:1]), 'report %s: crosshair readout %s' % (sid, ro[:2]))
            await page.locator('#s-charts').screenshot(path=os.path.join(out, 'report_%s_charts.png' % sid))
            await page.locator('#s-map').scroll_into_view_if_needed()
            await page.locator('#s-map').screenshot(path=os.path.join(out, 'report_%s_map.png' % sid))
            await page.click('#mapmode button[data-m=delta]')
            await page.locator('#s-map').screenshot(path=os.path.join(out, 'report_%s_map_delta.png' % sid))
            # select another lap from the table
            if len(laps) > 2:
                await page.locator('#laps tr.lap').nth(0).click()
                legend = await page.evaluate("document.querySelector('#legend').textContent")
                check(('L%d' % laps[0]['lap']) in legend, 'report %s: tapping L%d shows it in the charts' % (sid, laps[0]['lap']))
        # slower CPU (an older phone): the biggest session
        big = max(ids, key=lambda s: os.path.getsize(next(os.path.join(card, 'sessions', s, n) for n in os.listdir(os.path.join(card, 'sessions', s))
                                                          if n.endswith('.csv') and not n.endswith('_laps.csv'))))
        cdp = await ctx.new_cdp_session(page)
        await cdp.send('Emulation.setCPUThrottlingRate', {'rate': 4})
        await page.goto(mock.url + '/report?id=' + big)
        await page.wait_for_function('window.__ltReport', timeout=60000)
        r = await page.evaluate('window.__ltReport')
        check(r.get('ready') and r['ms'] < 3000, 'report %s with the CPU slowed 4x: page code %d ms for %d rows (limit 3000 ms)' % (big, r['ms'], r['rows']))
        await cdp.send('Emulation.setCPUThrottlingRate', {'rate': 1})

        # ---- fetch mode
        sid = ids[0]
        laps = read_laps(os.path.join(card, 'sessions', sid))
        await page.goto(mock.url + '/report.html?id=' + sid)
        await page.wait_for_function('window.__ltReport', timeout=30000)
        ok, why = compare_laps(await lap_table(page), laps)
        check(ok, 'report.html?id= (fetching /api/reportdata): %s' % why)

        # ---- downloads
        await page.goto(mock.url + '/')
        await page.wait_for_function('window.__ltIndex && window.__ltIndex.ready', timeout=20000)
        async with page.expect_download() as dl_info:
            await page.click('#sessions .card[data-id="%s"] a[href*="save=1"]' % sid)
        dl = await dl_info.value
        saved = os.path.join(out, dl.suggested_filename)
        await dl.save_as(saved)
        check(dl.suggested_filename == 'report_%s.html' % sid, 'download: report saved as %s' % dl.suggested_filename)
        time.sleep(0.3)
        check(os.path.exists(os.path.join(card, 'sessions', sid, '.dl_report.html')), 'download: marker .dl_report.html written')
        await page.goto('file://' + os.path.abspath(saved))
        await page.wait_for_function('window.__ltReport', timeout=30000)
        ok, why = compare_laps(await lap_table(page), laps)
        check(ok, 'download: the saved file opened from disk (no device): %s' % why)
        await page.goto(mock.url + '/')
        await page.wait_for_function('window.__ltIndex && window.__ltIndex.ready', timeout=20000)
        vbo = next(n for n in os.listdir(os.path.join(card, 'sessions', ids[0])) if n.endswith('.vbo'))
        async with page.expect_download() as dl_info:
            await page.click('#sessions .card[data-id="%s"] a[href*="%s"]' % (ids[0], vbo))
        dl = await dl_info.value
        tmp = os.path.join(tempfile.gettempdir(), 'lt_dl_' + vbo)
        await dl.save_as(tmp)
        with open(tmp, 'rb') as f1, open(os.path.join(card, 'sessions', ids[0], vbo), 'rb') as f2:
            same = f1.read() == f2.read()
        os.remove(tmp)
        check(same and dl.suggested_filename == vbo, 'download: %s byte-identical (CR LF kept), name kept' % vbo)
        time.sleep(0.3)
        await page.click('#reload')
        await page.wait_for_timeout(500)
        badge = await page.evaluate("document.querySelector('#sessions .card[data-id=\"%s\"] .badge').textContent" % ids[0])
        check(badge == 'ダウンロード済み', 'download: the card now says %s' % badge)

        # ---- uploads
        cfg = os.path.join(card, 'config', 'courses.json')
        with open(cfg, 'rb') as f:
            original = f.read()
        good = json.loads(original.decode('utf-8'))
        good['courses'][0]['name'] = good['courses'][0]['name'] + '（試験）'
        good_bytes = json.dumps(good, ensure_ascii=False, indent=2).encode('utf-8')
        up_dir = tempfile.mkdtemp(prefix='lt_up_')
        good_path = os.path.join(up_dir, 'courses 2.json')
        with open(good_path, 'wb') as f:
            f.write(good_bytes)
        row = '#configs .cfg[data-name="courses.json"]'
        await page.set_input_files(row + ' input[type=file]', good_path)
        await page.wait_for_function("document.querySelector('%s .verdict').textContent.includes('保存しました')" % row, timeout=10000)
        with open(cfg, 'rb') as f:
            check(f.read() == good_bytes, 'upload: a good courses.json (picked as "courses 2.json") replaced the stored one')
        # broken: the comma after the first gate removed (line / column known)
        text = good_bytes.decode('utf-8')
        cut = text.index('},', text.index('"gates"'))
        broken = text[:cut + 1] + text[cut + 2:]
        line = broken[:cut + 1].count('\n') + 1
        bad_path = os.path.join(up_dir, 'courses.json')
        with open(bad_path, 'w', encoding='utf-8') as f:
            f.write(broken)
        await page.set_input_files(row + ' input[type=file]', bad_path)
        await page.wait_for_function("document.querySelector('%s .verdict').textContent.includes('本機の検査')" % row, timeout=10000)
        verdict = await page.evaluate("document.querySelector('%s .verdict').textContent" % row)
        with open(cfg, 'rb') as f:
            unchanged = f.read() == good_bytes
        check(unchanged and ('%d 行目' % (line + 1)) in verdict and '必要です' in verdict and '変わっていません' in verdict,
              'upload: a broken courses.json refused with the place and the reason, stored file unchanged: %s' % verdict.replace('\n', ' / '))
        # wrong shape: a checklist sent as courses.json stays in the page
        shape_path = os.path.join(up_dir, 'checklist.json')
        with open(shape_path, 'w', encoding='utf-8') as f:
            f.write('{"version":1,"sport":true}')
        await page.set_input_files(row + ' input[type=file]', shape_path)
        await page.wait_for_timeout(300)
        verdict = await page.evaluate("document.querySelector('%s .verdict').textContent" % row)
        with open(cfg, 'rb') as f:
            unchanged = f.read() == good_bytes
        check(unchanged and '形ではありません' in verdict, 'upload: a checklist picked for courses.json is not sent: %s' % verdict.replace('\n', ' / '))
        await page.set_input_files(row + ' input[type=file]', bad_path)
        await page.wait_for_function("document.querySelector('%s .verdict').textContent.includes('本機の検査')" % row, timeout=10000)
        await page.locator('#s-config').screenshot(path=os.path.join(out, 'upload.png'))
        shutil.rmtree(up_dir)

        # ---- maintenance editor
        await page.locator('#s-maint').scroll_into_view_if_needed()
        first = page.locator('#maint .mi').first
        await first.locator('button', has_text='編集').click()
        await first.locator('input[data-k=interval_km]').fill('4321')
        await first.screenshot(path=os.path.join(out, 'maint_edit.png'))
        await page.click('#msend')
        await page.wait_for_function("document.querySelector('#mverdict').textContent.includes('保存しました')", timeout=10000)
        with open(os.path.join(card, 'config', 'maint.json'), encoding='utf-8') as f:
            m = json.load(f)
        check(m['items'][0]['interval_km'] == 4321 and m['items'][0].get('last_odo', 'x') is None and '_note' in m
              and '_open' not in m['items'][0],
              'maint: edited item stored (interval_km 4321; last_odo still null = not recorded; _note kept; no page-only keys)')
        await page.locator('#maint .mi').nth(1).locator('button', has_text='実施済み').click()  # nothing unsent: no dialog
        await page.wait_for_timeout(800)
        with open(os.path.join(card, 'config', 'maint.json'), encoding='utf-8') as f:
            m = json.load(f)
        check(bool(m['items'][1].get('last_date')) and m['items'][1].get('last_odo') is not None,
              'maint: 実施済み set %s to %s / %s km' % (m['items'][1]['id'], m['items'][1].get('last_date'), m['items'][1].get('last_odo')))
        # ODO base value
        await page.fill('#odokm', '23456.7')
        page.once('dialog', lambda d: asyncio.ensure_future(d.accept()))
        await page.click('#odoset')
        await page.wait_for_function("document.querySelector('#odoinfo').textContent.includes('ODO')", timeout=10000)
        info = await page.evaluate("document.querySelector('#odoinfo').textContent")
        st, _, body = request(mock.url + '/api/status')
        odo = json.loads(body.decode('utf-8')).get('odo_km') if st == 200 else None
        mi = await page.evaluate("document.querySelector('#maintinfo').textContent")
        check(odo == 23456.7 and 'にしました' in info and '23,457 km' in mi,
              'maint: ODO set from the page (status odo_km %s; %s; info shows %s)' % (odo, info, '23,457 km' in mi))
        st, _, body = request(mock.url + '/api/odo?km=abc', 'POST')
        check(st == 400 and b'bad-km' in body, 'maint: ODO "abc" refused (%d)' % st)
        await page.locator('#s-maint').screenshot(path=os.path.join(out, 'maint.png'))

        # ---- course from a RaceChrono VBO
        await page.set_input_files('#vbopick', a.vbo)
        await page.wait_for_function("!document.querySelector('#vbosend').disabled", timeout=20000)
        info = await page.evaluate("document.querySelector('#vboinfo').textContent")
        check('線 4 本' in info, 'course: the VBO head read in the page: %s' % info)
        await page.click('#vbosend')
        await page.wait_for_function("document.querySelector('#vboinfo').textContent.includes('コースに')", timeout=10000)
        info = await page.evaluate("document.querySelector('#vboinfo').textContent")
        check('コースにしました' in info, 'course: %s' % info)
        await page.locator('#s-course').screenshot(path=os.path.join(out, 'course.png'))

        # ---- delete
        await page.goto(mock.url + '/')
        await page.wait_for_function('window.__ltIndex && window.__ltIndex.ready', timeout=20000)
        if active:
            dis = await page.evaluate("document.querySelector('#sessions .card[data-id=\"%s\"] button.danger').disabled" % active)
            st, _, body = request(mock.url + '/api/delete?id=' + active, 'POST')
            check(dis and st == 409 and os.path.isdir(os.path.join(card, 'sessions', active)),
                  'delete: the active session %s cannot be deleted (button off, API 409)' % active)
        victim = ids[0]
        msgs = []
        page.once('dialog', lambda d: (msgs.append(d.message), asyncio.ensure_future(d.accept())))
        await page.click('#sessions .card[data-id="%s"] button.danger' % victim)
        await page.wait_for_function("!document.querySelector('#sessions .card[data-id=\"%s\"]')" % victim, timeout=10000)
        check(not os.path.exists(os.path.join(card, 'sessions', victim)) and msgs and '元に戻せません' in msgs[0],
              'delete: %s deleted after the confirm (%s)' % (victim, msgs[0].split('\n')[0] if msgs else 'no dialog'))
        await page.screenshot(path=os.path.join(out, 'delete.png'), full_page=True)
        check(not errors, 'no script errors in the pages %s' % errors[:3])
        await ctx.close()

        # ---- WebDAV (mock)
        st, h, _ = request(mock.url + '/', 'OPTIONS')
        check(st == 200 and h.get('DAV') == '1', 'webdav: OPTIONS DAV %s' % h.get('DAV'))
        st, _, body = request(mock.url + '/', 'PROPFIND', {'Depth': '1'})
        doc = xml.dom.minidom.parseString(body)
        hrefs = [e.firstChild.data for e in doc.getElementsByTagName('D:href')]
        check(st == 207 and '/sessions/' in hrefs and '/config/' in hrefs, 'webdav: PROPFIND / %d %s' % (st, hrefs))
        st, _, body = request(mock.url + '/config/', 'PROPFIND', {'Depth': '1'})
        hrefs = [e.firstChild.data for e in xml.dom.minidom.parseString(body).getElementsByTagName('D:href')]
        check('/config/wifi.json' not in hrefs and not any('/.' in x for x in hrefs), 'webdav: /config/ hides wifi.json and dot files %s' % hrefs)
        st, _, _ = request(mock.url + '/config/test.txt', 'PUT', data=b'x')
        check(st in (403, 405), 'webdav: PUT refused (%d)' % st)
        mock.close()

        # ---- format (a card without FAT)
        mock = Mock(card, a.report_bin, ['--unmounted'])
        ctx = await browser.new_context(viewport={'width': 390, 'height': 844}, device_scale_factor=2, is_mobile=True, has_touch=True)
        page = await ctx.new_page()
        await page.goto(mock.url + '/')
        await page.wait_for_function('window.__ltIndex && window.__ltIndex.ready', timeout=20000)
        shown = await page.locator('#format').is_visible()
        await page.locator('#s-status').screenshot(path=os.path.join(out, 'format.png'))
        page.once('dialog', lambda d: asyncio.ensure_future(d.accept()))
        await page.click('#format')
        await page.wait_for_function("document.querySelector('#format').hidden", timeout=10000)
        check(shown and not await page.locator('#format').is_visible(), 'format: shown only while the XTSD is not mounted, hidden after formatting')
        await ctx.close()
        mock.close()
        await browser.close()


def main():
    ap = argparse.ArgumentParser(description='page tests')
    ap.add_argument('--report-bin', required=True)
    ap.add_argument('--vbo', required=True, help='RaceChrono VBO export (also the course of the .rcz)')
    ap.add_argument('--rcz', action='append', default=[], help='unzipped RaceChrono .rcz directory')
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'web'))
    chromes = ['/usr/bin/google-chrome', '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome']
    ap.add_argument('--chrome', default=next((c for c in chromes if os.path.exists(c)), chromes[0]))
    a = ap.parse_args()
    try:
        import playwright  # noqa: F401
    except ImportError:
        print('FAIL python playwright is not installed (pip install playwright)')
        print('RESULT: FAIL')
        return 1
    card = tempfile.mkdtemp(prefix='lt_card_')
    try:
        os.makedirs(os.path.join(card, 'config'))
        for n in ('courses.json', 'maint.json', 'checklist.json'):
            src = os.path.join(ROOT, 'data', n)
            if os.path.exists(src):
                shutil.copy(src, os.path.join(card, 'config', n))
        courses = os.path.join(card, 'config', 'courses.json')
        sessions = []
        for inp in [a.vbo] + a.rcz:
            cmd = [a.report_bin, '--session', inp, card, '--course', a.vbo]
            if os.path.exists(courses):
                cmd += ['--courses', courses]
            r = subprocess.run(cmd, capture_output=True, text=True)
            ok = r.returncode == 0
            check(ok, 'card: %s' % (r.stdout.strip() or r.stderr.strip()))
            if ok:
                sessions.append(os.path.basename(r.stdout.split()[1]))
        if sessions:
            asyncio.run(run(a, card, sessions))
    finally:
        shutil.rmtree(card, ignore_errors=True)
    print('RESULT: %s' % ('FAIL (%d)' % len(FAILS) if FAILS else 'PASS'))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
# ============================================================================
# test_http_server.py — тестовый http-сервер для scripts/test_stage48_2_net.sh.
# ----------------------------------------------------------------------------
# Запуск:  python3 tools/test_http_server.py <порт> <каталог>
# В <каталог> при старте пишутся файлы big.html / huge.html (их контрольные
# суммы считает сам тест) — и отдаются те же байты. Из гостя QEMU (slirp)
# сервер виден как http://10.0.2.2:<порт>/.
#
#   /index.html   маленькая страница со ссылками (первая — на page2.html)
#   /page2.html   вторая страница
#   /big.html     ~600 КБ с Content-Length (больше старого предела 128 КБ)
#   /chunked      те же байты, что big.html, но Transfer-Encoding: chunked
#   /noclen       те же байты без Content-Length: конец — закрытие соединения
#   /redirect     302 -> /page2.html
#   /loop         302 -> /loop (бесконечные перенаправления)
#   /missing      404 с телом
#   /huge.html    3 МБ (браузер обязан обрезать на 2 МБ и сказать об этом)
#   /stall        заголовки и начало тела, затем тишина 60 с (проверка отмены)
#   /drop         соединение закрывается без ответа
#   /plain.txt    text/plain в UTF-8 с кириллицей
#   /image.png    image/png (браузер такое не показывает)
#   /blob.bin     100 КБ случайных байт (замер netbench)
# ============================================================================
import gzip
import os
import sys
import time
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1])
ROOT = sys.argv[2]


def make_big(paragraphs):  # ~290 байт на абзац
    parts = ['<!doctype html><html><head><meta charset="utf-8"><title>Big page</title></head><body>',
             '<h1>Big test page</h1><p>Start marker. <a href="/page2.html">second page</a></p>']
    for i in range(paragraphs):
        parts.append('<h2>Section %d</h2><p>Paragraph %d of the big page. '
                     'The quick brown fox jumps over the lazy dog. '
                     'Съешь ещё этих мягких французских булок, да выпей чаю. '
                     '<b>bold</b> <code>code %d</code> <a href="/page2.html?from=%d">link %d</a></p>'
                     % (i, i, i, i, i))
        if i % 7 == 0:
            parts.append('<ul><li>first item %d</li><li>second item</li></ul>' % i)
    parts.append('<p>END-OF-BIG-PAGE</p></body></html>')
    return ''.join(parts).encode('utf-8')


INDEX = ('<!doctype html><html><head><meta charset="utf-8"><title>Index</title></head><body>'
         '<h1>AMPLIFY test site</h1>'
         '<p><a href="page2.html">Go to the second page</a> and back.</p>'
         '<h2>More</h2><ul>'
         '<li><a href="/big.html">big page</a></li>'
         '<li><a href="/chunked">chunked page</a></li>'
         '<li><a href="/missing">missing page</a></li></ul>'
         '<p>Русский текст: проверка кодировки UTF-8.</p>'
         '<pre>preformatted   text\n  second line</pre>'
         '</body></html>').encode('utf-8')

PAGE2 = ('<!doctype html><html><head><meta charset="utf-8"><title>Second</title></head><body>'
         '<h1>Second page</h1><p><a href="/index.html">Back to index</a></p>'
         '<blockquote><p>Quoted paragraph.</p></blockquote>'
         '<ol><li>one</li><li>two</li><li>three</li></ol></body></html>').encode('utf-8')

# --- подэтап 48.2, «настоящие сайты»: то, на чём спотыкался браузер ---------
REFRESH = ('<html><head><meta http-equiv="refresh" content="0; url=/page2.html">'
           '<title>Moved</title></head><body><p>Redirecting...</p></body></html>').encode('utf-8')

# Видны должны быть две ссылки: обычная и та, что в <noscript>.
HIDDEN = ('<!doctype html><html><head><meta charset="utf-8"><title>Hidden</title></head><body>'
          '<h1>Hidden test</h1>'
          '<div hidden><a href="/h1">hidden attr</a><div>nested <div>deeper</div></div><a href="/h2">x</a></div>'
          '<p style="color:red; display: none"><a href="/h3">display none</a></p>'
          '<span aria-hidden="true"><a href="/h4">aria hidden</a></span>'
          '<button type="button">dead button <a href="/h5">inside button</a></button>'
          '<dialog><a href="/h6">closed dialog</a></dialog>'
          '<p class="hidden"><a href="/page2.html">visible link</a> (class is not an attribute)</p>'
          # Потоковая отрисовка React: настоящее содержимое лежит в <div hidden id="S:0">.
          '<template id="B:0"></template><div hidden id="S:0"><p><a href="/big.html">streamed content</a></p></div>'
          '<noscript><p><a href="/index.html">noscript link</a></p></noscript>'
          '</body></html>').encode('utf-8')

FORM = ('<!doctype html><html><head><meta charset="utf-8"><title>Form</title></head><body>'
        '<h1>Search form</h1>'
        '<form action="/find" method="get"><input type="hidden" name="h" value="1">'
        '<input type="text" name="q" placeholder="Что ищем"><input type="submit" value="Go"></form>'
        '</body></html>').encode('utf-8')

UTF = ('<!doctype html><html><head><meta charset="utf-8"><title>UTF</title></head><body>'
       '<p><a href="/путь?я=1">кириллическая ссылка</a></p></body></html>').encode('utf-8')


def make_mainjump():
    parts = ['<!doctype html><html><head><meta charset="utf-8"><title>Main</title></head><body><ul>']
    for i in range(90):
        parts.append('<li><a href="/page2.html?nav=%d">menu item %d</a></li>' % (i, i))
    parts.append('</ul><main><h1>Main content starts here</h1>')
    for i in range(60):
        parts.append('<p>Paragraph %d of the article body.</p>' % i)
    parts.append('</main></body></html>')
    return ''.join(parts).encode('utf-8')


MAINJUMP = make_mainjump()
LONG_Q = 'x=' + 'a' * 1500

BIG = make_big(2200)
HUGE = make_big(16500)     # ~4.8 МБ: больше предела браузера (4 МБ)
BLOB = os.urandom(100000)
MISSING = b'<html><head><title>404</title></head><body><h1>Not found</h1><p>No such page here.</p></body></html>'
PLAIN = 'Plain text file.\nВторая строка по-русски.\n'.encode('utf-8')

os.makedirs(ROOT, exist_ok=True)
for name, data in (('big.html', BIG), ('huge.html', HUGE)):
    with open(os.path.join(ROOT, name), 'wb') as f:
        f.write(data)


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, fmt, *args):
        pass

    def send_body(self, code, ctype, body):
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Connection', 'close')
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split('?')[0]
        self.close_connection = True
        if path in ('/', '/index.html'):
            self.send_body(200, 'text/html; charset=utf-8', INDEX)
        elif path == '/page2.html':
            self.send_body(200, 'text/html; charset=utf-8', PAGE2)
        elif path in ('/gzip', '/deflate', '/zlib'):
            # Сжатый ответ (те же байты, что big.html): gzip, «сырой» deflate
            # и zlib — серверы шлют Content-Encoding: deflate и так, и так.
            if path == '/gzip':
                body, enc = gzip.compress(BIG, 6), 'gzip'
            elif path == '/zlib':
                body, enc = zlib.compress(BIG, 6), 'deflate'
            else:
                co = zlib.compressobj(6, zlib.DEFLATED, -15)
                body, enc = co.compress(BIG) + co.flush(), 'deflate'
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Encoding', enc)
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Connection', 'close')
            self.end_headers()
            self.wfile.write(body)
        elif path == '/cookie':
            # Без cookie — перенаправление на себя с Set-Cookie: клиент без
            # хранилища cookies попадает в бесконечную петлю.
            if 'sid=amplify42' in (self.headers.get('Cookie') or ''):
                self.send_body(200, 'text/html; charset=utf-8',
                               b'<html><head><title>Cookie</title></head><body><h1>Cookie accepted</h1>'
                               b'<p><a href="/index.html">index</a></p></body></html>')
            else:
                self.send_response(302)
                self.send_header('Location', '/cookie')
                self.send_header('Set-Cookie', 'other=1; Path=/elsewhere')
                self.send_header('Set-Cookie', 'sid=amplify42; Path=/; HttpOnly')
                self.send_header('Content-Length', '0')
                self.send_header('Connection', 'close')
                self.end_headers()
        elif path == '/hints':
            # Промежуточный ответ 103 и сразу настоящий — одним куском.
            body = PAGE2
            self.wfile.write(b'HTTP/1.1 103 Early Hints\r\nLink: </x.css>; rel=preload\r\n\r\n'
                             b'HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n'
                             b'Content-Length: ' + str(len(body)).encode() + b'\r\nConnection: close\r\n\r\n' + body)
        elif path == '/refresh':
            self.send_body(200, 'text/html; charset=utf-8', REFRESH)
        elif path == '/hidden':
            self.send_body(200, 'text/html; charset=utf-8', HIDDEN)
        elif path == '/octet':
            # HTML под «никаким» типом: узнаётся по содержимому.
            self.send_body(200, 'application/octet-stream', PAGE2)
        elif path == '/form':
            self.send_body(200, 'text/html; charset=utf-8', FORM)
        elif path == '/find':
            query = self.path.split('?', 1)[1] if '?' in self.path else ''
            if query == 'q=two+words':
                self.send_body(200, 'text/html; charset=utf-8',
                               b'<html><head><title>Found</title></head><body><h1>Query accepted</h1></body></html>')
            else:
                self.send_body(404, 'text/html', MISSING)
        elif path == '/utf':
            self.send_body(200, 'text/html; charset=utf-8', UTF)
        elif path == '/%D0%BF%D1%83%D1%82%D1%8C':
            # Кириллица в ссылке обязана прийти в виде %XX (и путь, и запрос).
            query = self.path.split('?', 1)[1] if '?' in self.path else ''
            self.send_body(200 if query == '%D1%8F=1' else 404, 'text/html; charset=utf-8', PAGE2)
        elif path == '/mainjump':
            self.send_body(200, 'text/html; charset=utf-8', MAINJUMP)
        elif path == '/long':
            # Перенаправление на адрес длиннее прежнего предела (768 байт).
            self.send_response(302)
            self.send_header('Location', '/longdst?' + LONG_Q)
            self.send_header('Content-Length', '0')
            self.send_header('Connection', 'close')
            self.end_headers()
        elif path == '/longdst':
            query = self.path.split('?', 1)[1] if '?' in self.path else ''
            self.send_body(200 if query == LONG_Q else 400, 'text/html; charset=utf-8', PAGE2)
        elif path == '/big.html':
            self.send_body(200, 'text/html; charset=utf-8', BIG)
        elif path == '/huge.html':
            self.send_body(200, 'text/html; charset=utf-8', HUGE)
        elif path == '/blob.bin':
            self.send_body(200, 'application/octet-stream', BLOB)
        elif path == '/plain.txt':
            self.send_body(200, 'text/plain; charset=utf-8', PLAIN)
        elif path == '/image.png':
            self.send_body(200, 'image/png', b'\x89PNG\r\n\x1a\n' + b'\0' * 64)
        elif path == '/missing':
            self.send_body(404, 'text/html', MISSING)
        elif path == '/redirect':
            self.send_response(302)
            self.send_header('Location', '/page2.html')
            self.send_header('Content-Length', '0')
            self.send_header('Connection', 'close')
            self.end_headers()
        elif path == '/loop':
            self.send_response(302)
            self.send_header('Location', '/loop')
            self.send_header('Content-Length', '0')
            self.send_header('Connection', 'close')
            self.end_headers()
        elif path == '/chunked':
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Transfer-Encoding', 'chunked')
            self.send_header('Connection', 'close')
            self.end_headers()
            pos, n = 0, 0
            sizes = (1, 7, 1460, 4096, 333, 16384, 2, 65536, 5000)
            while pos < len(BIG):
                size = sizes[n % len(sizes)]
                n += 1
                piece = BIG[pos:pos + size]
                pos += len(piece)
                ext = b';note=x' if n % 3 == 0 else b''
                self.wfile.write(b'%x' % len(piece) + ext + b'\r\n' + piece + b'\r\n')
            self.wfile.write(b'0\r\nX-Trailer: done\r\n\r\n')
        elif path == '/noclen':
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Connection', 'close')
            self.end_headers()
            self.wfile.write(BIG)
        elif path == '/stall':
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', '500000')
            self.send_header('Connection', 'close')
            self.end_headers()
            self.wfile.write(b'<html><body><p>partial</p>' + b'x' * 4000)
            self.wfile.flush()
            try:
                time.sleep(60)
            except Exception:
                pass
        elif path == '/drop':
            self.connection.close()
        else:
            self.send_body(404, 'text/html', MISSING)


srv = ThreadingHTTPServer(('127.0.0.1', PORT), Handler)
srv.daemon_threads = True
srv.serve_forever()

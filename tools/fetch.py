"""fetch.py: a file downloaded by Windows' own HTTP stack (WinHTTP), so a server's certificate is checked by Windows'
own chain engine, the one its browsers and Windows Update use. tools/deps.py and tools/kb_demo.py download through it.

    import fetch
    fetch.download('https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz', 'third_party/_deps/ffmpeg.tar.xz')

Why not urllib: Python's ssl on Windows hands OpenSSL every certificate of the system's ROOT and CA stores as a trust
anchor, the expired ones too, and never asks Windows for a root it has not fetched yet. The CA store is Windows' cache
of intermediates seen while browsing, and a stale one there decides OpenSSL's chain: ISRG Root X2 cross-signed by
ISRG Root X1 (expired 2025-09-15) kept in a player's CA store made every Let's Encrypt host chained through Root YE
(ffmpeg.org, videolan.org) fail with "certificate has expired" (2026-10-26), while Windows builds the path through
the cross-sign the server sends (valid until 2032). Windows' engine takes the time-valid path, fetches a missing
root from Windows Update, and trusts what the machine's administrator (or a proxy or antivirus that reads HTTPS)
put into the stores, as the browsers do. The proxy is the system's (WinHTTP's automatic proxy: the settings, WPAD).

A certificate Windows refuses is told by name (out of its dates: the clock; an unknown root: a proxy or antivirus);
an HTTP status other than 200 stops the download. Redirects are followed, HTTPS to HTTP never. Elsewhere than
Windows: urllib. Only standard library.
"""
import datetime
import os
import sys


class FetchError(Exception):
    pass


if os.name == 'nt':
    import ctypes
    from ctypes import wintypes as wt

    _http = ctypes.WinDLL('winhttp', use_last_error=True)
    _kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    HINTERNET = ctypes.c_void_p
    DWORD_PTR = ctypes.c_size_t
    STATUS_CALLBACK = ctypes.WINFUNCTYPE(None, HINTERNET, DWORD_PTR, wt.DWORD, ctypes.c_void_p, wt.DWORD)

    def _api(dll, name, restype, *argtypes):
        f = getattr(dll, name)
        f.restype, f.argtypes = restype, argtypes
        return f

    WinHttpOpen = _api(_http, 'WinHttpOpen', HINTERNET, wt.LPCWSTR, wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD)
    WinHttpConnect = _api(_http, 'WinHttpConnect', HINTERNET, HINTERNET, wt.LPCWSTR, wt.WORD, wt.DWORD)
    WinHttpOpenRequest = _api(_http, 'WinHttpOpenRequest', HINTERNET, HINTERNET, wt.LPCWSTR, wt.LPCWSTR, wt.LPCWSTR,
                              wt.LPCWSTR, ctypes.c_void_p, wt.DWORD)
    WinHttpSetStatusCallback = _api(_http, 'WinHttpSetStatusCallback', ctypes.c_void_p, HINTERNET, STATUS_CALLBACK,
                                    wt.DWORD, DWORD_PTR)
    WinHttpSendRequest = _api(_http, 'WinHttpSendRequest', wt.BOOL, HINTERNET, wt.LPCWSTR, wt.DWORD, ctypes.c_void_p,
                              wt.DWORD, wt.DWORD, DWORD_PTR)
    WinHttpReceiveResponse = _api(_http, 'WinHttpReceiveResponse', wt.BOOL, HINTERNET, ctypes.c_void_p)
    WinHttpQueryHeaders = _api(_http, 'WinHttpQueryHeaders', wt.BOOL, HINTERNET, wt.DWORD, wt.LPCWSTR, ctypes.c_void_p,
                               ctypes.POINTER(wt.DWORD), ctypes.POINTER(wt.DWORD))
    WinHttpReadData = _api(_http, 'WinHttpReadData', wt.BOOL, HINTERNET, ctypes.c_void_p, wt.DWORD,
                           ctypes.POINTER(wt.DWORD))
    WinHttpCloseHandle = _api(_http, 'WinHttpCloseHandle', wt.BOOL, HINTERNET)
    FormatMessageW = _api(_kernel, 'FormatMessageW', wt.DWORD, wt.DWORD, ctypes.c_void_p, wt.DWORD, wt.DWORD,
                          wt.LPWSTR, wt.DWORD, ctypes.c_void_p)

    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY = 4   # (Windows 8.1 and later)
    WINHTTP_FLAG_SECURE = 0x00800000
    WINHTTP_QUERY_STATUS_CODE = 19
    WINHTTP_QUERY_FLAG_NUMBER = 0x20000000
    WINHTTP_CALLBACK_FLAG_SECURE_FAILURE = 0x00010000
    WINHTTP_CALLBACK_STATUS_SECURE_FAILURE = 0x00010000
    ERROR_WINHTTP_SECURE_FAILURE = 12175
    # WINHTTP_CALLBACK_STATUS_FLAG_*: what Windows found wrong with the server's certificate (winhttp.h)
    SECURE_FAILURES = (
        (0x00000020, 'a certificate of its chain is out of its dates: is this computer\'s clock right? (it says %s)'),
        (0x00000008, 'its chain ends at a root this computer does not trust (a proxy or an antivirus that reads HTTPS '
                     'puts its own certificate in: let it pass this host)'),
        (0x00000010, 'the certificate is for another name'),
        (0x00000004, 'the certificate was revoked'),
        (0x00000001, 'its revocation could not be checked'),
        (0x00000002, 'the certificate is invalid'),
        (0x80000000, 'the secure channel failed'),
    )

    def _message(code):
        """A WinHTTP or system error's text (WinHTTP's are in winhttp.dll's message table)."""
        buf = ctypes.create_unicode_buffer(512)
        n = FormatMessageW(0x800 | 0x1000 | 0x200, _http._handle, code, 0, buf, len(buf), None)
        return '%s (%d)' % (buf.value[:n].strip() if n else 'error', code)

    def _check(ok, what, url):
        if not ok:
            raise FetchError('%s: %s failed: %s' % (url, what, _message(ctypes.get_last_error())))
        return ok

    def download(url, path, chunk=1 << 20):
        """url's body written into path (created or replaced); FetchError when it cannot be had."""
        from urllib.parse import urlsplit
        u = urlsplit(url)
        if u.scheme not in ('https', 'http') or not u.hostname:
            raise FetchError('%s: not an HTTP or HTTPS address' % url)
        secure = u.scheme == 'https'
        where = (u.path or '/') + ('?' + u.query if u.query else '')
        failure = [0]

        def on_status(handle, context, status, info, length):
            if status == WINHTTP_CALLBACK_STATUS_SECURE_FAILURE and info:
                failure[0] = ctypes.cast(info, ctypes.POINTER(wt.DWORD))[0]
        callback = STATUS_CALLBACK(on_status)   # (kept alive until the request is closed)

        handles = []
        try:
            session = _check(WinHttpOpen('MafiaModLoader-fetch/1', WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, None, None, 0),
                             'WinHttpOpen', url)
            handles.append(session)
            conn = _check(WinHttpConnect(session, u.hostname, u.port or (443 if secure else 80), 0), 'connecting', url)
            handles.append(conn)
            req = _check(WinHttpOpenRequest(conn, 'GET', where, None, None, None, WINHTTP_FLAG_SECURE if secure else 0),
                         'WinHttpOpenRequest', url)
            handles.append(req)
            WinHttpSetStatusCallback(req, callback, WINHTTP_CALLBACK_FLAG_SECURE_FAILURE, 0)
            if not (WinHttpSendRequest(req, None, 0, None, 0, 0, 0) and WinHttpReceiveResponse(req, None)):
                err = ctypes.get_last_error()
                if err == ERROR_WINHTTP_SECURE_FAILURE:
                    now = datetime.datetime.now().astimezone().isoformat(' ', 'minutes')
                    why = [text % now if '%s' in text else text for bit, text in SECURE_FAILURES if failure[0] & bit]
                    raise FetchError('%s: Windows does not trust the server\'s certificate: %s' %
                                     (url, '; '.join(why) or _message(err)))
                raise FetchError('%s: %s' % (url, _message(err)))
            status, size = wt.DWORD(), wt.DWORD(4)
            _check(WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, None,
                                       ctypes.byref(status), ctypes.byref(size), None), 'reading the status', url)
            if status.value != 200:
                raise FetchError('%s: HTTP %d' % (url, status.value))
            buf, got = ctypes.create_string_buffer(chunk), wt.DWORD()
            with open(path, 'wb') as f:
                while True:
                    _check(WinHttpReadData(req, buf, chunk, ctypes.byref(got)), 'reading', url)
                    if not got.value:
                        break
                    f.write(memoryview(buf)[:got.value])
        finally:
            for h in reversed(handles):
                WinHttpCloseHandle(h)
else:
    def download(url, path, chunk=1 << 20):
        """url's body written into path (created or replaced); FetchError when it cannot be had."""
        import shutil
        import urllib.error
        import urllib.request
        try:
            with urllib.request.urlopen(url) as r, open(path, 'wb') as f:
                shutil.copyfileobj(r, f, chunk)
        except urllib.error.URLError as e:
            raise FetchError('%s: %s' % (url, e)) from None


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit('python tools/fetch.py <url> <file>')
    try:
        download(sys.argv[1], sys.argv[2])
    except FetchError as e:
        sys.exit(str(e))

/* Smoke test for the browser (wasm) build: boots the demo module and drives
 * the same connection API the page uses - GET, HEAD, ranges, chunked POST,
 * pipelining, keep-alive, traversal and smuggling rejection. Run after
 * sh wasm/build.sh: node tests/wasm-smoke.js [path/to/mast.js] */
const factory = require(process.argv[2] || require("path").join(__dirname, "..", "docs", "mast.js"));
let fails = 0;
function ok(cond, label) {
    if (cond) console.log("ok: " + label);
    else { console.log("FAIL: " + label); fails = 1; }
}
factory({}).then(m => {
    const td = new TextDecoder("utf-8");
    m.ccall("mast_boot", "number", [], []);
    const newConn = () => m.ccall("mast_conn_new", "number", [], []);
    const send = (conn, text) => {
        const bytes = new TextEncoder().encode(text);
        const ptr = m._malloc(Math.max(bytes.length, 1));
        m.writeArrayToMemory(bytes, ptr);
        m.ccall("mast_recv", "number", ["number", "number", "number"], [conn, ptr, bytes.length]);
        m._free(ptr);
        const len = m.ccall("mast_out_len", "number", ["number"], [conn]);
        const optr = m.ccall("mast_out_ptr", "number", ["number"], [conn]);
        const out = td.decode(m.HEAPU8.slice(optr, optr + len));
        m.ccall("mast_out_drain", null, ["number"], [conn]);
        return out;
    };
    const statusOf = r => parseInt(r.split(" ", 2)[1], 10);

    let c = newConn();
    let r = send(c, "GET / HTTP/1.1\r\nHost: demo\r\n\r\n");
    ok(statusOf(r) === 200 && r.includes("hello from mast"), "wasm: GET / serves index.html");
    ok(r.includes("ETag:") && r.includes("Last-Modified:") && r.includes("Accept-Ranges: bytes"),
       "wasm: validator headers present");

    /* keep-alive: a second request on the same connection */
    r = send(c, "GET /hello.txt HTTP/1.1\r\nHost: demo\r\n\r\n");
    ok(statusOf(r) === 200 && r.includes("hello, world"), "wasm: keep-alive second request");
    ok(m.ccall("mast_conn_requests", "number", ["number"], [c]) === 2, "wasm: connection counts requests");
    ok(!m.ccall("mast_conn_closed", "number", ["number"], [c]), "wasm: connection still open");

    /* pipelining: two requests, one feed, two ordered responses */
    r = send(c, "GET /hello.txt HTTP/1.1\r\nHost: demo\r\n\r\nGET /data.json HTTP/1.1\r\nHost: demo\r\n\r\n");
    ok((r.match(/HTTP\/1.1 200/g) || []).length === 2, "wasm: pipelined requests both answered");
    ok(r.indexOf("hello, world") < r.indexOf('"served": "by mast"'), "wasm: pipelined order preserved");

    /* fragmented: byte-by-byte feed */
    let frag = "";
    const line = "GET /style.css HTTP/1.1\r\nHost: demo\r\n\r\n";
    for (const ch of line) frag += send(c, ch);
    ok(statusOf(frag) === 200 && frag.includes("font-family"), "wasm: byte-by-byte request answered");

    r = send(c, "HEAD /big.txt HTTP/1.1\r\nHost: demo\r\n\r\n");
    ok(statusOf(r) === 200 && r.includes("Content-Length: 65536") && !r.includes("AAAA"),
       "wasm: HEAD reports length, sends no body");

    r = send(c, "GET /big.txt HTTP/1.1\r\nHost: demo\r\nRange: bytes=-26\r\n\r\n");
    ok(statusOf(r) === 206 && r.includes("Content-Range: bytes 65510-65535/65536") &&
       r.endsWith("QRSTUVWXYZABCDEFGHIJKLMNOP"),
       "wasm: suffix range is 206 with the right bytes");

    r = send(c, "GET /docs HTTP/1.1\r\nHost: demo\r\n\r\n");
    ok(statusOf(r) === 301 && r.includes("Location: /docs/"), "wasm: directory redirect");

    r = send(c, "POST /echo HTTP/1.1\r\nHost: demo\r\nContent-Length: 5\r\n\r\nhello");
    ok(statusOf(r) === 200 && r.endsWith("hello"), "wasm: POST echo content-length");

    r = send(c, "POST /echo HTTP/1.1\r\nHost: demo\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n");
    ok(statusOf(r) === 200 && r.endsWith("hello world"), "wasm: POST echo chunked");

    r = send(c, "GET /%2e%2e/%2e%2e/etc/passwd HTTP/1.1\r\nHost: demo\r\n\r\n");
    ok(statusOf(r) === 400, "wasm: encoded traversal is 400");
    ok(!m.ccall("mast_conn_closed", "number", ["number"], [c]),
       "wasm: a 400 with intact framing keeps the connection");

    c = newConn();
    r = send(c, "POST /echo HTTP/1.1\r\nHost: demo\r\nContent-Length: 4\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
    ok(statusOf(r) === 400, "wasm: CL+TE smuggling is 400");

    c = newConn();
    r = send(c, "GARBAGE\r\n\r\n");
    ok(statusOf(r) === 400, "wasm: garbage request line is 400");

    c = newConn();
    r = send(c, "GET /hello.txt HTTP/1.0\r\n\r\n");
    ok(statusOf(r) === 200 && r.includes("Connection: close"), "wasm: HTTP/1.0 closes after answering");

    c = newConn();
    r = send(c, "PUT /hello.txt HTTP/1.1\r\nHost: demo\r\n\r\n");
    ok(statusOf(r) === 405 && r.includes("Allow: GET, HEAD, POST, OPTIONS"), "wasm: PUT is 405 with Allow");
    {
        /* 405 framing: Allow sits before the blank line, the body matches
         * Content-Length exactly, and a pipelined request stays aligned */
        const headEnd = r.indexOf("\r\n\r\n");
        const head = r.slice(0, headEnd);
        const cl = parseInt((head.match(/Content-Length: (\d+)/) || [])[1], 10);
        ok(head.includes("Connection: keep-alive\r\nAllow: GET, HEAD, POST, OPTIONS"), "wasm: 405 Allow is inside the head");
        ok(r.length === headEnd + 4 + cl, "wasm: 405 Content-Length matches the wire body");
    }

    c = newConn();
    r = send(c, "PUT /hello.txt HTTP/1.1\r\nHost: demo\r\n\r\nGET /hello.txt HTTP/1.1\r\nHost: demo\r\n\r\n");
    ok((r.match(/HTTP\/1.1 (405|200)/g) || []).length === 2 &&
       r.indexOf("405") < r.indexOf("hello, world"),
       "wasm: pipelined request after a 405 stays aligned");

    c = newConn();
    r = send(c, "HEAD /nope HTTP/1.1\r\nHost: demo\r\n\r\nGET /hello.txt HTTP/1.1\r\nHost: demo\r\n\r\n");
    {
        const headEnd = r.indexOf("\r\n\r\n");
        const head = r.slice(0, headEnd);
        ok(head.startsWith("HTTP/1.1 404") && /Content-Length: \d+/.test(head),
           "wasm: HEAD 404 reports a Content-Length");
        ok(r.slice(headEnd + 4).startsWith("HTTP/1.1 200"),
           "wasm: HEAD 404 sends no body - the next response follows immediately");
    }

    c = newConn();
    r = send(c, "POST /echo HTTP/1.1\r\nHost: demo\r\nContent-Length: 100000\r\n\r\n" + "y".repeat(100000));
    ok(statusOf(r) === 200 && r.endsWith("y".repeat(100)) && r.includes("Content-Length: 100000"),
       "wasm: a 100 KB body in one call echoes (head cap does not eat the body)");

    c = newConn();
    r = send(c, "POST /echo HTTP/1.1\r\nHost: demo\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\n");
    ok(r === "HTTP/1.1 100 Continue\r\n\r\n", "wasm: 100 Continue precedes an accepted body");
    r = send(c, "hello");
    ok(statusOf(r) === 200 && r.endsWith("hello"), "wasm: body after 100 Continue echoes");

    c = newConn();
    r = send(c, "OPTIONS * HTTP/1.1\r\nHost: demo\r\n\r\n");
    ok(statusOf(r) === 204 && r.includes("Allow: GET, HEAD, POST, OPTIONS"), "wasm: OPTIONS * is 204 with Allow");

    c = newConn();
    r = send(c, "OPTIONS /hello.txt HTTP/1.1\r\nHost: demo\r\n\r\n");
    ok(statusOf(r) === 204 && r.includes("Allow: GET, HEAD, POST, OPTIONS"),
       "wasm: OPTIONS on a path carries the same Allow list");

    c = newConn();
    r = send(c, "get /hello.txt HTTP/1.1\r\nHost: demo\r\n\r\n");
    ok(statusOf(r) === 405, "wasm: lowercase get is 405, not 400");

    c = newConn();
    r = send(c, "GET /hello.txt HTTP/1.1\nHost: demo\n\n");
    ok(statusOf(r) === 200 && r.includes("hello, world"), "wasm: a bare-LF head is tolerated");

    c = newConn();
    r = send(c, "POST /echo HTTP/1.0\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\nhello");
    ok(statusOf(r) === 200 && !r.includes("100 Continue"),
       "wasm: HTTP/1.0 gets no interim 100");

    c = newConn();
    r = send(c, "POST /nope.txt HTTP/1.1\r\nHost: demo\r\nContent-Length: 1\r\nExpect: 100-continue\r\n\r\nx");
    ok(statusOf(r) === 404 && r.includes("Connection: close\r\n") && !r.includes("keep-alive"),
       "wasm: Expect rejected with 404 says Connection: close");

    c = newConn();
    r = send(c, "CONNECT x:443 HTTP/1.1\r\nHost: x\r\n\r\n");
    ok(statusOf(r) === 405 && r.includes("Allow: GET, HEAD, POST, OPTIONS"), "wasm: CONNECT is 405 + Allow");

    c = newConn();
    r = send(c, "POST /echo HTTP/1.1\r\nHost: demo\r\nTransfer-Encoding: chunked,\r\n\r\n5\r\nhello\r\n0\r\n\r\n");
    ok(statusOf(r) === 200 && r.endsWith("hello"), "wasm: Transfer-Encoding trailing comma tolerated");

    c = newConn();
    r = send(c, "GET / HTTP/1.1\r\nHost:\r\n\r\n");
    ok(statusOf(r) === 400, "wasm: empty Host is a 400");

    c = newConn();
    r = send(c, "GET /hello.txt HTTP/1.0\r\nConnection: keep-alive\r\nConnection: close\r\n\r\n");
    ok(statusOf(r) === 200 && r.includes("Connection: close"), "wasm: HTTP/1.0 close beats keep-alive");

    c = newConn();
    r = send(c, "GET /big.txt HTTP/1.1\r\nHost: demo\r\nRange: bytes=0-9\r\nRange: bytes=20-29\r\n\r\n");
    ok(statusOf(r) === 200, "wasm: duplicate Range serves the full body");

    c = newConn();
    r = send(c, "\n\n\n\n\n\n\n\n\nGET / HTTP/1.1\r\nHost: demo\r\n\r\n");
    ok(statusOf(r) === 400, "wasm: a flood of empty lines is a 400");

    c = newConn();
    r = send(c, "\r\n\r\nGET /hello.txt HTTP/1.1\r\nHost: demo\r\n\r\n");
    ok(statusOf(r) === 200, "wasm: two empty lines tolerated");

    c = newConn();
    r = send(c, "POST /echo HTTP/1.1\r\nHost: demo\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\nhello");
    ok(statusOf(r) === 200 && r.endsWith("hello"), "wasm: identical duplicate Content-Length merges");
    c = newConn();
    r = send(c, "POST /echo HTTP/1.1\r\nHost: demo\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello");
    ok(statusOf(r) === 400, "wasm: conflicting Content-Length is 400");

    c = newConn();
    r = send(c, "GET /big.txt HTTP/1.1\r\nHost: demo\r\nRange: bytes=0-99999999999999999999999\r\n\r\n");
    ok(statusOf(r) === 206 && r.includes("Content-Range: bytes 0-65535/65536"),
       "wasm: an overflowing last-byte-pos clamps to EOF");

    {
        /* preconditions and strict If-Range against the demo filesystem */
        c = newConn();
        r = send(c, "GET /hello.txt HTTP/1.1\r\nHost: demo\r\n\r\n");
        const et = (r.match(/ETag: ("[^"]+")/) || [])[1];
        const lm = (r.match(/Last-Modified: ([^\r]+)/) || [])[1];
        c = newConn();
        r = send(c, "GET /hello.txt HTTP/1.1\r\nHost: demo\r\nIf-Match: \"stale\"\r\n\r\n");
        ok(statusOf(r) === 412, "wasm: If-Match with a stale etag is 412");
        c = newConn();
        r = send(c, "GET /hello.txt HTTP/1.1\r\nHost: demo\r\nIf-Match: " + et + "\r\n\r\n");
        ok(statusOf(r) === 200, "wasm: If-Match with the current etag passes");
        c = newConn();
        r = send(c, "GET /hello.txt HTTP/1.1\r\nHost: demo\r\nIf-Unmodified-Since: Sun, 06 Nov 1994 08:49:37 GMT\r\n\r\n");
        ok(statusOf(r) === 412, "wasm: If-Unmodified-Since older than mtime is 412");
        c = newConn();
        r = send(c, "GET /hello.txt HTTP/1.1\r\nHost: demo\r\nIf-Range: " + lm + "\r\nRange: bytes=0-2\r\n\r\n");
        ok(statusOf(r) === 206, "wasm: If-Range with the exact Last-Modified serves the range");
        c = newConn();
        r = send(c, "GET /hello.txt HTTP/1.1\r\nHost: demo\r\nIf-Range: W/" + et + "\r\nRange: bytes=0-2\r\n\r\n");
        ok(statusOf(r) === 200, "wasm: If-Range with a weak etag serves the full body");
        c = newConn();
        r = send(c, "GET /hello.txt HTTP/1.1\r\nHost: demo\r\nIf-Range: Sun, 06 Nov 2094 08:49:37 GMT\r\nRange: bytes=0-2\r\n\r\n");
        ok(statusOf(r) === 200, "wasm: If-Range with a future date serves the full body");
    }

    c = newConn();
    r = send(c, "GET http://demo/hello.txt HTTP/1.1\r\nHost: demo\r\n\r\n");
    ok(statusOf(r) === 200 && r.includes("hello, world"), "wasm: absolute-form serves its path");
    c = newConn();
    r = send(c, "GET http://a/hello.txt HTTP/1.1\r\nHost: b\r\n\r\n");
    ok(statusOf(r) === 400, "wasm: absolute-form disagreeing with Host is 400");

    c = newConn();
    r = send(c, "GET /hello.txt HTTP/1.1\r\nHost: demo\r\n\r\n");
    const etag = (r.match(/ETag: ("[^"]+")/) || [])[1];
    c = newConn();
    r = send(c, "GET /hello.txt HTTP/1.1\r\nHost: demo\r\nIf-None-Match: \"nope\", " + etag + "\r\n\r\n");
    ok(statusOf(r) === 304, "wasm: If-None-Match list matches a member");
    c = newConn();
    r = send(c, "GET /hello.txt HTTP/1.1\r\nHost: demo\r\nRange: bytes=0-4\r\nIf-Range: \"stale\"\r\n\r\n");
    ok(statusOf(r) === 200, "wasm: If-Range with a stale validator serves the full body");
    c = newConn();
    r = send(c, "GET /hello.txt HTTP/1.1\r\nHost: demo\r\nRange: bytes=0-4\r\nIf-Range: " + etag + "\r\n\r\n");
    ok(statusOf(r) === 206, "wasm: If-Range with the current etag serves the range");

    c = newConn();
    r = send(c, "GET /hello.txt HTTP/1.1\r\nHost: demo\r\nRange: bytes=0-1,3-4\r\n\r\n");
    ok(statusOf(r) === 200 && r.includes("hello, world"), "wasm: multi-range serves the full body");

    c = newConn();
    r = send(c, "GET /hello.txt HTTP/1.1\r\nHost: demo\r\nConnection: disclose\r\n\r\nGET /hello.txt HTTP/1.1\r\nHost: demo\r\n\r\n");
    ok((r.match(/HTTP\/1.1 200/g) || []).length === 2, "wasm: Connection: disclose does not close");

    c = newConn();
    r = send(c, "GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n");
    ok(statusOf(r) === 400, "wasm: duplicate Host is 400");
    c = newConn();
    r = send(c, "POST /echo HTTP/1.1\r\nHost: demo\r\nContent-Length: +5\r\n\r\nhello");
    ok(statusOf(r) === 400, "wasm: Content-Length with a sign is 400");
    c = newConn();
    r = send(c, "POST /echo HTTP/1.1\r\nHost: demo\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
    ok(statusOf(r) === 400, "wasm: duplicate Transfer-Encoding is 400");
    c = newConn();
    r = send(c, "POST /echo HTTP/1.1\r\nHost: demo\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nhello\r\n");
    ok(statusOf(r) === 400, "wasm: malformed chunk framing is 400, not 413");
    c = newConn();
    r = send(c, "POST /echo HTTP/1.1\r\nHost: demo\r\nTransfer-Encoding: chunked\r\n\r\n100001\r\nyyyy");
    ok(statusOf(r) === 413, "wasm: an over-cap chunk size is 413");

    /* the demo page wires the buttons and the cache-busters */
    const html = require("fs").readFileSync(require("path").join(__dirname, "..", "docs", "index.html"), "utf8");
    const vjs = (html.match(/mast\.js\?v=([a-z0-9]{12})/) || [])[1];
    const vw = (html.match(/locateFile:[^;]*\?v=([a-z0-9]{12})/) || [])[1];
    const crypto = require("crypto");
    const wasmHash = crypto.createHash("sha256")
        .update(require("fs").readFileSync(require("path").join(__dirname, "..", "docs", "mast.wasm")))
        .digest("hex").slice(0, 12);
    ok(vjs && vjs === vw && vjs === wasmHash, "demo: cache-busters match the wasm hash");
    ok(html.includes("sendBtn.addEventListener") && html.includes("mast_recv"),
       "demo: send path wired to mast_recv");
    ok(/replace\(\/\\r\?\\n\/g, ['"]\\r\\n['"]\)/.test(html),
       "demo: textarea LF is normalized back to CRLF on the wire");
    ok((html.match(/disabled><\/select>/) || html.includes('id="preset" disabled')) &&
       html.includes('id="send" disabled'), "demo: controls start disabled until boot");

    process.exit(fails);
});

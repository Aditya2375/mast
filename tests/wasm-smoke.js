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
    ok(statusOf(r) === 405 && r.includes("Allow: GET, HEAD, POST"), "wasm: PUT is 405 with Allow");

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

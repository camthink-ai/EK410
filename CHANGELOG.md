# Changelog

All notable changes to the CamThink **EK410** firmware are documented here.

Format follows [Keep a Changelog](https://keepachangelog.com/). Versioning follows [Semantic Versioning](https://semver.org/).

中文说明见各版本条目下的 **中文** 小节。

---

## [Unreleased]

## [1.3.10] — 2026-11-25

Release-readiness audit of 1.3.9, plus what the first on-device run exposed.
Validated on **one** ESP32-C61HR2 bench unit (STA + web server + MQTT over
TCP) with `dist/ek410.bin` built by `tools/build_release.sh` from this source
and this `sdkconfig.defaults` (sha256 prefix `44b0e0576535`; zero compiler
warnings, host tests 8/8): `tools/fix_verification.py` **10/10** (JSON depth,
`devName` limit, MQTT password hidden, internal-heap headroom, failed URL OTA
reported with MQTT back and `http://` refused, URL display soak, raw frames
between images, local-upload OTA round trip, `sleepMin` clamp); and six
screens — a long paragraph, a canvas with two QR codes and four pictures
pushed by URL — displayed on the previous image and on this one and read back
from the device (`GET /api/v1/display/lastFrame`): the text and canvas frames
are byte-identical (1,920,000 pixels, 0 differ), the grey logo went from
11.3 % to 1.8 % coloured pixels, and POST-to-panel-done fell from 83.7 s to
52.6 s for the paragraph and from 83–88 s to 61–66 s for the pictures. On this
image three URL raw frames in a row were each followed by a rebuilt telemetry
preview (54 of 54 were missing before), and `speed` mode was exercised with
the setting put back afterwards. The previous image `2c5ba1cc5801` — this
source without the dither, text, preview and probe changes — ran a 13-step
boundary soak (10 displayed, 3 rejected as intended, zero reboots) and the
first 13.2 h of the 72 h soak: 383 rounds, no unexpected result, no reboot;
its serial log is what surfaced the missing preview. Earlier still,
`f541da6db01e` ran a 27-step display soak (20 displayed, 7 rejected as
intended, zero reboots) and a real-HTTPS URL OTA **failure** path
(`otaState=failed`, `ESP_ERR_INVALID_VERSION`, MQTT restored, no reboot), and
`5fc910f9736a` had been built in a stale `build/` and still carried the
2304-byte event-task stack (640 bytes of headroom instead of 1408). All are
superseded. The 72 h soak (`tools/soak_long.py`) was restarted on this image
at 13:00 on the day of the release: when v1.3.10 was tagged it had run half
an hour (15 rounds, no unexpected result), and its result was not in.

**Known on hardware, not fixed in this release** (details in
`docs/c61_memory_allocation_report.md` §11.5):

- **The larger a JPEG or PNG, the softer it comes out.** The decoder takes
  its output buffer from what is left of the largest free PSRAM block beside
  the body (48 KB reserved for the decoder pool). With the web console polling
  — free PSRAM in two blocks, ~800 KB + ~230 KB — a 1600×1200 JPEG is decoded
  to 400×300 (¼) only up to about 400 KB and to 200×150 (⅛) from there to the
  limit (measured: 66 KB → 400×300, 624 KB → 200×150 while 432 KB were free,
  in blocks of 230 KB and 197 KB); a 2400×1800 JPEG is always 300×225. The
  picture is upsampled smoothly now, so this costs detail, no longer grain.
  Placing the body so that the largest block keeps room for the RGB buffer,
  or decoding to RGB565, would move the ¼ point to ~640 KB or up to the limit.
- **Saving the last frame takes about 5 s after every display** (erase and
  rewrite of the 1 MB `lastfrm` partition, then the preview). The panel shows
  the picture before that; the job is only reported done afterwards. Older
  than this release.
- **Progressive JPEG is not decodable** (`JPEG decode fail`): the ROM decoder
  only handles baseline JPEG, and many web images are progressive.
- **Internal RAM dips to 8 bytes free at some point of every boot**
  (`getDiag.heap.minFreeInt` reads 8 fifty seconds after a fresh boot; Wi-Fi
  init starts from 64 KB, MQTT starts from 19.4 KB, the steady state is 22 KB).
  Boot has never failed on it, and the step that takes it there is not
  located yet. A side effect: `minFreeInt` cannot show a low during a soak,
  only the per-round `freeInt` / `largestInt` can.
- PSRAM can still fragment after a successful display (largest free block
  often ~0.5–0.8 MB). URL bodies are now read in up to three pieces and
  JPEG/PNG decode from those pieces, so the fragmentation no longer rejects
  a ~0.9 MB baseline JPEG or a 960000-byte raw frame.

**Not validated on hardware:** MQTTS, SoftAP and BLE provisioning, CYCLE
sleep and the wake URL on battery, MQTT downlink commands, a *successful*
URL OTA (the failure path was checked with real HTTPS; the device accepts
`https://` only, so it needs an image on a host it trusts), more than one
unit, and the 72 h soak (restarted on this image; 13 h on the previous one
`2c5ba1cc5801` — 383 rounds, no unexpected result, no reboot — surfaced the
missing preview). Run `tools/qa_gate.py` candidate + soak before shipping.

### Fixed

- **Internal RAM ran out with Wi-Fi, the web server and MQTT up** (found on hardware; 1.3.9 behaves the same). The render task reserved a 32 KB stack while its deepest measured path needs 3.6 KB, which left 5.4 KB of internal RAM in blocks of at most 3.3 KB: the first stop of MQTT — every URL image display, OTA attempt or broker change — left no block ≥ 8 KB, so MQTT stayed down (`deferred_heap`) until reboot; an HTTPS image got down to 1.4 KB, failed to read and took the web server and the network with it; a URL OTA could not create its 12 KB task (`task spawn failed`). With a 16 KB render stack and cJSON nodes in PSRAM the largest block at MQTT start went from 8.7 KB to 19.4 KB and the steady state from 5.4 KB / 3.3 KB (free / largest) to 21.8 KB / 13.3 KB.
- **MQTT task stack had 168 bytes left after connecting**, and a downlink command (`ping` → preview telemetry) nests deeper still. Stack 3072 → 4096, and the status / telemetry / ack publishers no longer keep 380-byte config structs on it.
- **System event task had ~600 bytes of stack left** after the `GOT_IP` chain that runs on every (re)connect — web server start, MQTT start, sleep schedule, OTA self-check. Stack 2304 → 3072 (`sdkconfig.defaults`).
- **Large URL images failed with `download OOM`** (an 857 KB JPEG after a few displays): the whole-body buffer was allocated only after the headers, by which time the incoming data had split the free PSRAM block. It is now reserved before the transfer and cut back to `Content-Length` as soon as the headers are in. The splitting that appeared *after* a display is the next bullet.
- **A raw frame or large JPEG/PNG fetched by URL was rejected after a successful display** (`need 937KB, max block 832KB` / `need …KB, max block …KB`; found on hardware). Web UI polls leave a 208-byte TCP control block that splits free PSRAM for two minutes. URL bodies are now read into up to three blocks after the headers (no leftover 64 KB hold-back on the first piece — TLS already owns its buffers). JPEG/PNG decoders already read through callbacks, so they consume the pieces in order; a raw frame streams the same way.
- **Render-job messages and small objects split the free PSRAM** (found on hardware). The ~19 KB job message was taken from the heap on every job, and lwIP, MQTT and cJSON put their few-hundred-byte objects in PSRAM too; allocated right above MQTT's buffers they stayed there as splitters. Job messages now come from three slots reserved at init, and a 20 KB hole below them takes the small objects. The largest free block before a JPEG/PNG download is ~1.0 MB whenever no poll is in `TIME_WAIT` (it used to be 826 KB from the first display on); an image larger than the block at that moment is rejected as `need …KB, max block …KB`.
- **Decoding a large JPEG/PNG left almost no PSRAM** (found on hardware, once files that size could be fetched): an 857 KB JPEG kept its 857 KB body in PSRAM next to the decoder pool (48 KB) and the RGB buffer, and free PSRAM fell to about 2 KB for the seconds of the decode. A JPEG or PNG over 700 KB is now refused before the body is read (`image >700KB limit`; the previous frame stays). Raw frames are streamed, not decoded, and keep the 1280 KB cap.
- **URL pictures came out as coarse speckle** (seen on hardware). The picture was dithered at its decoded resolution — 400×300 for a 1600×1200 JPEG, less for a small one — and the ink dots were then blown up by nearest sampling: every dot a 4×4 block on the panel, 6×6 for a wide logo. And with six inks and no grey, per-channel error diffusion put red, yellow and blue into grey text: 11 % of a grey logo's pixels came out coloured. `main/img_dither.c` now upsamples the picture smoothly and dithers at panel resolution — one ink dot per panel pixel with `imageQuality: quality`, 800×600 with `speed` — straight into the native frame, and a near-neutral pixel only takes black or white (1.8 % coloured pixels in that logo). The nearest-ink metric, six float distances per pixel, is a table now; the dither takes 5.7 s a picture instead of ~29 s (dither plus cover sampling). The ink colours also go through the same tone curve as the picture: a pure-colour area used to leave a few levels of error on every pixel, which the diffusion carried along until the values saturated and pushed a stream of error into the neighbouring colour.
- **A screen of long text spent about 37 s evaluating pixels** — more than the panel's own refresh. Every panel pixel re-decoded the UTF-8 string to find its character cell and tested the box of every canvas item, read from PSRAM. Cells are listed once per render and each column gets a table of the items covering it: a long paragraph went from 83.7 s to 53.1 s, and the frame it produces is byte-identical.
- **Telemetry went out without a preview after every URL raw frame** (found by the 72 h soak: 54 of 54). The frame was committed to flash while its 960 KB body was still allocated, so the 121 KB preview could not be made. The body is released first.
- **The boot-time panel probe reported `panelOk=false` for a whole boot** about one boot in three: it tried twice, 50 ms apart, and the master driver IC can need longer. It now retries for up to about a second.
- **`displayUrl` and `proxyImage` did not follow redirects** (`HTTP status 302`), and CDN, signed and short links all redirect: `esp_http_client` only follows them inside `esp_http_client_perform()`. The open / read sequence now follows up to 5 hops with IDF's own `esp_http_client_set_redirection()` — relative or absolute targets, http → https; an https page still cannot downgrade to http.
- **Sleep or a frame upload could start while an OTA was writing flash.** Both are refused while an OTA is running.
- **URL image display corrupted the PSRAM heap on every JPEG/PNG** (HTTP `displayUrl`, MQTT `display_url`, CYCLE `wakeUrl`): `epd_url_frame()` freed the dithered ink buffer before streaming it and freed it again. Regression since 1.3.4.
- **Deeply nested JSON rebooted the device** on every HTTP route, CT-Prov (HTTP and BLE) and MQTT downlink (cJSON recursion vs. 4–5 KB task stacks). Input is depth-bounded before it is parsed.
- **`setDevInfo` with a `devName` of 4000+ bytes aborted** in NVS. Names are limited to 32 bytes and rejected with an error.
- **JPEG size arithmetic wrapped in 32 bits**: a crafted SOF could allocate a small buffer and overflow it. Sizes are 64-bit with dimension caps. Large JPEGs (> ~7.9 MP, forced SPEED) now display by integer subsampling after the 1/8 decode instead of failing.
- **PNG decode errors leaked the RGB and row buffers** (up to 720 KB PSRAM per failed URL).
- **CYCLE could stay awake for the rest of a boot** — battery drain: after a short button press, after a BLE/SoftAP window closed without STA, or when the boot never got an IP. The window is re-armed on key release and on the next `GOT_IP`; an offline boot falls back to the max window.
- `sleep_go` re-checks provisioning after the panel wait: SoftAP opened during a long refresh was cut by the sleep.
- Hibernate saved under SoftAP is entered when the hotspot closes (the UI already said so).
- MQTT `sleep` / `set_power` follow-up error acks carry the requesting command; timer-driven entries no longer ack.
- **MQTT was recreated 3 s into an OTA or URL image download** that had just stopped it for heap. Both hold the restart until they release.
- **URL OTA failure was invisible**: `getDevInfo` reports `otaState` / `otaError`; the web UI polls them instead of showing "success". A failed attempt restores MQTT and the sleep schedule by itself.
- **URL OTA downloads on the render task** — the one task with a deep enough stack for HTTPS — instead of spawning a dedicated 12 KB task.
- Web UI: the device-name field is no longer reverted by the status poll while typing; the image-quality toggle no longer saves an unsaved work mode (it could hibernate the unit); System → Save does not post power defaults when the power form failed to load; a unit in `always` mode with a leftover `sleepMin` is shown as `always` (it used to be shown as cycle, and System → Save then switched it to timed sleep); editor file names are HTML-escaped; undecodable image uploads and blocked `localStorage` report instead of failing silently; overview uptime is live.
- Factory gesture needs both keys held for the whole 6 s. `wifi_ble_prov_enter()` is re-entrancy safe. A failed SoftAP open restores the CYCLE window; SoftAP close always arms the reconnect backoff.
- `sleepMin` is clamped (≤ 1 year) and a timer-wake setup failure no longer aborts into a reboot loop.
- Power / devName / factory-reset NVS writes return errors instead of `ESP_ERROR_CHECK` aborts. `clearEventLog` and factory reset report real results.
- SoftAP / BLE provisioning windows close after 15 min even while a client is attached.
- `proxyImage` jobs dropped from the queue answer with an error instead of HTTP 200 + empty body; waiter/runner hand-off is race-free.
- MQTT error events are logged once per changed failure (5 min) — a dead broker used to erase event-log sector 0 ~2000×/day.
- `lastfrm` skips the 1 MB erase when the frame is unchanged.
- Factory reset also wipes the NimBLE bond store.
- Panel refresh BUSY timeout 40 s → 60 s; AW9523B I2C uses a 100 ms timeout; canvas text ignores `\r` and other control characters; CT-Prov `config.set` rejects unknown sections before applying anything; a busy wake-URL queue no longer runs HTTPS on the 2.3 KB event task.
- MQTT no longer retries every 5 s when no broker is configured. A saved broker config clears the previous attempt's error, so "Test connection" cannot report the old failure for the new config. `ota_upload_abort()` keeps retrying for its mutex instead of leaving OTA busy until reboot. CT-Prov's MQTT save adds no stack on the 4 KB BLE worker.
- Web UI: the "hotspot lost" banner needs three missed polls (the MQTT bring-up pause raised a false alarm); overview placeholders show "—" instead of mock values (including a hard-coded MAC).

### Changed

- **`wakeSec` default is 120 s again** (it had silently become 600 between 1.3.0 and 1.3.2); README, API docs and the web UI now agree.
- **`getMqttParam` no longer returns the MQTT password** (`hasPassword` instead, like the Wi-Fi password). A blank password keeps the stored one only for the same broker and username; changing either clears it and `setMqttParam` answers with a `warn`. QA tools take `--mqtt-password` for the cases that change the broker or publish downlink.
- `setPowerParam` with neither `mode` nor `sleepMin` leaves the sleep schedule untouched.
- Rollback gate: a pending-verify image is confirmed only once the panel probe passed and the unit is reachable (web up or STA online).
- Release build is explicitly `-Og` (see `sdkconfig.defaults`); the Wi-Fi buffer options were removed from `sdkconfig.defaults` — the runtime table in `wifi.c` is authoritative.
- `tools/build_release.sh` stops when the clean fails, deletes a leftover `build/sdkconfig` and prints the effective configuration. `sdkconfig.defaults` only sets options that are not in an existing sdkconfig: a release built in a stale `build/` silently kept the old values (the first 1.3.10 image, `5fc910f9736a`, was built that way and still had the 2304-byte event-task stack). Build releases with the script, never with a bare `idf.py build`.
- `main/img_dither.c` is compiled at `-O2`; every other file stays at `-Og`. The dither took 8.8 s a picture at `-Og` and 5.7 s at `-O2`. It is self-contained numeric code with its own host test, so nothing is lost in debugging.
- Font bitmaps come from **Noto Sans CJK SC** (16×16 CJK) and **Noto Sans Mono CJK SC** (8×16 ASCII), both SIL OFL 1.1, instead of macOS system fonts; `qrcodegen.min.js` carries its MIT notice again.

### Added

- `docs/security.md` / `docs/zh/security.md`: what the unauthenticated LAN/SoftAP API exposes, and the decisions to take before mass production.
- `GET /api/v1/system/getDiag`: `heap` (`free`, `minFree`, `freeInt`, `minFreeInt`, `largestInt`, `freeSpiram`, `largestSpiram`) and `stackMin` (smallest free stack ever seen, in bytes, per long-lived task) — the numbers to watch when validating a build on hardware. On demand, not in `getDevInfo`: the web UI polls that every few seconds, and the ~20 extra nodes per reply cost 5 KB of internal RAM in the first 1.3.10 candidate (it dropped to 8 bytes free during a poll).
- Host regression tests: `python3 tools/run_host_tests.py` (`tools/tests/`: JSON depth guard, the panel-frame dither — 39 checks on tone, grey and layout, plus a byte-for-byte match with `frame_convert.pack_native` on both grids — native frame layout, QA gate semantics, embedded page in sync / deterministic / i18n consistent, font tables, repo hygiene). `tools/build_release.sh` runs them before building.
- `tools/fix_verification.py`: checks every fix of this release on a real device over HTTP — JSON depth, `devName` limit, MQTT password hidden, failed URL OTA reported **with MQTT back afterwards**, MQTT back after every URL display, internal-heap headroom, local-upload OTA round trip, `sleepMin` clamp — and fails on 1.3.9 firmware. Part of `qa_gate.py --tier candidate` (`--verify-url`, `--verify-rounds`). `--ota-url` adds a URL OTA round trip with a real `https://` image URL.
- `tools/soak_long.py`: unattended business-rhythm soak of one unit (72 h by default) — serves a generated image set from the host (JPEG, three kinds of PNG, a raw frame, redirects, a file exactly on the 700 KB limit), cycles `displayUrl` with MQTT telemetry in between, mixes in `uploadFrame`, files the firmware must refuse, the web console and optionally a public HTTPS image; reads `getDiag` after every round, checks MQTT is back, keeps the serial log, appends `rounds.jsonl` and rewrites `summary.json` each round. Never touches the device's configuration.
- QA gate: `firmware_binding` case (device `fwVersion` must equal the image's version), an all-skipped run is not `ok`, `--mqtt-password` pass-through.
- `tools/frame_convert.py` now writes the real native layout (CS0/CS1 halves, 90° rotated) — its old output was scrambled on the panel.

### 中文

- **内部 RAM 在 Wi-Fi + 网页服务 + MQTT 下耗尽**（真机发现，1.3.9 同样如此）。刷屏任务配了 32 KB 栈，而真机上最深的路径只用 3.6 KB；省下来的空间本可以让整机稳态从 5.4 KB 空闲（最大块 3.3 KB）变成 21.8 KB（最大块 13.3 KB）。此前 MQTT 第一次停止（每次 URL 刷图、每次升级、每次改 broker 配置）之后就再也起不来（`deferred_heap`）直到重启；HTTPS 取图把内部 RAM 压到 1.4 KB、读取失败，并连带网页和局域网掉线；URL 升级建不出 12 KB 的任务（`task spawn failed`）。MQTT 启动前最大连续块从 8.7 KB 提高到 19.4 KB。
- **MQTT 任务连接成功后栈只剩 168 字节**，下行命令（`ping` 触发带预览的遥测）嵌套更深。栈 3072 → 4096，状态 / 遥测 / 应答发布不再把 380 字节的配置结构体放在该栈上。
- **系统事件任务栈只剩约 600 字节**：每次（重）联网都要在它上面跑完起网页、起 MQTT、休眠排程、OTA 自检这一串。栈 2304 → 3072（`sdkconfig.defaults`）。
- **大图 URL 刷图报 `download OOM`**（连刷几张后，857 KB 的 JPEG）：整块图片缓冲过去是在收到响应头之后才分配，那时传过来的数据已经把空闲块切碎。现在在传输开始前先预留、一读到 `Content-Length` 就收缩到实际大小；放不下的文件报 `need 857KB, max block …KB`。
- **URL 取原生帧或大 JPEG/PNG 在成功刷过一张图之后被拒**（`need 937KB, max block 832KB` 等，真机发现）。网页轮询留下的 TCP 控制块会把空闲 PSRAM 切开；现在响应头之后按空闲块分最多 3 段读入（第一段不再预留 64 KB——TLS 缓冲此时已分配），JPEG/PNG 经回调顺序读分段缓冲，原生帧同样流式刷屏。
- **渲染任务消息和小对象把空闲 PSRAM 切开**（真机发现）。约 19 KB 的任务消息每次都从堆上分配，lwIP、MQTT、cJSON 的几百字节对象也都在 PSRAM 里；它们一旦落在 MQTT 缓冲区上方，就成了分割空闲区的隔板。现在消息改用启动时预留的 3 个槽位，其下方留一个 20 KB 的空洞收留小对象。
- **大 JPEG/PNG 解码期间 PSRAM 几乎耗尽**（让这么大的图能取回之后才暴露）：857 KB 的 JPEG 解码时，本体 857 KB、解码器工作池 48 KB、RGB 缓冲同时驻留 PSRAM，那几秒内空闲一度只剩约 2 KB。现在超过 700 KB 的 JPEG / PNG 在读正文之前就拒绝（`image >700KB limit`，屏幕保留上一画面）；原生帧是流式刷屏、不解码，仍按 1280 KB 上限。
- **URL 刷图颗粒粗、灰字带彩点**（真机可见）。过去图片在解码分辨率上抖动——1600×1200 的 JPEG 是 400×300，小图更低——再把墨点最近邻放大，屏上每个抖动点是一块 4×4 像素，宽标志图是 6×6。再加上只有六种墨色、没有灰，按通道做的误差扩散会把红、黄、蓝混进灰色文字：一张灰色标志图里 11% 的像素是彩色的。新增 `main/img_dither.c`：先平滑放大，再按整屏分辨率抖动（`imageQuality: quality` 时屏上每个像素一个墨点，`speed` 时 800×600），直接写进原生帧；接近灰色的像素只取黑白（同一张标志图降到 1.8%）。最近墨色从每像素六次浮点距离计算改为查表，一张图的抖动从约 29 秒（抖动 + 放大采样）降到 5.7 秒。墨色的参考值也走和图片相同的阶调曲线：过去纯色区域每个像素都会留下几级“误差”被一路往后带，数值饱和后把一股误差推进相邻的颜色。
- **长文本一屏在逐像素求值上花约 37 秒**——比面板自己的刷新还久。每个像素都要重新解码一遍 UTF-8 找到字位，再把画布里每个图元的包围盒（在外部内存里）测一遍。现在每次渲染只列一次字位，每一列建一张“哪些图元覆盖该行”的表：一段长文从 83.7 秒降到 53.1 秒，输出的帧逐字节不变。
- **每次 URL 原生帧刷图之后，遥测都不带预览图**（72 小时长测发现，54 次全部如此）。提交上一帧时 960 KB 的正文还占着内存，121 KB 的预览生成不出来。现在先释放正文。
- **开机时的面板探测约每三次有一次让 `panelOk=false` 持续整个开机周期**：只试两次、间隔 50 毫秒，而主片驱动 IC 可能需要更久。现在最长重试约 1 秒。
- **`displayUrl` / `proxyImage` 不跟随跳转**（`HTTP status 302`）：CDN、签名地址、短链接都会跳转，而 `esp_http_client` 只在 `esp_http_client_perform()` 里处理。现在跟随最多 5 跳（复用 IDF 的 `esp_http_client_set_redirection()`，支持相对与绝对地址、http → https；https 页面仍不能降级到 http）。
- **升级写 Flash 期间可能进入休眠或被原图上传打断**：升级进行中两者都会被拒绝。
- URL 升级改为在刷屏任务上下载（整机唯一栈足够深的任务），不再新建 12 KB 专用任务；失败后自行恢复 MQTT 与休眠排程。
- **URL 刷图堆损坏**（HTTP / MQTT / CYCLE 唤醒图）：码流缓冲被提前释放又二次释放，1.3.4 起的回归。
- **深层嵌套 JSON 导致重启**：HTTP、CT-Prov（含 BLE）、MQTT 下行全部受影响，解析前限制嵌套深度。
- **`devName` 超 4000 字节触发 abort**：限制 32 字节并返回错误。
- **JPEG 尺寸计算 32 位回绕可堆溢出**：改 64 位并限制尺寸；大照片改为 1/8 解码后整数抽样，不再直接失败。
- **PNG 解码失败泄漏** RGB 缓冲。
- **CYCLE 多条路径永不休眠**（短按按键、配网关闭时未联网、开机拿不到 IP）：按键松开和联网时补布防，离线开机用最大窗口兜底。
- OTA / URL 下载期间 MQTT 不再被 3 秒自动重试拉起；URL 升级失败网页如实提示并轮询 `otaState`。
- 网页：设备名不再被轮询冲掉；画质开关不再连带保存未确认的工作模式；常开设备不再因残留的 `sleepMin` 被显示成定时刷新（此前点保存会把设备切到定时休眠）；上传不可解码图片、浏览器禁用存储不再静默失败；运行时长显示真实值。
- 出厂手势需双键持续按住；BLE 进入防重入；`sleepMin` 加上限且定时器失败不再重启循环；NVS 写入失败不再 abort；配网窗口 15 分钟硬上限；出厂重置同时清除蓝牙配对。
- **`wakeSec` 默认恢复为 120 秒**（此前被悄悄改成 600）。
- **`getMqttParam` 不再回传 MQTT 密码**（改为 `hasPassword`）；更换 broker 或用户名时空密码不再沿用旧密码，QA 工具新增 `--mqtt-password`。
- 汉字点阵改用 **Noto Sans CJK SC**、ASCII 点阵改用 **Noto Sans Mono CJK SC**（均为 OFL 1.1）；恢复 qrcodegen MIT 声明。
- 新增安全说明文档；QA gate 绑定设备实际固件版本、全跳过不算通过；`frame_convert.py` 改为真实的原生帧布局。
- 未配置 MQTT broker 时不再每 5 秒空转重试；保存新配置后清除上一次的错误，测试连接不会误报旧错误。
- 新增按需读取的诊断接口 `GET /api/v1/system/getDiag`：`heap`（堆余量）与 `stackMin`（各任务最小剩余栈）。不放进 `getDevInfo`——网页每隔几秒轮询它，1.3.10 首版把诊断塞进去后每次轮询多耗 5 KB 内部 RAM。
- 新增主机回归测试 `python3 tools/run_host_tests.py`（发布脚本会自动跑；含整屏抖动的 39 项检查，以及与 `frame_convert.pack_native` 的逐字节对比）。`main/img_dither.c` 单独按 `-O2` 编译，其余文件仍是 `-Og`：这个内核在 `-Og` 下一张图要 8.8 秒，`-O2` 下 5.7 秒，它是自成一体的数值代码并有主机测试，以及上机核对工具 `tools/fix_verification.py`（已接入 `qa_gate.py --tier candidate`，在 1.3.9 固件上会逐项报失败；同时核对每次 URL 刷图和升级失败后 MQTT 恢复、内部 RAM 余量）。；新增 72 小时业务节奏长测 `tools/soak_long.py`（本机起图片服务、可接串口、逐轮落盘，不改设备配置）。
- 上机验证范围：**一台** ESP32-C61HR2 台架机（STA + 网页服务 + TCP MQTT），镜像是用 `tools/build_release.sh` 从本源码和本 `sdkconfig.defaults` 构建的 `dist/ek410.bin`（sha256 前缀 `44b0e0576535`；编译零告警，主机测试 8/8）：`tools/fix_verification.py` **10/10**（深层 JSON、`devName` 长度、MQTT 密码不回传、内部 RAM 余量、URL 升级失败可见且 MQTT 恢复、`http://` 被拒、URL 刷图、原生帧夹在图片之间、本地上传 OTA 往返、`sleepMin` 上限）；另把六个画面——一段长文、带两个二维码的画布、四张 URL 图片——先在上一个镜像上、再在本镜像上刷出，并从设备读回它保存的帧（`GET /api/v1/display/lastFrame`）对比：长文与画布的帧逐字节相同（192 万像素 0 差异），灰色标志图的彩色像素从 11.3% 降到 1.8%，从发请求到刷完的时间长文由 83.7 秒降到 52.6 秒、图片由 83–88 秒降到 61–66 秒。在本镜像上连续三次 URL 原生帧之后遥测预览都重新生成（此前 54 次全部缺失）；“速度优先”模式也测过，设置已恢复。上一个镜像 `2c5ba1cc5801`（同一份源码，不含抖动、文本、预览、面板探测的改动）跑过 13 步边界 soak（10 步显示、3 步按预期拒绝、零重启）和 72 小时长测的前 13.2 小时：383 轮，无意外结果、无重启——它的串口日志暴露了预览缺失。更早的 `f541da6db01e` 跑过 27 步刷屏 soak（20 步显示、7 步按预期拒绝、零重启）和真实 HTTPS URL 升级**失败**路径（`otaState=failed`，`ESP_ERR_INVALID_VERSION`，MQTT 恢复，未重启）；`5fc910f9736a` 是在残留的 `build/` 目录里构建的，系统事件任务栈仍是 2304（余量 640 而不是 1408）。以上均已被取代。72 小时长测（`tools/soak_long.py`）在发布当天 13:00 于本镜像上重新开始：打 `v1.3.10` 标签时它跑了半小时（15 轮，无意外结果），结果尚未出来。
- **真机上发现、本版未修**（详见 `docs/c61_memory_allocation_report.md` 第 11.5 节）：**JPEG / PNG 越大，画面越软**（解码输出只能用最大空闲 PSRAM 块剩下的部分；网页轮询时空闲分成约 800 KB + 230 KB 两块）：1600×1200 的 JPEG 只在文件约 400 KB 以内才解到 400×300（¼），再大到上限都是 200×150（⅛）——实测 66 KB → 400×300、624 KB → 200×150（当时还空闲 432 KB，但分成 230 KB 与 197 KB 两块）；2400×1800 的 JPEG 始终是 300×225；PNG 是逐级变粗（420 KB 的 1600×1200 PNG → 320×240）；现在会经平滑放大，所以损失的是细节，不再是颗粒。让正文避开最大块，或改用 RGB565 解码输出，可把 ¼ 的分界点提到约 640 KB 乃至上限；**每次刷屏之后保存上一帧约需 5 秒**（擦除并重写 1 MB 的 `lastfrm` 分区，再生成预览）：此时面板已经显示新画面，只是任务要到这之后才报告完成，属原有逻辑；**渐进式 JPEG 无法解码**（报 `JPEG decode fail`）；**内部 RAM 在每次启动的某个时刻只剩 8 字节**（全新开机 50 秒时 `getDiag.heap.minFreeInt` 已是 8；Wi-Fi 初始化时还有 64 KB，MQTT 启动时 19.4 KB，稳态 22 KB——启动从未因此失败，但哪一步把水位压到这里还没定位；这也意味着长测里的 `minFreeInt` 反映不了运行期的低点，只能看每轮的 `freeInt` / `largestInt`）；刷图后 PSRAM 最大连续块仍可能变小，但 URL 正文已改为最多 3 段读入并由 JPEG/PNG 回调消费，业务上不再因此拒图。
- **尚未上机验证**：MQTTS、SoftAP 与 BLE 配网、CYCLE 休眠与电池下的唤醒图、MQTT 下行命令、URL 升级的**成功**路径（失败路径已用真实 HTTPS 验证；设备只接受 `https://` 地址，成功路径需要一台设备信任的公网主机上的正式镜像）、多台设备；72 小时长测已在本镜像上重新开始（上一个镜像 `2c5ba1cc5801` 跑了 13 小时：383 轮，无意外结果、无重启，正是它暴露了预览缺失）。

## [1.3.9] — 2026-11-23

### Fixed

- Add `mqtt_restore_after_temp_stop()` and use it for URL TLS (incl. SoftAP+STA), OTA fail restore, HTTP/CT-Prov MQTT save-fail after stop.
- `mqtt_stop` takes the mutex before cancelling the start-retry timer (busy return no longer strands deferred_heap recovery).

### 中文

- 统一临时 stop 后恢复；SoftAP 下 URL/OTA 也会拉回 MQTT；stop busy 不再误掐重试定时器。

## [1.3.8] — 2026-11-23

### Fixed

- MQTT `deferred_heap` no longer sticks forever: `mqtt_stop` re-arms start-retry on STA; coalesced `mqtt_start` arms retry; URL TLS release kicks retry after failed restore.
- `mqttReason` reports `deferred_heap` only while largest internal block is actually too small (else `connecting`).

### 中文

- 修复 MQTT deferred_heap 粘连：stop 后重武装重试、URL 刷屏失败强制 kick、状态不再假粘连。

## [1.3.7] — 2026-11-23

### Fixed

- Render queue supersede now publishes MQTT completion `error` ack (no stranded `queued`).
- CYCLE `sleep_timer` spawn failure clears `s_entering` and re-arms (no sticky sleep gate).
- SUBSCRIBE-fail teardown uses timer retry if stop-task create fails.
- `mqtt_publish_telemetry` / `publish_up` return broker publish success; `ping` acks honestly.
- SoftAP hibernate save toasts deferred sleep instead of false "sleeping".
- `setMqttParam` restores MQTT if NVS save fails after a preparatory stop.
- Proxy image HTTP open failure calls `url_tls_release`.
- Deep-sleep abort (panel busy / provisioning) sends follow-up MQTT `error` ack.
- Last-frame flash write failure invalidates meta and aborts capture.

### 中文

- 队列丢弃补完成 ack；sleep 定时器 spawn 回滚；zombie stop 定时器重试；telemetry/ping 诚实；SoftAP hibernate toast；NVS 失败恢复 MQTT；proxy TLS 释放；深睡推迟补 ack；lastfrm 写失败清 meta。

## [1.3.6] — 2026-11-23

### Fixed

- MQTT SUBSCRIBE failure tears down the client (no zombie `mqtt_is_started` session).
- `rx_reset` no longer frees `rxBuf` without the client mutex (DISCONNECT vs stop race).
- MQTT/HTTP `sleep_now` / hibernate report error when spawn fails (no false success ack).
- Deep-sleep waits longer for `mqtt_stop` before forcing Wi‑Fi off.
- `render_text` OOM / fill failure clears `lastDisplayOk` so completion `cmd_ack` is honest.
- SoftAP MQTT save reply-loss path no longer POSTs `testMqtt` (poll `getDevInfo` only).
- MQTT uplink publish retries mutex take and logs `evtlog` on final drop (no silent ack loss).

### 中文

- SUBSCRIBE 失败销毁会话；rx_reset 不再无锁释放；休眠/休眠 ack 诚实；深睡延长 mqtt_stop 等待；文本 OOM 刷新 lastDisplayOk；SoftAP 边缘禁止 testMqtt；publish 互斥重试并记日志。

## [1.3.5] — 2026-11-23

### Fixed

- MQTT fragment reassembly holds the client mutex so `mqtt_stop` cannot free `rxBuf` mid-copy.
- Panel stream/URL paths honor `epd_commit_display` failure (no false lastfrm / displayed).
- SoftAP open restores MQTT if Wi-Fi bringup fails after `mqtt_stop`.
- SoftAP/BLE close re-enters `sleep_on_sta_online` so CYCLE `wakeUrl` is not skipped.
- MQTT `clear` / `render_text` / `render_canvas` publish completion `cmd_ack` after the panel job.
- SPA editor no longer double-toasts success; Markdown push waits for `displayBusy` like the editor.
- `testMqtt` / SoftAP keep_live / session paths require STA for `connected`.

### Changed

- `CMakeLists.txt` sets `PROJECT_VER` from `VERSION` so `esp_app_desc` matches `fwVersion`.

### 中文

- 分片重组与 stop 互斥；刷屏 commit 失败如实上报；SoftAP 半开失败恢复 MQTT；配网关闭补跑 wakeUrl；MQTT 刷屏完成 ack；SPA 推屏 toast/等待修正；connected 统一要求 STA；镜像版本绑定 VERSION。

## [1.3.4] — 2026-11-23

### Fixed

- CYCLE sleep: button-wake provisioning no longer sets `s_cycleStarted` early, so GOT_IP after SoftAP/BLE close can arm the wake window again.
- MQTT uplink publish (`cmd_ack` / status) holds the client mutex so `mqtt_stop` cannot destroy the client mid-send.
- SoftAP httpd pause incomplete drops the sticky pause claim (resume) so SoftAP/LAN web can self-heal; if httpd is still up the claim is cleared immediately.
- BOOT (IO9) factory reset now `esp_restart()` like button/HTTP factory.
- URL Quality dither failure frees the RGB buffer (was leaking hundreds of KB PSRAM).
- `uploadFrame` finish returns error on size mismatch / commit failure (HTTP reports failure).
- `sleep_now` / hibernate refuse SoftAP with an explicit error instead of HTTP/MQTT success + silent no-op.
- Last-frame flash: write body before header; stream verifies CRC before painting the panel.
- `getMqttParam.connected` requires STA (aligned with `getDevInfo.mqttConnected`).

### Changed

- `tools/build_release.sh` runs `bump_version --check` + `gen_www.py` before build.
- `gen_www.py` uses gzip `mtime=0` for deterministic `www_html.h`.
- README SoftAP SSID docs use `EK410_<MAC6>` (matches firmware).

### 中文

- 修复按键唤醒后 CYCLE 睡眠可能永不布防、MQTT 发布与 stop 竞态、httpd pause 粘连、IO9 出厂不重启、Quality dither 泄漏、上传误报成功、SoftAP 下休眠假成功、lastfrm CRC/写序、MQTT connected 语义。
- 发布脚本增加版本/网页预检；gzip 确定性；README SoftAP SSID 文案对齐。

## [1.3.3] — 2026-11-23

### Changed

- LAN MQTT “Test connection” now saves then polls `getDevInfo` (same path as SoftAP); no longer blocks on sync `testMqtt`.

### Fixed

- MQTT display commands (`clear` / `render_text` / `render_canvas` / `display_url`) publish `cmd_ack` status `queued` on accept (was `ok` + detail `queued`), matching HTTP and CHANGELOG async semantics.
- Fragmented MQTT downlink chunk gap/overflow/orphan mid-chunk now returns `cmd_ack` error `fragment desync` instead of silent drop.

### 中文

- LAN「测试 MQTT」与 SoftAP 一致：保存后轮询状态，不再同步卡在 testMqtt。
- 刷屏类下行即时 ack 状态改为 `queued`；分片失步/溢出时上报 `fragment desync`。

## [1.3.2] — 2026-11-20

### Added

- Web local firmware upload (`POST /api/v1/system/uploadFirmware`) alongside HTTPS URL OTA; pending-verify reboot after success.
- Markdown mode: Spectra-6 background and text color controls (preview + push).

### Changed

- System page desktop layout: full-width 2-column cards (work mode / OTA stay wide).
- MQTT QoS option labels use standard QoS 0/1/2 wording; topic details copy cleaned up.

### Fixed

- STA/LAN MQTT start no longer pauses httpd (pause was leaving the device IP unreachable).
- Local OTA upload failure restores MQTT and re-arms cycle sleep; busy MQTT stop refuses the upload.
- HTTPS URL OTA now stops MQTT before download (same as local upload), refuses when upload is busy, and restores MQTT + sleep on failure / task spawn failure.
- SoftAP “Test connection” only reuses a live MQTT session when the form is unchanged since last load/save; dirty saves without an HTTP ack report unknown instead of trusting the old session; baseline is locked to the submitted payload so edits during the request stay dirty; post-save `loadCfg` keeps local MQTT edits; oversize CA errors re-enable Save/Test buttons.
- Cold-boot STA join: recoverable disconnects during `wifi_sta_start()` (ASSOC_FAIL 203 / AUTH_FAIL / handshake / no-AP) trigger up to 3 quick in-flight reconnects instead of waiting the full 20s timeout; settle delay after disconnect raised to 250ms.

### 中文

- 网页支持本地固件上传；Markdown 可调背景/文字六色。
- System 桌面端双列布局；QoS 文案规范化。
- 修复 STA 起 MQTT 误停局域网网页、OTA 失败后未恢复 MQTT/休眠、SoftAP 测试误报旧会话成功（含保存无应答与请求中再编辑）；保存后回填不覆盖本地新编辑；超长 CA 不再卡死按钮。
- HTTPS URL OTA 与本地上传对齐：先停 MQTT、busy 互斥、失败恢复 MQTT/休眠。
- 冷启动首次关联失败（如 reason=203）在连接等待窗口内快速重试，不再傻等 20 秒。

## [1.3.1] — 2026-11-19

### Changed

- Single firmware image: SoftAP XOR BLE CT-Prov in-process (no dual OTA SKU / reboot switch).
- NimBLE enabled in `sdkconfig.defaults`; PAGE/MAIN toggle NimBLE without changing boot slot.
- Release entry: `tools/build_release.sh` / `flash_release.sh` → one `dist/ek410.bin` (legacy `*_dual_sku.sh` are aliases). SoftAP↔BLE smoke: `ct_prov_xor_test.py`.
- Keep SoftAP-oriented Wi-Fi buffer defaults; do not merge old BLE-SKU tighter RX sizes into the single image.
- MQTT fragmented commands transfer the reassembly buffer to the cmd queue (no second full copy); OOM / size / queue-full return explicit acks.
- Event log: no full rewrite on download; HTTP stream snapshots then sends outside the mutex; header persist every 8 lines (sector 0 only); boot recovers incomplete tails; compact only when over capacity.
- Async display HTTP/MQTT accept replies use `queued` / `queue full` / `sleep pending`; SoftAP shows event-log unavailable on old partition tables.
- `mqtt_stop()` returns success/busy; BLE/SoftAP enter use `mqtt_stop_wait()` and abort+restore on timeout (no NimBLE start over a live MQTT client).
- Stop telemetry timer only after taking the MQTT mutex; `mqtt_restart()` / CT-Prov disable propagate stop failure so a busy client keeps periodic telemetry.
- BLE exit: coalesce heap, lean `wifi_init` after NimBLE, httpd before wifi_init + retry timer; `wifi_sta_reconnect_saved(0)` starts Wi‑Fi after teardown; `mqtt_stop` cancels start-retry so MQTT cannot restart mid-BLE.
- MQTT broker change (HTTP `setMqttParam` + CT-Prov `config.set`): stop old client first, then persist host/CA, then start — busy stop keeps old NVS and live connection.
- `cfg_set_mqtt_ca_pem` / `cfg_set_mqtt_attr` return NVS errors; CA write rolls back RAM on failure; Web reports `mqtt CA save failed` / `mqtt config save failed` instead of false OK.
- QA gate: `tools/qa_gate.py` + `qa_lib` (smoke/candidate/soak, JSON statuses); plan in `docs/qa_automation.md`.
- LAN Web: embed gzip `INDEX_HTML` (~63KB wire), chunked send, `max_open_sockets=3`; cold boot uses lean Wi‑Fi buffers; MQTT requires `largest_int≥10KB`; defer httpd until STA online so opening the device IP no longer triggers `wifi:mem fail` / `phy_track_pll` NO_MEM abort.
- Product loops: `mqttReason` / `provError` / `displayBusy` / `sleepDeferred` / batterySense; SoftAP+BLE Web entry; port↔transport warn; OTA confirm on GOT_IP/httpd; sleep/MQTT-busy/display completion surfaced in SPA.
- SoftAP+MQTT: pause SoftAP httpd → start MQTT → resume lean httpd (2 sockets); skip telemetry preview while SoftAP up so customers can join home Wi‑Fi and see `mqttConnected` before closing the hotspot.
- SoftAP MQTT test UX: reply `setMqttParam` before pause; UI polls `getDevInfo` (no racing `testMqtt`); pause API returns real stop status; `mqtt_open` checks task create; QoS change forces reconnect; expose `mqttLastError` / `mqttInfraOk`.

### 中文

- 单镜像：SoftAP 与 BLE CT-Prov 进程内互斥启停，不再切换 OTA SKU。
- 发布入口改为 `build_release.sh` / `flash_release.sh`，只产出一份 `ek410.bin`；旧双 SKU 脚本名保留为别名。
- MQTT 分片所有权转交队列；事件日志下载不再全分区擦写；旧分区表上 Web 明确显示日志不可用。
- 更换 MQTT broker：先停旧客户端再写 NVS；停止忙时保留旧配置与旧连接。
- MQTT CA / broker 持久化失败会返回错误；CA 写失败时回滚内存中的 PEM。
- 自动化验收入口 `qa_gate.py`（smoke / candidate / soak）与方案文档。
- 局域网打开设备 IP 不再因首页过大 / 内部堆过紧而重启：gzip 首页、lean Wi‑Fi、MQTT 堆门槛、STA 未上线时推迟 httpd。
- 产品闭环：MQTT/配网失败原因、刷屏忙闲、睡眠推迟、端口协议告警、SPA 开 BLE。
- SoftAP 下可连 MQTT：短停网页腾堆 → 起 MQTT → 瘦身 httpd 恢复；遥测在 SoftAP 期跳过预览图。
- SoftAP「测试 MQTT」不再与停 httpd 竞态：先回保存响应再启动；前端轮询状态；QoS 变更会重连。

---

## [1.3.0] — 2026-11-17

### Added

- Dual OTA SKU: SoftAP (`ek410`) + BLE CT-Prov (`ek410_ble`); `tools/build_dual_sku.sh` / `flash_dual_sku.sh` / `ct_prov_dual_test.py`.
- NeoLink CT-Prov SoftAP + BLE provisioning (`docs/ct_prov.md`).
- Cycle power mode: configurable online window `wakeSec` (30–600, default 120) via SoftAP / HTTP / MQTT; DeviceTypes `ek410_Eink` updated.
- Hardware RESET / power-on: exit SoftAP/BLE handoff and restore flash `lastfrm` (CamThink logo if empty).
- Deep sleep: turn off status LED, panel rail, and battery sense before `esp_deep_sleep_start`.

### Changed

- SoftAP / BLE idle auto-close default **2 minutes**.
- BLE SKU warms Wi-Fi STA for scan after NimBLE (reduced RX buffers / BA win).

### Fixed

- SoftAP web: desktop first paint no longer flashes the mobile dock layout (FOUC).
- Wi-Fi `esp_wifi_init` `NO_MEM` after NimBLE blocking NeoLink scan.

### 中文

- SoftAP / BLE 双 OTA 镜像与 NeoLink CT-Prov；cycle 在线窗 `wakeSec` 可配。
- 硬件 RESET 退出配网并恢复上次画面；深睡前关灯与面板供电。
- SoftAP 网页桌面端首屏不再闪手机布局；修复 BLE 后 Wi‑Fi 扫描初始化失败。

---

## [1.2.2] — 2026-11-16

### Added

- Persist last successful panel frame in flash (`lastfrm` partition, 960000B native 4bpp).
- HTTP `GET /api/v1/display/getLastFrameMeta` and `GET /api/v1/display/lastFrame`.
- SoftAP: auto-load current panel into content preview; "Load from panel" button.

### 中文

- 成功刷屏后把当前画面写入 Flash；Web/API 可读取并预览。

---

## [1.2.1] — 2026-11-16

### Added

- SoftAP `POST /api/v1/display/proxyImage` — device STA fetches a remote image so the Web UI can preview same-origin (no browser CORS / SoftAP WAN dependency).
- Image mode: preview-first gate — push to panel only after URL/local preview is ready; push uses browser Spectra-6 dither + `uploadFrame`.
- Repo formalization: bilingual README, `docs/`, `VERSION` / `main/version.h`, `tools/bump_version.py`.

### Changed

- Centralized firmware version string (`EK410_VERSION_STRING`) via `main/version.h`.
- Asset layout: `assets/brand/`, `assets/splash/`; UI design preview under `docs/design/`.

### 中文

- 新增设备代拉图片预览接口；网页先预览再推屏。
- 版本号统一由根目录 `VERSION` + `main/version.h` 管理。
- 仓库目录与双语 README 整理为正式工程结构。

---

## [1.2.0] — 2026-11-13

### Added

- Image quality mode (Quality / Speed) persisted in NVS; SoftAP + power API.
- URL image stage progress text on panel (Quality path).
- SoftAP image URL / local upload / Markdown / canvas editor paths.

### Fixed

- SoftAP sheet / editor layout on phone; MQTT auto-connect after STA.
- English on-panel error strings for URL decode failures.

### 中文

- 图片质量档位、URL 刷屏阶段提示、SoftAP 显示与配网体验完善。

---

## [1.1.0] — 2026-11-12

### Added

- SoftAP config SPA (`main/www`), NeoMind MQTT topics, OTA dual-partition rollback.
- Panel bring-up on VE071A / EK410 pin map (`pindefine.h`).

### 中文

- SoftAP 配置页、NeoMind MQTT、OTA 回滚；面板引脚定稿。

---

## [1.0.0] — 2026-11-11

### Added

- Initial ESP32-C61 firmware skeleton: Wi-Fi, config NVS, EPD709 driver path.

### 中文

- 首个可构建固件骨架。

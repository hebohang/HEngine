import http from 'node:http';
import fs from 'node:fs';
import crypto from 'node:crypto';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import {spawn} from 'node:child_process';

const args = process.argv.slice(2);
const htmlPath = args[0];

function optionValue(name) {
	const prefix = `${name}=`;
	const arg = args.find((value) => value.startsWith(prefix));
	return arg ? arg.substring(prefix.length) : null;
}

const filter = optionValue('--filter') || 'WebGPUBrowser.*';
const captureDirValue = optionValue('--capture-dir') || optionValue('--screenshot-dir');
const captureDir = captureDirValue ? path.resolve(captureDirValue) : null;
const useCrossOriginIsolation =
	args.includes('--cross-origin-isolated') || process.env.LLGI_WEBGPU_CROSS_ORIGIN_ISOLATED === '1';

if (!htmlPath || args.includes('--help')) {
	console.error('Usage: node run_webgpu_browser_test.mjs <LLGI_Test.html> [--filter=WebGPUBrowser.*] [--capture-dir=path|--screenshot-dir=path] [--cross-origin-isolated]');
	process.exit(2);
}

const resolvedHtmlPath = path.resolve(htmlPath);
if (!fs.existsSync(resolvedHtmlPath)) {
	console.error(`Not found: ${resolvedHtmlPath}`);
	process.exit(2);
}

const root = path.dirname(resolvedHtmlPath);
const htmlFile = path.basename(resolvedHtmlPath);
const executablePath = findBrowserExecutable();

if (captureDir) {
	fs.mkdirSync(captureDir, {recursive: true});
}

function findBrowserExecutable() {
	const candidates = [
		process.env.CHROME_PATH,
		process.env.EDGE_PATH,
		'/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
		'/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge',
		'/usr/bin/google-chrome',
		'/usr/bin/google-chrome-stable',
		'/usr/bin/chromium',
		'/usr/bin/chromium-browser',
		'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe',
		'C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe',
		'C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe',
		'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe',
	].filter(Boolean);

	for (const candidate of candidates) {
		if (fs.existsSync(candidate)) {
			return candidate;
		}
	}

	console.error('A WebGPU-capable Chrome or Edge executable is required.');
	console.error('Set CHROME_PATH, for example on Windows:');
	console.error('$env:CHROME_PATH = "C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe"');
	console.error('or on macOS:');
	console.error('CHROME_PATH="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"');
	process.exit(2);
}

function chromeGpuArgs() {
	if (process.platform === 'win32') {
		return ['--use-angle=d3d11'];
	}
	if (process.platform === 'darwin') {
		return ['--use-angle=metal'];
	}
	return [];
}

function contentType(filePath) {
	if (filePath.endsWith('.html')) return 'text/html';
	if (filePath.endsWith('.js')) return 'application/javascript';
	if (filePath.endsWith('.wasm')) return 'application/wasm';
	if (filePath.endsWith('.data')) return 'application/octet-stream';
	return 'application/octet-stream';
}

function responseHeaders(filePath) {
	const headers = {
		'Content-Type': contentType(filePath),
		'Cache-Control': 'no-store, max-age=0',
		'Pragma': 'no-cache',
		'Expires': '0',
	};

	if (useCrossOriginIsolation) {
		headers['Cross-Origin-Opener-Policy'] = 'same-origin';
		headers['Cross-Origin-Embedder-Policy'] = 'require-corp';
		headers['Cross-Origin-Resource-Policy'] = 'same-origin';
	}

	return headers;
}

const server = http.createServer((request, response) => {
	const requestUrl = new URL(request.url, 'http://127.0.0.1');
	if (requestUrl.pathname === '/favicon.ico') {
		response.writeHead(204);
		response.end();
		return;
	}

	const relativePath = decodeURIComponent(requestUrl.pathname === '/' ? `/${htmlFile}` : requestUrl.pathname);
	const filePath = path.resolve(root, `.${relativePath}`);

	if (!filePath.startsWith(root) || !fs.existsSync(filePath) || !fs.statSync(filePath).isFile()) {
		response.writeHead(404);
		response.end('Not found');
		return;
	}

	response.writeHead(200, responseHeaders(filePath));
	fs.createReadStream(filePath).pipe(response);
});

await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
const {port} = server.address();
const url = new URL(`http://127.0.0.1:${port}/${htmlFile}`);
url.searchParams.set('filter', filter);
if (captureDir) {
	url.searchParams.set('webgpu_capture', '1');
}

function delay(ms) {
	return new Promise((resolve) => setTimeout(resolve, ms));
}

async function withTimeout(promise, timeoutMs, message) {
	let timeoutId;
	const timeout = new Promise((_, reject) => {
		timeoutId = setTimeout(() => reject(new Error(message)), timeoutMs);
	});
	try {
		return await Promise.race([promise, timeout]);
	} finally {
		clearTimeout(timeoutId);
	}
}

async function getFreePort() {
	const probe = http.createServer();
	await new Promise((resolve) => probe.listen(0, '127.0.0.1', resolve));
	const freePort = probe.address().port;
	await new Promise((resolve) => probe.close(resolve));
	return freePort;
}

async function waitForDevToolsHttp(devtoolsPort, chrome, getStderr) {
	const start = Date.now();
	while (Date.now() - start < 30000) {
		if (chrome.exitCode !== null) {
			throw new Error(`Chrome exited before DevTools was ready.${getStderr() ? `\n${getStderr()}` : ''}`);
		}
		try {
			const response = await fetch(`http://127.0.0.1:${devtoolsPort}/json/version`);
			if (response.ok) {
				return;
			}
		} catch {
		}
		await delay(100);
	}
	throw new Error('Timed out waiting for Chrome DevTools HTTP endpoint.');
}

async function createPage(devtoolsPort) {
	const response = await fetch(`http://127.0.0.1:${devtoolsPort}/json/new?${encodeURIComponent('about:blank')}`, {method: 'PUT'});
	if (!response.ok) {
		throw new Error(`Failed to create Chrome page: ${response.status} ${response.statusText}`);
	}
	return await response.json();
}

class RawWebSocket {
	constructor(webSocketUrl, onText) {
		this.url = new URL(webSocketUrl);
		this.onText = onText;
		this.socket = null;
		this.buffer = Buffer.alloc(0);
		this.handshakeDone = false;
	}

	async open() {
		await withTimeout(new Promise((resolve, reject) => {
			const key = crypto.randomBytes(16).toString('base64');
			const onError = (error) => {
				if (!this.handshakeDone) {
					reject(error);
				}
			};
			this.socket = net.createConnection({host: this.url.hostname, port: Number(this.url.port)}, () => {
				const requestPath = `${this.url.pathname}${this.url.search}`;
				this.socket.write([
					`GET ${requestPath} HTTP/1.1`,
					`Host: ${this.url.host}`,
					'Upgrade: websocket',
					'Connection: Upgrade',
					'Origin: http://127.0.0.1',
					`Sec-WebSocket-Key: ${key}`,
					'Sec-WebSocket-Version: 13',
					'\r\n',
				].join('\r\n'));
			});
			this.socket.on('error', onError);
			this.socket.on('data', (chunk) => {
				try {
					this.handleData(chunk, () => {
						this.socket.off('error', onError);
						this.socket.on('error', () => {});
						resolve();
					}, reject);
				} catch (error) {
					reject(error);
				}
			});
		}), 10000, 'Timed out opening Chrome DevTools WebSocket.');
	}

	handleData(chunk, resolveOpen, rejectOpen) {
		this.buffer = Buffer.concat([this.buffer, chunk]);
		if (!this.handshakeDone) {
			const headerEnd = this.buffer.indexOf('\r\n\r\n');
			if (headerEnd < 0) {
				return;
			}

			const header = this.buffer.subarray(0, headerEnd).toString();
			if (!header.startsWith('HTTP/1.1 101')) {
				rejectOpen(new Error(`Chrome DevTools WebSocket handshake failed: ${header.split('\r\n')[0]}`));
				return;
			}

			this.handshakeDone = true;
			this.buffer = this.buffer.subarray(headerEnd + 4);
			resolveOpen();
		}

		this.parseFrames();
	}

	parseFrames() {
		while (this.buffer.length >= 2) {
			const first = this.buffer[0];
			const second = this.buffer[1];
			const opcode = first & 0x0f;
			const masked = (second & 0x80) !== 0;
			let length = second & 0x7f;
			let offset = 2;

			if (length === 126) {
				if (this.buffer.length < offset + 2) return;
				length = this.buffer.readUInt16BE(offset);
				offset += 2;
			} else if (length === 127) {
				if (this.buffer.length < offset + 8) return;
				const bigLength = this.buffer.readBigUInt64BE(offset);
				if (bigLength > BigInt(Number.MAX_SAFE_INTEGER)) {
					throw new Error('Chrome DevTools WebSocket frame is too large.');
				}
				length = Number(bigLength);
				offset += 8;
			}

			let mask;
			if (masked) {
				if (this.buffer.length < offset + 4) return;
				mask = this.buffer.subarray(offset, offset + 4);
				offset += 4;
			}

			if (this.buffer.length < offset + length) return;
			let payload = Buffer.from(this.buffer.subarray(offset, offset + length));
			this.buffer = this.buffer.subarray(offset + length);

			if (masked) {
				for (let i = 0; i < payload.length; i++) {
					payload[i] ^= mask[i % 4];
				}
			}

			if (opcode === 0x1) {
				this.onText(payload.toString());
			} else if (opcode === 0x8) {
				this.socket.end();
			} else if (opcode === 0x9) {
				this.sendFrame(0xA, payload);
			}
		}
	}

	sendText(text) {
		this.sendFrame(0x1, Buffer.from(text));
	}

	sendFrame(opcode, payload = Buffer.alloc(0)) {
		const mask = crypto.randomBytes(4);
		let header;
		if (payload.length < 126) {
			header = Buffer.alloc(2);
			header[1] = 0x80 | payload.length;
		} else if (payload.length < 65536) {
			header = Buffer.alloc(4);
			header[1] = 0x80 | 126;
			header.writeUInt16BE(payload.length, 2);
		} else {
			header = Buffer.alloc(10);
			header[1] = 0x80 | 127;
			header.writeBigUInt64BE(BigInt(payload.length), 2);
		}

		header[0] = 0x80 | opcode;
		const maskedPayload = Buffer.from(payload);
		for (let i = 0; i < maskedPayload.length; i++) {
			maskedPayload[i] ^= mask[i % 4];
		}
		this.socket.write(Buffer.concat([header, mask, maskedPayload]));
	}

	close() {
		if (this.socket) {
			this.socket.end();
			this.socket.destroy();
		}
	}
}

class CdpClient {
	constructor(webSocketUrl) {
		this.nextId = 1;
		this.pending = new Map();
		this.handlers = new Map();
		this.socket = new RawWebSocket(webSocketUrl, (text) => this.onMessage(text));
	}

	async open() {
		await this.socket.open();
	}

	on(eventName, handler) {
		const handlers = this.handlers.get(eventName) || [];
		handlers.push(handler);
		this.handlers.set(eventName, handlers);
	}

	onMessage(text) {
		const message = JSON.parse(text);
		if (message.id && this.pending.has(message.id)) {
			const {resolve, reject} = this.pending.get(message.id);
			this.pending.delete(message.id);
			if (message.error) {
				reject(new Error(message.error.message));
			} else {
				resolve(message.result);
			}
			return;
		}

		for (const handler of this.handlers.get(message.method) || []) {
			handler(message.params || {});
		}
	}

	send(method, params = {}, timeoutMs = 10000) {
		const id = this.nextId++;
		this.socket.sendText(JSON.stringify({id, method, params}));
		return withTimeout(new Promise((resolve, reject) => {
			this.pending.set(id, {resolve, reject});
		}), timeoutMs, `Timed out waiting for Chrome DevTools method ${method}.`);
	}

	close() {
		this.socket.close();
	}
}

function consoleArgumentToString(argument) {
	if ('value' in argument) {
		return String(argument.value);
	}
	return argument.description || argument.unserializableValue || '';
}

async function evaluateJson(client, expression) {
	const result = await client.send('Runtime.evaluate', {
		expression: `JSON.stringify((${expression}))`,
		awaitPromise: true,
		returnByValue: true,
	});
	return result.result && result.result.value ? JSON.parse(result.result.value) : null;
}

function sanitizeCaptureName(name) {
	const sanitized = String(name || '')
		.replace(/[<>:"/\\|?*\x00-\x1F]+/g, '_')
		.replace(/^\.+$/, '')
		.trim();
	return sanitized || 'webgpu_capture';
}

async function getCanvasScreenshotClip(client) {
	const rect = await evaluateJson(client, `(() => {
		const canvas = document.querySelector('canvas');
		if (!canvas) {
			return null;
		}
		const bounds = canvas.getBoundingClientRect();
		return {
			x: Math.max(0, bounds.x),
			y: Math.max(0, bounds.y),
			width: bounds.width,
			height: bounds.height
		};
	})()`);

	if (!rect || rect.width <= 0 || rect.height <= 0) {
		throw new Error('Browser WebGPU canvas was not found or has an invalid size.');
	}

	return {...rect, scale: 1};
}

async function writeCanvasCaptureResult(client, requestId, status, message) {
	await client.send('Runtime.evaluate', {
		expression: `(() => {
			const module = globalThis.Module;
			if (!module) {
				return;
			}
			module.llgiWebGPUCaptureResults = module.llgiWebGPUCaptureResults || {};
			module.llgiWebGPUCaptureResults[${JSON.stringify(String(requestId))}] = {
				status: ${JSON.stringify(status)},
				message: ${JSON.stringify(message || '')}
			};
		})()`,
		returnByValue: true,
	});
}

async function captureCanvasToFile(client, outputPath) {
	await client.send('Page.bringToFront');
	const screenshot = await client.send('Page.captureScreenshot', {
		format: 'png',
		clip: await getCanvasScreenshotClip(client),
		captureBeyondViewport: false,
	}, 30000);
	fs.writeFileSync(outputPath, Buffer.from(screenshot.data, 'base64'));
}

async function readCanvasCaptureRequests(client) {
	return await evaluateJson(client, `(() => {
		const module = globalThis.Module;
		if (!module || !Array.isArray(module.llgiWebGPUCaptureRequests)) {
			return [];
		}

		return module.llgiWebGPUCaptureRequests
			.filter((request) => request)
			.map((request) => {
				return {
					id: String(request.id || ''),
					name: String(request.name || '')
				};
			});
	})()`);
}

async function processCanvasCaptureRequests(client, processedRequestIds) {
	if (!captureDir) {
		return;
	}

	const requests = await readCanvasCaptureRequests(client);
	for (const request of requests || []) {
		if (!request.id || processedRequestIds.has(request.id)) {
			continue;
		}

		processedRequestIds.add(request.id);
		let captureName = sanitizeCaptureName(request.name);
		if (!captureName.toLowerCase().endsWith('.png')) {
			captureName += '.png';
		}

		const outputPath = path.join(captureDir, captureName);
		try {
			await captureCanvasToFile(client, outputPath);
			console.log(`[capture] ${outputPath}`);
			await writeCanvasCaptureResult(client, request.id, 'passed', outputPath);
		} catch (error) {
			await writeCanvasCaptureResult(client, request.id, 'failed', error.message);
			throw error;
		}
	}
}

async function waitForTestResult(client, timeoutMs, onPoll) {
	const start = Date.now();
	while (Date.now() - start < timeoutMs) {
		if (onPoll) {
			await onPoll();
		}
		const value = await evaluateJson(client, 'globalThis.Module && globalThis.Module.llgiTestResult || null');
		if (value) {
			return value;
		}
		await delay(100);
	}
	throw new Error('Timed out waiting for LLGI browser WebGPU test result.');
}

let chrome;
let client;
let userDataDir;
let chromeExit;
let chromeStderr = '';
try {
	userDataDir = fs.mkdtempSync(path.join(os.tmpdir(), 'llgi-webgpu-chrome-'));
	const devtoolsPort = await getFreePort();
	const browserArgs = [
		`--remote-debugging-port=${devtoolsPort}`,
		'--remote-debugging-address=127.0.0.1',
		'--remote-allow-origins=*',
		`--user-data-dir=${userDataDir}`,
		'--no-first-run',
		'--no-default-browser-check',
		'--enable-unsafe-webgpu',
		'--ignore-gpu-blocklist',
		...chromeGpuArgs(),
		'about:blank',
	];
	if (process.env.LLGI_WEBGPU_HEADLESS !== '0') {
		browserArgs.unshift('--headless=new');
	}
	chrome = spawn(executablePath, browserArgs, {stdio: ['ignore', 'ignore', 'pipe']});
	chrome.stderr.on('data', (chunk) => {
		chromeStderr += chunk.toString();
	});
	chromeExit = new Promise((resolve) => chrome.once('exit', resolve));

	await waitForDevToolsHttp(devtoolsPort, chrome, () => chromeStderr.trim());
	const page = await createPage(devtoolsPort);
	client = new CdpClient(page.webSocketDebuggerUrl);
	await client.open();
	client.on('Runtime.consoleAPICalled', (params) => {
		const text = (params.args || []).map(consoleArgumentToString).join(' ');
		console.log(`[browser:${params.type}] ${text}`);
	});
	client.on('Runtime.exceptionThrown', (params) => {
		const details = params.exceptionDetails || {};
		const exception = details.exception || {};
		console.error(`[browser:pageerror] ${exception.description || details.text || 'Unhandled exception'}`);
	});

	await client.send('Runtime.enable');
	await client.send('Page.enable');
	await client.send('Page.navigate', {url: url.toString()});
	const processedCaptureRequestIds = new Set();
	const value = await waitForTestResult(client, 60000, () => processCanvasCaptureRequests(client, processedCaptureRequestIds));
	await processCanvasCaptureRequests(client, processedCaptureRequestIds);
	if (!value || value.status !== 'passed') {
		console.error(value && value.message ? value.message : 'LLGI browser WebGPU test failed.');
		process.exitCode = 1;
	} else {
		await delay(500);
		const lateError = await evaluateJson(client, 'globalThis.Module && globalThis.Module.llgiLastWebGPUError || null');
		if (lateError) {
			console.error(lateError);
			process.exitCode = 1;
		}
	}
} finally {
	if (client) {
		await client.send('Browser.close', {}, 2000).catch(() => {});
		client.close();
	}
	if (chrome) {
		await Promise.race([chromeExit, delay(2000)]);
		if (chrome.exitCode === null) {
			chrome.kill();
			await Promise.race([chromeExit, delay(2000)]);
		}
	}
	if (userDataDir) {
		try {
			fs.rmSync(userDataDir, {recursive: true, force: true, maxRetries: 10, retryDelay: 100});
		} catch (error) {
			console.warn(`Failed to remove temporary browser profile ${userDataDir}: ${error.message}`);
		}
	}
	await new Promise((resolve) => server.close(resolve));
}

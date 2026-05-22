'use strict'

const http    = require('http')
const express = require('express')
const { WebSocketServer } = require('ws')
const path    = require('path')
const config  = require('./config')
const { Expo } = require('expo-server-sdk')

const app    = express()
const server = http.createServer(app)
const wss    = new WebSocketServer({ server })
const expo   = new Expo()

// ── In-memory state ──────────────────────────────────────────────────────────
let streaming    = false
let latestFrame  = null       // Buffer | null — most recent JPEG from ESP32
const alerts     = []         // newest first, capped at MAX_ALERTS
const MAX_ALERTS = 50
let alertCounter = 0
let currentMode  = 'SLEEP_OFF' // 'SLEEP_ON' (baby asleep) | 'SLEEP_OFF' (baby awake)

const pushTokens   = new Set()        // registered Expo push tokens from mobile app
const mjpegClients = new Set()    // active GET /api/stream response objects
const wsClients    = new Set()    // active browser WebSocket connections

// ── Helpers ──────────────────────────────────────────────────────────────────
function broadcast(obj) {
    const msg = JSON.stringify(obj)
    for (const ws of wsClients) {
        if (ws.readyState === 1) ws.send(msg)
    }
}

// Middleware: ESP32 must send x-device-key header matching config.
function requireKey(req, res, next) {
    if (req.headers['x-device-key'] !== config.DEVICE_KEY) {
        return res.status(401).json({ error: 'Unauthorized' })
    }
    next()
}

function pushFrameToMjpeg(buf) {
    if (mjpegClients.size === 0) return
    const header = `--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ${buf.length}\r\n\r\n`
    for (const res of mjpegClients) {
        try {
            res.write(header)
            res.write(buf)
            res.write('\r\n')
        } catch (_) {
            mjpegClients.delete(res)
        }
    }
}

// ── WebSocket ────────────────────────────────────────────────────────────────
wss.on('connection', (ws) => {
    wsClients.add(ws)
    // Sync new client immediately — no need for it to poll.
    ws.send(JSON.stringify({ type: 'state', streaming }))
    ws.send(JSON.stringify({ type: 'history', alerts }))
    ws.send(JSON.stringify({ type: 'mode', mode: currentMode }))
    ws.on('close', () => wsClients.delete(ws))
    ws.on('error', () => wsClients.delete(ws))
})

// ── Push notification helper ─────────────────────────────────────────────────
async function sendPushNotifications(alert) {
    if (pushTokens.size === 0) return
    const messages = []
    for (const token of pushTokens) {
        if (!Expo.isExpoPushToken(token)) continue
        messages.push({
            to:    token,
            sound: 'default',
            title: `Baby Alert: ${alert.type}`,
            body:  alert.message,
            data:  { alert },
        })
    }
    const chunks = expo.chunkPushNotifications(messages)
    for (const chunk of chunks) {
        try {
            const receipts = await expo.sendPushNotificationsAsync(chunk)
            receipts.forEach(r => {
                if (r.status === 'error') console.error('[Push] Error:', r.message)
            })
        } catch (err) {
            console.error('[Push] Send failed:', err.message)
        }
    }
}

// ── Body parsers (must be registered before routes) ──────────────────────────
app.use('/api/frame',  express.raw({ type: 'image/jpeg', limit: '300kb' }))
app.use('/api/alerts', express.json({ limit: '8kb' }))
app.use(express.json({ limit: '8kb' }))

// ── Static ───────────────────────────────────────────────────────────────────
app.get('/', (_req, res) => res.sendFile(path.join(__dirname, 'index.html')))

// ── Camera control (browser → server) ───────────────────────────────────────
// ESP32 polls /api/status to decide whether to capture and upload frames.
app.get('/api/status', (_req, res) => res.send(streaming ? '1' : '0'))

app.post('/api/start', (_req, res) => {
    streaming = true
    broadcast({ type: 'state', streaming })
    console.log(`[${new Date().toISOString()}] Streaming started`)
    res.json({ streaming })
})

app.post('/api/stop', (_req, res) => {
    streaming = false
    broadcast({ type: 'state', streaming })
    console.log(`[${new Date().toISOString()}] Streaming stopped`)
    res.json({ streaming })
})

// ── Baby mode (mobile app → server → ESP32 polls → Nucleo UART) ─────────────
// ESP32 polls GET /api/mode; mobile app sets via POST /api/mode.
app.get('/api/mode', (_req, res) => res.send(currentMode))

app.post('/api/mode', (req, res) => {
    const { mode } = req.body || {}
    if (mode !== 'SLEEP_ON' && mode !== 'SLEEP_OFF') {
        return res.status(400).json({ error: 'mode must be SLEEP_ON or SLEEP_OFF' })
    }
    currentMode = mode
    broadcast({ type: 'mode', mode: currentMode })
    console.log(`[${new Date().toISOString()}] Baby mode -> ${currentMode}`)
    res.json({ mode: currentMode })
})

// ── Frame upload (ESP32 → server) ────────────────────────────────────────────
app.post('/api/frame', requireKey, (req, res) => {
    if (!Buffer.isBuffer(req.body) || req.body.length === 0) {
        return res.status(400).json({ error: 'Empty or invalid frame body' })
    }
    latestFrame = req.body
    pushFrameToMjpeg(latestFrame)
    broadcast({ type: 'frame', data: latestFrame.toString('base64') })
    res.sendStatus(200)
})

// ── MJPEG stream (browser / external client → server) ────────────────────────
// Any HTTP client can open GET /api/stream for a live MJPEG feed.
// The connection is kept alive; new frames are pushed as they arrive.
app.get('/api/stream', (req, res) => {
    res.setHeader('Content-Type', 'multipart/x-mixed-replace; boundary=frame')
    res.setHeader('Cache-Control', 'no-cache, no-store')
    res.setHeader('Connection', 'keep-alive')
    res.flushHeaders()

    if (latestFrame) {
        res.write(`--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ${latestFrame.length}\r\n\r\n`)
        res.write(latestFrame)
        res.write('\r\n')
    }

    mjpegClients.add(res)
    req.on('close', () => mjpegClients.delete(res))
})

// ── Alerts (ESP32 → server) ──────────────────────────────────────────────────
app.post('/api/alerts', requireKey, async (req, res) => {
    const { device_id, type, message } = req.body || {}
    if (!type) {
        return res.status(400).json({ error: 'Missing required field: type' })
    }

    const alert = {
        id:        ++alertCounter,
        device_id: String(device_id || 'unknown'),
        type:      String(type).toUpperCase(),
        message:   String(message || type),
        timestamp: new Date().toISOString(),
    }

    alerts.unshift(alert)
    if (alerts.length > MAX_ALERTS) alerts.pop()

    broadcast({ type: 'alert', alert })
    sendPushNotifications(alert).catch(err => console.error('[Push]', err.message))
    console.log(`[${alert.timestamp}] ALERT ${alert.type} — ${alert.message}`)
    res.status(201).json(alert)
})

// ── Recent alerts (browser → server) ────────────────────────────────────────
app.get('/api/alerts', (_req, res) => res.json(alerts))

// ── Mobile push token registration ───────────────────────────────────────────
app.post('/api/register-token', (req, res) => {
    const { token } = req.body || {}
    if (!token || !Expo.isExpoPushToken(token)) {
        return res.status(400).json({ error: 'Invalid Expo push token' })
    }
    pushTokens.add(token)
    console.log(`[Push] Token registered (total: ${pushTokens.size})`)
    res.json({ ok: true })
})

// ── Start ────────────────────────────────────────────────────────────────────
server.listen(config.PORT, '0.0.0.0', () => {
    console.log(`\nBaby Monitor backend on http://localhost:${config.PORT}`)
    console.log(`Dashboard:   http://localhost:${config.PORT}/`)
    console.log(`MJPEG stream: http://localhost:${config.PORT}/api/stream`)
    console.log(`Device key:  ${config.DEVICE_KEY}\n`)
})

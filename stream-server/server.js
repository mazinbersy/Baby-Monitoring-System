const express = require("express");
const { WebSocketServer } = require("ws");
const http = require("http");

const app = express();
const server = http.createServer(app);
const wss = new WebSocketServer({ server });

// Track connected browser clients
const clients = new Set();

let streaming = false;

wss.on("connection", (ws) => {
  clients.add(ws);
  console.log(`Browser connected. Total: ${clients.size}`);
  ws.on("close", () => {
    clients.delete(ws);
    console.log(`Browser disconnected. Total: ${clients.size}`);
  });
});

// ESP32 polls this to know whether to stream
app.get("/status", (req, res) => {
  res.send(streaming ? "1" : "0");
});

app.post("/start", (req, res) => {
  streaming = true;
  res.sendStatus(200);
});

app.post("/stop", (req, res) => {
  streaming = false;
  res.sendStatus(200);
});

// ESP32 posts raw JPEG frames here
app.post("/frame", express.raw({ type: "image/jpeg", limit: "1mb" }), (req, res) => {
  const frame = req.body;
  // Forward to all connected browsers as base64
  const b64 = frame.toString("base64");
  for (const client of clients) {
    if (client.readyState === 1) {
      client.send(b64);
    }
  }
  res.sendStatus(200);
});

// Serve the web app
app.get("/", (req, res) => {
  res.sendFile(__dirname + "/index.html");
});

const PORT = 3000;
server.listen(PORT, "0.0.0.0", () => {
  console.log(`Server running at http://localhost:${PORT}`);
});
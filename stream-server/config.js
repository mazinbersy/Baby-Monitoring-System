'use strict'

// Central configuration.
// Override any value with an environment variable before starting the server.
// Example:  PORT=8080 DEVICE_KEY=my-secret node server.js

module.exports = {
    PORT:       Number(process.env.PORT)       || 3000,
    DEVICE_KEY: process.env.DEVICE_KEY         || 'bms-secret-key-2024',
}

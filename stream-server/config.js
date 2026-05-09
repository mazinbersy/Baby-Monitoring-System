'use strict'
module.exports = {
    PORT:       Number(process.env.PORT)       || 3000,
    DEVICE_KEY: process.env.DEVICE_KEY         || 'bms-secret-key-2024',
}

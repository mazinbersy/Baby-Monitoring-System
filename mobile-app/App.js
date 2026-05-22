import { registerRootComponent } from 'expo'
import { useState, useEffect, useRef, useCallback } from 'react'
import {
    StyleSheet, View, Text, Image, FlatList,
    TouchableOpacity, SafeAreaView, ActivityIndicator, Platform,
} from 'react-native'
import * as Notifications from 'expo-notifications'
import * as Device from 'expo-device'
import Constants from 'expo-constants'

// ─── Configuration ────────────────────────────────────────────────────────────
// Replace with your actual Railway service domain.
const SERVER_URL = 'https://baby-monitoring-system-production.up.railway.app'
const WS_URL     = SERVER_URL.replace(/^https/, 'wss').replace(/^http/, 'ws')

Notifications.setNotificationHandler({
    handleNotification: async () => ({
        shouldShowAlert: true,
        shouldPlaySound: true,
        shouldSetBadge: true,
    }),
})

export default function App() {
    const [frameUri,   setFrameUri]   = useState(null)
    const [alerts,     setAlerts]     = useState([])
    const [streaming,  setStreaming]  = useState(false)
    const [connected,  setConnected]  = useState(false)
    const [mode,   setMode] = useState('SLEEP_OFF')
    const wsRef = useRef(null)

    const connectWebSocket = useCallback(() => {
        if (wsRef.current) wsRef.current.close()
        const ws = new WebSocket(WS_URL)
        wsRef.current = ws

        ws.onopen  = () => setConnected(true)
        ws.onclose = () => {
            setConnected(false)
            setTimeout(connectWebSocket, 3000)
        }
        ws.onerror = () => {}
        ws.onmessage = (e) => {
            const msg = JSON.parse(e.data)
            if (msg.type === 'frame') {
                setFrameUri(`data:image/jpeg;base64,${msg.data}`)
            } else if (msg.type === 'state') {
                setStreaming(msg.streaming)
                if (!msg.streaming) setFrameUri(null)
            } else if (msg.type === 'alert') {
                setAlerts(prev => [msg.alert, ...prev].slice(0, 50))
                // App is in foreground — start stream so parent sees it immediately
                fetch(`${SERVER_URL}/api/start`, { method: 'POST' }).catch(() => {})
            } else if (msg.type === 'history') {
                setAlerts(msg.alerts)
            } else if (msg.type === 'mode') {
                setMode(msg.mode)
            }
        }
    }, [])

    useEffect(() => {
        registerForPushNotifications()
        connectWebSocket()

        // App opened by tapping a notification — start the stream
        const sub = Notifications.addNotificationResponseReceivedListener(() => {
            fetch(`${SERVER_URL}/api/start`, { method: 'POST' }).catch(() => {})
        })

        return () => {
            wsRef.current?.close()
            sub.remove()
        }
    }, [connectWebSocket])

    async function registerForPushNotifications() {
        if (!Device.isDevice) {
            console.log('[Push] Simulator — skipping push registration')
            return
        }
        const { status: existing } = await Notifications.getPermissionsAsync()
        let final = existing
        if (existing !== 'granted') {
            const { status } = await Notifications.requestPermissionsAsync()
            final = status
        }
        if (final !== 'granted') return

        const projectId = Constants.expoConfig?.extra?.eas?.projectId
        const tokenData = await Notifications.getExpoPushTokenAsync(projectId ? { projectId } : {})
        const token = tokenData.data

        try {
            await fetch(`${SERVER_URL}/api/register-token`, {
                method:  'POST',
                headers: { 'Content-Type': 'application/json' },
                body:    JSON.stringify({ token }),
            })
            console.log('[Push] Token registered')
        } catch (err) {
            console.warn('[Push] Registration failed:', err.message)
        }
    }

    async function toggleStream() {
        try {
            await fetch(`${SERVER_URL}${streaming ? '/api/stop' : '/api/start'}`, { method: 'POST' })
        } catch (err) {
            console.warn('[Stream] Toggle failed:', err.message)
        }
    }

    async function setModeCmd(newMode) {
        setMode(newMode)  // optimistic update — UI responds instantly
        console.log('[Mode] Sending to server:', newMode)
        try {
            await fetch(`${SERVER_URL}/api/mode`, {
                method:  'POST',
                headers: { 'Content-Type': 'application/json' },
                body:    JSON.stringify({ mode: newMode }),
            })
        } catch (err) {
            console.warn('[Mode] Set failed:', err.message)
        }
    }

    const alertColor = (type) => {
        if (type === 'CRYING')     return '#e74c3c'
        if (type === 'NO_MOTION')  return '#f59e0b'
        if (type === 'BABY_AWAKE') return '#22c55e'
        if (type === 'DOOR_CROSS') return '#a855f7'
        if (type.includes('TEMP')) return '#e67e22'
        if (type === 'MOVEMENT')   return '#3498db'
        return '#95a5a6'
    }

    return (
        <SafeAreaView style={styles.container}>
            {/* Header */}
            <View style={styles.header}>
                <Text style={styles.title}>Baby Monitor</Text>
                <View style={styles.statusRow}>
                    <View style={[styles.dot, connected ? styles.dotGreen : styles.dotGray]} />
                    <Text style={styles.statusText}>{connected ? 'Connected' : 'Connecting…'}</Text>
                </View>
            </View>

            {/* Stream view */}
            <View style={styles.streamBox}>
                {streaming && frameUri ? (
                    <Image source={{ uri: frameUri }} style={styles.streamImage} resizeMode="contain" />
                ) : (
                    <View style={styles.streamPlaceholder}>
                        {streaming && !frameUri ? (
                            <ActivityIndicator size="large" color="#fff" />
                        ) : (
                            <Text style={styles.placeholderText}>Stream is off</Text>
                        )}
                    </View>
                )}
            </View>

            {/* Controls */}
            <TouchableOpacity
                style={[styles.button, streaming ? styles.buttonStop : styles.buttonStart]}
                onPress={toggleStream}
            >
                <Text style={styles.buttonText}>{streaming ? 'Stop Stream' : 'Start Stream'}</Text>
            </TouchableOpacity>

            {/* Baby mode */}
            <Text style={styles.sectionTitle}>Baby Status</Text>
            <View style={styles.modeRow}>
                <TouchableOpacity
                    style={[styles.modeButton, mode === 'SLEEP_OFF' && styles.modeButtonAwake]}
                    onPress={() => setModeCmd('SLEEP_OFF')}
                >
                    <Text style={styles.modeButtonText}>Baby is Awake</Text>
                </TouchableOpacity>
                <TouchableOpacity
                    style={[styles.modeButton, mode === 'SLEEP_ON' && styles.modeButtonAsleep]}
                    onPress={() => setModeCmd('SLEEP_ON')}
                >
                    <Text style={styles.modeButtonText}>Baby is Asleep</Text>
                </TouchableOpacity>
            </View>

            {/* Alert list */}
            <Text style={styles.sectionTitle}>Recent Alerts</Text>
            <FlatList
                data={alerts}
                keyExtractor={(item) => String(item.id)}
                style={styles.list}
                renderItem={({ item }) => (
                    <View style={[styles.alertCard, { borderLeftColor: alertColor(item.type) }]}>
                        <View style={styles.alertTop}>
                            <Text style={[styles.alertType, { color: alertColor(item.type) }]}>
                                {item.type}
                            </Text>
                            <Text style={styles.alertTime}>
                                {new Date(item.timestamp).toLocaleTimeString()}
                            </Text>
                        </View>
                        <Text style={styles.alertMessage}>{item.message}</Text>
                    </View>
                )}
                ListEmptyComponent={
                    <Text style={styles.empty}>No alerts yet</Text>
                }
            />
        </SafeAreaView>
    )
}

const styles = StyleSheet.create({
    container:         { flex: 1, backgroundColor: '#0f172a' },
    header:            { paddingHorizontal: 20, paddingTop: 12, paddingBottom: 8 },
    title:             { fontSize: 22, fontWeight: '700', color: '#f1f5f9' },
    statusRow:         { flexDirection: 'row', alignItems: 'center', marginTop: 4 },
    dot:               { width: 8, height: 8, borderRadius: 4, marginRight: 6 },
    dotGreen:          { backgroundColor: '#22c55e' },
    dotGray:           { backgroundColor: '#64748b' },
    statusText:        { fontSize: 13, color: '#94a3b8' },
    streamBox:         {
        marginHorizontal: 16, marginTop: 8,
        height: 220, borderRadius: 12, overflow: 'hidden',
        backgroundColor: '#1e293b',
    },
    streamImage:       { width: '100%', height: '100%' },
    streamPlaceholder: { flex: 1, justifyContent: 'center', alignItems: 'center' },
    placeholderText:   { color: '#475569', fontSize: 16 },
    button:            {
        marginHorizontal: 16, marginTop: 12, marginBottom: 4,
        paddingVertical: 12, borderRadius: 10, alignItems: 'center',
    },
    buttonStart:       { backgroundColor: '#22c55e' },
    buttonStop:        { backgroundColor: '#ef4444' },
    buttonText:        { color: '#fff', fontWeight: '700', fontSize: 16 },
    sectionTitle:      { color: '#94a3b8', fontSize: 13, fontWeight: '600', marginLeft: 20, marginTop: 16, marginBottom: 8 },
    list:              { flex: 1, paddingHorizontal: 16 },
    alertCard:         {
        backgroundColor: '#1e293b', borderRadius: 10,
        padding: 12, marginBottom: 8,
        borderLeftWidth: 4,
    },
    alertTop:          { flexDirection: 'row', justifyContent: 'space-between', marginBottom: 4 },
    alertType:         { fontWeight: '700', fontSize: 14 },
    alertTime:         { color: '#64748b', fontSize: 12 },
    alertMessage:      { color: '#cbd5e1', fontSize: 13 },
    empty:             { color: '#475569', textAlign: 'center', marginTop: 20 },
    modeRow:           { flexDirection: 'row', marginHorizontal: 16, marginBottom: 4, gap: 8 },
    modeButton:        {
        flex: 1, paddingVertical: 12, borderRadius: 10, alignItems: 'center',
        backgroundColor: '#1e293b', borderWidth: 2, borderColor: '#334155',
    },
    modeButtonAwake:   { borderColor: '#22c55e', backgroundColor: '#052e16' },
    modeButtonAsleep:  { borderColor: '#6366f1', backgroundColor: '#1e1b4b' },
    modeButtonText:    { color: '#f1f5f9', fontWeight: '600', fontSize: 14 },
})

registerRootComponent(App)

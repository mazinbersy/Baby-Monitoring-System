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
            } else if (msg.type === 'history') {
                setAlerts(msg.alerts)
            }
        }
    }, [])

    useEffect(() => {
        registerForPushNotifications()
        connectWebSocket()
        return () => wsRef.current?.close()
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

    const alertColor = (type) => {
        if (type === 'CRYING')   return '#e74c3c'
        if (type.includes('TEMP')) return '#e67e22'
        if (type === 'MOVEMENT') return '#3498db'
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
})

registerRootComponent(App)

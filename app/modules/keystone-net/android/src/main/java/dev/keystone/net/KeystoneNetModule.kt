package dev.keystone.net

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import expo.modules.kotlin.modules.Module
import expo.modules.kotlin.modules.ModuleDefinition
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import java.security.MessageDigest
import java.security.cert.CertificateException
import java.security.cert.X509Certificate
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicBoolean
import javax.net.ssl.SSLContext
import javax.net.ssl.X509TrustManager

// Certificate-pinned WebSocket and mDNS discovery for the Keystone hub.
// JS contract: app/src/native/types.ts. UNVERIFIED on a device (written without an Android
// toolchain); see docs/app-protocol.md section 10.

private const val EVENT_SOCKET = "onSocketEvent"
private const val EVENT_HUB_FOUND = "onHubFound"
private const val EVENT_HUB_LOST = "onHubLost"
private const val SERVICE_TYPE = "_keystone._tcp."

private fun hex(bytes: ByteArray): String =
  bytes.joinToString(":") { "%02X".format(it.toInt() and 0xff) }

private fun parsePin(s: String): ByteArray? {
  val h = s.replace(":", "").trim()
  if (h.length != 64 || !h.all { it in '0'..'9' || it in 'a'..'f' || it in 'A'..'F' }) return null
  return ByteArray(32) { i -> h.substring(i * 2, i * 2 + 2).toInt(16).toByte() }
}

/** Trusts exactly one certificate: the one whose SHA-256 is the pin. Nothing else is checked
 *  (not the chain, not the host name): the pin IS the identity. With no pin (`connectLearning`, first-time
 *  setup only) it accepts any certificate and just records what it was. */
private class PinTrust(private val pin: ByteArray?) : X509TrustManager {
  @Volatile var seen: String? = null
  @Volatile var mismatch: Boolean = false

  override fun checkClientTrusted(chain: Array<out X509Certificate>?, authType: String?) {
    throw CertificateException("client certificates are not used")
  }

  override fun checkServerTrusted(chain: Array<out X509Certificate>?, authType: String?) {
    val leaf = chain?.firstOrNull() ?: throw CertificateException("the server sent no certificate")
    val digest = MessageDigest.getInstance("SHA-256").digest(leaf.encoded)
    seen = hex(digest)
    if (pin != null && !MessageDigest.isEqual(digest, pin)) {
      mismatch = true
      throw CertificateException("certificate does not match the pinned fingerprint")
    }
  }

  override fun getAcceptedIssuers(): Array<X509Certificate> = emptyArray()
}

private class Conn(val ws: WebSocket, val trust: PinTrust?)

class KeystoneNetModule : Module() {
  private val conns = ConcurrentHashMap<String, Conn>()

  // Discovery state (touched from NsdManager callbacks, which run on one binder thread).
  private var discovery: NsdManager.DiscoveryListener? = null
  private val resolveQueue = ArrayDeque<NsdServiceInfo>()
  private var resolving = false

  private val context: Context
    get() = requireNotNull(appContext.reactContext) { "no React context" }

  override fun definition() = ModuleDefinition {
    Name("KeystoneNet")
    Events(EVENT_SOCKET, EVENT_HUB_FOUND, EVENT_HUB_LOST)

    Function("connect") { id: String, url: String, fingerprint: String -> connect(id, url, fingerprint) }
    Function("connectLearning") { id: String, url: String -> openSocket(id, url, null) }
    Function("send") { id: String, text: String -> conns[id]?.ws?.send(text); Unit }
    Function("close") { id: String -> conns.remove(id)?.ws?.close(1000, null); Unit }
    Function("startDiscovery") { startDiscovery() }
    Function("stopDiscovery") { stopDiscovery() }

    OnDestroy {
      stopDiscovery()
      conns.values.forEach { it.ws.cancel() }
      conns.clear()
    }
  }

  // ---- pinned WebSocket ----------------------------------------------------------------

  private fun emit(id: String, kind: String, vararg extra: Pair<String, Any?>) {
    sendEvent(EVENT_SOCKET, mapOf<String, Any?>("id" to id, "kind" to kind) + extra)
  }

  private fun connect(id: String, url: String, fingerprint: String) {
    val pin = parsePin(fingerprint)
    if (pin == null) {
      emit(id, "error", "message" to "bad fingerprint")
      emit(id, "close")
      return
    }
    openSocket(id, url, pin)
  }

  /** `pin` null = learn: accept any certificate and report its fingerprint in `open`. */
  private fun openSocket(id: String, url: String, pin: ByteArray?) {
    val trust = PinTrust(pin)
    val tls = SSLContext.getInstance("TLS").apply { init(null, arrayOf(trust), null) }
    val client = OkHttpClient.Builder()
      .sslSocketFactory(tls.socketFactory, trust)
      .hostnameVerifier { _, _ -> true } // safe only because the certificate itself is pinned
      .readTimeout(0, java.util.concurrent.TimeUnit.MILLISECONDS)
      .build()
    val closedOnce = AtomicBoolean(false)
    val finish = {
      if (closedOnce.compareAndSet(false, true)) {
        conns.remove(id)
        emit(id, "close")
      }
    }
    val ws = client.newWebSocket(Request.Builder().url(url).build(), object : WebSocketListener() {
      // onOpen only runs after the handshake, i.e. after PinTrust accepted the certificate.
      override fun onOpen(webSocket: WebSocket, response: Response) {
        emit(id, "open", "fingerprint" to (trust.seen ?: ""))
      }

      override fun onMessage(webSocket: WebSocket, text: String) {
        emit(id, "message", "data" to text)
      }

      override fun onClosing(webSocket: WebSocket, code: Int, reason: String) {
        webSocket.close(code, null)
      }

      override fun onClosed(webSocket: WebSocket, code: Int, reason: String) = finish()

      override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
        if (trust.mismatch) {
          emit(id, "error", "pinMismatch" to true, "seen" to trust.seen, "message" to "certificate mismatch")
        } else {
          emit(id, "error", "message" to (t.message ?: t.javaClass.simpleName))
        }
        finish()
        client.dispatcher.executorService.shutdown()
      }
    })
    conns[id] = Conn(ws, trust)
  }

  // ---- mDNS discovery -----------------------------------------------------------------

  private fun nsd(): NsdManager = context.getSystemService(Context.NSD_SERVICE) as NsdManager

  private fun startDiscovery() {
    if (discovery != null) return
    val listener = object : NsdManager.DiscoveryListener {
      override fun onDiscoveryStarted(serviceType: String) {}
      override fun onDiscoveryStopped(serviceType: String) {}
      override fun onStartDiscoveryFailed(serviceType: String, errorCode: Int) { discovery = null }
      override fun onStopDiscoveryFailed(serviceType: String, errorCode: Int) {}
      override fun onServiceFound(info: NsdServiceInfo) {
        synchronized(resolveQueue) { resolveQueue.addLast(info) }
        resolveNext()
      }
      override fun onServiceLost(info: NsdServiceInfo) {
        sendEvent(EVENT_HUB_LOST, mapOf<String, Any?>("instance" to info.serviceName))
      }
    }
    discovery = listener
    nsd().discoverServices(SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD, listener)
  }

  private fun stopDiscovery() {
    val l = discovery ?: return
    discovery = null
    synchronized(resolveQueue) { resolveQueue.clear(); resolving = false }
    try { nsd().stopServiceDiscovery(l) } catch (_: IllegalArgumentException) {}
  }

  // NsdManager resolves one service at a time; a second concurrent call fails with FAILURE_ALREADY_ACTIVE.
  private fun resolveNext() {
    val next = synchronized(resolveQueue) {
      if (resolving) return
      val n = resolveQueue.removeFirstOrNull() ?: return
      resolving = true
      n
    }
    val done = {
      synchronized(resolveQueue) { resolving = false }
      resolveNext()
    }
    @Suppress("DEPRECATION")
    nsd().resolveService(next, object : NsdManager.ResolveListener {
      override fun onResolveFailed(info: NsdServiceInfo, errorCode: Int) = done()

      override fun onServiceResolved(info: NsdServiceInfo) {
        val attrs = info.attributes
        fun txt(k: String): String? = attrs[k]?.toString(Charsets.UTF_8)
        if (txt("v") == "1") {
          @Suppress("DEPRECATION")
          val host = info.host
          sendEvent(EVENT_HUB_FOUND, mapOf<String, Any?>(
            "instance" to info.serviceName,
            "name" to txt("name"),
            // Not hostName: that can do a blocking reverse lookup on this callback thread.
            "host" to (host?.hostAddress ?: ""),
            "address" to host?.hostAddress,
            "port" to info.port,
            "tls" to (txt("tls") == "1"),
          ))
        }
        done()
      }
    })
  }
}

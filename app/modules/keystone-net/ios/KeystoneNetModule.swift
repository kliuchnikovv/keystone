import CryptoKit
import ExpoModulesCore
import Foundation
import Security

// Certificate-pinned WebSocket and mDNS discovery for the Keystone hub.
// JS contract: app/src/native/types.ts. UNVERIFIED on a device (written without Xcode);
// see docs/app-protocol.md section 10.

private func hexColon(_ bytes: [UInt8]) -> String {
  bytes.map { String(format: "%02X", $0) }.joined(separator: ":")
}

private func parsePin(_ s: String) -> [UInt8]? {
  let h = s.replacingOccurrences(of: ":", with: "").trimmingCharacters(in: .whitespaces)
  guard h.count == 64 else { return nil }
  var out: [UInt8] = []
  var i = h.startIndex
  while i < h.endIndex {
    let j = h.index(i, offsetBy: 2)
    guard let b = UInt8(h[i..<j], radix: 16) else { return nil }
    out.append(b)
    i = j
  }
  return out
}

/// One pinned WebSocket. The URLSession delegate decides trust: the server certificate is
/// accepted only if its SHA-256 equals the pin, and `open` is only reported after the
/// handshake, so the app never sends the token to an unverified server.
private final class PinnedSocket: NSObject, URLSessionWebSocketDelegate {
  let id: String
  let pin: [UInt8]
  let emit: (String, String, [String: Any?]) -> Void
  private var session: URLSession?
  private var task: URLSessionWebSocketTask?
  private var seen: String?
  private var mismatch = false
  private var finished = false
  private let lock = NSLock()

  init(id: String, pin: [UInt8], emit: @escaping (String, String, [String: Any?]) -> Void) {
    self.id = id
    self.pin = pin
    self.emit = emit
  }

  func start(url: URL) {
    let s = URLSession(configuration: .ephemeral, delegate: self, delegateQueue: nil)
    session = s
    let t = s.webSocketTask(with: url)
    task = t
    t.resume()
  }

  func send(_ text: String) {
    task?.send(.string(text)) { _ in }
  }

  func close() {
    task?.cancel(with: .normalClosure, reason: nil)
  }

  private func finish() {
    lock.lock()
    let first = !finished
    finished = true
    lock.unlock()
    if first {
      emit(id, "close", [:])
      session?.invalidateAndCancel()
    }
  }

  private func receiveLoop() {
    task?.receive { [weak self] result in
      guard let self = self else { return }
      switch result {
      case .success(let message):
        if case .string(let text) = message { self.emit(self.id, "message", ["data": text]) }
        self.receiveLoop()
      case .failure:
        break  // didCompleteWithError / didClose report the end
      }
    }
  }

  // Server trust: the pin is the identity (no chain, no host name check).
  func urlSession(_ session: URLSession, didReceive challenge: URLAuthenticationChallenge,
                  completionHandler: @escaping (URLSession.AuthChallengeDisposition, URLCredential?) -> Void) {
    guard challenge.protectionSpace.authenticationMethod == NSURLAuthenticationMethodServerTrust,
          let trust = challenge.protectionSpace.serverTrust else {
      completionHandler(.performDefaultHandling, nil)
      return
    }
    guard let chain = SecTrustCopyCertificateChain(trust) as? [SecCertificate], let leaf = chain.first else {
      completionHandler(.cancelAuthenticationChallenge, nil)
      return
    }
    let digest = Array(SHA256.hash(data: SecCertificateCopyData(leaf) as Data))
    seen = hexColon(digest)
    if digest == pin {
      completionHandler(.useCredential, URLCredential(trust: trust))
    } else {
      mismatch = true
      completionHandler(.cancelAuthenticationChallenge, nil)
    }
  }

  func urlSession(_ session: URLSession, webSocketTask: URLSessionWebSocketTask, didOpenWithProtocol proto: String?) {
    emit(id, "open", ["fingerprint": seen ?? ""])
    receiveLoop()
  }

  func urlSession(_ session: URLSession, webSocketTask: URLSessionWebSocketTask,
                  didCloseWith closeCode: URLSessionWebSocketTask.CloseCode, reason: Data?) {
    finish()
  }

  func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
    lock.lock()
    let already = finished
    lock.unlock()
    if let error = error, !already {
      if mismatch {
        emit(id, "error", ["pinMismatch": true, "seen": seen, "message": "certificate mismatch"])
      } else {
        emit(id, "error", ["message": error.localizedDescription])
      }
    }
    finish()
  }
}

/// Browses `_keystone._tcp` and resolves each hub to an address and port.
private final class HubBrowser: NSObject, NetServiceBrowserDelegate, NetServiceDelegate {
  private let browser = NetServiceBrowser()
  private var pending: [String: NetService] = [:]  // kept alive while resolving
  let onFound: ([String: Any?]) -> Void
  let onLost: (String) -> Void

  init(onFound: @escaping ([String: Any?]) -> Void, onLost: @escaping (String) -> Void) {
    self.onFound = onFound
    self.onLost = onLost
    super.init()
    browser.delegate = self
  }

  func start() { browser.searchForServices(ofType: "_keystone._tcp.", inDomain: "local.") }

  func stop() {
    browser.stop()
    pending.values.forEach { $0.stop() }
    pending.removeAll()
  }

  func netServiceBrowser(_ browser: NetServiceBrowser, didFind service: NetService, moreComing: Bool) {
    pending[service.name] = service
    service.delegate = self
    service.resolve(withTimeout: 5)
  }

  func netServiceBrowser(_ browser: NetServiceBrowser, didRemove service: NetService, moreComing: Bool) {
    pending[service.name] = nil
    onLost(service.name)
  }

  func netServiceDidResolveAddress(_ sender: NetService) {
    pending[sender.name] = nil
    var txt: [String: String] = [:]
    if let data = sender.txtRecordData() {
      for (k, v) in NetService.dictionary(fromTXTRecord: data) { txt[k] = String(data: v, encoding: .utf8) }
    }
    guard txt["v"] == "1" else { return }
    var ip: String?
    for data in sender.addresses ?? [] {
      data.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
        guard let base = raw.baseAddress else { return }
        let sa = base.assumingMemoryBound(to: sockaddr.self)
        if sa.pointee.sa_family == sa_family_t(AF_INET) {
          var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
          if getnameinfo(sa, socklen_t(data.count), &host, socklen_t(host.count), nil, 0, NI_NUMERICHOST) == 0 {
            ip = String(cString: host)
          }
        }
      }
      if ip != nil { break }
    }
    onFound([
      "instance": sender.name,
      "name": txt["name"],
      "host": sender.hostName ?? "",
      "address": ip,
      "port": sender.port,
      "tls": txt["tls"] == "1",
    ])
  }

  func netService(_ sender: NetService, didNotResolve errorDict: [String: NSNumber]) {
    pending[sender.name] = nil
  }
}

public final class KeystoneNetModule: Module {
  private var sockets: [String: PinnedSocket] = [:]
  private var browser: HubBrowser?
  private let lock = NSLock()

  private func emit(_ id: String, _ kind: String, _ extra: [String: Any?]) {
    var body: [String: Any?] = ["id": id, "kind": kind]
    for (k, v) in extra { body[k] = v }
    sendEvent("onSocketEvent", body)
    if kind == "close" {
      lock.lock(); sockets[id] = nil; lock.unlock()
    }
  }

  public func definition() -> ModuleDefinition {
    Name("KeystoneNet")
    Events("onSocketEvent", "onHubFound", "onHubLost")

    Function("connect") { (id: String, url: String, fingerprint: String) in
      guard let pin = parsePin(fingerprint), let u = URL(string: url) else {
        self.emit(id, "error", ["message": "bad fingerprint or url"])
        self.emit(id, "close", [:])
        return
      }
      let s = PinnedSocket(id: id, pin: pin) { [weak self] id, kind, extra in self?.emit(id, kind, extra) }
      self.lock.lock(); self.sockets[id] = s; self.lock.unlock()
      s.start(url: u)
    }

    Function("send") { (id: String, text: String) in
      self.lock.lock(); let s = self.sockets[id]; self.lock.unlock()
      s?.send(text)
    }

    Function("close") { (id: String) in
      self.lock.lock(); let s = self.sockets[id]; self.lock.unlock()
      s?.close()
    }

    Function("startDiscovery") {
      DispatchQueue.main.async {
        if self.browser != nil { return }
        let b = HubBrowser(
          onFound: { [weak self] hub in self?.sendEvent("onHubFound", hub) },
          onLost: { [weak self] name in self?.sendEvent("onHubLost", ["instance": name]) })
        self.browser = b
        b.start()
      }
    }

    Function("stopDiscovery") {
      DispatchQueue.main.async {
        self.browser?.stop()
        self.browser = nil
      }
    }

    OnDestroy {
      DispatchQueue.main.async {
        self.browser?.stop()
        self.browser = nil
      }
      self.lock.lock()
      let all = Array(self.sockets.values)
      self.sockets.removeAll()
      self.lock.unlock()
      all.forEach { $0.close() }
    }
  }
}

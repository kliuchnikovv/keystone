import CoreLocation
import Foundation

/// Watches a circular region and says when the phone enters or leaves it.
///
/// The geofence is what makes "when I come home" work at all — the app itself is
/// killed by iOS the moment it goes to the background, so the *system* has to
/// wake us. `CLLocationManager` does that: the region monitoring survives the
/// app being closed, and the delegate callback brings it back.
///
/// Two details matter for a home automation:
///
///   - **Hysteresis.** A single radius makes the light flicker on the edge of
///     the zone: entering at 100 m and leaving at 100 m means a short walk past
///     the gate toggles it. So there are two radii — enter at the inner one,
///     leave at the outer one.
///   - **Confirmation.** Leaving for five minutes to buy bread should not turn
///     the lights off. So "away" is only reported after the phone has been
///     outside for a while; the confirmation is done here, in the native code,
///     so the hub only ever sees clean "home" / "away".
///
/// The callback fires on the main thread; the module hands the result to JS,
/// which reports it to the hub.
final class GeofenceWatcher: NSObject, CLLocationManagerDelegate {
  /// How far outside the zone the phone must get before "away" is reported.
  private static let confirmSeconds: TimeInterval = 300  // 5 minutes

  private let manager = CLLocationManager()
  private let onChange: (Bool) -> Void  // true = home, false = away
  private let onError: (String) -> Void

  /// The zone the phone is watching (metres).
  private var radius: CLLocationDistance = 150
  private var latitude: CLLocationDegrees = 0
  private var longitude: CLLocationDegrees = 0
  private var monitoring = false

  /// When the phone was last seen outside the zone; nil while it is inside.
  private var outsideSince: Date?
  /// What we last told the caller — so the same state is not reported twice.
  private var lastReported: Bool?

  private var innerRadius: CLLocationDistance { radius }
  private var outerRadius: CLLocationDistance { radius * 1.5 }

  init(onChange: @escaping (Bool) -> Void, onError: @escaping (String) -> Void) {
    self.onChange = onChange
    self.onError = onError
    super.init()
    manager.delegate = self
    manager.desiredAccuracy = kCLLocationAccuracyHundredMeters
    // The geofence is what wakes the app; a continuous location feed would only
    // drain the battery.
    manager.pausesLocationUpdatesAutomatically = true
  }

  /// Is the system allowed to tell us about the region?
  func authorizationStatus() -> String {
    switch manager.authorizationStatus {
    case .notDetermined: return "notDetermined"
    case .restricted: return "restricted"
    case .denied: return "denied"
    case .authorizedAlways: return "always"
    case .authorizedWhenInUse: return "whenInUse"
    @unknown default: return "unknown"
    }
  }

  /// Ask for the permission (the app must ask when it is in the foreground).
  func requestAuthorization() {
    manager.requestAlwaysAuthorization()
  }

  /// Start watching the zone. `radius` is in metres.
  func start(latitude: CLLocationDegrees, longitude: CLLocationDegrees, radius: CLLocationDistance) {
    stopMonitoring()
    self.latitude = latitude
    self.longitude = longitude
    self.radius = max(50, min(radius, 1000))
    guard CLLocationManager.isMonitoringAvailable(for: CLCircularRegion.self) else {
      onError("This device cannot watch a region.")
      return
    }
    guard manager.authorizationStatus == .authorizedAlways else {
      // Without "always" the region is only monitored while the app is in the
      // foreground, which defeats the purpose.
      onError("The app needs the \"always\" location permission to notice when you arrive.")
      requestAuthorization()
      return
    }
    let center = CLLocationCoordinate2D(latitude: latitude, longitude: longitude)
    let region = CLCircularRegion(center: center, radius: outerRadius, identifier: "keystone.home")
    region.notifyOnEntry = true
    region.notifyOnExit = true
    manager.startMonitoring(for: region)
    monitoring = true
    // Where are we now? Without this the first "home" would only come after the
    // person walks out and back in.
    manager.requestLocation()
  }

  private func stopMonitoring() {
    if monitoring {
      for region in manager.monitoredRegions where region.identifier == "keystone.home" {
        manager.stopMonitoring(for: region)
      }
      monitoring = false
    }
  }

  func stop() {
    stopMonitoring()
    outsideSince = nil
    lastReported = nil
  }

  // MARK: - CLLocationManagerDelegate

  func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
    if manager.authorizationStatus == .authorizedAlways, monitoring {
      // The permission arrived after the request: start watching for real.
      start(latitude: latitude, longitude: longitude, radius: radius)
    }
  }

  func locationManager(_ manager: CLLocationManager, didEnterRegion region: CLRegion) {
    guard region.identifier == "keystone.home" else { return }
    outsideSince = nil
    report(true)
  }

  func locationManager(_ manager: CLLocationManager, didExitRegion region: CLRegion) {
    guard region.identifier == "keystone.home" else { return }
    // Not "away" yet: the phone may only have stepped outside for a moment.
    outsideSince = Date()
    scheduleConfirmation()
  }

  func locationManager(_ manager: CLLocationManager, didUpdateLocations locations: [CLLocation]) {
    guard let loc = locations.last else { return }
    let distance = loc.distance(from: CLLocation(latitude: latitude, longitude: longitude))
    if distance <= innerRadius {
      // Inside: home, right away.
      if outsideSince != nil { outsideSince = nil }
      report(true)
    } else if distance >= outerRadius {
      // Outside: away, but only after the confirmation window.
      if outsideSince == nil { outsideSince = Date() }
      scheduleConfirmation()
    } else {
      // In the hysteresis band: the state does not change, and that is the
      // whole point of the band.
      return
    }
  }

  func locationManager(_ manager: CLLocationManager, didFailWithError error: Error) {
    onError(error.localizedDescription)
  }

  /// Report "away" once the phone has been outside long enough.
  private func scheduleConfirmation() {
    DispatchQueue.main.asyncAfter(deadline: .now() + Self.confirmSeconds) { [weak self] in
      guard let self = self, let since = self.outsideSince else { return }
      // The phone came back inside in the meantime.
      if Date().timeIntervalSince(since) >= Self.confirmSeconds {
        self.outsideSince = nil
        self.report(false)
      }
    }
  }

  /// Tell the caller only when the state actually changed.
  private func report(_ home: Bool) {
    if let last = lastReported, last == home { return }
    lastReported = home
    onChange(home)
  }
}

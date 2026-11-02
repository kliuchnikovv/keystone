import CoreBluetooth
import Foundation

/// The devices around the phone, as seen over Bluetooth.
///
/// The central manager is created only when the scan starts: iOS asks the person for the
/// Bluetooth permission the first time a central manager appears, and it has no business
/// asking before they opened the search.
///
/// The scan looks for everything (no service filter), because the person is looking for
/// whatever is near them — a Matter device, a thermostat, a beacon. Duplicates are
/// allowed because the signal strength is the whole point, and one reading is noise.
final class BleScanner: NSObject, CBCentralManagerDelegate {
  /// How many devices are reported at most. A shop full of beacons is not interesting.
  private static let maxDevices = 50

  /// One report per device per this many seconds. The signal wobbles; the person does not
  /// need to see every wobble.
  private static let minInterval: TimeInterval = 0.25

  private var central: CBCentralManager?
  private var lastReport: [String: Date] = [:]
  private var known: Set<String> = []
  private var currentState = "unknown"

  private let onAdvertisement: ([String: Any?]) -> Void
  private let onState: ([String: Any?]) -> Void

  init(onAdvertisement: @escaping ([String: Any?]) -> Void, onState: @escaping ([String: Any?]) -> Void) {
    self.onAdvertisement = onAdvertisement
    self.onState = onState
    super.init()
  }

  static func stateName(_ state: CBManagerState) -> String {
    switch state {
    case .unknown: return "unknown"
    case .resetting: return "resetting"
    case .unsupported: return "unsupported"
    case .unauthorized: return "unauthorized"
    case .poweredOff: return "poweredOff"
    case .poweredOn: return "ready"
    @unknown default: return "unknown"
    }
  }

  static func authorizationName(_ authorization: CBManagerAuthorization) -> String {
    switch authorization {
    case .notDetermined: return "notDetermined"
    case .restricted: return "restricted"
    case .denied: return "denied"
    case .allowedAlways: return "allowed"
    @unknown default: return "notDetermined"
    }
  }

  /// The state of the radio, as far as we know it.
  var state: String { currentState }

  /// Whether the person allowed Bluetooth. This is a class property, so it can be read
  /// even before the manager exists.
  var authorization: String { Self.authorizationName(CBManager.authorization) }

  func start() {
    if central == nil {
      central = CBCentralManager(delegate: self, queue: nil,
                                 options: [CBCentralManagerOptionShowPowerAlertKey: true])
    }
    currentState = Self.stateName(central?.state ?? .unknown)
    onState(["state": currentState, "authorization": authorization])
    guard let central = central, central.state == .poweredOn else { return }
    central.scanForPeripherals(withServices: nil,
                               options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
  }

  func stop() {
    central?.stopScan()
  }

  // MARK: - CBCentralManagerDelegate

  func centralManagerDidUpdateState(_ central: CBCentralManager) {
    currentState = Self.stateName(central.state)
    onState(["state": currentState, "authorization": authorization])
    if central.state == .poweredOn {
      central.scanForPeripherals(withServices: nil,
                                 options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
    } else {
      central.stopScan()
    }
  }

  func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                      advertisementData: [String: Any], rssi RSSI: NSNumber) {
    let id = peripheral.identifier.uuidString
    let now = Date()
    if let last = lastReport[id], now.timeIntervalSince(last) < Self.minInterval { return }
    lastReport[id] = now
    if !known.contains(id) {
      guard known.count < Self.maxDevices else { return }
      known.insert(id)
    }

    var serviceUuids: [String] = []
    if let uuids = advertisementData[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID] {
      serviceUuids = uuids.map { $0.uuidString.lowercased() }
    }

    var serviceData: [String: [Int]] = [:]
    if let data = advertisementData[CBAdvertisementDataServiceDataKey] as? [CBUUID: Data] {
      for (uuid, bytes) in data {
        serviceData[uuid.uuidString.lowercased()] = bytes.map { Int($0) }
      }
    }

    // The manufacturer data starts with the company id — two bytes, little endian — and
    // the payload of the company follows.
    var manufacturerData: [String: [Int]] = [:]
    if let data = advertisementData[CBAdvertisementDataManufacturerDataKey] as? Data, data.count >= 2 {
      let start = data.startIndex
      let company = Int(data[start]) | (Int(data[start + 1]) << 8)
      manufacturerData[String(company)] = data.dropFirst(2).map { Int($0) }
    }

    onAdvertisement([
      "id": id,
      "rssi": RSSI.intValue,
      "name": advertisementData[CBAdvertisementDataLocalNameKey] as? String,
      "serviceUuids": serviceUuids,
      "serviceData": serviceData,
      "manufacturerData": manufacturerData,
    ])
  }
}

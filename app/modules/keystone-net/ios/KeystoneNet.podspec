Pod::Spec.new do |s|
  s.name           = 'KeystoneNet'
  s.version        = '0.1.0'
  s.summary        = 'Certificate-pinned WebSocket and mDNS discovery for the Keystone hub'
  s.description    = 'Pins the hub certificate by SHA-256 fingerprint before a WebSocket opens, and browses _keystone._tcp.'
  s.license        = { :type => 'MIT' }
  s.author         = 'keystone'
  s.homepage       = 'https://github.com/kliuchnikovv/keystone-esp'
  s.platforms      = { :ios => '16.4' }
  s.swift_version  = '5.9'
  s.source         = { git: '' }
  s.static_framework = true

  s.dependency 'ExpoModulesCore'

  s.source_files = "*.{h,m,swift}"
  s.frameworks = 'CoreBluetooth', 'Network'
  s.pod_target_xcconfig = {
    'DEFINES_MODULE' => 'YES',
    'SWIFT_COMPILATION_MODE' => 'wholemodule'
  }
end

#include <doctest/doctest.h>

#include <initializer_list>

#include "core/matter_entity.hpp"

using namespace core::matter;
using hubproto::DeviceType;
using hubproto::Value;
namespace A = hubproto::attr;

namespace {

// An EndpointView over fixed arrays: the struct holds pointers, so the caller owns storage.
struct FakeEndpoint {
  std::uint32_t clusters[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  std::uint16_t types[4] = {0, 0, 0, 0};
  EndpointView view;

  FakeEndpoint(std::uint16_t endpoint_id, std::initializer_list<std::uint32_t> cluster_ids,
               std::initializer_list<std::uint16_t> device_types = {}) {
    std::size_t n = 0;
    for (auto c : cluster_ids) clusters[n++] = c;
    view.endpoint_id = endpoint_id;
    view.clusters = clusters;
    view.cluster_count = n;
    n = 0;
    for (auto t : device_types) types[n++] = t;
    view.device_types = types;
    view.device_type_count = n;
  }
};

const hubproto::AttrDecl* find_attr(const EntityPlan& p, std::uint8_t id) {
  for (std::uint8_t i = 0; i < p.attr_count; ++i) {
    if (p.attrs[i].attr_id == id) return &p.attrs[i];
  }
  return nullptr;
}

const NodeAttr* find_node_attr(const NodeEntity& e, std::uint8_t id) {
  for (std::uint8_t i = 0; i < e.attr_count; ++i) {
    if (e.attrs[i].attr == id) return &e.attrs[i];
  }
  return nullptr;
}

MatterValue as_bool(bool v) {
  MatterValue mv;
  mv.boolean = v;
  return mv;
}

MatterValue as_signed(std::int64_t v) {
  MatterValue mv;
  mv.signed_int = v;
  return mv;
}

MatterValue as_unsigned(std::uint64_t v) {
  MatterValue mv;
  mv.unsigned_int = v;
  return mv;
}

}  // namespace

TEST_CASE("the binding table maps hub attributes to Matter paths and back") {
  const auto* onoff = binding_for_attr(A::kOnOff);
  REQUIRE(onoff != nullptr);
  CHECK(onoff->cluster == kOnOffCluster);
  CHECK(onoff->path == 0x0000);
  CHECK(onoff->type == hubproto::ValueType::Bool);

  const auto* level = binding_for_attr(A::kBrightness);
  REQUIRE(level != nullptr);
  CHECK(level->cluster == kLevelControl);
  CHECK(level->flags == (hubproto::kFlagReadable | hubproto::kFlagWritable | hubproto::kFlagReportable));

  // A read-only sensor attribute is not writable.
  const auto* temp = binding_for_attr(A::kTemperature);
  REQUIRE(temp != nullptr);
  CHECK(temp->cluster == kTemperature);
  CHECK((temp->flags & hubproto::kFlagWritable) == 0);

  // Colour: one hub attribute, one Matter attribute (colour temperature in mireds).
  const auto* ct = binding_for_attr(A::kColorTemp);
  REQUIRE(ct != nullptr);
  CHECK(ct->cluster == kColorControl);
  CHECK(ct->path == 0x0007);

  // The reverse lookup is the same table.
  CHECK(binding_for_path(kTemperature, 0x0000) == temp);
  CHECK(binding_for_path(kOnOffCluster, 0x0000) == onoff);
  CHECK(binding_for_path(0xDEAD, 0x0001) == nullptr);
}

TEST_CASE("an on/off plug becomes a Switch, a light stays a Light") {
  FakeEndpoint plug(1, {kOnOffCluster}, {kOnOffPlugInUnit});
  const auto p = plan_from_endpoint(plug.view);
  CHECK(p.known);
  CHECK(p.device_type == DeviceType::Switch);
  REQUIRE(p.attr_count == 1);
  CHECK(p.attrs[0].attr_id == A::kOnOff);
  CHECK(p.attrs[0].value_type == hubproto::ValueType::Bool);
  CHECK(p.attrs[0].flags == (hubproto::kFlagReadable | hubproto::kFlagWritable | hubproto::kFlagReportable));

  FakeEndpoint bulb(1, {kOnOffCluster}, {0x0100 /* OnOffLight */});
  CHECK(plan_from_endpoint(bulb.view).device_type == DeviceType::Light);

  FakeEndpoint dimmable(2, {kOnOffCluster, kLevelControl}, {0x0101 /* DimmableLight */});
  const auto d = plan_from_endpoint(dimmable.view);
  CHECK(d.device_type == DeviceType::Light);
  REQUIRE(d.attr_count == 2);
  CHECK(find_attr(d, A::kOnOff) != nullptr);
  CHECK(find_attr(d, A::kBrightness) != nullptr);

  // A dimmable plug keeps the plug's device type.
  FakeEndpoint dim_plug(3, {kOnOffCluster, kLevelControl}, {kDimmablePlugInUnit});
  CHECK(plan_from_endpoint(dim_plug.view).device_type == DeviceType::Switch);
}

TEST_CASE("a thermostat is Climate and its LocalTemperature is not declared twice") {
  // The thermostat cluster carries temperature itself, and real devices also expose the
  // standalone TemperatureMeasurement cluster on the same endpoint; the hub has one attribute.
  FakeEndpoint thermostat(1, {kThermostat, kTemperature}, {0x0301 /* Thermostat device type */});
  const auto p = plan_from_endpoint(thermostat.view);
  CHECK(p.known);
  CHECK(p.device_type == DeviceType::Climate);
  CHECK(p.attr_count == 3);
  std::size_t temperatures = 0;
  for (std::uint8_t i = 0; i < p.attr_count; ++i) {
    if (p.attrs[i].attr_id == A::kTemperature) ++temperatures;
  }
  CHECK(temperatures == 1);
  CHECK(find_attr(p, A::kSetpoint) != nullptr);
  CHECK(find_attr(p, A::kMode) != nullptr);
  CHECK((find_attr(p, A::kSetpoint)->flags & hubproto::kFlagWritable) != 0);
}

TEST_CASE("sensors: temperature and humidity on one endpoint, presence and contact get their own type") {
  FakeEndpoint sensor(1, {kTemperature, kHumidity});
  const auto s = plan_from_endpoint(sensor.view);
  CHECK(s.known);
  CHECK(s.device_type == DeviceType::Sensor);
  CHECK(s.attr_count == 2);
  CHECK(find_attr(s, A::kTemperature)->value_type == hubproto::ValueType::I16);
  CHECK(find_attr(s, A::kHumidity)->value_type == hubproto::ValueType::U16);

  FakeEndpoint occupancy(2, {kOccupancy});
  CHECK(plan_from_endpoint(occupancy.view).device_type == DeviceType::Occupancy);

  FakeEndpoint contact(3, {kBooleanState});
  CHECK(plan_from_endpoint(contact.view).device_type == DeviceType::Contact);

  // A battery-only endpoint is a Sensor: nothing better describes a PowerSource.
  FakeEndpoint battery(4, {kPowerSource});
  const auto b = plan_from_endpoint(battery.view);
  CHECK(b.device_type == DeviceType::Sensor);
  CHECK(find_attr(b, A::kBattery)->flags == (hubproto::kFlagReadable | hubproto::kFlagReportable));
}

TEST_CASE("an endpoint with no cluster the hub models is skipped") {
  FakeEndpoint root(0, {0x001D /* Descriptor */, 0x001E /* Binding */, 0x001F /* AccessControl */});
  const auto p = plan_from_endpoint(root.view);
  CHECK_FALSE(p.known);
  CHECK(p.attr_count == 0);
}

TEST_CASE("one node: measurement endpoints merge into one entity, actuators stay separate") {
  // TIMMERFLOTTE: a temperature sensor endpoint and a humidity sensor endpoint in one box.
  FakeEndpoint temperature(1, {kTemperature}, {0x0302 /* Temperature Sensor */});
  FakeEndpoint humidity(2, {kHumidity}, {0x0307 /* Humidity Sensor */});
  FakeEndpoint root(0, {0x001D, 0x001E, 0x001F});
  const EndpointView endpoints[] = {root.view, temperature.view, humidity.view};

  const NodePlan n = plan_node(endpoints, 3);
  CHECK(n.known);
  REQUIRE(n.entity_count == 1);
  const NodeEntity& sensor = n.entities[0];
  CHECK(sensor.device_type == DeviceType::Sensor);
  REQUIRE(sensor.attr_count == 2);

  const NodeAttr* t = find_node_attr(sensor, A::kTemperature);
  REQUIRE(t != nullptr);
  CHECK(t->type == hubproto::ValueType::I16);
  CHECK(t->endpoint == 1);  // the path to read and subscribe
  CHECK(t->cluster == kTemperature);
  CHECK(t->path == 0x0000);

  const NodeAttr* h = find_node_attr(sensor, A::kHumidity);
  REQUIRE(h != nullptr);
  CHECK(h->type == hubproto::ValueType::U16);
  CHECK(h->endpoint == 2);
  CHECK(h->cluster == kHumidity);

  // Two on/off endpoints are two things to control, so they do not merge.
  FakeEndpoint left(1, {kOnOffCluster}, {0x0100});
  FakeEndpoint right(2, {kOnOffCluster}, {0x0100});
  const EndpointView gangs[] = {left.view, right.view};
  const NodePlan two = plan_node(gangs, 2);
  CHECK(two.known);
  REQUIRE(two.entity_count == 2);
  CHECK(two.entities[0].device_type == DeviceType::Light);
  CHECK(two.entities[0].attrs[0].endpoint == 1);
  CHECK(two.entities[1].attrs[0].endpoint == 2);

  // A node with nothing the hub models produces no entities at all.
  const EndpointView only_root[] = {root.view};
  CHECK_FALSE(plan_node(only_root, 1).known);
  CHECK(plan_node(nullptr, 0).known == false);
}

TEST_CASE("a thermostat's temperature keeps the thermostat path, not the sensor one") {
  // kTemperature appears twice in the table; the endpoint decides which row applies.
  FakeEndpoint thermostat(1, {kThermostat, kTemperature}, {0x0301});
  const NodePlan n = plan_node(&thermostat.view, 1);
  REQUIRE(n.entity_count == 1);
  CHECK(n.entities[0].device_type == DeviceType::Climate);
  const NodeAttr* t = find_node_attr(n.entities[0], A::kTemperature);
  REQUIRE(t != nullptr);
  CHECK(t->cluster == kThermostat);
  CHECK(t->endpoint == 1);
}

TEST_CASE("an app command becomes a Matter write or invoke") {
  SUBCASE("on/off is the OnOff command, not a write") {
    const WritePlan p = to_matter(A::kOnOff, Value::make_bool(true), 7);
    CHECK(p.ok());
    CHECK(p.kind == SendKind::kInvoke);
    CHECK(p.endpoint == 7);
    CHECK(p.cluster == kOnOffCluster);
    CHECK(p.id == kOnOffCommand);
    REQUIRE(p.field_count == 1);
    CHECK(p.fields[0].tag == 0);
    CHECK(p.fields[0].value.type == hubproto::ValueType::Bool);
    CHECK(p.fields[0].value.as_bool());
  }

  SUBCASE("brightness is MoveToLevelWithOnOff with the rest of the fields zeroed") {
    const WritePlan p = to_matter(A::kBrightness, Value::make_u8(128), 3);
    CHECK(p.ok());
    CHECK(p.kind == SendKind::kInvoke);
    CHECK(p.cluster == kLevelControl);
    CHECK(p.id == kMoveToLevelWithOnOff);
    REQUIRE(p.field_count == 4);
    CHECK(p.fields[0].value.as_u8() == 128);
    CHECK(p.fields[1].value.type == hubproto::ValueType::U16);
    CHECK(p.fields[1].value.as_u16() == 0);  // TransitionTime: immediately
    CHECK(p.fields[2].value.as_u8() == 0);   // OptionsMask
    CHECK(p.fields[3].value.as_u8() == 0);   // OptionsOverride
  }

  SUBCASE("colour temperature and the thermostat are plain writes") {
    const WritePlan ct = to_matter(A::kColorTemp, Value::make_u16(370), 2);
    CHECK(ct.kind == SendKind::kWrite);
    CHECK(ct.cluster == kColorControl);
    CHECK(ct.id == 0x0007);
    REQUIRE(ct.field_count == 1);
    CHECK(ct.fields[0].value.as_u16() == 370);

    // The setpoint crosses back to centi-degrees, the inverse of to_hub.
    const WritePlan sp = to_matter(A::kSetpoint, Value::make_i16(215), 1);
    CHECK(sp.kind == SendKind::kWrite);
    CHECK(sp.cluster == kThermostat);
    CHECK(sp.id == 0x0012);
    REQUIRE(sp.field_count == 1);
    CHECK(sp.fields[0].value.type == hubproto::ValueType::I16);
    CHECK(sp.fields[0].value.as_i16() == 2150);

    const WritePlan mode = to_matter(A::kMode, Value::make_enum8(4), 1);
    CHECK(mode.kind == SendKind::kWrite);
    CHECK(mode.id == 0x001C);
    REQUIRE(mode.field_count == 1);
    CHECK(mode.fields[0].value.type == hubproto::ValueType::U8);  // an enum is a uint8 on the wire
    CHECK(mode.fields[0].value.as_u8() == 4);
  }

  SUBCASE("read-only attributes and out-of-range values produce nothing to send") {
    CHECK_FALSE(to_matter(A::kTemperature, Value::make_i16(215), 1).ok());
    CHECK_FALSE(to_matter(A::kHumidity, Value::make_u16(450), 1).ok());
    CHECK_FALSE(to_matter(A::kEvent, Value::make_u8(1), 1).ok());
    CHECK_FALSE(to_matter(A::kBrightness, Value::make_u8(255), 1).ok());
    CHECK_FALSE(to_matter(A::kMode, Value::make_enum8(2), 1).ok());
    // A value of the wrong type is a protocol error, not something to send.
    CHECK_FALSE(to_matter(A::kOnOff, Value::make_u8(1), 1).ok());
    CHECK_FALSE(to_matter(A::kBrightness, Value::make_bool(true), 1).ok());
  }
}

TEST_CASE("Matter values become hub values, unit conversions included") {
  Value v{};

  SUBCASE("booleans") {
    CHECK(to_hub(A::kOnOff, as_bool(true), v));
    CHECK(v.type == hubproto::ValueType::Bool);
    CHECK(v.as_bool());
    CHECK(to_hub(A::kOnOff, as_bool(false), v));
    CHECK_FALSE(v.as_bool());
    CHECK(to_hub(A::kOccupied, as_bool(true), v));
    CHECK(v.as_bool());
    CHECK(to_hub(A::kContact, as_bool(true), v));
    CHECK(v.as_bool());
  }

  SUBCASE("temperature: 0.01 °C in, 0.1 °C out, rounded") {
    CHECK(to_hub(A::kTemperature, as_signed(2150), v));
    CHECK(v.type == hubproto::ValueType::I16);
    CHECK(v.as_i16() == 215);
    CHECK(to_hub(A::kTemperature, as_signed(2155), v));
    CHECK(v.as_i16() == 216);
    CHECK(to_hub(A::kTemperature, as_signed(-55), v));
    CHECK(v.as_i16() == -6);
    // 0x8000 is the device's "invalid" marker.
    CHECK_FALSE(to_hub(A::kTemperature, as_signed(INT16_MIN), v));
  }

  SUBCASE("humidity: 0.01 % in, 0.1 % out") {
    CHECK(to_hub(A::kHumidity, as_unsigned(4567), v));
    CHECK(v.type == hubproto::ValueType::U16);
    CHECK(v.as_u16() == 457);
    CHECK(to_hub(A::kHumidity, as_unsigned(10000), v));
    CHECK(v.as_u16() == 1000);
    CHECK_FALSE(to_hub(A::kHumidity, as_unsigned(10001), v));
    CHECK_FALSE(to_hub(A::kHumidity, as_unsigned(0xFFFF), v));
  }

  SUBCASE("illuminance: log scale in, lux out") {
    CHECK(to_hub(A::kIlluminance, as_unsigned(0), v));
    CHECK(v.as_u16() == 0);
    CHECK(to_hub(A::kIlluminance, as_unsigned(1), v));
    CHECK(v.as_u16() == 1);
    CHECK_FALSE(to_hub(A::kIlluminance, as_unsigned(0xFFFF), v));
  }

  SUBCASE("brightness and colour temperature are passed through") {
    CHECK(to_hub(A::kBrightness, as_unsigned(128), v));
    CHECK(v.type == hubproto::ValueType::U8);
    CHECK(v.as_u8() == 128);
    CHECK_FALSE(to_hub(A::kBrightness, as_unsigned(255), v));  // ZCL "invalid"
    CHECK(to_hub(A::kColorTemp, as_unsigned(370), v));
    CHECK(v.type == hubproto::ValueType::U16);
    CHECK(v.as_u16() == 370);
  }

  SUBCASE("battery: half-percent in, percent out") {
    CHECK(to_hub(A::kBattery, as_unsigned(200), v));
    CHECK(v.as_u8() == 100);
    CHECK(to_hub(A::kBattery, as_unsigned(1), v));
    CHECK(v.as_u8() == 1);
    CHECK_FALSE(to_hub(A::kBattery, as_unsigned(255), v));  // 0xFF = unknown
  }

  SUBCASE("thermostat setpoint and mode") {
    CHECK(to_hub(A::kSetpoint, as_signed(2100), v));
    CHECK(v.as_i16() == 210);
    CHECK(to_hub(A::kMode, as_unsigned(4 /* Heat */), v));
    CHECK(v.type == hubproto::ValueType::Enum8);
    CHECK(v.as_enum8() == 4);
    CHECK_FALSE(to_hub(A::kMode, as_unsigned(2), v));  // not a mode we model
  }

  SUBCASE("an attribute the hub does not model is rejected") {
    CHECK_FALSE(to_hub(A::kPowerW, as_unsigned(12), v));
    CHECK_FALSE(to_hub(A::kEvent, as_unsigned(1), v));
  }
}

# arduinojson — ArduinoJson 7.x Library (vendored)

Vendored copy of the ArduinoJson C++ JSON library, adapted as an ESP-IDF component.

## Overview

ArduinoJson is a popular, efficient, and well-tested JSON library for embedded systems. This component provides a local vendored copy adapted for ESP-IDF, used by every other component that needs JSON encoding/decoding.

## Dependencies

None (standalone header-only library).

## Library Features (ArduinoJson 7.x)

- **Zero-copy parsing**: Input buffer not duplicated in memory
- **Static allocation**: Optional fixed-size document to avoid heap fragmentation
- **Schema-based extraction**: Parse only needed fields for memory efficiency
- **Pretty-printing**: Optional formatted JSON output
- **MessagePack support**: Binary serialization alternative

## Key Types

### JsonDocument

Main container for JSON data:

```cpp
JsonDocument doc;  // Dynamic allocation (uses heap)
```

For static allocation (recommended in embedded):

```cpp
StaticJsonDocument<256> doc;  // 256-byte fixed buffer (no heap)
```

### JsonObject

```cpp
JsonObject obj = doc.to<JsonObject>();
obj["key"] = "value";
obj["number"] = 42;
obj["nested"] = obj.createNestedObject();
```

### JsonArray

```cpp
JsonArray arr = doc.to<JsonArray>();
arr.add("item1");
arr.add(42);
arr.add(true);
```

## Public API (Subset)

### Deserialization

```cpp
// Parse JSON string
DeserializationError error = deserializeJson(doc, input);
if (error) {
    Serial.print("Failed to parse: ");
    Serial.println(error.c_str());
}
```

### Serialization

```cpp
// Serialize to string
char buffer[256];
serializeJson(doc, buffer, sizeof(buffer));

// Serialize to Stream (UART, file, etc.)
serializeJson(doc, Serial);

// Pretty-print
serializeJsonPretty(doc, Serial);
```

### Document Access

```cpp
// Read values
const char* name = doc["name"];
int age = doc["age"];
bool active = doc["active"];

// Access nested objects
JsonObject nested = doc["nested"];
const char* value = nested["key"];

// Check existence
if (doc.containsKey("optional")) {
    // ...
}

// Iterate arrays
for (JsonObject item : doc["items"].as<JsonArray>()) {
    Serial.println(item["name"].as<const char*>());
}
```

## Usage in ZHAC

### hap_json Component

```cpp
#include <ArduinoJson.h>

bool hap_json_encode_set_attr(uint64_t ieee, const char* key, int64_t val,
                               uint8_t* buf, size_t cap, uint16_t* out_len) {
    JsonDocument doc;
    char ieee_str[20];
    snprintf(ieee_str, sizeof(ieee_str), "0x%016llX", ieee);

    doc["ieee"] = ieee_str;
    doc["key"] = key;
    doc["val"] = val;

    *out_len = serializeJson(doc, (char*)buf, cap);
    return *out_len > 0;
}
```

### Decoding HAP Payloads

```cpp
bool hap_json_decode_set_attr(const uint8_t* payload, uint16_t len, HapSetAttrReq* out) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, (const char*)payload, len);
    if (error) return false;

    out->ieee = parse_ieee_hex_string(doc["ieee"]);
    strlcpy(out->key, doc["key"], sizeof(out->key));
    out->int_val = doc["val"];
    return true;
}
```

### REST API Responses

```cpp
JsonDocument doc;
JsonArray devices = doc.to<JsonArray>();

for (int i = 0; i < count; i++) {
    JsonObject dev = devices.createNestedObject();
    dev["ieee"] = format_ieee(pool[i].ieee);
    dev["name"] = pool[i].friendly_name;
    dev["online"] = pool[i].last_seen > 0;
}

char buf[1024];
size_t len = serializeJson(doc, buf, sizeof(buf));
httpd_resp_send(req, buf, len);
```

## Memory Considerations

### Dynamic vs Static

| Approach | Pros | Cons |
|----------|------|------|
| `JsonDocument` (dynamic) | Flexible size | Heap fragmentation |
| `StaticJsonDocument<N>` | No heap usage | Fixed max size |

### Recommendations for ESP32

```cpp
// Small payloads (< 256 bytes)
StaticJsonDocument<256> doc;

// Medium payloads (< 1 KB)
StaticJsonDocument<1024> doc;

// Large payloads (use dynamic, but be careful)
JsonDocument doc;
doc.capacity(4096);  // Pre-allocate
```

### Memory Capacity

ArduinoJson 7.x automatically grows capacity as needed for dynamic documents. For static documents, size must be sufficient for the entire JSON structure.

## ArduinoJson 7.x Improvements

Over ArduinoJson 6.x:
- Improved memory usage (smaller document overhead)
- Better deserialization performance
- Simplified API (removed `JsonVariant` complexity)
- Native ESP-IDF compatibility

## Component Structure

```
components/arduinojson/
├── CMakeLists.txt          # ESP-IDF component definition
├── idf_component.yml       # Component manifest (local component)
└── src/
    ├── ArduinoJson.h       # Main header (C-style include)
    ├── ArduinoJson.hpp     # C++ header
    └── ArduinoJson/        # Full library implementation
        ├── Document/       # JsonDocument
        ├── Deserialization/ # deserializeJson
        ├── Serialization/  # serializeJson
        ├── Json/           # JsonObject, JsonArray
        └── ...             # Additional modules
```

## Related Components

- **hap_json** — Primary consumer, JSON encode/decode for HAP payloads
- **REST API** (in `zhac-net-core/`) — JSON request/response handling
- **ws_server** — JSON WebSocket message formatting
- **mqtt_gw** — JSON MQTT payload construction
